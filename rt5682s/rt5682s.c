#define DESCRIPTOR_DEF
#include "rt5682s.h"
#include "registers.h"

#define bool int
#define MHz 1000000

static ULONG Rt5682DebugLevel = 100;
static ULONG Rt5682DebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

NTSTATUS rt5682s_set_component_pll(PRTEK_CONTEXT  pDevice,
	int pll_id, int source, unsigned int freq_in,
	unsigned int freq_out);
NTSTATUS rt5682s_set_tdm_slot(PRTEK_CONTEXT  pDevice, unsigned int tx_mask,
	unsigned int rx_mask, int slots, int slot_width);
NTSTATUS rt5682s_set_component_sysclk(PRTEK_CONTEXT  pDevice,
	int clk_id);
void rt5682s_update_reclock(IN PRTEK_CONTEXT pDevice);

unsigned int __sw_hweight32(unsigned int w)
{
	w -= (w >> 1) & 0x55555555;
	w = (w & 0x33333333) + ((w >> 2) & 0x33333333);
	w = (w + (w >> 4)) & 0x0f0f0f0f;
	return (w * 0x01010101) >> 24;
}

#define hweight_long __sw_hweight32

NTSTATUS
DriverEntry(
	__in PDRIVER_OBJECT  DriverObject,
	__in PUNICODE_STRING RegistryPath
)
{
	NTSTATUS               status = STATUS_SUCCESS;
	WDF_DRIVER_CONFIG      config;
	WDF_OBJECT_ATTRIBUTES  attributes;

	RtekPrint(DEBUG_LEVEL_INFO, DBG_INIT,
		"Driver Entry\n");

	WDF_DRIVER_CONFIG_INIT(&config, Rt5682EvtDeviceAdd);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

	//
	// Create a framework driver object to represent our driver.
	//

	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		&attributes,
		&config,
		WDF_NO_HANDLE
	);

	if (!NT_SUCCESS(status))
	{
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_INIT,
			"WdfDriverCreate failed with status 0x%x\n", status);
	}

	return status;
}


static NTSTATUS rt5682s_reg_write(PRTEK_CONTEXT pDevice, uint16_t reg, uint16_t data)
{
	uint16_t rawdata[2];
	rawdata[0] = RtlUshortByteSwap(reg);
	rawdata[1] = RtlUshortByteSwap(data);
	return SpbWriteDataSynchronously(&pDevice->I2CContext, rawdata, sizeof(rawdata));
}

static NTSTATUS rt5682s_reg_read(PRTEK_CONTEXT pDevice, uint16_t reg, uint16_t* data)
{
	uint16_t reg_swap = RtlUshortByteSwap(reg);
	uint16_t data_swap = 0;
	NTSTATUS ret = SpbXferDataSynchronously(&pDevice->I2CContext, &reg_swap, sizeof(uint16_t), &data_swap, sizeof(uint16_t));
	*data = RtlUshortByteSwap(data_swap);
	return ret;
}

static NTSTATUS rt5682s_reg_update(
	_In_ PRTEK_CONTEXT pDevice,
	uint16_t reg,
	uint16_t mask,
	uint16_t val
) {
	uint16_t tmp = 0, orig = 0;

	NTSTATUS status = rt5682s_reg_read(pDevice, reg, &orig);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	tmp = orig & ~mask;
	tmp |= val & mask;

	if (tmp != orig) {
		status = rt5682s_reg_write(pDevice, reg, tmp);
	}
	return status;
}

static NTSTATUS rt5682s_reg_burstWrite(PRTEK_CONTEXT pDevice, struct reg* regs, int regCount) {
	NTSTATUS status = STATUS_NO_MEMORY;
	for (int i = 0; i < regCount; i++) {
		struct reg* regToSet = &regs[i];
		status = rt5682s_reg_write(pDevice, regToSet->reg, regToSet->val);
		if (!NT_SUCCESS(status)) {
			return status;
		}
	}
	return status;
}

static Platform GetPlatform() {
	int cpuinfo[4];
	__cpuidex(cpuinfo, 0, 0);

	int temp = cpuinfo[2];
	cpuinfo[2] = cpuinfo[3];
	cpuinfo[3] = temp;

	char vendorName[13];
	RtlZeroMemory(vendorName, 13);
	memcpy(vendorName, &cpuinfo[1], 12);

	__cpuidex(cpuinfo, 1, 0);

	UINT16 family = (cpuinfo[0] >> 8) & 0xF;
	UINT8 model = (cpuinfo[0] >> 4) & 0xF;
	UINT8 stepping = cpuinfo[0] & 0xF;
	if (family == 0xF || family == 0x6) {
		model += (((cpuinfo[0] >> 16) & 0xF) << 4);
	}
	if (family == 0xF) {
		family += (cpuinfo[0] >> 20) & 0xFF;
	}

	if (strcmp(vendorName, "AuthenticAMD") == 0) {
		if (family == 25 && model == 80)
			return PlatformRyzenCezanne;
		else
			return PlatformRyzenMendocino; //family 23 for Mendocino (model 160)
	} else if (strcmp(vendorName, "GenuineIntel") == 0) {
		if (model == 122 || model == 92) //92 = Apollo Lake but keep for compatibility
			return PlatformGeminiLake;
		else
			return PlatformTigerLake;
	}
	return PlatformNone;
}

static void rt5682s_calibrate(_In_  PRTEK_CONTEXT  pDevice)
{
	int count;
	UINT16 value;

	rt5682s_reg_write(pDevice, RT5682S_PWR_ANLG_1, 0xaa80);
	
	LARGE_INTEGER WaitInterval;
	WaitInterval.QuadPart = -10 * 1000 * 15;
	KeDelayExecutionThread(KernelMode, false, &WaitInterval);

	rt5682s_reg_write(pDevice, RT5682S_PWR_ANLG_1, 0xfa80);
	rt5682s_reg_write(pDevice, RT5682S_PWR_DIG_1, 0x01c0);
	rt5682s_reg_write(pDevice, RT5682S_MICBIAS_2, 0x0380);
	rt5682s_reg_write(pDevice, RT5682S_GLB_CLK, 0x8000);
	rt5682s_reg_write(pDevice, RT5682S_ADDA_CLK_1, 0x1001);
	rt5682s_reg_write(pDevice, RT5682S_CHOP_DAC_2, 0x3030);
	rt5682s_reg_write(pDevice, RT5682S_CHOP_ADC, 0xb000);
	rt5682s_reg_write(pDevice, RT5682S_STO1_ADC_MIXER, 0x686c);
	rt5682s_reg_write(pDevice, RT5682S_CAL_REC, 0x5151);
	rt5682s_reg_write(pDevice, RT5682S_HP_CALIB_CTRL_2, 0x0321);
	rt5682s_reg_write(pDevice, RT5682S_HP_LOGIC_CTRL_2, 0x0004);
	rt5682s_reg_write(pDevice, RT5682S_HP_CALIB_CTRL_1, 0x7c00);
	rt5682s_reg_write(pDevice, RT5682S_HP_CALIB_CTRL_1, 0xfc00);

	for (count = 0; count < 60; count++) {
		rt5682s_reg_read(pDevice, RT5682S_HP_CALIB_ST_1, &value);
		if (!(value & 0x8000))
			break;

		WaitInterval.QuadPart = -10 * 1000 * 10;
		KeDelayExecutionThread(KernelMode, false, &WaitInterval);
	}

	if (count >= 60)
		DbgPrint("HP Calibration Failure\n");

	/* restore settings */
	rt5682s_reg_write(pDevice, RT5682S_MICBIAS_2, 0x0180);
	rt5682s_reg_write(pDevice, RT5682S_CAL_REC, 0x5859);
	rt5682s_reg_write(pDevice, RT5682S_STO1_ADC_MIXER, 0xc0c4);
	rt5682s_reg_write(pDevice, RT5682S_HP_CALIB_CTRL_2, 0x0320);
	rt5682s_reg_write(pDevice, RT5682S_PWR_DIG_1, 0x00c0);
	rt5682s_reg_write(pDevice, RT5682S_PWR_ANLG_1, 0x0800);
	rt5682s_reg_write(pDevice, RT5682S_GLB_CLK, 0x0000);
}

NTSTATUS BOOTCODEC(
	_In_  PRTEK_CONTEXT  devContext
)
{
	NTSTATUS status = rt5682s_reg_write(devContext, RT5682S_RESET, 0);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	LARGE_INTEGER WaitInterval;
	WaitInterval.QuadPart = -10 * 1000 * 50;
	KeDelayExecutionThread(KernelMode, false, &WaitInterval);

	UINT16 val;
	status = rt5682s_reg_read(devContext, RT5682S_DEVICE_ID, &val);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	if (val != DEVICE_ID) {
		DbgPrint("Device with ID 0x%x is not ALC5682S\n", val);
		return STATUS_NO_SUCH_DEVICE;
	}

	{
		//Apply patch list
		struct reg patch_list[] = {
			{RT5682S_I2C_CTRL,			0x0007},
			{RT5682S_DIG_IN_CTRL_1,			0x0000},
			{RT5682S_CHOP_DAC_2,			0x2020},
			{RT5682S_VREF_REC_OP_FB_CAP_CTRL_2,	0x0101},
			{RT5682S_VREF_REC_OP_FB_CAP_CTRL_1,	0x80c0},
			{RT5682S_HP_CALIB_CTRL_9,		0x0002},
			{RT5682S_DEPOP_1,			0x0000},
			{RT5682S_HP_CHARGE_PUMP_2,		0x3c15},
			{RT5682S_DAC1_DIG_VOL,			0xfefe},
			{RT5682S_SAR_IL_CMD_2,			0xac00},
			{RT5682S_SAR_IL_CMD_3,			0x024c},
			{RT5682S_CBJ_CTRL_6,			0x0804},
		};
		status = rt5682s_reg_burstWrite(devContext, patch_list, sizeof(patch_list) / sizeof(struct reg));
		if (!NT_SUCCESS(status)) {
			return status;
		}
	}

	rt5682s_reg_update(devContext, RT5682S_PWR_DIG_2,
		RT5682S_DLDO_I_LIMIT_MASK, RT5682S_DLDO_I_LIMIT_DIS);

	WaitInterval.QuadPart = -10 * 1000 * 20;
	KeDelayExecutionThread(KernelMode, false, &WaitInterval);

	rt5682s_calibrate(devContext);

	rt5682s_reg_update(devContext, RT5682S_MICBIAS_2,
		RT5682S_PWR_CLK25M_MASK | RT5682S_PWR_CLK1M_MASK,
		RT5682S_PWR_CLK25M_PD | RT5682S_PWR_CLK1M_PU);
	rt5682s_reg_update(devContext, RT5682S_PWR_ANLG_1,
		RT5682S_PWR_BG, RT5682S_PWR_BG);
	rt5682s_reg_update(devContext, RT5682S_HP_LOGIC_CTRL_2,
		RT5682S_HP_SIG_SRC_MASK, RT5682S_HP_SIG_SRC_1BIT_CTL);
	rt5682s_reg_update(devContext, RT5682S_HP_CHARGE_PUMP_2,
		RT5682S_PM_HP_MASK, RT5682S_PM_HP_HV);
	rt5682s_reg_update(devContext, RT5682S_HP_AMP_DET_CTL_1,
		RT5682S_CP_SW_SIZE_MASK, RT5682S_CP_SW_SIZE_L | RT5682S_CP_SW_SIZE_S);

	//Set Clocks (AlderLake)

	struct reg setClocksAlderLake[] = {
		//{RT5682S_PLL_CTRL_3, 0x0006}, //reclocking should set these
		//{RT5682S_PLL_CTRL_4, 0x0203},
		//{RT5682S_PLL_CTRL_7, 0x0001},
		{RT5682S_RC_CLK_CTRL, 0xc009},
		{RT5682S_I2S2_M_CLK_CTRL_1, 0x0020},

		{RT5682S_GLB_CLK, 0x0000}, //Reclocking should set 0x4000

		//set bclk1 ratio ADL
		//{RT5682S_TDM_TCON_CTRL_1, 0x0020}, //Reclocking should set this

		{RT5682S_ADDA_CLK_1, 0x1021},
	};

	status = rt5682s_reg_burstWrite(devContext, setClocksAlderLake, sizeof(setClocksAlderLake) / sizeof(struct reg));
	if (!NT_SUCCESS(status)) {
		return status;
	}

	struct reg finishInit2[] = {

		//set clk
		{RT5682S_PLL_TRACK_2, 0x0100},

		//Update more defaults
		{RT5682S_STO1_ADC_DIG_VOL, 0x2faf},
		{RT5682S_REC_MIXER, 0x1940},
		{RT5682S_I2S2_SDP, 0x4000},
		{RT5682S_PLL_TRACK_3, 0x0100},
		{RT5682S_GPIO_CTRL_1, 0x6960},
		{RT5682S_DMIC_CTRL_1, 0x0800},

		//Headphone defaults
		{RT5682S_HP_CTRL_2, 0x6001},
		{RT5682S_STO1_ADC_MIXER, 0x6064},
		{RT5682S_PWR_DIG_1, 0x8dd1},
		{RT5682S_PWR_DIG_2, 0x840a},
		{RT5682S_PWR_ANLG_2, 0x8001},
		{RT5682S_PWR_ANLG_3, 0x52a1},
		{RT5682S_CLK_DET, 0x8000},
		{RT5682S_DEPOP_1, 0x7b},

		{RT5682S_BIAS_CUR_CTRL_12, 0xa82a}
	};

	status = rt5682s_reg_burstWrite(devContext, finishInit2, sizeof(finishInit2) / sizeof(struct reg));
	if (!NT_SUCCESS(status)) {
		return status;
	}

	struct reg setDefaultsAlderLake[] = {
		//For Alder Lake
		{RT5682S_HP_CTRL_1, 0x8080},
		{RT5682S_DAC1_DIG_VOL, 0xecec},
		{RT5682S_STO1_DAC_MIXER, 0x2080},
		{RT5682S_I2S1_SDP, 0x0000}, //Reclocking should set 0x2220
		{RT5682S_TDM_ADDA_CTRL_1, 0x8000},
		{RT5682S_TDM_ADDA_CTRL_2, 0x0080}
	};

	status = rt5682s_reg_burstWrite(devContext, setDefaultsAlderLake, sizeof(setDefaultsAlderLake) / sizeof(struct reg));
	if (!NT_SUCCESS(status)) {
		return status;
	}

	rt5682s_update_reclock(devContext);

	if (GetPlatform() == PlatformRyzenMendocino) {
		struct reg setDefaultsMendocino[] = {
			//For Mendocino
			{RT5682S_DAC1_DIG_VOL, 0xeaea},
			{RT5682S_STO1_ADC_DIG_VOL, 0x6565},
			{RT5682S_STO1_DAC_MIXER, 0xa0a0},
			{RT5682S_A_DAC1_MUX, 0x0311},
			{RT5682S_REC_MIXER, 0x0d40}
		};

		status = rt5682s_reg_burstWrite(devContext, setDefaultsMendocino, sizeof(setDefaultsMendocino) / sizeof(struct reg));
		if (!NT_SUCCESS(status)) {
			return status;
		}

		rt5682s_reg_update(devContext, RT5682S_I2S1_SDP, RT5682S_I2S_DF_MASK,
			RT5682S_I2S_DF_PCM_A);
		rt5682s_reg_update(devContext, RT5682S_TDM_TCON_CTRL_1, RT5682S_TDM_BCLK_MS1_MASK | RT5682S_TDM_DF_MASK,
			RT5682S_TDM_BCLK_MS1_128 | RT5682S_TDM_DF_PCM_A);

		rt5682s_set_component_pll(devContext, RT5682S_PLL2, RT5682S_PLL_S_MCLK, 48000000, 48000 * 512);
		rt5682s_set_tdm_slot(devContext, 3, 3, 8, 16);
		rt5682s_set_component_sysclk(devContext, RT5682S_SCLK_S_PLL2);
	}
	else if (GetPlatform() == PlatformRyzenCezanne) {
		struct reg setDefaultsCezanne[] = {
			//For Cezanne
			{RT5682S_DAC1_DIG_VOL, 0xfcfc},
			{RT5682S_STO1_ADC_DIG_VOL, 0x6565},
			{RT5682S_STO1_DAC_MIXER, 0xa0a0},
			{RT5682S_A_DAC1_MUX, 0x0311},
			{RT5682S_REC_MIXER, 0x0d40},

			//Set Clocks for Cezanne
			{RT5682S_I2S1_SDP, 0x3300},
			{RT5682S_ADDA_CLK_1, 0x1121},
			{RT5682S_TDM_ADDA_CTRL_2, 0x0000},
			{RT5682S_TDM_TCON_CTRL_1, 0x0101}
		};

		status = rt5682s_reg_burstWrite(devContext, setDefaultsCezanne, sizeof(setDefaultsCezanne) / sizeof(struct reg));
		if (!NT_SUCCESS(status)) {
			return status;
		}

		rt5682s_reg_update(devContext, RT5682S_TDM_TCON_CTRL_1, RT5682S_TDM_BCLK_MS1_MASK | RT5682S_TDM_DF_MASK,
			RT5682S_TDM_BCLK_MS1_64 | RT5682S_TDM_DF_I2S);

		rt5682s_set_component_pll(devContext, RT5682S_PLL2, RT5682S_PLL_S_MCLK, 48000000, 48000 * 512);
		rt5682s_set_component_sysclk(devContext, RT5682S_SCLK_S_PLL2);
	}

	//Set Jack Detect 

	{
		int btndet_delay = 16;

		rt5682s_reg_update(devContext, RT5682S_CBJ_CTRL_5,
			RT5682S_JD_FAST_OFF_SRC_MASK, RT5682S_JD_FAST_OFF_SRC_JDH);
		rt5682s_reg_update(devContext, RT5682S_CBJ_CTRL_2,
			RT5682S_EXT_JD_SRC, RT5682S_EXT_JD_SRC_MANUAL);
		rt5682s_reg_update(devContext, RT5682S_CBJ_CTRL_1,
			RT5682S_EMB_JD_MASK | RT5682S_DET_TYPE |
			RT5682S_POL_FAST_OFF_MASK | RT5682S_MIC_CAP_MASK,
			RT5682S_EMB_JD_EN | RT5682S_DET_TYPE |
			RT5682S_POL_FAST_OFF_HIGH | RT5682S_MIC_CAP_HS);
		rt5682s_reg_update(devContext, RT5682S_SAR_IL_CMD_1,
			RT5682S_SAR_POW_MASK, RT5682S_SAR_POW_EN);
		rt5682s_reg_update(devContext, RT5682S_GPIO_CTRL_1,
			RT5682S_GP1_PIN_MASK, RT5682S_GP1_PIN_IRQ);
		rt5682s_reg_update(devContext, RT5682S_PWR_ANLG_3,
			RT5682S_PWR_BGLDO, RT5682S_PWR_BGLDO);
		rt5682s_reg_update(devContext, RT5682S_PWR_ANLG_2,
			RT5682S_PWR_JD_MASK, RT5682S_PWR_JD_ENABLE);
		rt5682s_reg_update(devContext, RT5682S_RC_CLK_CTRL,
			RT5682S_POW_IRQ | RT5682S_POW_JDH, RT5682S_POW_IRQ | RT5682S_POW_JDH);
		rt5682s_reg_update(devContext, RT5682S_IRQ_CTRL_2,
			RT5682S_JD1_EN_MASK | RT5682S_JD1_POL_MASK,
			RT5682S_JD1_EN | RT5682S_JD1_POL_NOR);
		rt5682s_reg_update(devContext, RT5682S_4BTN_IL_CMD_4,
			RT5682S_4BTN_IL_HOLD_WIN_MASK | RT5682S_4BTN_IL_CLICK_WIN_MASK,
			(btndet_delay << RT5682S_4BTN_IL_HOLD_WIN_SFT | btndet_delay));
		rt5682s_reg_update(devContext, RT5682S_4BTN_IL_CMD_5,
			RT5682S_4BTN_IL_HOLD_WIN_MASK | RT5682S_4BTN_IL_CLICK_WIN_MASK,
			(btndet_delay << RT5682S_4BTN_IL_HOLD_WIN_SFT | btndet_delay));
		rt5682s_reg_update(devContext, RT5682S_4BTN_IL_CMD_6,
			RT5682S_4BTN_IL_HOLD_WIN_MASK | RT5682S_4BTN_IL_CLICK_WIN_MASK,
			(btndet_delay << RT5682S_4BTN_IL_HOLD_WIN_SFT | btndet_delay));
		rt5682s_reg_update(devContext, RT5682S_4BTN_IL_CMD_7,
			RT5682S_4BTN_IL_HOLD_WIN_MASK | RT5682S_4BTN_IL_CLICK_WIN_MASK,
			(btndet_delay << RT5682S_4BTN_IL_HOLD_WIN_SFT | btndet_delay));
	}

	return STATUS_SUCCESS;
}

int CsAudioArg2 = 1;

VOID
CSAudioRegisterEndpoint(
	PRTEK_CONTEXT pDevice
) {
	CsAudioArg arg;
	RtlZeroMemory(&arg, sizeof(CsAudioArg));
	arg.argSz = sizeof(CsAudioArg);
	arg.endpointType = CSAudioEndpointTypeHeadphone;
	arg.endpointRequest = CSAudioEndpointRegister;
	ExNotifyCallback(pDevice->CSAudioAPICallback, &arg, &CsAudioArg2);

	arg.endpointType = CSAudioEndpointTypeMicJack;
	ExNotifyCallback(pDevice->CSAudioAPICallback, &arg, &CsAudioArg2); //register both in case user decides to record first
}

void rt5682s_update_reclock(IN PRTEK_CONTEXT pDevice) {
	UINT32 mclk = pDevice->mclk;
	UINT32 freq = pDevice->freq;
	UINT32 slotWidth = pDevice->slotWidth;

	if (!pDevice->ReclockRequested)
		return;

	UINT32 outclk = freq * 512;
	if (mclk != outclk)
		rt5682s_set_component_pll(pDevice, RT5682S_PLL2, RT5682S_PLL_S_MCLK, mclk, outclk);
	rt5682s_set_tdm_slot(pDevice, 1, 1, 2, slotWidth);
	rt5682s_set_component_sysclk(pDevice, mclk == outclk ? RT5682S_SCLK_S_MCLK : RT5682S_SCLK_S_PLL2);
	rt5682s_reg_update(pDevice, RT5682S_PWR_ANLG_3,
		RT5682S_PWR_LDO_PLLB | RT5682S_PWR_BIAS_PLLB | RT5682S_RSTB_PLLB | RT5682S_PWR_PLLB,
		mclk != outclk ? (RT5682S_PWR_LDO_PLLB | RT5682S_PWR_BIAS_PLLB | RT5682S_RSTB_PLLB | RT5682S_PWR_PLLB) : 0);
}

void StartStopSpeaker(
	IN PRTEK_CONTEXT pDevice, 
	IN BOOLEAN start) {
	CsAudioArg localArg;
	RtlZeroMemory(&localArg, sizeof(CsAudioArg));
	localArg.argSz = sizeof(localArg);
	localArg.endpointType = CSAudioEndpointTypeSpeaker;
	localArg.endpointRequest = start ? CSAudioEndpointStart : CSAudioEndpointStop;
	ExNotifyCallback(pDevice->CSAudioAPICallback, &localArg, &CsAudioArg2);
}

VOID
CsAudioCallbackFunction(
	IN PRTEK_CONTEXT pDevice,
	CsAudioArg* arg,
	PVOID Argument2
) {
	if (!pDevice) {
		return;
	}

	if (Argument2 == &CsAudioArg2) {
		return;
	}

	pDevice->CSAudioManaged = TRUE;

	CsAudioArg localArg;
	RtlZeroMemory(&localArg, sizeof(CsAudioArg));
	RtlCopyMemory(&localArg, arg, min(arg->argSz, sizeof(CsAudioArg)));

	if (localArg.endpointType == CSAudioEndpointTypeDSP && localArg.endpointRequest == CSAudioEndpointRegister) {
		CSAudioRegisterEndpoint(pDevice);
	}
	else if (localArg.endpointType != CSAudioEndpointTypeHeadphone &&
		localArg.endpointType != CSAudioEndpointTypeMicJack) { //check both in case user decides to record first
		return;
	}

	if ((localArg.endpointRequest == CSAudioEndpointStart || localArg.endpointRequest == CSAudioEndpointStop) &&
		localArg.endpointType == CSAudioEndpointTypeHeadphone) {
		pDevice->HeadphonePlaying = (localArg.endpointRequest == CSAudioEndpointStart);
		if (pDevice->JackType == 0 && GetPlatform() == PlatformRyzenCezanne) {
			StartStopSpeaker(pDevice, localArg.endpointRequest == CSAudioEndpointStart);
		}
	}

	if (localArg.endpointRequest == CSAudioEndpointI2SParameters &&
		localArg.i2sParameters.version >= 1) { //Supports version 1 or higher

		Platform currentPlatform = GetPlatform();
		if (currentPlatform == PlatformTigerLake) { //Reclock requested
			UINT32 mclk = localArg.i2sParameters.mclk;
			UINT32 freq = localArg.i2sParameters.frequency;
			UINT32 slotWidth = localArg.i2sParameters.valid_bits;

			pDevice->mclk = mclk;
			pDevice->freq = freq;
			pDevice->slotWidth = slotWidth;
			pDevice->ReclockRequested = TRUE;

			rt5682s_update_reclock(pDevice);
		}
	}
}

static const struct pll_calc_map plla_table[] = {
	{2048000, 24576000, 0, 46, 2, true, false, false, false},
	{256000, 24576000, 0, 382, 2, true, false, false, false},
	{512000, 24576000, 0, 190, 2, true, false, false, false},
	{4096000, 24576000, 0, 22, 2, true, false, false, false},
	{1024000, 24576000, 0, 94, 2, true, false, false, false},
	{11289600, 22579200, 1, 22, 2, false, false, false, false},
	{1411200, 22579200, 0, 62, 2, true, false, false, false},
	{2822400, 22579200, 0, 30, 2, true, false, false, false},
	{12288000, 24576000, 1, 22, 2, false, false, false, false},
	{1536000, 24576000, 0, 62, 2, true, false, false, false},
	{3072000, 24576000, 0, 30, 2, true, false, false, false},
	{24576000, 49152000, 4, 22, 0, false, false, false, false},
	{3072000, 49152000, 0, 30, 0, true, false, false, false},
	{6144000, 49152000, 0, 30, 0, false, false, false, false},
	{49152000, 98304000, 10, 22, 0, false, true, false, false},
	{6144000, 98304000, 0, 30, 0, false, true, false, false},
	{12288000, 98304000, 1, 22, 0, false, true, false, false},
	{48000000, 3840000, 10, 22, 23, false, false, false, false},
	{24000000, 3840000, 4, 22, 23, false, false, false, false},
	{19200000, 3840000, 3, 23, 23, false, false, false, false},
	{38400000, 3840000, 8, 23, 23, false, false, false, false},
};

static const struct pll_calc_map pllb_table[] = {
	{48000000, 24576000, 8, 6, 3, false, false, false, false},
	{48000000, 22579200, 23, 12, 3, false, false, false, true},
	{24000000, 24576000, 3, 6, 3, false, false, false, false},
	{24000000, 22579200, 23, 26, 3, false, false, false, true},
	{19200000, 24576000, 2, 6, 3, false, false, false, false},
	{19200000, 22579200, 3, 5, 3, false, false, false, true},
	{38400000, 24576000, 6, 6, 3, false, false, false, false},
	{38400000, 22579200, 8, 5, 3, false, false, false, true},
	{3840000, 49152000, 0, 6, 0, true, false, false, false},
};

static int find_pll_inter_combination(unsigned int f_in, unsigned int f_out,
	struct pll_calc_map* a, struct pll_calc_map* b)
{
	int i, j;

	/* Look at PLLA table */
	for (i = 0; i < sizeof(plla_table) / sizeof(plla_table[0]); i++) {
		if (plla_table[i].freq_in == f_in && plla_table[i].freq_out == f_out) {
			memcpy(a, plla_table + i, sizeof(*a));
			return USE_PLLA;
		}
	}

	/* Look at PLLB table */
	for (i = 0; i < sizeof(pllb_table) / sizeof(pllb_table[0]); i++) {
		if (pllb_table[i].freq_in == f_in && pllb_table[i].freq_out == f_out) {
			memcpy(b, pllb_table + i, sizeof(*b));
			return USE_PLLB;
		}
	}

	/* Find a combination of PLLA & PLLB */
	for (i = (sizeof(plla_table) / sizeof(plla_table[0])) - 1; i >= 0; i--) {
		if (plla_table[i].freq_in == f_in && plla_table[i].freq_out == 3840000) {
			for (j = (sizeof(pllb_table) / sizeof(pllb_table[0])) - 1; j >= 0; j--) {
				if (pllb_table[j].freq_in == 3840000 &&
					pllb_table[j].freq_out == f_out) {
					memcpy(a, plla_table + i, sizeof(*a));
					memcpy(b, pllb_table + j, sizeof(*b));
					return USE_PLLAB;
				}
			}
		}
	}

	return -1;
}


NTSTATUS rt5682s_set_component_pll(PRTEK_CONTEXT  pDevice,
	int pll_id, int source, unsigned int freq_in,
	unsigned int freq_out)
{
	struct pll_calc_map a_map, b_map;

	if (!freq_in || !freq_out) {
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "PLL disabled\n");
		rt5682s_reg_update(pDevice, RT5682S_GLB_CLK,
			RT5682S_SCLK_SRC_MASK, RT5682S_CLK_SRC_MCLK << RT5682S_SCLK_SRC_SFT);
		return STATUS_SUCCESS;
	}

	switch (source) {
	case RT5682S_PLL_S_MCLK:
		rt5682s_reg_update(pDevice, RT5682S_GLB_CLK,
			RT5682S_PLL_SRC_MASK, RT5682S_PLL_SRC_MCLK);
		break;
	case RT5682S_PLL_S_BCLK1:
		rt5682s_reg_update(pDevice, RT5682S_GLB_CLK,
			RT5682S_PLL_SRC_MASK, RT5682S_PLL_SRC_BCLK1);
		break;
	default:
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "Unknown PLL Source %d\n", source);
		return STATUS_INVALID_PARAMETER;
	}

	int pll_comb = find_pll_inter_combination(freq_in, freq_out,
		&a_map, &b_map);

	if ((pll_id == RT5682S_PLL1 && pll_comb == USE_PLLA) ||
		(pll_id == RT5682S_PLL2 && (pll_comb == USE_PLLB ||
			pll_comb == USE_PLLAB))) {
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "Supported freq conversion for PLL%d:(%d->%d): %d\n",
			pll_id + 1, freq_in, freq_out, rt5682s->pll_comb);
	}
	else {
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "Unsupported freq conversion for PLL%d:(%d->%d): %d\n",
			pll_id + 1, freq_in, freq_out, rt5682s->pll_comb);
		return STATUS_INVALID_PARAMETER;
	}

	if (pll_comb == USE_PLLA || pll_comb == USE_PLLAB) {
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "PLLA: fin=%d fout=%d m_bp=%d k_bp=%d m=%d n=%d k=%d\n",
			a_map.freq_in, a_map.freq_out, a_map.m_bp, a_map.k_bp,
			(a_map.m_bp ? 0 : a_map.m), a_map.n, (a_map.k_bp ? 0 : a_map.k));
		rt5682s_reg_update(pDevice, RT5682S_PLL_CTRL_1,
			RT5682S_PLLA_N_MASK, a_map.n);
		rt5682s_reg_update(pDevice, RT5682S_PLL_CTRL_2,
			RT5682S_PLLA_M_MASK | RT5682S_PLLA_K_MASK,
			a_map.m << RT5682S_PLLA_M_SFT | a_map.k);
		rt5682s_reg_update(pDevice, RT5682S_PLL_CTRL_6,
			RT5682S_PLLA_M_BP_MASK | RT5682S_PLLA_K_BP_MASK,
			a_map.m_bp << RT5682S_PLLA_M_BP_SFT |
			a_map.k_bp << RT5682S_PLLA_K_BP_SFT);
	}

	if (pll_comb == USE_PLLB || pll_comb == USE_PLLAB) {
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "PLLB: fin=%d fout=%d m_bp=%d k_bp=%d m=%d n=%d k=%d byp_ps=%d sel_ps=%d\n",
			b_map.freq_in, b_map.freq_out, b_map.m_bp, b_map.k_bp,
			(b_map.m_bp ? 0 : b_map.m), b_map.n, (b_map.k_bp ? 0 : b_map.k),
			b_map.byp_ps, b_map.sel_ps);
		rt5682s_reg_update(pDevice, RT5682S_PLL_CTRL_3,
			RT5682S_PLLB_N_MASK, b_map.n);
		rt5682s_reg_update(pDevice, RT5682S_PLL_CTRL_4,
			RT5682S_PLLB_M_MASK | RT5682S_PLLB_K_MASK,
			b_map.m << RT5682S_PLLB_M_SFT | b_map.k);
		rt5682s_reg_update(pDevice, RT5682S_PLL_CTRL_6,
			RT5682S_PLLB_SEL_PS_MASK | RT5682S_PLLB_BYP_PS_MASK |
			RT5682S_PLLB_M_BP_MASK | RT5682S_PLLB_K_BP_MASK,
			b_map.sel_ps << RT5682S_PLLB_SEL_PS_SFT |
			b_map.byp_ps << RT5682S_PLLB_BYP_PS_SFT |
			b_map.m_bp << RT5682S_PLLB_M_BP_SFT |
			b_map.k_bp << RT5682S_PLLB_K_BP_SFT);
	}

	if (pll_comb == USE_PLLB)
		rt5682s_reg_update(pDevice, RT5682S_PLL_CTRL_7,
			RT5682S_PLLB_SRC_MASK, RT5682S_PLLB_SRC_DFIN);

	return STATUS_SUCCESS;
}

NTSTATUS rt5682s_set_tdm_slot(PRTEK_CONTEXT  pDevice, unsigned int tx_mask,
	unsigned int rx_mask, int slots, int slot_width)
{
	unsigned int cl, val = 0, tx_slotnum;

	if (tx_mask || rx_mask)
		rt5682s_reg_update(pDevice, RT5682S_TDM_ADDA_CTRL_2, RT5682S_TDM_EN, RT5682S_TDM_EN);
	else
		rt5682s_reg_update(pDevice, RT5682S_TDM_ADDA_CTRL_2, RT5682S_TDM_EN, 0);

	/* Tx slot configuration */
	tx_slotnum = hweight_long(tx_mask);
	if (tx_slotnum) {
		if (tx_slotnum > slots) {
			RtekPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "Invalid or oversized Tx slots.\n");
			return STATUS_INVALID_PARAMETER;
		}
		val |= (tx_slotnum - 1) << RT5682S_TDM_ADC_DL_SFT;
	}

	switch (slots) {
	case 4:
		val |= RT5682S_TDM_TX_CH_4;
		val |= RT5682S_TDM_RX_CH_4;
		break;
	case 6:
		val |= RT5682S_TDM_TX_CH_6;
		val |= RT5682S_TDM_RX_CH_6;
		break;
	case 8:
		val |= RT5682S_TDM_TX_CH_8;
		val |= RT5682S_TDM_RX_CH_8;
		break;
	case 2:
		break;
	default:
		return STATUS_INVALID_PARAMETER;
	}

	rt5682s_reg_update(pDevice, RT5682S_TDM_CTRL,
		RT5682S_TDM_TX_CH_MASK | RT5682S_TDM_RX_CH_MASK |
		RT5682S_TDM_ADC_DL_MASK, val);

	switch (slot_width) {
	case 8:
		if (tx_mask || rx_mask)
			return STATUS_INVALID_PARAMETER;
		cl = RT5682S_I2S1_TX_CHL_8 | RT5682S_I2S1_RX_CHL_8;
		break;
	case 16:
		val = RT5682S_TDM_CL_16;
		cl = RT5682S_I2S1_TX_CHL_16 | RT5682S_I2S1_RX_CHL_16;
		break;
	case 20:
		val = RT5682S_TDM_CL_20;
		cl = RT5682S_I2S1_TX_CHL_20 | RT5682S_I2S1_RX_CHL_20;
		break;
	case 24:
		val = RT5682S_TDM_CL_24;
		cl = RT5682S_I2S1_TX_CHL_24 | RT5682S_I2S1_RX_CHL_24;
		break;
	case 32:
		val = RT5682S_TDM_CL_32;
		cl = RT5682S_I2S1_TX_CHL_32 | RT5682S_I2S1_RX_CHL_32;
		break;
	default:
		return STATUS_INVALID_PARAMETER;
	}

	rt5682s_reg_update(pDevice, RT5682S_TDM_TCON_CTRL_1,
		RT5682S_TDM_CL_MASK, val);
	rt5682s_reg_update(pDevice, RT5682S_I2S1_SDP,
		RT5682S_I2S1_TX_CHL_MASK | RT5682S_I2S1_RX_CHL_MASK, cl);

	return STATUS_SUCCESS;
}

NTSTATUS rt5682s_set_component_sysclk(PRTEK_CONTEXT  pDevice,
	int clk_id)
{
	unsigned int src = 0;

	switch (clk_id) {
	case RT5682S_SCLK_S_MCLK:
		src = RT5682S_CLK_SRC_MCLK;
		break;
	case RT5682S_SCLK_S_PLL1:
		src = RT5682S_CLK_SRC_PLL1;
		break;
	case RT5682S_SCLK_S_PLL2:
		src = RT5682S_CLK_SRC_PLL2;
		break;
	case RT5682S_SCLK_S_RCCLK:
		src = RT5682S_CLK_SRC_RCCLK;
		break;
	default:
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "Invalid clock id (%d)\n", clk_id);
		return STATUS_INVALID_PARAMETER;
	}

	rt5682s_reg_update(pDevice, RT5682S_GLB_CLK,
		RT5682S_SCLK_SRC_MASK, src << RT5682S_SCLK_SRC_SFT);
	rt5682s_reg_update(pDevice, RT5682S_ADDA_CLK_1,
		RT5682S_I2S_M_CLK_SRC_MASK, src << RT5682S_I2S_M_CLK_SRC_SFT);
	rt5682s_reg_update(pDevice, RT5682S_I2S2_M_CLK_CTRL_1,
		RT5682S_I2S2_M_CLK_SRC_MASK, src << RT5682S_I2S2_M_CLK_SRC_SFT);

	RtekPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "Sysclk is %dHz and clock id is %d\n",
		freq, clk_id);

	return STATUS_SUCCESS;
}

NTSTATUS
OnPrepareHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesRaw,
	_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

This routine caches the SPB resource connection ID.

Arguments:

FxDevice - a handle to the framework device object
FxResourcesRaw - list of translated hardware resources that
the PnP manager has assigned to the device
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PRTEK_CONTEXT pDevice = GetDeviceContext(FxDevice);
	BOOLEAN fSpbResourceFound = FALSE;
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;

	UNREFERENCED_PARAMETER(FxResourcesRaw);

	//
	// Parse the peripheral's resources.
	//

	ULONG resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

	for (ULONG i = 0; i < resourceCount; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor;
		UCHAR Class;
		UCHAR Type;

		pDescriptor = WdfCmResourceListGetDescriptor(
			FxResourcesTranslated, i);

		switch (pDescriptor->Type)
		{
		case CmResourceTypeConnection:
			//
			// Look for I2C or SPI resource and save connection ID.
			//
			Class = pDescriptor->u.Connection.Class;
			Type = pDescriptor->u.Connection.Type;
			if (Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
				Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
			{
				if (fSpbResourceFound == FALSE)
				{
					status = STATUS_SUCCESS;
					pDevice->I2CContext.I2cResHubId.LowPart = pDescriptor->u.Connection.IdLowPart;
					pDevice->I2CContext.I2cResHubId.HighPart = pDescriptor->u.Connection.IdHighPart;
					fSpbResourceFound = TRUE;
				}
				else
				{
				}
			}
			break;
		default:
			//
			// Ignoring all other resource types.
			//
			break;
		}
	}

	//
	// An SPB resource is required.
	//

	if (fSpbResourceFound == FALSE)
	{
		status = STATUS_NOT_FOUND;
	}

	status = SpbTargetInitialize(FxDevice, &pDevice->I2CContext);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	return status;
}

NTSTATUS
OnReleaseHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

Arguments:

FxDevice - a handle to the framework device object
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PRTEK_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	UNREFERENCED_PARAMETER(FxResourcesTranslated);

	SpbTargetDeinitialize(FxDevice, &pDevice->I2CContext);

	if (pDevice->CSAudioAPICallbackObj) {
		ExUnregisterCallback(pDevice->CSAudioAPICallbackObj);
		pDevice->CSAudioAPICallbackObj = NULL;
	}

	if (pDevice->CSAudioAPICallback) {
		ObfDereferenceObject(pDevice->CSAudioAPICallback);
		pDevice->CSAudioAPICallback = NULL;
	}

	return status;
}

NTSTATUS
OnSelfManagedIoInit(
	_In_
	WDFDEVICE FxDevice
) {
	PRTEK_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	// CS Audio Callback

	UNICODE_STRING CSAudioCallbackAPI;
	RtlInitUnicodeString(&CSAudioCallbackAPI, L"\\CallBack\\CsAudioCallbackAPI");


	OBJECT_ATTRIBUTES attributes;
	InitializeObjectAttributes(&attributes,
		&CSAudioCallbackAPI,
		OBJ_KERNEL_HANDLE | OBJ_OPENIF | OBJ_CASE_INSENSITIVE | OBJ_PERMANENT,
		NULL,
		NULL
	);
	status = ExCreateCallback(&pDevice->CSAudioAPICallback, &attributes, TRUE, TRUE);
	if (!NT_SUCCESS(status)) {

		return status;
	}

	pDevice->CSAudioAPICallbackObj = ExRegisterCallback(pDevice->CSAudioAPICallback,
		CsAudioCallbackFunction,
		pDevice
	);
	if (!pDevice->CSAudioAPICallbackObj) {

		return STATUS_NO_CALLBACK_ACTIVE;
	}

	CSAudioRegisterEndpoint(pDevice);

	return status;
}

NTSTATUS
OnD0Entry(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine allocates objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PRTEK_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	pDevice->JackType = 0;

	status = BOOTCODEC(pDevice);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	pDevice->ConnectInterrupt = true;

	return status;
}

NTSTATUS
OnD0Exit(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine destroys objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PRTEK_CONTEXT pDevice = GetDeviceContext(FxDevice);

	pDevice->ConnectInterrupt = false;

	return STATUS_SUCCESS;
}

enum {
	SAR_PWR_OFF,
	SAR_PWR_NORMAL,
	SAR_PWR_SAVING,
};

static void rt5682s_sar_power_mode(PRTEK_CONTEXT pDevice, int mode)
{
	LARGE_INTEGER WaitInterval;

	switch (mode) {
	case SAR_PWR_SAVING:
		rt5682s_reg_update(pDevice, RT5682S_CBJ_CTRL_3,
			RT5682S_CBJ_IN_BUF_MASK, RT5682S_CBJ_IN_BUF_DIS);
		rt5682s_reg_update(pDevice, RT5682S_CBJ_CTRL_1,
			RT5682S_MB1_PATH_MASK | RT5682S_MB2_PATH_MASK,
			RT5682S_CTRL_MB1_REG | RT5682S_CTRL_MB2_REG);
		rt5682s_reg_update(pDevice, RT5682S_SAR_IL_CMD_1,
			RT5682S_SAR_BUTDET_MASK | RT5682S_SAR_BUTDET_POW_MASK |
			RT5682S_SAR_SEL_MB1_2_CTL_MASK, RT5682S_SAR_BUTDET_DIS |
			RT5682S_SAR_BUTDET_POW_SAV | RT5682S_SAR_SEL_MB1_2_MANU);
		
		WaitInterval.QuadPart = -10 * 1000 * 5;
		KeDelayExecutionThread(KernelMode, false, &WaitInterval);

		rt5682s_reg_update(pDevice, RT5682S_SAR_IL_CMD_1,
			RT5682S_SAR_BUTDET_MASK, RT5682S_SAR_BUTDET_EN);

		WaitInterval.QuadPart = -10 * 1000 * 5;
		KeDelayExecutionThread(KernelMode, false, &WaitInterval);

		rt5682s_reg_update(pDevice, RT5682S_SAR_IL_CMD_2,
			RT5682S_SAR_ADC_PSV_MASK, RT5682S_SAR_ADC_PSV_ENTRY);
		break;
	case SAR_PWR_NORMAL:
		rt5682s_reg_update(pDevice, RT5682S_CBJ_CTRL_3,
			RT5682S_CBJ_IN_BUF_MASK, RT5682S_CBJ_IN_BUF_EN);
		rt5682s_reg_update(pDevice, RT5682S_CBJ_CTRL_1,
			RT5682S_MB1_PATH_MASK | RT5682S_MB2_PATH_MASK,
			RT5682S_CTRL_MB1_FSM | RT5682S_CTRL_MB2_FSM);
		rt5682s_reg_update(pDevice, RT5682S_SAR_IL_CMD_1,
			RT5682S_SAR_SEL_MB1_2_CTL_MASK, RT5682S_SAR_SEL_MB1_2_AUTO);
		
		WaitInterval.QuadPart = -10 * 1000 * 5;
		KeDelayExecutionThread(KernelMode, false, &WaitInterval);

		rt5682s_reg_update(pDevice, RT5682S_SAR_IL_CMD_1,
			RT5682S_SAR_BUTDET_MASK | RT5682S_SAR_BUTDET_POW_MASK,
			RT5682S_SAR_BUTDET_EN | RT5682S_SAR_BUTDET_POW_NORM);
		break;
	case SAR_PWR_OFF:
		rt5682s_reg_update(pDevice, RT5682S_CBJ_CTRL_1,
			RT5682S_MB1_PATH_MASK | RT5682S_MB2_PATH_MASK,
			RT5682S_CTRL_MB1_FSM | RT5682S_CTRL_MB2_FSM);
		rt5682s_reg_update(pDevice, RT5682S_SAR_IL_CMD_1,
			RT5682S_SAR_BUTDET_MASK | RT5682S_SAR_BUTDET_POW_MASK |
			RT5682S_SAR_SEL_MB1_2_CTL_MASK, RT5682S_SAR_BUTDET_DIS |
			RT5682S_SAR_BUTDET_POW_SAV | RT5682S_SAR_SEL_MB1_2_MANU);
		break;
	default:
		DbgPrint("Invalid SAR Power mode: %d\n", mode);
		break;
	}
}

static void rt5682s_enable_push_button_irq(PRTEK_CONTEXT pDevice,
	bool enable)
{

	if (enable) {
		rt5682s_reg_update(pDevice, RT5682S_SAR_IL_CMD_13,
			RT5682S_SAR_SOUR_MASK, RT5682S_SAR_SOUR_BTN);
		rt5682s_reg_update(pDevice, RT5682S_SAR_IL_CMD_1,
			RT5682S_SAR_BUTDET_MASK | RT5682S_SAR_BUTDET_POW_MASK |
			RT5682S_SAR_SEL_MB1_2_CTL_MASK, RT5682S_SAR_BUTDET_EN |
			RT5682S_SAR_BUTDET_POW_NORM | RT5682S_SAR_SEL_MB1_2_AUTO);
		rt5682s_reg_write(pDevice, RT5682S_IL_CMD_1, 0x0040);
		rt5682s_reg_update(pDevice, RT5682S_4BTN_IL_CMD_2,
			RT5682S_4BTN_IL_MASK | RT5682S_4BTN_IL_RST_MASK,
			RT5682S_4BTN_IL_EN | RT5682S_4BTN_IL_NOR);
		rt5682s_reg_update(pDevice, RT5682S_IRQ_CTRL_3,
			RT5682S_IL_IRQ_MASK, RT5682S_IL_IRQ_EN);
	}
	else {
		rt5682s_reg_update(pDevice, RT5682S_IRQ_CTRL_3,
			RT5682S_IL_IRQ_MASK, RT5682S_IL_IRQ_DIS);
		rt5682s_reg_update(pDevice, RT5682S_4BTN_IL_CMD_2,
			RT5682S_4BTN_IL_MASK, RT5682S_4BTN_IL_DIS);
		rt5682s_reg_update(pDevice, RT5682S_SAR_IL_CMD_13,
			RT5682S_SAR_SOUR_MASK, RT5682S_SAR_SOUR_TYPE);
		rt5682s_reg_update(pDevice, RT5682S_SAR_IL_CMD_1,
			RT5682S_SAR_BUTDET_MASK | RT5682S_SAR_BUTDET_POW_MASK |
			RT5682S_SAR_SEL_MB1_2_CTL_MASK, RT5682S_SAR_BUTDET_DIS |
			RT5682S_SAR_BUTDET_POW_SAV | RT5682S_SAR_SEL_MB1_2_MANU);
	}
}

int rt5682s_headset_detect(PRTEK_CONTEXT pDevice, int jack_insert) {
	UINT16 val, count;
	if (jack_insert) {
		rt5682s_enable_push_button_irq(pDevice, false);

		rt5682s_reg_update(pDevice, RT5682S_PWR_ANLG_1,
			RT5682S_PWR_VREF1 | RT5682S_PWR_VREF2 | RT5682S_PWR_MB,
			RT5682S_PWR_VREF1 | RT5682S_PWR_VREF2 | RT5682S_PWR_MB);
		rt5682s_reg_update(pDevice, RT5682S_PWR_ANLG_1,
			RT5682S_PWR_FV1 | RT5682S_PWR_FV2, 0);

		LARGE_INTEGER WaitInterval;
		WaitInterval.QuadPart = -10 * 1000 * 15;
		KeDelayExecutionThread(KernelMode, false, &WaitInterval);

		rt5682s_reg_update(pDevice, RT5682S_PWR_ANLG_1,
			RT5682S_PWR_FV1 | RT5682S_PWR_FV2,
			RT5682S_PWR_FV1 | RT5682S_PWR_FV2);
		rt5682s_reg_update(pDevice, RT5682S_PWR_ANLG_3,
			RT5682S_PWR_CBJ, RT5682S_PWR_CBJ);
		rt5682s_reg_write(pDevice, RT5682S_SAR_IL_CMD_3, 0x0365);
		rt5682s_reg_update(pDevice, RT5682S_HP_CHARGE_PUMP_2,
			RT5682S_OSW_L_MASK | RT5682S_OSW_R_MASK,
			RT5682S_OSW_L_DIS | RT5682S_OSW_R_DIS);
		rt5682s_reg_update(pDevice, RT5682S_SAR_IL_CMD_13,
			RT5682S_SAR_SOUR_MASK, RT5682S_SAR_SOUR_TYPE);
		rt5682s_reg_update(pDevice, RT5682S_CBJ_CTRL_3,
			RT5682S_CBJ_IN_BUF_MASK, RT5682S_CBJ_IN_BUF_EN);
		rt5682s_reg_update(pDevice, RT5682S_CBJ_CTRL_1,
			RT5682S_TRIG_JD_MASK, RT5682S_TRIG_JD_LOW);

		WaitInterval.QuadPart = -10 * 1000 * 45;
		KeDelayExecutionThread(KernelMode, false, &WaitInterval);

		rt5682s_reg_update(pDevice, RT5682S_CBJ_CTRL_1,
			RT5682S_TRIG_JD_MASK, RT5682S_TRIG_JD_HIGH);

		count = 0;
		rt5682s_reg_read(pDevice, RT5682S_CBJ_CTRL_2, &val);
		val &= RT5682S_JACK_TYPE_MASK;
		while (val == 0 && count < 50) {
			WaitInterval.QuadPart = -10 * 1000 * 15;
			KeDelayExecutionThread(KernelMode, false, &WaitInterval);

			rt5682s_reg_read(pDevice, RT5682S_CBJ_CTRL_2, &val);
			val &= RT5682S_JACK_TYPE_MASK;
			count++;
		}

		switch (val) {
		case 0x1:
		case 0x2:
			pDevice->JackType = SND_JACK_HEADSET;
			rt5682s_reg_write(pDevice, RT5682S_SAR_IL_CMD_3, 0x024c);
			rt5682s_reg_update(pDevice, RT5682S_CBJ_CTRL_1,
				RT5682S_FAST_OFF_MASK, RT5682S_FAST_OFF_EN);
			rt5682s_reg_update(pDevice, RT5682S_SAR_IL_CMD_1,
				RT5682S_SAR_SEL_MB1_2_MASK, val << RT5682S_SAR_SEL_MB1_2_SFT);

			rt5682s_enable_push_button_irq(pDevice, true);
			rt5682s_sar_power_mode(pDevice, SAR_PWR_NORMAL);
			break;
		default:
			pDevice->JackType = SND_JACK_HEADPHONE;
		}

		rt5682s_reg_update(pDevice, RT5682S_HP_CHARGE_PUMP_2,
			RT5682S_OSW_L_MASK | RT5682S_OSW_R_MASK,
			RT5682S_OSW_L_EN | RT5682S_OSW_R_EN);
	}
	else {
		rt5682s_sar_power_mode(pDevice, SAR_PWR_OFF);
		rt5682s_enable_push_button_irq(pDevice, false);
		rt5682s_reg_update(pDevice, RT5682S_CBJ_CTRL_1,
			RT5682S_TRIG_JD_MASK, RT5682S_TRIG_JD_LOW);

		rt5682s_reg_update(pDevice, RT5682S_PWR_ANLG_3,
			RT5682S_PWR_CBJ, 0);
		rt5682s_reg_update(pDevice, RT5682S_CBJ_CTRL_1,
			RT5682S_FAST_OFF_MASK, RT5682S_FAST_OFF_DIS);
		rt5682s_reg_update(pDevice, RT5682S_CBJ_CTRL_3,
			RT5682S_CBJ_IN_BUF_MASK, RT5682S_CBJ_IN_BUF_DIS);

		pDevice->JackType = 0;
	}

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Jack Type: %d\n", pDevice->JackType);
	return pDevice->JackType;
}

static int rt5682s_button_detect(PRTEK_CONTEXT pDevice)
{
	UINT16 btn_type, val;

	rt5682s_reg_read(pDevice, RT5682S_4BTN_IL_CMD_1, &val);
	btn_type = val & 0xfff0;
	rt5682s_reg_write(pDevice, RT5682S_4BTN_IL_CMD_1, val);
	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"btn_type=%x\n", btn_type);
	rt5682s_reg_update(pDevice,
		RT5682S_SAR_IL_CMD_2, RT5682S_SAR_ADC_PSV_MASK, RT5682S_SAR_ADC_PSV_ENTRY);

	return btn_type;
}

void rt5682s_jackdetect(PRTEK_CONTEXT pDevice) {
	NTSTATUS status = STATUS_SUCCESS;

	UINT16 val;
	status = rt5682s_reg_read(pDevice, RT5682S_AJD1_CTRL, &val);
	if (!NT_SUCCESS(status)) {
		DbgPrint("Failed to read jack detect\n");
		return;
	}

	val = val & RT5682S_JDH_RS_MASK;
	if (!val) {
		/* jack in */
		if (pDevice->JackType == 0) {
			/* jack was out, report jack type */
			pDevice->JackType = rt5682s_headset_detect(pDevice, 1);
		}
		else if ((pDevice->JackType & SND_JACK_HEADSET) == SND_JACK_HEADSET) {
			/* jack is already in, report button event */
			int btn_type = rt5682s_button_detect(pDevice);
			/**
			 * rt5682 can report three kinds of button behavior,
			 * one click, double click and hold. However,
			 * currently we will report button pressed/released
			 * event. So all the three button behaviors are
			 * treated as button pressed.
			 */
			int rawButton = 0;

			switch (btn_type) {
			case 0x8000:
			case 0x4000:
			case 0x2000:
				rawButton = 1;
				break;
			case 0x1000:
			case 0x0800:
			case 0x0400:
				rawButton = 2;
				break;
			case 0x0200:
			case 0x0100:
			case 0x0080:
				rawButton = 3;
				break;
			case 0x0040:
			case 0x0020:
			case 0x0010:
				rawButton = 4;
				break;
			case 0x0000: /* unpressed */
				break;
			default:
				RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"Unexpected button code 0x%04x\n",
					btn_type);
				break;
			}

			Rt5682MediaReport report;
			report.ReportID = REPORTID_MEDIA;
			report.ControlCode = rawButton;

			size_t bytesWritten;
			Rt5682ProcessVendorReport(pDevice, &report, sizeof(report), &bytesWritten);
		}
	}
	else {
		/* jack out */
		pDevice->JackType = rt5682s_headset_detect(pDevice, 0);
	}

	CsAudioSpecialKeyReport report;
	report.ReportID = REPORTID_SPECKEYS;
	report.ControlCode = CONTROL_CODE_JACK_TYPE;
	report.ControlValue = pDevice->JackType;

	size_t bytesWritten;
	Rt5682ProcessVendorReport(pDevice, &report, sizeof(report), &bytesWritten);
}

VOID
RtekJdetWorkItem(
	IN WDFWORKITEM  WorkItem
)
{
	WDFDEVICE Device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
	PRTEK_CONTEXT pDevice = GetDeviceContext(Device);

	rt5682s_jackdetect(pDevice);

	if (GetPlatform() == PlatformRyzenCezanne) { //Speakers are managed by jack driver on Cezanne
		if (pDevice->JackType) {
			StartStopSpeaker(pDevice, FALSE);
		}
		else {
			StartStopSpeaker(pDevice, pDevice->HeadphonePlaying);
		}
	}
}

BOOLEAN OnInterruptIsr(
	WDFINTERRUPT Interrupt,
	ULONG MessageID) {
	UNREFERENCED_PARAMETER(MessageID);

	WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
	PRTEK_CONTEXT pDevice = GetDeviceContext(Device);

	if (!pDevice->ConnectInterrupt)
		return true;

	NTSTATUS status = STATUS_SUCCESS;

	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_WORKITEM_CONFIG workitemConfig;
	WDFWORKITEM hWorkItem;

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&attributes, RTEK_CONTEXT);
	attributes.ParentObject = Device;
	WDF_WORKITEM_CONFIG_INIT(&workitemConfig, RtekJdetWorkItem);

	WdfWorkItemCreate(&workitemConfig,
		&attributes,
		&hWorkItem);

	WdfWorkItemEnqueue(hWorkItem);

	return true;
}

NTSTATUS
Rt5682EvtDeviceAdd(
	IN WDFDRIVER       Driver,
	IN PWDFDEVICE_INIT DeviceInit
)
{
	NTSTATUS                      status = STATUS_SUCCESS;
	WDF_IO_QUEUE_CONFIG           queueConfig;
	WDF_OBJECT_ATTRIBUTES         attributes;
	WDFDEVICE                     device;
	WDF_INTERRUPT_CONFIG interruptConfig;
	WDFQUEUE                      queue;
	UCHAR                         minorFunction;
	PRTEK_CONTEXT               devContext;

	UNREFERENCED_PARAMETER(Driver);

	PAGED_CODE();

	RtekPrint(DEBUG_LEVEL_INFO, DBG_PNP,
		"Rt5682EvtDeviceAdd called\n");

	//
	// Tell framework this is a filter driver. Filter drivers by default are  
	// not power policy owners. This works well for this driver because
	// HIDclass driver is the power policy owner for HID minidrivers.
	//

	WdfFdoInitSetFilter(DeviceInit);

	{
		WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
		WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

		pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
		pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
		pnpCallbacks.EvtDeviceSelfManagedIoInit = OnSelfManagedIoInit;
		pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
		pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;

		WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);
	}

	//
	// Setup the device context
	//

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, RTEK_CONTEXT);

	//
	// Create a framework device object.This call will in turn create
	// a WDM device object, attach to the lower stack, and set the
	// appropriate flags and attributes.
	//

	status = WdfDeviceCreate(&DeviceInit, &attributes, &device);

	if (!NT_SUCCESS(status))
	{
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfDeviceCreate failed with status code 0x%x\n", status);

		return status;
	}

	{
		WDF_DEVICE_STATE deviceState;
		WDF_DEVICE_STATE_INIT(&deviceState);

		deviceState.NotDisableable = WdfFalse;
		WdfDeviceSetDeviceState(device, &deviceState);
	}

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);

	queueConfig.EvtIoInternalDeviceControl = Rt5682EvtInternalDeviceControl;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&queue
	);

	if (!NT_SUCCESS(status))
	{
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create manual I/O queue to take care of hid report read requests
	//

	devContext = GetDeviceContext(device);

	devContext->FxDevice = device;

	devContext->ReclockRequested = FALSE;

	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

	queueConfig.PowerManaged = WdfFalse;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->ReportQueue
	);

	if (!NT_SUCCESS(status))
	{
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create an interrupt object for hardware notifications
	//
	WDF_INTERRUPT_CONFIG_INIT(
		&interruptConfig,
		OnInterruptIsr,
		NULL);
	interruptConfig.PassiveHandling = TRUE;

	status = WdfInterruptCreate(
		device,
		&interruptConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->Interrupt);

	if (!NT_SUCCESS(status))
	{
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"Error creating WDF interrupt object - %!STATUS!",
			status);

		return status;
	}

	return status;
}

VOID
Rt5682EvtInternalDeviceControl(
	IN WDFQUEUE     Queue,
	IN WDFREQUEST   Request,
	IN size_t       OutputBufferLength,
	IN size_t       InputBufferLength,
	IN ULONG        IoControlCode
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	WDFDEVICE           device;
	PRTEK_CONTEXT     devContext;
	BOOLEAN             completeRequest = TRUE;

	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	device = WdfIoQueueGetDevice(Queue);
	devContext = GetDeviceContext(device);

	RtekPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
		"%s, Queue:0x%p, Request:0x%p\n",
		DbgHidInternalIoctlString(IoControlCode),
		Queue,
		Request
	);

	//
	// Please note that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl. So depending on the ioctl code, we will either
	// use retreive function or escape to WDM to get the UserBuffer.
	//

	switch (IoControlCode)
	{

	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		//
		// Retrieves the device's HID descriptor.
		//
		status = Rt5682GetHidDescriptor(device, Request);
		break;

	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		//
		//Retrieves a device's attributes in a HID_DEVICE_ATTRIBUTES structure.
		//
		status = Rt5682GetDeviceAttributes(Request);
		break;

	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		//
		//Obtains the report descriptor for the HID device.
		//
		status = Rt5682GetReportDescriptor(device, Request);
		break;

	case IOCTL_HID_GET_STRING:
		//
		// Requests that the HID minidriver retrieve a human-readable string
		// for either the manufacturer ID, the product ID, or the serial number
		// from the string descriptor of the device. The minidriver must send
		// a Get String Descriptor request to the device, in order to retrieve
		// the string descriptor, then it must extract the string at the
		// appropriate index from the string descriptor and return it in the
		// output buffer indicated by the IRP. Before sending the Get String
		// Descriptor request, the minidriver must retrieve the appropriate
		// index for the manufacturer ID, the product ID or the serial number
		// from the device extension of a top level collection associated with
		// the device.
		//
		status = Rt5682GetString(Request);
		break;

	case IOCTL_HID_WRITE_REPORT:
	case IOCTL_HID_SET_OUTPUT_REPORT:
		//
		//Transmits a class driver-supplied report to the device.
		//
		status = Rt5682WriteReport(devContext, Request);
		break;

	case IOCTL_HID_READ_REPORT:
	case IOCTL_HID_GET_INPUT_REPORT:
		//
		// Returns a report from the device into a class driver-supplied buffer.
		// 
		status = Rt5682ReadReport(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_SET_FEATURE:
		//
		// This sends a HID class feature report to a top-level collection of
		// a HID class device.
		//
		status = Rt5682SetFeature(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_GET_FEATURE:
		//
		// returns a feature report associated with a top-level collection
		status = Rt5682GetFeature(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_ACTIVATE_DEVICE:
		//
		// Makes the device ready for I/O operations.
		//
	case IOCTL_HID_DEACTIVATE_DEVICE:
		//
		// Causes the device to cease operations and terminate all outstanding
		// I/O requests.
		//
	default:
		status = STATUS_NOT_SUPPORTED;
		break;
	}

	if (completeRequest)
	{
		WdfRequestComplete(Request, status);

		RtekPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s completed, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			Queue,
			Request
		);
	}
	else
	{
		RtekPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s deferred, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			Queue,
			Request
		);
	}

	return;
}

NTSTATUS
Rt5682GetHidDescriptor(
	IN WDFDEVICE Device,
	IN WDFREQUEST Request
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	size_t              bytesToCopy = 0;
	WDFMEMORY           memory;

	UNREFERENCED_PARAMETER(Device);

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682GetHidDescriptor Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputMemory(Request, &memory);

	if (!NT_SUCCESS(status))
	{
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputMemory failed 0x%x\n", status);

		return status;
	}

	//
	// Use hardcoded "HID Descriptor" 
	//
	bytesToCopy = DefaultHidDescriptor.bLength;

	if (bytesToCopy == 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;

		RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"DefaultHidDescriptor is zero, 0x%x\n", status);

		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0, // Offset
		(PVOID)&DefaultHidDescriptor,
		bytesToCopy);

	if (!NT_SUCCESS(status))
	{
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfMemoryCopyFromBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, bytesToCopy);

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682GetHidDescriptor Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
Rt5682GetReportDescriptor(
	IN WDFDEVICE Device,
	IN WDFREQUEST Request
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	ULONG_PTR           bytesToCopy;
	WDFMEMORY           memory;

	PRTEK_CONTEXT devContext = GetDeviceContext(Device);

	UNREFERENCED_PARAMETER(Device);

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682GetReportDescriptor Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputMemory(Request, &memory);
	if (!NT_SUCCESS(status))
	{
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputMemory failed 0x%x\n", status);

		return status;
	}

	//
	// Use hardcoded Report descriptor
	//
	bytesToCopy = DefaultHidDescriptor.DescriptorList[0].wReportLength;

	if (bytesToCopy == 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;

		RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"DefaultHidDescriptor's reportLength is zero, 0x%x\n", status);

		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0,
		(PVOID)DefaultReportDescriptor,
		bytesToCopy);
	if (!NT_SUCCESS(status))
	{
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfMemoryCopyFromBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, bytesToCopy);

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682GetReportDescriptor Exit = 0x%x\n", status);

	return status;
}


NTSTATUS
Rt5682GetDeviceAttributes(
	IN WDFREQUEST Request
)
{
	NTSTATUS                 status = STATUS_SUCCESS;
	PHID_DEVICE_ATTRIBUTES   deviceAttributes = NULL;

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682GetDeviceAttributes Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputBuffer(Request,
		sizeof(HID_DEVICE_ATTRIBUTES),
		(PVOID *)&deviceAttributes,
		NULL);
	if (!NT_SUCCESS(status))
	{
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Set USB device descriptor
	//

	deviceAttributes->Size = sizeof(HID_DEVICE_ATTRIBUTES);
	deviceAttributes->VendorID = RT5682_VID;
	deviceAttributes->ProductID = RT5682_PID;
	deviceAttributes->VersionNumber = RT5682_VERSION;

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, sizeof(HID_DEVICE_ATTRIBUTES));

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682GetDeviceAttributes Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
Rt5682GetString(
	IN WDFREQUEST Request
)
{

	NTSTATUS status = STATUS_SUCCESS;
	PWSTR pwstrID;
	size_t lenID;
	WDF_REQUEST_PARAMETERS params;
	void *pStringBuffer = NULL;

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682GetString Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	switch ((ULONG_PTR)params.Parameters.DeviceIoControl.Type3InputBuffer & 0xFFFF)
	{
	case HID_STRING_ID_IMANUFACTURER:
		pwstrID = L"Rt5682.\0";
		break;

	case HID_STRING_ID_IPRODUCT:
		pwstrID = L"MaxTouch Touch Screen\0";
		break;

	case HID_STRING_ID_ISERIALNUMBER:
		pwstrID = L"123123123\0";
		break;

	default:
		pwstrID = NULL;
		break;
	}

	lenID = pwstrID ? wcslen(pwstrID) * sizeof(WCHAR) + sizeof(UNICODE_NULL) : 0;

	if (pwstrID == NULL)
	{

		RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Rt5682GetString Invalid request type\n");

		status = STATUS_INVALID_PARAMETER;

		return status;
	}

	status = WdfRequestRetrieveOutputBuffer(Request,
		lenID,
		&pStringBuffer,
		&lenID);

	if (!NT_SUCCESS(status))
	{

		RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Rt5682GetString WdfRequestRetrieveOutputBuffer failed Status 0x%x\n", status);

		return status;
	}

	RtlCopyMemory(pStringBuffer, pwstrID, lenID);

	WdfRequestSetInformation(Request, lenID);

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682GetString Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
Rt5682WriteReport(
	IN PRTEK_CONTEXT DevContext,
	IN WDFREQUEST Request
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;
	size_t bytesWritten = 0;

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682WriteReport Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Rt5682WriteReport Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"Rt5682WriteReport No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			case REPORTID_SPECKEYS:
				status = STATUS_SUCCESS;

				CsAudioSpecialKeyReport report;
				report.ReportID = REPORTID_SPECKEYS;
				report.ControlCode = CONTROL_CODE_JACK_TYPE;
				report.ControlValue = DevContext->JackType;

				size_t bytesWritten;
				Rt5682ProcessVendorReport(DevContext, &report, sizeof(report), &bytesWritten);
				break;
			default:

				RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"Rt5682WriteReport Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682WriteReport Exit = 0x%x\n", status);

	return status;

}

NTSTATUS
Rt5682ProcessVendorReport(
	IN PRTEK_CONTEXT DevContext,
	IN PVOID ReportBuffer,
	IN ULONG ReportBufferLen,
	OUT size_t* BytesWritten
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDFREQUEST reqRead;
	PVOID pReadReport = NULL;
	size_t bytesReturned = 0;

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682ProcessVendorReport Entry\n");

	status = WdfIoQueueRetrieveNextRequest(DevContext->ReportQueue,
		&reqRead);

	if (NT_SUCCESS(status))
	{
		status = WdfRequestRetrieveOutputBuffer(reqRead,
			ReportBufferLen,
			&pReadReport,
			&bytesReturned);

		if (NT_SUCCESS(status))
		{
			//
			// Copy ReportBuffer into read request
			//

			if (bytesReturned > ReportBufferLen)
			{
				bytesReturned = ReportBufferLen;
			}

			RtlCopyMemory(pReadReport,
				ReportBuffer,
				bytesReturned);

			//
			// Complete read with the number of bytes returned as info
			//

			WdfRequestCompleteWithInformation(reqRead,
				status,
				bytesReturned);

			RtekPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
				"Rt5682ProcessVendorReport %d bytes returned\n", bytesReturned);

			//
			// Return the number of bytes written for the write request completion
			//

			*BytesWritten = bytesReturned;

			RtekPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
				"%s completed, Queue:0x%p, Request:0x%p\n",
				DbgHidInternalIoctlString(IOCTL_HID_READ_REPORT),
				DevContext->ReportQueue,
				reqRead);
		}
		else
		{
			RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"WdfRequestRetrieveOutputBuffer failed Status 0x%x\n", status);
		}
	}
	else
	{
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfIoQueueRetrieveNextRequest failed Status 0x%x\n", status);
	}

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682ProcessVendorReport Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
Rt5682ReadReport(
	IN PRTEK_CONTEXT DevContext,
	IN WDFREQUEST Request,
	OUT BOOLEAN* CompleteRequest
)
{
	NTSTATUS status = STATUS_SUCCESS;

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682ReadReport Entry\n");

	//
	// Forward this read request to our manual queue
	// (in other words, we are going to defer this request
	// until we have a corresponding write request to
	// match it with)
	//

	status = WdfRequestForwardToIoQueue(Request, DevContext->ReportQueue);

	if (!NT_SUCCESS(status))
	{
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestForwardToIoQueue failed Status 0x%x\n", status);
	}
	else
	{
		*CompleteRequest = FALSE;
	}

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682ReadReport Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
Rt5682SetFeature(
	IN PRTEK_CONTEXT DevContext,
	IN WDFREQUEST Request,
	OUT BOOLEAN* CompleteRequest
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682SetFeature Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Rt5682SetFeature Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"Rt5682WriteReport No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			default:

				RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"Rt5682SetFeature Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682SetFeature Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
Rt5682GetFeature(
	IN PRTEK_CONTEXT DevContext,
	IN WDFREQUEST Request,
	OUT BOOLEAN* CompleteRequest
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682GetFeature Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_XFER_PACKET))
	{
		RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"Rt5682GetFeature Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"Rt5682GetFeature No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			default:

				RtekPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"Rt5682GetFeature Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	RtekPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"Rt5682GetFeature Exit = 0x%x\n", status);

	return status;
}

PCHAR
DbgHidInternalIoctlString(
	IN ULONG IoControlCode
)
{
	switch (IoControlCode)
	{
	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		return "IOCTL_HID_GET_DEVICE_DESCRIPTOR";
	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		return "IOCTL_HID_GET_REPORT_DESCRIPTOR";
	case IOCTL_HID_READ_REPORT:
		return "IOCTL_HID_READ_REPORT";
	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		return "IOCTL_HID_GET_DEVICE_ATTRIBUTES";
	case IOCTL_HID_WRITE_REPORT:
		return "IOCTL_HID_WRITE_REPORT";
	case IOCTL_HID_SET_FEATURE:
		return "IOCTL_HID_SET_FEATURE";
	case IOCTL_HID_GET_FEATURE:
		return "IOCTL_HID_GET_FEATURE";
	case IOCTL_HID_GET_STRING:
		return "IOCTL_HID_GET_STRING";
	case IOCTL_HID_ACTIVATE_DEVICE:
		return "IOCTL_HID_ACTIVATE_DEVICE";
	case IOCTL_HID_DEACTIVATE_DEVICE:
		return "IOCTL_HID_DEACTIVATE_DEVICE";
	case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
		return "IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST";
	case IOCTL_HID_SET_OUTPUT_REPORT:
		return "IOCTL_HID_SET_OUTPUT_REPORT";
	case IOCTL_HID_GET_INPUT_REPORT:
		return "IOCTL_HID_GET_INPUT_REPORT";
	default:
		return "Unknown IOCTL";
	}
}