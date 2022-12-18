# rt5682s
Realtek ALC 5682s I2C Codec driver

Supports:
* Jack Detection
* Headphone output
* Sleep/Wake
* Microphone input

Note:
* Intel SST and AMD ACP proprietary drivers do NOT have documented interfaces, so this driver will not work with them.
* Using this driver on chromebooks with this audio chip will require using CoolStar ACP Audio, CoolStar SST Audio or CoolStar SOF Audio
* Certain chromebooks will also need the Chrome EC + Chrome EC I2C drivers to be able to use this driver

Tested on Framework Laptop Chromebook Edition
