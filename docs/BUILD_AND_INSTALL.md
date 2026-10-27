# SenseCAP Indicator D1: PlatformIO build and installation

**Status: implemented and locally compiled on macOS.** Both firmware projects and the 480-pixel generator are included. Physical D1 installation and timing/performance checks remain outstanding; see [validation notes](VALIDATION.md).

## 1. Set up VS Code

Use the **PlatformIO IDE** extension already installed in VS Code. Its bundled PlatformIO Core provides the firmware CLI; a separate Core, Arduino IDE, or global ESP-IDF installation is unnecessary for this workflow. Open the PlatformIO terminal with **PlatformIO → Quick Access → Miscellaneous → PlatformIO Core CLI**. [PlatformIO VS Code documentation](https://docs.platformio.org/en/latest/integration/ide/vscode.html)

Check the tools from that terminal:

```bash
pio --version
git --version
uv --version
```

Install [uv](https://docs.astral.sh/uv/getting-started/installation/) if needed (`brew install uv` on macOS). Keep PlatformIO's Python/toolchain environment separate from the generator's `.venv`. Git and network access are needed for the initial platform and framework downloads.

Open `sensecap-galaxy.code-workspace` in VS Code. Alternatively, use **File → Add Folder to Workspace** to add `firmware/esp32` and `firmware/rp2040` alongside the repository root. PlatformIO can then show both projects in **Project Tasks**; select the intended project and environment before using Build or Upload.

## 2. Firmware project configuration

The implemented projects use this structure:

```text
firmware/
  esp32/
    platformio.ini
    CMakeLists.txt
    sdkconfig.defaults
    partitions.csv
    main/
      CMakeLists.txt
      ... display, touch, playback, UART, manifest parser, GX register sequence
  rp2040/
    platformio.ini
    src/
      ... Pico SDK storage and UART code
    lib/
      ... pinned FatFs and SPI storage integration
```

### ESP32-S3: PlatformIO with ESP-IDF

Configuration in `firmware/esp32/platformio.ini`:

```ini
[platformio]
src_dir = main
default_envs = indicator_esp32

[env:indicator_esp32]
platform = platformio/espressif32@6.5.0
board = esp32-s3-devkitc-1
framework = espidf
upload_protocol = esptool
upload_speed = 460800
monitor_speed = 115200
board_build.partitions = partitions.csv
board_build.flash_mode = qio
```

Platform version **6.5.0 supplies ESP-IDF 5.1.2**, within the vendor SDK's required 5.1.x family. This combination compiled locally; exact resolved package versions are recorded in [VALIDATION.md](VALIDATION.md). Each PlatformIO Espressif platform release selects its ESP-IDF version, so upgrading the platform requires revalidating the drivers. [Platform release](https://github.com/platformio/platform-espressif32/releases/tag/v6.5.0), [ESP-IDF integration](https://docs.platformio.org/en/latest/frameworks/espidf.html)

`esp32-s3-devkitc-1` supplies the chip/toolchain profile. It does **not** configure Indicator display or touch pins, and its default board description has no PSRAM. The committed configuration enables 8MB flash, octal PSRAM, 80MHz flash/PSRAM clocks, and the Indicator's RGB/touch/I/O-expander pins. The application checks PSRAM at boot. The default display is GX/ST7701; for a DX panel use `menuconfig → Galaxy hardware → DX display variant`. Touch swap/mirror options and RGB pixel clock are in the same menu. These settings still require confirmation on the physical unit. [PlatformIO board profile](https://docs.platformio.org/en/latest/boards/espressif32/esp32-s3-devkitc-1.html)

The tested GX D1 needs **Mirror touch X** and **Mirror touch Y** enabled, with X/Y swap disabled. Physical left taps control brightness, physical right taps control speed, and the source-picker header closes the menu. Fresh configurations enable both mirrors by default. If upgrading an existing generated `sdkconfig` with the old settings disabled, enable both mirror options under `menuconfig → Galaxy hardware` and rebuild; saved calibration settings take precedence over defaults. The connected development device's configuration was updated explicitly. Adjust these options only if calibration on your panel requires it. Control changes and header taps are logged with calibrated coordinates.

The RGB driver uses two 10-line internal-SRAM bounce buffers. The project's hidden `GALAXY_LCD_BOUNCE_BUFFER` option selects PSRAM instruction/rodata access, IRAM-safe LCD and UART interrupts, and restart at VSYNC. The display initializes on the default main-task CPU0; a reader pinned to CPU1 installs the storage UART there before the main task makes any storage requests. The UART retains its level-2 interrupt and 32-byte receive threshold. This separates storage interrupts from the display's RGB/GDMA interrupt work. This applies to existing generated `sdkconfig` files on rebuild as well as new installations; no manual configuration reset is needed. The boot log reports display initialization on CPU0 and UART initialization on CPU1, alongside geometry, bounce-buffer size, and UART configuration. Keep ESP-IDF's default main-task affinity on CPU0 and dual-core operation enabled.

### RP2040: PlatformIO with Pico SDK

Configuration in `firmware/rp2040/platformio.ini`:

```ini
[platformio]
default_envs = indicator_rp2040

[env:indicator_rp2040]
platform = https://github.com/maxgerhardt/platform-raspberrypi.git#5d4561a05e3b212660ac6fdd3fbfb328d1988aa1
board = seeed_indicator_rp2040
framework = picosdk
upload_protocol = picotool
monitor_speed = 115200
build_flags =
    -DPIO_STDIO_USB
    -I../shared
    -Ilib/fatfs
    -DPICO_BOOT_STAGE2_CHOOSE_W25Q080=1
    -DPICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    -DFS_FATFS_WINDOW_ALIGNMENT=4
    -Wall
    -Wextra
```

This uses a **community platform**, whose pinned revision includes the Indicator board definition, Pico SDK builder, and UF2 generation. The standard `platformio/raspberrypi` package documents an Arduino/Mbed integration instead. The selected community platform's manifest pins its Pico SDK fork to `4315984000afe20ac82c78c597a3c4a591f524b0`; read-only FatFs is vendored and pinned separately; resolved tool versions are recorded in the validation notes. [Standard platform](https://docs.platformio.org/en/latest/platforms/raspberrypi.html), [community manifest](https://github.com/maxgerhardt/platform-raspberrypi/blob/5d4561a05e3b212660ac6fdd3fbfb328d1988aa1/platform.json), [board definition](https://github.com/maxgerhardt/platform-raspberrypi/blob/5d4561a05e3b212660ac6fdd3fbfb328d1988aa1/boards/seeed_indicator_rp2040.json)

The Pico SDK builder falls back to a generic Pico SDK board header for this board ID. The storage firmware explicitly configures Indicator SPI1 on GPIO10/11/12/13, the interprocessor UART on GPIO16/17, and GPIO18 high for peripheral power. Debug output uses USB. FatFs is configured for read-only FAT32; received SD blocks are checked with CRC16. This builder uses PlatformIO/SCons; do not assume an unmodified standalone Pico SDK CMake project will build through it. [Pico SDK builder](https://github.com/maxgerhardt/platform-raspberrypi/blob/5d4561a05e3b212660ac6fdd3fbfb328d1988aa1/builder/frameworks/picosdk.py)

## 3. Build both images

In VS Code, open **PlatformIO → Project Tasks**, expand the correct project and environment, and choose **General → Build**. The first build downloads the pinned platform, SDK, and compiler packages. Build both images before installing either one.

Equivalent commands, run from the repository root in the PlatformIO terminal:

```bash
pio run -d firmware/esp32 -e indicator_esp32
pio run -d firmware/rp2040 -e indicator_rp2040
```

Build artifacts:

| Processor | Build output |
| --- | --- |
| ESP32-S3 | `firmware/esp32/.pio/build/indicator_esp32/firmware.bin`, bootloader, and partition-table images |
| RP2040 | `firmware/rp2040/.pio/build/indicator_rp2040/firmware.uf2` |

Use PlatformIO's Upload target for the ESP32 so it supplies the build's bootloader, partition table, and application addresses. A standalone application `.bin` is not a merged full-flash image.

For ESP-IDF configuration, use the PlatformIO target rather than a separate `idf.py` environment:

```bash
pio run -d firmware/esp32 -e indicator_esp32 -t menuconfig
```

Normal users should use the committed configuration. If changing SDK/platform versions, use **Clean** before rebuilding:

```bash
pio run -d firmware/esp32 -e indicator_esp32 -t clean
pio run -d firmware/rp2040 -e indicator_rp2040 -t clean
```

## 4. Identify the two USB devices

Connect the Indicator with a USB data cable and list devices:

```bash
pio device list
```

The device exposes separate connections for the processors. Seeed identifies the ESP32 connection as **USB-SERIAL CH340**, and the RP2040 connection as a USB serial device, typically `/dev/cu.usbmodem...` on macOS. Use the reported description and actual port; numbering can change after reconnecting or installing firmware. [Seeed port identification](https://wiki.seeedstudio.com/SenseCAP_Indicator_How_To_Flash_The_Default_Firmware/#faq)

The commands below use example port suffixes `XXXX` and `YYYY`. Replace the entire port string with the actual one. On Windows use `COMn`; on Linux use the port reported by `pio device list`. Close serial monitors before uploading.

## 5. Install the RP2040 storage firmware

Install the RP2040 image first, then the matching ESP32 image from the same firmware release. The screen may be blank or report a peer mismatch until both are installed.

For the first installation, use the ROM's UF2 mode:

1. Disconnect power/USB from the Indicator.
2. Locate the **internal RP2040 boot button** using the photo in [Seeed's UF2 instructions](https://wiki.seeedstudio.com/SenseCAP_Indicator_How_To_Flash_The_Default_Firmware/#flash-the-uf2-file).
3. Hold that button while reconnecting USB, then release it. Wait for the RP2040 boot drive, normally named `RPI-RP2`, to appear.
4. Copy `firmware/rp2040/.pio/build/indicator_rp2040/firmware.uf2` onto that drive. It disconnects and the RP2040 restarts after installation.

On macOS, after confirming the boot volume's name:

```bash
cp firmware/rp2040/.pio/build/indicator_rp2040/firmware.uf2 /Volumes/RPI-RP2/
```

Alternatively, with the RP2040 already in BOOTSEL mode, use **indicator_rp2040 → General → Upload**, or:

```bash
pio run -d firmware/rp2040 -e indicator_rp2040 -t upload
```

The Pico SDK USB reset interface is included. Automatic entry into BOOTSEL on later uploads also depends on the host uploader's reset handling. Use the physical boot-button procedure if it cannot enter upload mode. UF2 copying also provides a fallback when `picotool` cannot access the USB device.

## 6. Install the ESP32 display firmware

Select the ESP32 CH340 serial port and **indicator_esp32 → General → Upload**, or run:

```bash
pio run -d firmware/esp32 -e indicator_esp32 -t upload --upload-port /dev/cu.wchusbserialXXXX
```

The firmware restarts after upload. Open its USB serial log at 115200 baud:

```bash
pio device monitor --port /dev/cu.wchusbserialXXXX --baud 115200
```

Exit the monitor with **Ctrl+C**. To inspect the RP2040's separate USB diagnostics after it boots:

```bash
pio device monitor --port /dev/cu.usbmodemYYYY --baud 115200
```

The 115200 monitor setting is for diagnostics. The internal streaming UART uses a fixed 1,000,000 baud rate, checked during the protocol handshake.

## 7. Prepare the card and install imagery

Use the existing 32GB microSD card with a FAT32 partition. On macOS, Disk Utility's **View → Show All Devices** lets you select the card device; if it needs reformatting, choose **MS-DOS (FAT)** with **Master Boot Record**, and name it `GALAXY`. Formatting erases its contents, so copy off anything needed first. Check the resulting filesystem is FAT32 rather than exFAT. Select the physical card, not a small existing GALAXY volume: erasing only the volume leaves Linux or other partitions intact. Afterward verify the GALAXY volume uses approximately the full 32GB card. The firmware reports mount errors without formatting the card itself.

Before copying datasets, verify the actual filesystem in Terminal:

```bash
diskutil info /Volumes/GALAXY
```

The filesystem personality should be **MS-DOS FAT32**. Master Boot Record describes the partition scheme; it does not determine the filesystem inside a partition. A volume named GALAXY can still be formatted as Mac OS Extended/HFS+, APFS, or exFAT. Dataset verification checks image files and their manifests, and does not establish that the card uses FAT32.

Install the generator dependencies:

```bash
uv venv --python 3.12
uv pip install -r generator/requirements.txt
```

For VISTA or Rubin, install the optional pyvips binary package, which includes libvips on supported platforms:

```bash
uv pip install 'pyvips[binary]'
```

Alternatively, use a system libvips installation (`brew install vips` on macOS) followed by `uv pip install pyvips`.

Start with the small diagnostic dataset (no downloads), then add astronomical sources. Inspect each contact sheet and validate the encoded files:

```bash
uv run --no-project generator/generate_strip.py --demo
uv run --no-project generator/validate_dataset.py generator/output/demo
mkdir -p /Volumes/GALAXY/galaxy
cp -R generator/output/demo /Volumes/GALAXY/galaxy/
uv run --no-project generator/validate_dataset.py /Volumes/GALAXY/galaxy/demo
```

For astronomical imagery, run `uv run --no-project generator/generate_strip.py --source carina`, validate `generator/output/carina`, and copy that directory into the same card `galaxy/` folder. Inspect the generated previews before copying. Each source directory must contain its manifest and all band data files; keep source TIFFs in the computer's cache. When updating a source, replace its complete directory to avoid mixing datasets. Eject the card, power the Indicator off, insert the card, and power it back on.

On Windows or Linux use the card's actual mount path and copy the same directory structure:

```text
GALAXY/
  galaxy/
    carina/
      manifest.json
      b0000000.dat
      ...
    pandora/
      manifest.json
      b0000000.dat
      ...
```

## 8. Verify the installation

The firmware reports the firmware/protocol versions, initialized display and PSRAM, RP2040 handshake, mounted card, discovered sources, and selected dataset. Confirm correct colors, steady scrolling, fades between bands, and the existing left/right tap controls. Hold a touch for 0.7 seconds to open the source picker; tap a row to select, the header to close, or bottom left/right to page. Selecting the current source resumes its existing playback; selecting a different source loads and buffers it. Use the initial small dataset to exercise multiple transitions before loading Rubin.

If `pio` is unavailable, open PlatformIO's own terminal. If no projects appear in Project Tasks, check the workspace contains both project folders and their `platformio.ini` files. An unknown `picosdk` framework usually means the standard Raspberry Pi platform was selected rather than the pinned community integration. A dark/unstable screen needs investigation of PSRAM, display variant, driver initialization, and peer/card logs. If a port is busy, close its monitor; if upload fails, recheck the processor/port, USB cable, and boot mode.

**No FAT32 filesystem:** read the RP2040 USB log. FatFs error 13 means the card initialized but no supported filesystem was found. Failed mounts now print MBR partition types and boot-sector fields. An MBR partition type `af` denotes an Apple filesystem; the diagnostic also checks its HFS+/HFSX volume-header signature. Move the card to the computer, preserve any files that lack another copy, format it as MS-DOS (FAT) with MBR, verify `diskutil info`, and copy the existing generated directories back. Image regeneration is unnecessary.

**Image wraps at a vertical seam, including the source picker:** this indicates shifted display scanout rather than strip width. Rebuild and upload the ESP32 project with the bounce-buffer configuration above. The SD datasets and RP2040 image do not need updating for this display change. Test both playback and the menu, including after changing brightness, speed, or source (which saves NVS settings).

**Playback briefly starts, then returns to buffering:** inspect the ESP32 log for `RP2040 timeout` and `storage` UART warnings. This can be the app reconnecting after lost transport data rather than a processor reboot. Rebuild and upload the latest ESP32 firmware, which services the UART before its hardware FIFO fills and reports overflow/framing errors and failed RPC attempts. Keep the fixed internal baud rate the same on both processors. No dataset regeneration is needed for this transport change.

The buffering screen shows the selected source name (for example, `CARINA`) above the column progress, so you can identify which source is loading. When playback begins for a newly loaded source, its name also stays visible in a banner for five seconds while the image scrolls. The banner then disappears automatically; normal band transitions do not restart it.

To distinguish normal source changes from a fault, leave the screen untouched while recording the ESP32 log. `Source selected` marks a menu selection; `Storage recovery` and `Read failed` mark failed reads/reloads. A repeated ROM boot banner indicates a processor reset. During playback, a progress line reports the source, band, column, buffered columns, speed, and free heap every 30 seconds. Selecting a different source or beginning the next band requires a new initial buffer fill.

Connecting or disconnecting a serial monitor can itself toggle the ESP32's reset lines on the D1's USB serial bridge. Allow one startup/buffer fill after connecting and keep the same monitor open during a playback test. On macOS, hangup-on-close was enabled on the connected device during diagnosis; clearing the port's `HUPCL` flag while the monitor was open avoids that hangup when capture finishes. A reset coinciding with an upload or monitor operation does not establish a spontaneous playback crash.

To restore factory operation, reinstall **both** official processor images using [Seeed's firmware recovery guide](https://wiki.seeedstudio.com/SenseCAP_Indicator_How_To_Flash_The_Default_Firmware/). The galaxy application replaces their normal firmware. Use the vendor's merged-image instructions only with its merged image, not this project's standalone ESP32 application binary.
