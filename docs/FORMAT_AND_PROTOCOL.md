# D1 dataset and storage protocol, version 1

## Card datasets

Each dataset is `/galaxy/<name>/manifest.json` plus all files it names. Dataset names contain 1–31 ASCII letters, digits, underscores, or hyphens. Firmware enumerates up to 64 datasets. Manifests are at most 128KiB.

Required manifest fields:

```json
{
  "format": "indicator-galaxy",
  "version": 1,
  "source": "example",
  "title": "Example image",
  "credit": "Image attribution",
  "width": 480,
  "height": 480,
  "pixel_format": "RGB565_BE",
  "layout": "column-major",
  "source_width": 500,
  "source_height": 480,
  "gamma": 1.0,
  "total_bytes": 480000,
  "bands": [{
    "width": 500,
    "valid_height": 480,
    "chunks": [{
      "file": "b0000000.dat",
      "first_column": 0,
      "columns": 500,
      "bytes": 480000,
      "crc32": "12345678"
    }]
  }]
}
```

The example checksum is a placeholder. Real checksums are eight lowercase hexadecimal digits, CRC-32/ISO-HDLC of the complete chunk. Run `generator/validate_dataset.py` before and after copying to the card to verify file content. Firmware validates the header, band/chunk ranges, exact file sizes, and every UART packet's CRC; it does **not** scan each entire band to recalculate its file checksum during playback.

`width` at the top level is the viewport width. A band's `width` is its full mosaic width and must be at least 480. `valid_height` is the source content height; remaining rows are black padding. Chunk ranges must be contiguous and cover the full band. Chunk byte counts equal `columns × 960` and must fit in a FAT32 file (at most 2^32 − 1 bytes). Aggregate dataset sizes use 64-bit host arithmetic and can exceed 4GiB; device offsets are local to a chunk.

Chunk filenames are `bBBBBCCC.dat`, with a four-digit band and three-digit chunk number. The generator supports at most 9,999 bands and 1,000 chunks per band, additionally bounded by manifest size. Each pixel is an unsigned big-endian RGB565 word: red in bits 15–11, green in 10–5, blue in 4–0. Byte offset of pixel `(x,y)` within a chunk is `(x × 480 + y) × 2`. The ESP32 converts to native words and assembles row-major DMA frames.

Generation uses a staging directory. Only a complete dataset receives its published directory name; an existing version is kept as `<name>.previous`. Previews decode the actual bytes and are not needed by the device. Legacy PyPortal data lacks the version/header and is rejected.

## UART transport

ESP32 TX GPIO19 → RP2040 RX GPIO17; RP2040 TX GPIO16 → ESP32 RX GPIO20. Both use **1,000,000 baud, 8N1**, no hardware flow control. USB diagnostics are separate. Protocol implementation is `firmware/shared/protocol.h`.

Each frame is COBS-encoded, followed by a zero delimiter. Decoded layout:

| Offset | Size | Meaning |
| --- | ---: | --- |
| 0 | 1 | Protocol version, `1` |
| 1 | 1 | Operation |
| 2 | 2 | Request sequence number |
| 4 | 2 | Status (`0` for requests and successful replies) |
| 6 | 2 | Payload length, at most 7,680 bytes |
| 8 | N | Payload |
| 8 + N | 4 | CRC32 of header and payload |

Wire integers are little endian; pixel payloads preserve big-endian RGB565. CRC uses polynomial `0xedb88320`, initial/final XOR `0xffffffff`; `123456789` yields `0xcbf43926`. Replies echo operation and sequence. Frames with invalid COBS, lengths, version, or CRC are discarded. The ESP32 allows one outstanding request, waits at most 1.2 seconds per attempt, and retries at most three times. Absolute-offset READ and OPEN requests are idempotent.

| Operation | Request | Successful reply |
| --- | --- | --- |
| `1 HELLO` | Empty | Protocol uint16, baud uint32; mounts/remounts FAT32 if necessary |
| `2 LIST` | Empty | Newline-separated dataset directory names |
| `3 OPEN` | ASCII absolute path without NUL | uint32 file size |
| `4 READ` | uint32 offset, uint16 count | Exactly the requested bytes |
| `5 CLOSE` | Empty | Empty |

Status values: `0 OK`, `1 BAD_REQUEST`, `2 NO_CARD`, `3 IO_ERROR`, `4 NOT_FOUND`, `5 BAD_VERSION` (reserved), `6 TOO_LARGE`, `7 BAD_FILESYSTEM`. A failed HELLO distinguishes an uninitialized/missing card from a recognized disk without FAT32 or a disk read error. Older ESP32 images display status 7 as a generic storage error; use matching firmware images for the descriptive message. A version-incompatible frame is discarded and causes a timeout. Only `/galaxy/<valid-name>/manifest.json` and valid chunk filenames can be opened. Paths cannot contain extra separators or traversal components. Reads cannot extend past the opened file.

Each READ grants permission for exactly that many response bytes, up to eight columns. This single-request window provides bounded flow control without separate credit packets. A source change waits for any outstanding read before replacing its metadata; a generation counter discards stale band results. Read errors hold complete image data and trigger rediscovery/reload. No writes or card formatting operations are exposed.

The ESP32 storage reader is pinned to CPU1 and installs the UART driver from that core, so the UART ISR also runs on CPU1. Startup waits for driver initialization before sending HELLO or other metadata requests. Display initialization and RGB/GDMA interrupts run on the default main-task CPU0. This CPU assignment is an implementation detail and does not change wire framing, baud rate, or RP2040 compatibility.
