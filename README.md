# SenseCAP Indicator D1 Galaxy

Scroll through astronomical mosaics on the Seeed Studio SenseCAP Indicator D1's **480 × 480** display, using a **32GB FAT32 microSD card**. This port is based on [pyportal-galaxy by rhammell](https://github.com/rhammell/pyportal-galaxy). The original [PyPortal Galaxy Viewer tutorial](https://www.hackster.io/rhammell/pyportal-galaxy-viewer-bf7b13) describes the project's origins.

The generator and both PlatformIO firmware projects are implemented. Both firmware targets compile on macOS; generator and protocol/parser checks pass. Both images have been flashed on a D1. The display and storage UART now run their interrupts on separate ESP32 cores. A five-minute hardware check exercised 10, 23, and 46 px/s on VISTA and Pandora without transport errors or spontaneous resets. The user confirmed display/source-menu alignment; the touch X calibration was corrected for the tested GX panel. **Complete-source playback and long-term stability still need hardware verification.** See the device investigations in [validation notes](docs/VALIDATION.md).

Use [Build and installation](docs/BUILD_AND_INSTALL.md) for the complete VS Code PlatformIO workflow. Open [sensecap-galaxy.code-workspace](sensecap-galaxy.code-workspace) to work with both firmware projects. [PORTING_PLAN.md](PORTING_PLAN.md) records the architecture and remaining hardware checks; [data format and protocol](docs/FORMAT_AND_PROTOCOL.md) documents the generator/firmware interface.

## Quick start

Run Python commands from the repository root. Install [uv](https://docs.astral.sh/uv/getting-started/installation/) first (`brew install uv` on macOS):

```bash
uv venv --python 3.12
uv pip install -r generator/requirements.txt
uv run --no-project generator/generate_strip.py --demo
uv run --no-project generator/validate_dataset.py generator/output/demo
```

The demo creates two 480-pixel-tall bands with color bars and a square grid, requiring about 2.8MB of raw data and no image download. Inspect `generator/output/demo/contact.png` and the individual decoded previews.

Copy the entire dataset directory to the card, after confirming its mount path:

```bash
mkdir -p /Volumes/GALAXY/galaxy
cp -R generator/output/demo /Volumes/GALAXY/galaxy/
```

Eject the card, insert it with the Indicator powered off, and install **both** firmware images using [the installation directions](docs/BUILD_AND_INSTALL.md). Build commands in the PlatformIO terminal:

```bash
pio run -d firmware/rp2040 -e indicator_rp2040
pio run -d firmware/esp32 -e indicator_esp32
```

## Generate astronomical imagery

```bash
uv run --no-project generator/generate_strip.py --list
uv run --no-project generator/generate_strip.py --source carina
uv run --no-project generator/validate_dataset.py generator/output/carina
```

Downloads are resumable and cached in `generator/cache/`. Each source becomes a directory in `generator/output/`, with `manifest.json`, band data, decoded previews, and `contact.png`. Copy complete source directories into `/galaxy/` on the card. Multiple sources can coexist.

Large mosaics benefit from pyvips, which processes image regions without decoding the entire mosaic into Python RAM. The binary extra includes libvips on supported platforms:

```bash
uv pip install 'pyvips[binary]'
```

Alternatively, use a system libvips installation (`brew install vips` on macOS), then `uv pip install pyvips`. [pyvips installation](https://libvips.github.io/pyvips/README.html#binary-installation)

| Option | Purpose |
| --- | --- |
| `--source carina` | Choose a built-in mosaic; default is `pandora` |
| `--input mosaic.tif` | Use a local image at least 480 pixels wide |
| `--demo` | Generate color bars and a square grid without downloading |
| `--name custom` | Dataset directory name; default is source name, `local`, or `demo` |
| `--gamma 1.2` | Lift faint midtones before RGB565 quantization |
| `--output path` | Choose the parent output directory |

Band height is **480 pixels**; width remains the source mosaic's width. The final partial band is padded with black, preserving proportions. Column-major big-endian RGB565 uses **960 bytes per column**. Data files split on column boundaries below FAT32's per-file limit; complete mosaics are retained.

Generation stages output before publication. Replacing a dataset keeps the previous version as `<name>.previous`; move or remove that backup before replacing the same dataset again. On the SD card, replace a complete source directory while the device is powered off rather than merging files from separate generations.

## Card format

Use one full-card **FAT32** partition with **Master Boot Record (MBR)**. In macOS Disk Utility select **View → Show All Devices**, select the physical card, and erase with **MS-DOS (FAT)** / **Master Boot Record**, named `GALAXY`. This erases all existing partitions and data. Confirm the GALAXY volume uses approximately the full card capacity; formatting only an existing small volume leaves other partitions in place.

The firmware mounts the card read-only and never formats it. Its contents should look like:

```text
GALAXY/
  galaxy/
    demo/
      manifest.json
      b0000000.dat
      b0001000.dat
      ... previews ...
    carina/
      manifest.json
      ... band files ...
```

The original 240-pixel PyPortal `.dat`/`.idx` files must be regenerated. They are incompatible with the D1 format.

## Controls and playback

- **Short tap on the left:** cycle brightness through 10%, 20%, 75%, and 100%.
- **Short tap on the right:** cycle speed. Playback starts at 10 px/s on first use. Faster presets are derived from the first band's measured transfer throughput, with a 60% budget and a 120 px/s ceiling; actual performance needs hardware testing.
- **Hold for 0.7 seconds:** open or close the source picker. Tap a source to select it, the header to close, or the bottom left/right to change pages. Playback pauses while the picker is open.

Source, brightness, and speed selection are saved in NVS. Each band fades in, pans horizontally, and fades out; playback loops through all bands. The ESP32 uses a 1,024-column read-ahead ring and two RGB framebuffers in PSRAM. Two 10-line SRAM bounce buffers feed the display, with VSYNC recovery and PSRAM instruction/rodata access enabled so settings saves do not leave scanout permanently shifted. An I/O stall holds complete image data, and storage failures trigger reconnect/reload attempts. USB serial diagnostics run at 115200 baud; the separate internal data UART runs at 1Mbaud.

## Source sizes

Nominal native-resolution output sizes, including a padded final band; previews and filesystem overhead are additional. Time is horizontal travel at 10 px/s, excluding fades and pauses.

| Source | Telescope / object | Bands | Raw data | Travel time |
| --- | --- | ---: | ---: | ---: |
| `carina` | JWST / Cosmic Cliffs | 18 | 0.252GB | 7.05hr |
| `tarantula` | JWST / 30 Doradus | 18 | 0.252GB | 7.04hr |
| `pandora` | JWST / Abell 2744 | 28 | 0.474GB | 13.35hr |
| `jades` | JWST / GOODS-South | 18 | 0.209GB | 5.81hr |
| `vista` | VISTA / Milky Way centre | 63 | 2.419GB | 69.16hr |
| `rubin` | Rubin / Virgo Cluster | 108 | 10.155GB | 292.39hr |

All six datasets total approximately **13.76GB** of raw data and fit on a 32GB card with room for previews. Downloaded source TIFFs stay on the computer. VISTA and Rubin need pyvips for practical host memory use; even with pyvips, one decoded band plus conversion buffers needs RAM.

## Repository layout

```text
firmware/esp32/             ESP-IDF display/playback PlatformIO project
firmware/rp2040/            Pico SDK read-only SD service PlatformIO project
firmware/shared/            Versioned UART framing, CRC, and path validation
firmware/pyportal_galaxy/   Original reference firmware (not for the D1)
generator/                 Image generation and dataset verification
  cache/                   Downloaded sources (ignored)
  output/                  Source datasets and previews (ignored)
tests/                     Generator and native protocol/parser checks
docs/                      Build/install, data format, and validation notes
third_party/               Dependency provenance and license notices
```

## Verification

```bash
uv run --no-project python -m unittest discover -s tests -v
```

See [validation notes](docs/VALIDATION.md) for the native C++ checks, exact dependency versions, build results, and physical tests still outstanding. Code adapted from Seeed and vendored FatFs retain their notices in [third_party/NOTICES.md](third_party/NOTICES.md).

## Image Credits

- **Carina / Tarantula** — NASA, ESA, CSA, STScI (ESA/Webb for Tarantula)
- **Pandora's Cluster** — NASA, ESA, CSA, Ivo Labbe (Swinburne), Rachel Bezanson (University of Pittsburgh); Image Processing: Alyssa Pagan (STScI)
- **JADES** — NASA, ESA, CSA, Brant Robertson (UC Santa Cruz), Ben Johnson (CfA), Sandro Tacchella (Cambridge), Marcia Rieke (University of Arizona), Daniel Eisenstein (CfA); Image Processing: Alyssa Pagan (STScI)
- **Milky Way centre** — ESO/VVV Survey/D. Minniti. Acknowledgement: Ignacio Toledo, Martin Kornmesser
- **Virgo Cluster** — RubinObs/NOIRLab/SLAC/DOE/NSF/AURA
