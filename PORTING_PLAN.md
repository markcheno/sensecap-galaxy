# SenseCAP Indicator D1 port: implementation and validation

Status, October 26, 2026: **the generator, both PlatformIO firmware projects, shared protocol, and installation docs are implemented.** Local builds and host checks pass. Physical D1 bring-up and sustained performance validation remain outstanding; they have not been represented as tested.

## Display and strip dimensions

The implemented generator uses **480-pixel-tall bands** and a **480 × 480 viewport**. Each band retains the mosaic's full width for horizontal scrolling. Final partial bands are padded with black.

| Setting | D1 value |
| --- | ---: |
| `HEIGHT` | 480 pixels |
| `SCREEN_COLS` | 480 pixels |
| `COL_BYTES` | 960 bytes |
| RGB565 framebuffer | 460,800 bytes |
| Two RGB framebuffers | 921,600 bytes |
| 1,024-column read-ahead ring | 983,040 bytes |

The retained original sketch under `firmware/pyportal_galaxy/` uses the old 320 × 240 display. Its data files must be regenerated for the D1.

## Accepted requirements

- SenseCAP Indicator D1, built-in display and 32GB FAT32 microSD card.
- Complete mosaics, multiple installed sources, selectable on the touchscreen.
- PlatformIO in VS Code; ESP-IDF for the ESP32-S3.
- Preserve short left/right taps for brightness/speed, fades, and band looping.
- Initial default of 10 px/s; derive faster presets from actual transfer measurements.
- uv for host Python dependency management and commands.
- Attribute [rhammell's original pyportal-galaxy](https://github.com/rhammell/pyportal-galaxy) and retain astronomical image credits.

## Architecture implemented

```text
Python generator (uv)
          |
          v
FAT32 microSD -> RP2040 / read-only FatFs + SPI1
                                  |
                       1 Mbaud framed UART, CRC32
                                  |
                                  v
                   ESP32-S3 read-ahead ring
                                  |
                   RGB565 framebuffers in PSRAM
                                  |
                                  v
                       480 × 480 RGB display
```

The card is wired to the RP2040, rather than directly to the ESP32. SPI1 uses RP GPIO10/11/12/13; the interprocessor UART uses RP TX/RX GPIO16/17 and ESP RX/TX GPIO20/19. Both processor images are required.

- `firmware/rp2040/`: Pico SDK service, read-only FAT32, SPI SD-block CRC16 verification, bounded file-range requests, error recovery, USB diagnostics. It never formats the card.
- `firmware/esp32/`: ESP-IDF display/touch/PWM initialization, manifest validation, asynchronous storage reads, ring buffering, double-framebuffer presentation with VSYNC synchronization, fades, source picker, and NVS settings.
- `firmware/shared/protocol.h`: versioned COBS frames, CRC32, sequence IDs, bounded messages and path validation. One outstanding request acts as the flow-control window; reads use absolute offsets for safe retry.
- `generator/generate_strip.py`: 480-pixel full-coverage bands, RGB565 dithering/gamma, FAT32-safe chunks, manifests, real-byte previews, contact sheet, transactional publishing, and a diagnostic demo.
- `generator/validate_dataset.py`: schema, ranges, byte counts, and complete file CRC32 checks, usable on both host output and the copied card.

The ESP32 uses PlatformIO Espressif32 **6.5.0 / ESP-IDF 5.1.2**. The RP2040 uses the community `maxgerhardt/platform-raspberrypi` integration pinned to commit `5d4561a05e3b212660ac6fdd3fbfb328d1988aa1`, with its pinned Pico SDK. FatFs and Seeed-derived panel initialization are pinned and attributed in [third-party notices](third_party/NOTICES.md).

## Completed software work

- [x] Two compilable PlatformIO projects and a VS Code workspace.
- [x] Explicit 480 × 480 dimensions, octal PSRAM configuration, GX display defaults, DX/touch calibration options.
- [x] RP2040 card service and bounded UART protocol with retry and integrity checking.
- [x] Versioned per-source manifests and per-band/chunk files; no dataset-wide 4GiB cap.
- [x] Full-coverage generator, final-band padding, preview decoding, optional pyvips, and uv instructions.
- [x] Read-ahead playback, fades, saved settings, and long-press source selection.
- [x] Python conversion/integrity checks, native protocol tests, and tests of the actual firmware manifest parser.
- [x] Build/install, card preparation, recovery, format/protocol, and validation documentation.

See [validation results and commands](docs/VALIDATION.md). Host checks include a synthetic manifest larger than 4GiB in aggregate without requiring giant test files. All six native-resolution mosaics should occupy about 13.76GB of raw RGB565 data; actual source dimensions determine generated sizes. No claim is made that every large observatory download has been regenerated during this port.

## Hardware acceptance work remaining

- [x] Flash both matching images and verify RP2040 USB/BOOTSEL upload on this unit; diagnose readable card sectors and an HFS+ volume instead of FAT32.
- [ ] Confirm the panel variant, color order, orientation, touch coordinates, and brightness PWM using the generated demo.
- [ ] Mount the actual 32GB card and verify its full-volume FAT32 layout.
- [ ] Measure combined card/UART/display throughput, frame stability, retries, and slow-card pauses. Confirm the measured speed presets leave adequate margin.
- [ ] Check band fades, source changes, persisted settings, missing/corrupt files, card removal/reinsertion, and peer resets.
- [ ] Play multiple complete datasets, including an aggregate dataset larger than 4GiB, then run an overnight soak for memory growth and display corruption.

The default RGB clock is a conservative 12MHz. Two 10-line SRAM bounce buffers feed scanout, with PSRAM instruction/rodata access enabled for NVS writes and automatic recovery at VSYNC. Frame presentation waits two VSYNC boundaries before reusing a buffer, allowing for prefetch; the resulting frame rate and faster clock choices need physical validation. File checksums are verified by the host validator; playback checks metadata, file lengths, SD-block CRC16, and UART CRC32 without hashing entire bands before showing them.

Installation and commands: [docs/BUILD_AND_INSTALL.md](docs/BUILD_AND_INSTALL.md). Data layout and protocol: [docs/FORMAT_AND_PROTOCOL.md](docs/FORMAT_AND_PROTOCOL.md).
