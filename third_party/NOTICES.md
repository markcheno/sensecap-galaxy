# Third-party code and attribution

The galaxy viewer and source catalog derive from [rhammell/pyportal-galaxy](https://github.com/rhammell/pyportal-galaxy). The original PyPortal firmware is retained in `firmware/pyportal_galaxy/`. Astronomical image credits remain in `README.md` and generated manifests.

## Seeed Indicator hardware initialization

Source: [Seeed-Solution/SenseCAP_Indicator_ESP32](https://github.com/Seeed-Solution/SenseCAP_Indicator_ESP32), commit `77edb8d2b9a92fc67965c1b2d4a838f0d09a1800`.

`firmware/esp32/main/st7701_init.inc` adapts the GX/ST7701 register sequence from `components/bsp/src/boards/lcd_panel_config.c`. `board.cpp` adapts Indicator wiring, I/O-expander register operations, FT touch-register reads, and panel timings from the board and I2C drivers. Changes replace the SDK board abstraction with direct ESP-IDF calls, preserve unrelated expander state, use a standalone 9-bit command transport, and add framebuffer ownership and PWM brightness control.

Upstream copyright notices include:

- Copyright 2015–2022 Espressif Systems (Shanghai) CO LTD.
- Copyright 2022 Seeed Technology Co. Ltd.

License: Apache-2.0; see [LICENSE-Seeed.txt](LICENSE-Seeed.txt).

## FatFs

Vendored `ff.c`, `ff.h`, and `diskio.h` from [zephyrproject-rtos/fatfs](https://github.com/zephyrproject-rtos/fatfs/tree/4f60a12c58c3de4246857ad87c40239ecd93349b), commit `4f60a12c58c3de4246857ad87c40239ecd93349b`. The files retain ChaN's copyright and redistribution notice. `ffconf.h` is adapted for read-only FAT32, ASCII long filenames, and no RTC. `ascii.c` is a restricted ASCII conversion implementation for this project's bounded dataset names. Zephyr's window-alignment macro is set to four bytes in the PlatformIO configuration. The application supplies its own read-only SPI block-device implementation.

## Toolchain dependencies

PlatformIO Espressif32 6.5.0 supplies ESP-IDF 5.1.2. The RP2040 build uses [maxgerhardt/platform-raspberrypi](https://github.com/maxgerhardt/platform-raspberrypi/tree/5d4561a05e3b212660ac6fdd3fbfb328d1988aa1) and its pinned Pico SDK fork. Their source and license files are downloaded by PlatformIO, rather than copied into this repository. See `platformio.ini` files and `docs/VALIDATION.md` for versions and limitations.
