# minimal_OPNA_player
YM2608 (OPNA) player, using minimal amount of hardware to play PC98 music in .VGM and .S98 formats

## Overview
This is a **lightweight** (in current form) opensource project designed to interface a Yamaha YM2608 (OPNA) with an STM32F103C6T6 microcontroller, one of the cheapest ones. You can literally make it on a breadboard!

## Features
- Real hardware! As authentic as could be in "minimal" (also may known as "hell yeah")
- No need in external clock source!
- Micro SD card as a storage. Imagine how many tunes you can put in there!
- Basic components! Well, aside from YM2608 and its DAC, YM3016.
- Comments in (almost) ***EVERY SINGLE LINE*** of code!

## Schematic
Refer to **[`schematic_v1.pdf`](./schematic_v1.pdf)** for pinouts and connections.

## Installation
Install ArduinoIDE, download STM32 core from [here](https://github.com/stm32duino/Arduino_Core_STM32), install [STM32CubeProg](https://www.st.com/en/development-tools/stm32cubeprog.html), install SdFat and GyverOLED libraries (available in library manager). Bam, done!
BEFORE flashing, make sure to edit SdFat configuration in `Documents\Arduino\libraries\SdFat\src\SdFatConfig.h`:
```
line 96 -  #define ENABLE_ARDUINO_SERIAL 0   will free some memory, allowing to disable Serial
line 169 - #define SPI_DRIVER_SELECT 2       will enable software SPI. i have no idea why SPI2 hangs the chip.
```
Yes, it reqires software SPI. I still have no idea why my microcontroller hangs dead when I'm trying to use hardware SPI2.
Compilation - Smallest (-Os) with LTO

## Contributing
please do.

## What's next?
I dunno lul. I wanna make a port to STM32F4 with several firmware versions. Maybe I'll make it chew .M/.M2 files from Kajihara's driver. Since it's still plugged in my Line In, nothing stops me from making it a MIDI controlled synth. Hell yeah.
