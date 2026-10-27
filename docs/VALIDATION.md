# Port validation

Initial local verification was performed on October 26, 2026, on macOS with an Apple Silicon host. Both PlatformIO targets compile. That initial pass did not flash firmware or make physical streaming measurements. The subsequent device investigation below records the first hardware checks. The agent has not erased or written the SD card.

## Reproduce the checks

From the repository root, in PlatformIO's terminal:

```bash
pio run -d firmware/esp32 -e indicator_esp32
pio run -d firmware/rp2040 -e indicator_rp2040
```

For Python checks only:

```bash
uv venv --python 3.12
uv pip install -r generator/requirements.txt
uv run --no-project python -m unittest discover -s tests -v
```

Install optional `uv pip install 'pyvips[binary]'` to exercise both image backends. Tests also work without it, skipping the backend comparison. This port was checked before and after installing pyvips.

After the ESP32 build has installed ESP-IDF, run all host checks:

```bash
uv run --no-project tests/check_host.py
```

This requires a native C/C++ compiler, such as Xcode Command Line Tools on macOS. It compiles the shared protocol and the actual ESP32 manifest parser against ESP-IDF's cJSON, with AddressSanitizer and UndefinedBehaviorSanitizer. Use `--no-sanitizers` if the host compiler does not support them. Temporary executables and fixture data are cleaned up automatically. If using a custom PlatformIO installation, set `PLATFORMIO_CORE_DIR` to its core directory so the script can find ESP-IDF's cJSON sources.

The agent's verification used an isolated core at `/tmp/sensecap-pio` and the installed PlatformIO executable at `/Users/mark/.platformio/penv/bin/pio`; these paths are not required for normal use. Use the same spelling of a custom core path consistently on macOS: mixing `/tmp` and `/private/tmp` can confuse older PlatformIO ESP-IDF object-path handling. Standard PlatformIO Home-directory installations do not need this workaround.

## Checks performed

| Check | Result / coverage |
| --- | --- |
| ESP32 PlatformIO target | Builds application, bootloader, partition table, and firmware image |
| RP2040 PlatformIO target | Builds and links Pico SDK/FatFs service and generates `firmware.uf2` |
| Pixel encoding | Checks known red/green/blue words, column order, 960-byte columns, and decoding back to PNG |
| Whole-mosaic coverage | Checks multiple bands, final-band black padding, and no vertical stretching |
| Chunk boundaries | Forces small files at non-Bayer-aligned boundaries and compares concatenated data with unsplit encoding |
| Data integrity | Validates generated file lengths and CRC32, and rejects modified content |
| Publication | Preserves the previous dataset, refuses to overwrite its backup, and does not publish failed generation |
| Image backends | Pillow and pyvips crops agree, including final partial-band padding |
| Protocol | Round-trips every payload size from 0 to 7,680 bytes; checks CRC32/CRC16 reference values, corruption rejection, malformed COBS and path validation |
| Firmware manifest parser | Accepts actual generated manifests; rejects incompatible dimensions, version, byte order, short widths, missing coverage, bad sizes, traversal, deep nesting, trailing junk and numeric overflow |
| Aggregate sizes | Accepts a synthetic 5.76GB dataset manifest whose individual files remain below FAT32's limit; no giant fixture files are required |
| Diagnostic dataset | Generates two 1,440 × 480 bands, verifies 2,764,800 raw bytes, parses its manifest in the firmware host test, and visually checks decoded color-bar/grid contact sheet |

The larger observatory mosaics have not all been downloaded and regenerated during this port. Their size estimates use the catalog's nominal dimensions. CRC16 checks on the SPI driver and real media behavior still need physical verification even though their checksum primitive is covered by host tests.

## Dependency versions used

| Dependency | Version / revision |
| --- | --- |
| PlatformIO Core | 6.2.0 |
| Espressif32 platform | 6.5.0 |
| ESP-IDF | 5.1.2, package `3.50102.240122` |
| Xtensa compiler | 12.2.0+20230208 |
| Community RP2040 platform | `5d4561a05e3b212660ac6fdd3fbfb328d1988aa1` |
| Pico SDK fork | `4315984000afe20ac82c78c597a3c4a591f524b0` |
| Arm compiler | 9.3.1, package `1.90301.200702` |
| Picotool package | `5.160100.260719` |
| FatFs source | `4f60a12c58c3de4246857ad87c40239ecd93349b` |
| Python | 3.12.13 |
| Pillow / requests | 13.0.0 / 2.34.2 |
| pyvips / binary libvips | 3.2.0 / 8.18.7 |

The RP2040 build emits warnings from the pinned Pico SDK and FatFs sources: an unused DMA argument, an unsigned comparison, and an unused read-only FatFs variable. The application sources build without those warnings. See [third-party provenance](../third_party/NOTICES.md).

## First device mount investigation

After installation, the D1 displayed `Waiting for card / No FAT32 card`. Read-only USB diagnostics identified `FR_NO_FILESYSTEM` (13), rather than an SD initialization failure. The card successfully initialized with OCR `c0 ff 80 00` and block addressing; its raw sectors were readable with CRC16 checks.

The MBR contained partition type `0xaf`, beginning at LBA 2048 with 61,949,952 sectors. Its boot sector had no FAT BPB. The volume header at partition offset 1024 bytes contained signature `0x482b`, version 4: **HFS+**, consistent with [Apple's HFS Plus format specification](https://developer.apple.com/library/archive/technotes/tn/tn1150.html). Thus MBR was correct, but the partition contained a Mac filesystem rather than FAT32. Image verification on the computer can pass on HFS+ because it checks file contents, not the volume format.

The RP2040 was updated over USB BOOTSEL with a read-only layout diagnostic. Mount failures now distinguish `BAD_FILESYSTEM` from `NO_CARD` and `IO_ERROR`; the ESP32 displays `No FAT32 filesystem` for the new status. Native tests cover those distinctions, and both firmware builds and the host checks pass. No image regeneration or SD writes were performed during diagnosis. The user subsequently reformatted/copied the card and reported working playback; later ESP32 logs confirm the Pandora dataset loads successfully.

## Horizontal wrap investigation

The user subsequently reported working playback but a vertical wrap about one-fifth of the screen from the left; the source-picker menu had the same wrap. This localizes the symptom to display output rather than the image dataset. The original renderer writes all 480 framebuffer columns and rows.

The original RGB configuration streamed directly from PSRAM, with automatic restart disabled, while brightness/source/speed saves wrote NVS flash during active display output. This is consistent with the permanent scanout shift described by [Espressif's RGB LCD documentation](https://docs.espressif.com/projects/esp-idf/en/v5.1.2/esp32s3/api-reference/peripherals/lcd.html#bounce-buffer-with-single-psram-frame-buffer). It is a diagnosis from the symptom and driver configuration, not a measured bus trace.

The display update adds two 4,800-pixel (10-line) internal-SRAM bounce buffers, consuming 19,200 bytes total, while retaining both full PSRAM framebuffers. It enables PSRAM instruction/rodata access, an IRAM-safe RGB ISR, and restart at VSYNC. The existing two-boundary presentation wait accounts for bounce-buffer prefetch. Width, height, source data, and the 12MHz pixel clock are unchanged.

The ESP32 target compiles successfully. Inspection of the generated configuration confirms all five original display flags are enabled, including migration from the previous `sdkconfig` with those flags disabled. The update was flashed and verified on the connected D1. Its boot log confirms 8MB PSRAM, code and rodata mapped to PSRAM, two 4,800-pixel SRAM bounce buffers, and loading the 28-band Pandora dataset. Following the transport adjustment below, the user confirmed continued scrolling and correct image/source-menu alignment. Longer playback and repeated settings-change checks remain necessary.

## Repeated buffering investigation

After the display update, the user reported that playback scrolls briefly and then returns to buffering. A 50-second ESP32 log showed one processor boot, followed by `RP2040 timeout` at 14.8, 30.1, and 46.6 seconds. Each timeout triggered a source reload and a new buffer fill. Thus the observed cycle was reconnect/reload rather than an ESP32 reboot. Initial reads measured about 65–78 columns/s.

The previous ESP-IDF UART settings used a level-1 interrupt and the default 120-byte receive threshold in a 128-byte hardware FIFO. At 1Mbaud, only about 80 microseconds remain to service that interrupt before overflow. Display bounce-buffer refills also run from interrupts. Interrupt contention is a working explanation for the lost responses; the original build did not collect UART error events, so it did not identify the exact loss mechanism. See [Espressif's UART interrupt/event documentation](https://docs.espressif.com/projects/esp-idf/en/v5.1.2/esp32s3/api-reference/peripherals/uart.html#use-interrupts); the FIFO threshold default is also verified against the installed ESP-IDF 5.1.2 `uart.c` source.

The ESP32 transport update uses a level-2 IRAM UART interrupt and a 32-byte receive threshold, providing about 960 microseconds of FIFO headroom and allowing it to preempt the default level-1 display interrupts. `UART_ISR_IN_IRAM` is selected automatically for existing configurations. It also logs UART receive errors and per-attempt RPC timeout diagnostics. Protocol, baud rate, datasets, and RP2040 firmware remain compatible.

The updated ESP32 target built successfully and was flashed with hash verification. A 95-second serial observation of firmware ELF SHA prefix `52e587e63919a856` recorded one initial boot, no UART error events, no RPC timeouts, no panic/watchdog messages, and no repeated recovery cycle. Pandora initially measured 77.6 columns/s; the subsequent VISTA source loaded at 68.3 seconds and measured 77.8 columns/s, each selecting a 46 px/s maximum preset. The user confirmed that scrolling continues and the image/source menu are correctly aligned. This supports the transport adjustment over the observed interval; it is not an overnight stability measurement. The serial monitor was closed after the check.

The user later reported recurring buffering despite correct menu alignment. A further 125-second log of the same firmware showed no crash or storage error, but did show the VISTA source reloading at 36.3 seconds. The user confirmed menu use during this observation. The original picker unconditionally reloaded even the currently selected source, which caused an unnecessary initial buffer fill. Selecting the current source now closes the menu and resumes its existing playback, while allowing an explicit reload if the source is in a failed state. Different-source selection still loads and buffers that source normally. Logs now distinguish menu actions, storage recovery, playback starts, and 30-second progress reports; read failures include the source/band/column. The project also sets ESP-IDF's `PROJECT_VER` explicitly so a newly initialized Git repository without commits does not break CMake's version lookup.

This update built successfully and was flashed with hash verification (ELF SHA prefix `fd0d2d8c34e287c0`). A subsequent 310-second hands-free observation recorded one initial boot, one VISTA playback start, ten progress reports, and zero read failures, storage recoveries, RPC timeouts, UART receive errors, panic/watchdog messages, or menu events. Playback advanced from column 688 at 39.4 seconds to column 6,916 at 310.2 seconds at the selected 23 px/s preset; the read-ahead buffer stayed at 1,019–1,020 columns and reported free heap remained 6,358,219 bytes. The user independently confirmed continuous scrolling during the observation. No spontaneous crash/rebuffer cycle was reproduced in this interval; the fastest preset and long-term stability remain unproven by this test.

One buffering report during diagnosis coincided with installing/restarting the updated firmware. The macOS serial port also had `HUPCL` enabled; that flag was cleared while the monitor was open to disable its hangup-on-close action. Firmware uploads and serial-monitor resets must be distinguished from spontaneous playback faults. The monitor was closed after the 310-second capture, and the user confirmed playback continued scrolling without another buffer fill.

The user clarified that repeated buffering first appeared with the horizontal-split display fix. This makes it a regression associated with adding bounce-buffer interrupt work; the menu adjustment does not explain the original reported fault. The initial post-display-fix log did capture repeated RP2040 timeouts, even though later captures following the UART-priority adjustment did not reproduce them. The short successful captures do not establish long-term stability.

The follow-up update installs the storage UART from the reader task pinned to CPU1 and waits on an initialization semaphore before any metadata requests. ESP-IDF attaches the UART ISR to the core that calls `uart_driver_install()`. The default CPU0 main task still initializes the display and its RGB/GDMA interrupts. This separates the interrupt workloads while retaining the alignment fix and the existing UART receive headroom/IRAM settings. Boot messages report each core explicitly. The ESP32 build and verified flash upload passed; the resulting ELF SHA prefix is `417b721126454595`. This addresses shared-core contention as a suspected regression mechanism without claiming that a bus trace has proven the exact byte-loss cause.

A 345-second capture of the CPU-isolated build confirmed display initialization on CPU0 and storage UART initialization on CPU1. It exercised VISTA at 23 and 46 px/s, a menu-requested change to Pandora, and Pandora at 46 and 10 px/s. The log contained one initial boot and zero read failures, storage recoveries, RPC timeouts, UART errors, panic messages, or watchdog messages. The source change was explicitly logged as a menu action and required the expected initial buffer fill. The test covers these observed intervals, not complete-source or overnight playback.

During that test the user reported physical left taps changing speed and right taps changing brightness. The GX touch X axis was reversed relative to the image. `GALAXY_TOUCH_MIRROR_X` now defaults to enabled, and the existing local generated configuration was explicitly changed to match. X/Y swap and Y mirroring remain disabled. The firmware logs brightness/speed actions with their calibrated tap X coordinate. This calibration update compiled and flashed with hash verification (ELF SHA prefix `deb82e99a78cc838`), retaining CPU isolation and the display alignment fix. Existing generated configurations keep their saved calibration choices; see the upgrade instruction in the build guide.

The calibration build's next capture confirmed CPU0 display/CPU1 UART setup, successful Pandora loading, and continued progress through 129 seconds at the saved 10 px/s preset with 1,022–1,023 buffered columns and stable reported free heap. No tap actions or transport errors were recorded before the host serial read failed with `Device not configured`. The ESP32 USB serial device briefly reappeared; a subsequent raw capture without DTR/RTS changes failed the same way. A port enumeration then showed the RP2040 still present but the ESP32 USB serial port missing. The user reported opening the source menu, rather than a deliberate reconnection, and confirmed that physical left/right controls were now correct and scrolling continued. The port subsequently returned; a raw capture showed application uptime exceeding 600 seconds, menu-requested source changes, and continued playback. This host USB interruption is not evidence of a logged ESP32 firmware panic, and its cause remains unresolved.

The user then found that tapping the menu header did nothing, while tapping the bottom closed it. This confirmed that the touch Y axis was reversed as well. Both touch mirrors now default to enabled, the local generated configuration enables both explicitly, and X/Y swap remains disabled. Header-close actions log the calibrated X/Y coordinates. Existing installations should enable both mirror options as described in the build guide. This calibration is based on physical tests of the connected GX D1, rather than assumed from the rendered menu's appearance.

The final X/Y calibration build passed and was flashed with hash verification (ELF SHA prefix `032c3aa69e099740`). A raw serial capture without modem-control toggles recorded `Source menu closed via header (tap x=345 y=46)` at application uptime 103 seconds. It did not reload JADES on that header close. A subsequent separately logged menu selection changed JADES to Tarantula. The user confirmed that the header now closes the menu and scrolling continues without unwanted buffering. Physical left/right controls had already been confirmed after X calibration. These checks distinguish menu calibration faults and deliberate source loads from the original post-display-update transport timeouts.

The 150-second final capture also recorded physical-right speed taps at calibrated X=408 selecting 23 and then 46 px/s. Tarantula continued at 46 px/s, advancing from column 1,265 at 147 seconds to column 2,646 at 177 seconds, while buffered columns increased from 903 to 1,015. It recorded zero read failures, storage recoveries, RPC timeouts, UART receive errors, panic messages, or watchdog messages, and the raw capture completed normally. The serial connection was then closed without changing the modem-control lines. Full-source and overnight stability remain outside these checks.

## Physical acceptance still required

1. Install both images using [BUILD_AND_INSTALL.md](BUILD_AND_INSTALL.md), starting with the diagnostic dataset.
2. Confirm PSRAM initialization, correct panel variant, RGB colors, orientation, square geometry, touch coordinates, and all brightness levels.
3. Confirm the actual 32GB card mounts as FAT32 and that the GALAXY volume occupies the full card. Verify the copied dataset with the host validator.
4. Observe buffering, fades, default speed, measured faster presets, source selection, and saved settings after a power cycle.
5. Exercise card removal/reinsertion, peer reset, missing/truncated files, and source changes during reads. Verify reconnect behavior and that no partial columns appear.
6. Play multiple complete sources, including a dataset larger than 4GiB in aggregate; run an overnight soak while watching for memory growth, retries, and display corruption.

The default panel clock is 12MHz, and presentation waits two VSYNC boundaries before reusing a framebuffer. Actual frame rate and any faster clock need on-device measurement. Transfer-derived speed presets are estimates with a 60% budget and 120 px/s ceiling, not hardware-certified speeds. Playback does not hash whole data files; use `validate_dataset.py` before and after copying, alongside the live SD-block and UART checks.
