"""Verify a D1 dataset, including byte counts and CRC32 of every data file."""
import argparse
import json
import re
import zlib
from pathlib import Path


def validate(directory: Path) -> dict:
    manifest_path = directory / "manifest.json"
    if manifest_path.stat().st_size > 128 * 1024:
        raise ValueError("manifest exceeds 128 KiB")
    m = json.loads(manifest_path.read_text())
    if not isinstance(m, dict):
        raise ValueError("manifest must be a JSON object")
    expected = {"format": "indicator-galaxy", "version": 1, "width": 480,
                "height": 480, "pixel_format": "RGB565_BE", "layout": "column-major"}
    if any(m.get(k) != v for k, v in expected.items()):
        raise ValueError("unsupported dataset format or display dimensions")
    if not isinstance(m.get("bands"), list) or not 1 <= len(m["bands"]) <= 9999:
        raise ValueError("invalid bands")
    total = 0
    for band in m["bands"]:
        width = band.get("width")
        valid_h = band.get("valid_height")
        if type(width) is not int or not 480 <= width <= 2**32 - 481:
            raise ValueError("invalid band width")
        if type(valid_h) is not int or not 1 <= valid_h <= 480:
            raise ValueError("invalid band height")
        chunks = band.get("chunks")
        if not isinstance(chunks, list) or not 1 <= len(chunks) <= 1000:
            raise ValueError("invalid chunks")
        first = 0
        for c in chunks:
            if not isinstance(c.get("file"), str) or not re.fullmatch(r"b[0-9]{7}\.dat", c["file"]):
                raise ValueError("invalid data filename")
            columns = c.get("columns")
            if type(columns) is not int or columns < 1 or c.get("first_column") != first:
                raise ValueError("invalid chunk column range")
            size = columns * 960
            if size > 2**32 - 1 or c.get("bytes") != size:
                raise ValueError("invalid chunk byte count")
            path = directory / c["file"]
            if path.stat().st_size != size:
                raise ValueError(f"file size mismatch: {path.name}")
            crc = 0
            with path.open("rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    crc = zlib.crc32(block, crc)
            if c.get("crc32") != f"{crc:08x}":
                raise ValueError(f"checksum mismatch: {path.name}")
            total += size
            first += columns
        if first != width:
            raise ValueError("incomplete band")
    if total != m.get("total_bytes"):
        raise ValueError("dataset total byte count mismatch")
    return m


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        manifest = validate(args.directory)
    except (OSError, ValueError, TypeError, KeyError) as exc:
        parser.exit(1, f"Invalid dataset: {exc}\n")
    print(f"Verified {len(manifest['bands'])} bands, {manifest['total_bytes']:,} bytes")
