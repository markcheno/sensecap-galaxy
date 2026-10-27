"""Generate SenseCAP Indicator D1 datasets from astronomical mosaics.

480-pixel bands retain the source width and are stored as column-major,
big-endian RGB565 (960 bytes per column). Each output/<name>/ directory
contains manifest.json, FAT32-safe band chunks, previews, and a contact sheet.
Copy the complete directory into /galaxy/ on the card.

    uv run --no-project generator/generate_strip.py --source carina
    uv run --no-project generator/generate_strip.py --input mosaic.png --name local
    uv run --no-project generator/generate_strip.py --list
"""

import argparse
import array
import math
import json
import re
import shutil
import tempfile
import zlib
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

import requests
from PIL import Image, ImageChops, ImageDraw

# Astronomical mosaics are far larger than Pillow's decompression-bomb
# guard (~89.5 megapixels), which exists to stop malicious files rather
# than legitimate ones. Every source here is from a named observatory.
Image.MAX_IMAGE_PIXELS = None

# SenseCAP Indicator D1 display profile.
HEIGHT = 480
COL_BYTES = HEIGHT * 2

# Pan speeds the firmware cycles through on touch, used for the estimates.
SPEED_LEVELS = (10, 30, 60)

# Widest preview to write. Longer strips are downscaled by an integer
# factor rather than truncated: seeing the whole strip matters more than
# seeing it in full detail.
PREVIEW_MAX_COLS = 20000

# Columns converted per block. Keeps memory flat and gives progress.
CHUNK_COLS = 4096

# Viewport width. A source must provide at least one complete screenful.
SCREEN_COLS = 480

# FAT32 maximum file size. The firmware reads the card as FAT32, so the
# .dat must stay under this limit.
FAT32_MAX_BYTES = 2**32 - 1

HERE = Path(__file__).parent
CACHE_DIR = HERE / "cache"
OUTPUT_DIR = HERE / "output"


@dataclass(frozen=True)
class Source:
    """One downloadable mosaic. Dimensions are nominal, for planning; the
    real ones are read from the file itself at generation time."""

    title: str
    telescope: str
    width: int
    height: int
    url: str
    filename: str
    credit: str
    note: str = ""


# Every URL here was confirmed to return the stated file. Sizes in the
# comments are the download, not the decoded size in memory.
SOURCES: dict[str, Source] = {
    "carina": Source(
        title="Cosmic Cliffs, NGC 3324",
        telescope="JWST NIRCam",
        width=14575,
        height=8441,
        url=(
            "https://mast.stsci.edu/api/latest/Download/file?uri=mast:OPO/"
            "product/STSCI_PR_2022-031/STSCI-J-p22031a-f-14575x8441.tif"
        ),
        filename="carina_cosmic_cliffs_14575x8441.tif",
        credit="NASA, ESA, CSA, STScI",
        note="137 MB. Small and quick -- a good end-to-end test of the pipeline.",
    ),
    "tarantula": Source(
        title="Tarantula Nebula (30 Doradus)",
        telescope="JWST NIRCam",
        width=14557,
        height=8418,
        url="https://esawebb.org/media/archives/images/original/weic2212a.tif",
        filename="tarantula_14557x8418.tif",
        credit="ESA/Webb, NASA, CSA, STScI",
        note="125 MB.",
    ),
    "pandora": Source(
        title="Pandora's Cluster, Abell 2744",
        telescope="JWST NIRCam",
        width=17644,
        height=13422,
        url=(
            "https://assets.science.nasa.gov/content/dam/science/missions/webb/"
            "science/2023/02/STScI-01GQQF9WVPFVMCVHRZY54N2TAR.png/jcr:content/"
            "renditions/Full%20Res%20(For%20Print).tif"
        ),
        filename="pandora_abell2744_17644x13422.tif",
        credit="NASA, ESA, CSA, Ivo Labbe (Swinburne), Rachel Bezanson "
               "(University of Pittsburgh); Image Processing: Alyssa Pagan (STScI)",
        note="178 MB. Deep field behind a lensing galaxy cluster.",
    ),
    "jades": Source(
        title="JADES, GOODS-South",
        telescope="JWST NIRCam",
        width=12097,
        height=8482,
        url=(
            "https://assets.science.nasa.gov/content/dam/science/missions/webb/"
            "science/2023/06/STScI-01H1Q2DKR7FRQAMDGBGCDAMSPX.png/jcr:content/"
            "renditions/Full%20Res%20(For%20Print).tif"
        ),
        filename="jades_goods_south_12097x8482.tif",
        credit="NASA, ESA, CSA, Brant Robertson (UC Santa Cruz), Ben Johnson (CfA), "
               "Sandro Tacchella (Cambridge), Marcia Rieke (University of Arizona), "
               "Daniel Eisenstein (CfA); Image Processing: Alyssa Pagan (STScI)",
        note="127 MB. Deep field of more than 45,000 galaxies.",
    ),
    "vista": Source(
        title="Milky Way Centre, 40K reduction",
        telescope="ESO VISTA",
        width=40000,
        height=30132,
        url="https://cdn2.eso.org/images/publicationtiff40k/eso1242a.tif",
        filename="vista_milky_way_centre_40k.tif",
        credit="ESO/VVV Survey/D. Minniti. Acknowledgement: Ignacio Toledo, Martin Kornmesser",
        note="4.0 GB. Needs pyvips. The 8.8-gigapixel original is a .psb no library can open.",
    ),
    "rubin": Source(
        title="Cosmic Treasure Chest (Virgo Cluster)",
        telescope="Rubin LSSTCam",
        width=97943,
        height=51536,
        url="https://storage.noirlab.edu/media/archives/images/original/noirlab2521a.tif",
        filename="rubin_cosmic_treasure_chest.tif",
        credit="RubinObs/NOIRLab/SLAC/DOE/NSF/AURA",
        note="14.1 GB. Needs pyvips, and a lot of patience on the download.",
    ),
}

DEFAULT_SOURCE = "pandora"


# --------------------------------------------------------------- download


def remote_size(url: str) -> int:
    """Size of the remote file in bytes, or 0 if the server won't say.

    Some servers (science.nasa.gov assets) answer HEAD with 404 while
    serving GET normally, so fall back to a one-byte ranged GET."""
    head = requests.head(url, allow_redirects=True, timeout=60)
    if head.ok:
        return int(head.headers.get("Content-Length", 0))
    resp = requests.get(url, headers={"Range": "bytes=0-0"}, stream=True, timeout=60)
    resp.raise_for_status()
    resp.close()
    content_range = resp.headers.get("Content-Range", "")
    if "/" in content_range:
        return int(content_range.rsplit("/", 1)[1])
    return int(resp.headers.get("Content-Length", 0))


def ensure_source(src: Source) -> Path:
    """Download the mosaic if it is not already cached, resuming a partial
    file rather than starting over. These run from 125 MB to 14 GB, so a
    dropped connection should not cost the whole transfer.

    Downloads land in a .part file that is renamed only once complete, so
    a file under its final name is always whole and is used without
    asking the server anything. The NASA server's size reports are
    unreliable enough that comparing sizes can't be trusted."""

    CACHE_DIR.mkdir(exist_ok=True)
    dest = CACHE_DIR / src.filename
    if dest.exists():
        print(f"Cached: {dest.name} ({dest.stat().st_size / 1e6:.0f} MB)")
        return dest

    part = dest.with_name(dest.name + ".part")
    total = remote_size(src.url)
    have = part.stat().st_size if part.exists() else 0

    if have > total > 0:
        # Partial file is larger than the source: stale, so start over.
        have = 0

    headers = {"Range": f"bytes={have}-"} if have else {}
    resp = requests.get(src.url, headers=headers, stream=True, timeout=120)

    # Resuming a .part that already holds the whole file asks for bytes
    # past the end, which the server refuses with 416: nothing left.
    if resp.status_code == 416 and have:
        resp.close()
        part.rename(dest)
        print(f"Cached: {dest.name} ({have / 1e6:.0f} MB)")
        return dest
    resp.raise_for_status()

    # A server that ignores the Range header sends 200 and the whole file.
    resuming = resp.status_code == 206
    if not resuming:
        have = 0

    action = "Resuming" if resuming else "Downloading"
    print(f"{action} {src.filename} ({total / 1e6:.0f} MB) ...")

    written = have
    next_report = written + 50e6
    with open(part, "ab" if resuming else "wb") as f:
        for block in resp.iter_content(chunk_size=1 << 20):
            f.write(block)
            written += len(block)
            if written >= next_report:
                pct = f" ({100 * written / total:.0f}%)" if total else ""
                print(f"  {written / 1e6:.0f} MB{pct}", flush=True)
                next_report = written + 50e6

    if total and written < total:
        sys.exit(f"Download ended early at {written / 1e6:.0f} of "
                 f"{total / 1e6:.0f} MB. Run again to resume.")
    part.rename(dest)
    print(f"  done, {written / 1e6:.0f} MB")
    return dest


def _warn_if_memory_tight(path: Path) -> None:
    """Pillow decodes the whole mosaic to crop it. Say so before it tries."""
    with Image.open(path) as probe:
        w, h = probe.size
    need_gib = w * h * 3 / 1024**3
    if need_gib > 2.0:
        print(
            f"  note: Pillow will decode all {w * h / 1e6:.0f} megapixels "
            f"(~{need_gib:.1f} GiB of RAM).\n"
            f"        If this thrashes, install pyvips to stream instead:\n"
            f"        brew install vips\n"
            f"        uv pip install pyvips"
        )


def apply_gamma(band: Image.Image, gamma: float) -> Image.Image:
    """Lift faint detail before the RGB565 conversion throws it away.

    Deep-sky images are mostly near-black, and RGB565 has only 32 levels
    per channel in red and blue -- so shadow detail that survives an 8-bit
    TIFF can band badly on the panel. Gamma above 1.0 brightens midtones
    and spreads that detail across more of the available levels.
    """
    if gamma == 1.0:
        return band
    table = [round(255 * (i / 255) ** (1 / gamma)) for i in range(256)]
    return band.point(table * 3)


# ------------------------------------------------- crop & process


def open_mosaic(path: Path):
    """Open the mosaic once, returning (image, width, height, is_pyvips).

    pyvips is preferred for large images because it can crop without
    decoding the whole file. Pillow works fine for smaller mosaics.
    """
    try:
        import pyvips
        img = pyvips.Image.new_from_file(str(path), access="random")
        return img, img.width, img.height, True
    except (ImportError, OSError):
        _warn_if_memory_tight(path)
        img = Image.open(path)
        w, h = img.size
        return img, w, h, False


def crop_band_at(img, y0: int, band_h: int, src_w: int, src_h: int,
                 scale: float, is_pyvips: bool) -> Image.Image | None:
    """Crop a single band at an absolute pixel row and scale it to HEIGHT.

    Returns a PIL Image or None if the row is out of bounds."""
    actual_h = min(band_h, src_h - y0)
    if actual_h <= 0:
        return None

    out_w = max(1, round(src_w * scale))

    if is_pyvips:
        region = img.crop(0, y0, src_w, actual_h).colourspace("srgb").cast("uchar")
        if region.bands > 3:
            region = region[0:3]
        elif region.bands == 1:
            region = region.bandjoin([region, region])
        band = Image.frombytes(
            "RGB", (region.width, region.height), region.write_to_memory()
        )
    else:
        band = img.crop((0, y0, src_w, y0 + actual_h)).convert("RGB")

    scaled_h = max(1, round(actual_h * scale))
    if band.size != (out_w, scaled_h):
        band = band.resize((out_w, scaled_h), Image.Resampling.LANCZOS)
    padded = Image.new("RGB", (out_w, HEIGHT))
    padded.paste(band, (0, 0))
    return padded


# --------------------------------------------------------------- packing

# 4x4 Bayer matrix (values 0-15) for ordered dithering. A fixed pattern,
# unlike error diffusion, stays put as the strip scrolls instead of
# shimmering.
BAYER4 = (
    (0, 8, 2, 10),
    (12, 4, 14, 6),
    (3, 11, 1, 9),
    (15, 7, 13, 5),
)

_dither_cache = {}


def dither_offsets(width: int, phase: int = 0) -> Image.Image:
    """Per-pixel offsets added before truncating to RGB565.

    Each offset spans one quantization step of its channel (0-7 for red
    and blue's 5 bits, 0-3 for green's 6 bits), so truncation rounds up
    or down in proportion to how close a value is to the next level.
    This turns banding in faint gradients into fine grain, and treats
    all three channels evenly instead of letting green, with its extra
    bit, survive alone near black.
    """
    key = (width, phase % 4)
    if key not in _dither_cache:
        reps = -(-(width + 4) // 4)
        rows = [
            (bytes(v for m in row for v in (m // 2, m // 4, m // 2)) * reps)[(phase % 4) * 3: (phase % 4 + width) * 3]
            for row in BAYER4
        ]
        raw = b"".join(rows[y % 4] for y in range(HEIGHT))
        _dither_cache[key] = Image.frombytes("RGB", (width, HEIGHT), raw)
    return _dither_cache[key]


def pack_columns(band: Image.Image, out, label: str = "", x_offset: int = 0) -> int:
    """Pack a band into column-major big-endian RGB565, writing to `out`.

    Contiguous columns support sequential SD reads and UART transfers.
    The ESP32 converts the explicit big-endian words into native RGB565.

    Returns the number of columns written.
    """
    width = band.width
    n_chunks = math.ceil(width / CHUNK_COLS)
    prefix = f"  {label} " if label else "  "

    for i, x0 in enumerate(range(0, width, CHUNK_COLS)):
        cw = min(CHUNK_COLS, width - x0)
        piece = band.crop((x0, 0, x0 + cw, HEIGHT))
        piece = ImageChops.add(piece, dither_offsets(cw, x_offset + x0))
        raw = piece.transpose(Image.Transpose.TRANSPOSE).tobytes()

        pixels = array.array(
            "H",
            (
                ((raw[j] & 0xF8) << 8)
                | ((raw[j + 1] & 0xFC) << 3)
                | (raw[j + 2] >> 3)
                for j in range(0, len(raw), 3)
            ),
        )
        if sys.byteorder == "little":
            pixels.byteswap()
        out.write(pixels.tobytes())

        print(f"{prefix}chunk {i + 1}/{n_chunks}", flush=True)

    return width


def write_dat(band: Image.Image, dest: Path) -> int:
    """Pack a single band into a new .dat file. Returns total bytes."""
    with open(dest, "wb") as out:
        cols = pack_columns(band, out)
    return cols * COL_BYTES


def build_preview(dat_path: Path, dest: Path, length: int) -> int:
    """Decode the finished .dat back into a viewable image.

    Reading back the real bytes (rather than re-saving the band) is what
    makes this a check: if the column ordering, byte order, or band
    position is wrong, it is wrong here too and plainly visible.
    """

    factor = max(1, math.ceil(length / PREVIEW_MAX_COLS))
    pw = math.ceil(length / factor)
    note = f" ({factor}x downscaled)" if factor > 1 else ""
    print(f"Building preview{note} ...")

    # Expand RGB565 -> RGB888 through a lookup over all 16-bit values,
    # replicating each channel's high bits into its low bits.
    lut = [
        bytes(
            (
                ((v >> 8) & 0xF8) | (v >> 13),
                ((v >> 3) & 0xFC) | ((v >> 9) & 0x03),
                ((v << 3) & 0xF8) | ((v >> 2) & 0x07),
            )
        )
        for v in range(65536)
    ]

    # A whole number of preview columns per block, so nothing drifts.
    block_cols = factor * max(1, 4096 // factor)

    preview = Image.new("RGB", (pw, HEIGHT))
    with open(dat_path, "rb") as f:
        x = 0
        for c0 in range(0, length, block_cols):
            ncols = min(block_cols, length - c0)
            pixels = array.array("H")
            pixels.frombytes(f.read(ncols * COL_BYTES))
            if sys.byteorder == "little":
                pixels.byteswap()
            raw = b"".join(map(lut.__getitem__, pixels))

            # The .dat is column-major, so transpose back to ncols x HEIGHT.
            block = Image.frombytes("RGB", (HEIGHT, ncols), raw)
            block = block.transpose(Image.Transpose.TRANSPOSE)

            # The last block absorbs rounding so the preview fills fully.
            bw = pw - x if c0 + ncols >= length else ncols // factor
            if factor > 1:
                block = block.resize((bw, HEIGHT), Image.Resampling.BOX)
            preview.paste(block, (x, 0))
            x += bw

    preview.save(dest, optimize=True)
    return factor


# ------------------------------------------------------------------ main


def list_sources() -> None:
    width = max(len(k) for k in SOURCES)
    print("Available sources:\n")
    for key, s in SOURCES.items():
        gpx = s.width * s.height / 1e9
        star = " (default)" if key == DEFAULT_SOURCE else ""
        print(f"  {key:<{width}}  {s.title}{star}")
        print(f"  {'':<{width}}  {s.telescope} · {s.width:,} x {s.height:,} · {gpx:.2f} Gpx")
        if s.note:
            print(f"  {'':<{width}}  {s.note}")
        print()


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Generate 480-pixel SenseCAP Indicator D1 band datasets.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--list", action="store_true", help="list sources and exit")
    p.add_argument(
        "--source", default=DEFAULT_SOURCE, choices=sorted(SOURCES), help="which mosaic"
    )
    p.add_argument(
        "--gamma",
        type=float,
        default=1.0,
        help="brighten midtones before RGB565; >1 lifts faint nebulosity",
    )
    p.add_argument(
        "--name", help="dataset directory name (default: source name, or local)"
    )
    inputs = p.add_mutually_exclusive_group()
    inputs.add_argument("--input", type=Path, help="use a local mosaic instead of downloading")
    inputs.add_argument("--demo", action="store_true", help="generate a small hardware-check dataset")
    p.add_argument("--output", type=Path, default=OUTPUT_DIR, help="dataset parent directory")
    args = p.parse_args()
    if not math.isfinite(args.gamma) or args.gamma <= 0:
        p.error("--gamma must be a finite positive number")
    args.name = args.name or ("demo" if args.demo else "local" if args.input else args.source)
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,31}", args.name):
        p.error("--name must contain 1-31 ASCII letters, digits, underscores or hyphens")
    return args


def print_run_times(total_cols: int) -> None:
    """Estimate travel time at example speeds; device presets are measured."""
    print("\nTravel time at example speeds (device fast presets depend on throughput):")
    for speed in SPEED_LEVELS:
        secs = total_cols / speed
        if secs < 3600:
            print(f"  {speed:>3} px/s   {secs / 60:.1f} min")
        elif secs < 86400:
            print(f"  {speed:>3} px/s   {secs / 3600:.1f} hr")
        else:
            print(f"  {speed:>3} px/s   {secs / 86400:.1f} days")


def file_crc32(path: Path) -> str:
    crc = 0
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            crc = zlib.crc32(block, crc)
    return f"{crc:08x}"


def generate_dataset(source_file: Path, dest: Path, *, source: str,
                     title: str, credit: str, gamma: float = 1.0) -> dict:
    """Stage a complete dataset, publishing only after all files are ready.

    Existing datasets are preserved under <name>.previous. A failed generation
    cannot replace the currently published dataset. Memory use is one band.
    """
    if not math.isfinite(gamma) or gamma <= 0:
        raise ValueError("gamma must be finite and positive")
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,31}", dest.name):
        raise ValueError("invalid dataset directory name")
    previous = dest.with_name(dest.name + ".previous")
    if previous.exists():
        raise FileExistsError(f"Move or remove {previous} before replacing {dest}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{dest.name}-", dir=dest.parent))
    img = None
    try:
        img, width, height, is_vips = open_mosaic(source_file)
        if not SCREEN_COLS <= width <= 2**32 - 481:
            raise ValueError(f"mosaic width must be {SCREEN_COLS} to {2**32 - 481} pixels")
        count = math.ceil(height / HEIGHT)
        if count > 9999:
            raise ValueError("too many bands (maximum 9999)")
        manifest = {
            "format": "indicator-galaxy", "version": 1,
            "source": source, "title": title, "credit": credit,
            "width": SCREEN_COLS, "height": HEIGHT,
            "pixel_format": "RGB565_BE", "layout": "column-major",
            "source_width": width, "source_height": height,
            "gamma": gamma, "bands": [], "total_bytes": 0,
        }
        # Contact sheet stays bounded even for very large mosaics.
        thumb_w, thumb_h = 480, 64
        sheet = Image.new("RGB", (thumb_w * 3, math.ceil(count / 3) * (thumb_h + 20)))
        draw = ImageDraw.Draw(sheet)
        chunk_cols = FAT32_MAX_BYTES // COL_BYTES
        if chunk_cols < 1:
            raise ValueError("file size limit cannot hold a column")
        for b in range(count):
            print(f"Band {b + 1}/{count}", flush=True)
            band = crop_band_at(img, b * HEIGHT, HEIGHT, width, height, 1.0, is_vips)
            band = apply_gamma(band, gamma)
            entry = {"width": width, "valid_height": min(HEIGHT, height - b * HEIGHT),
                     "chunks": []}
            thumbs = []
            for ci, x0 in enumerate(range(0, width, chunk_cols)):
                if ci > 999:
                    raise ValueError("too many chunks in one band")
                cols = min(chunk_cols, width - x0)
                filename = f"b{b:04d}{ci:03d}.dat"
                path = stage / filename
                with path.open("wb") as out:
                    pack_columns(band.crop((x0, 0, x0 + cols, HEIGHT)), out,
                                 x_offset=x0)
                size = path.stat().st_size
                entry["chunks"].append({"file": filename, "first_column": x0,
                                        "columns": cols, "bytes": size,
                                        "crc32": file_crc32(path)})
                manifest["total_bytes"] += size
                preview = stage / f"{path.stem}.png"
                build_preview(path, preview, cols)
                with Image.open(preview) as decoded:
                    # Allocate contact-sheet width in proportion to each chunk.
                    tw = max(1, round(thumb_w * cols / width))
                    thumbs.append(decoded.resize((tw, thumb_h), Image.Resampling.BOX))
            xx, yy = (b % 3) * thumb_w, (b // 3) * (thumb_h + 20)
            draw.text((xx + 4, yy + 3), f"Band {b + 1}", fill="white")
            offset = 0
            for thumb in thumbs:
                sheet.paste(thumb, (xx + offset, yy + 20))
                offset += thumb.width
            manifest["bands"].append(entry)
        sheet.save(stage / "contact.png")
        encoded = json.dumps(manifest, indent=2, ensure_ascii=True) + "\n"
        if len(encoded.encode()) > 128 * 1024:
            raise ValueError("manifest exceeds firmware's 128 KiB limit")
        (stage / "manifest.json").write_text(encoded)
        if dest.exists():
            dest.rename(previous)
        try:
            stage.rename(dest)
        except BaseException:
            if previous.exists() and not dest.exists():
                previous.rename(dest)
            raise
        print(f"Wrote {dest}: {count} bands, {manifest['total_bytes'] / 1e9:.3f} GB")
        print_run_times(sum(max(0, b["width"] - SCREEN_COLS) for b in manifest["bands"]))
        print("Transition time adds approximately 1.25 seconds per band.")
        return manifest
    finally:
        if img is not None and hasattr(img, "close"):
            img.close()
        if stage.exists():
            shutil.rmtree(stage)


def main() -> None:
    args = parse_args()
    if args.list:
        list_sources()
        return
    src = SOURCES[args.source]
    if args.demo:
        args.output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "demo.png"
            image = Image.new("RGB", (1440, 960))
            draw = ImageDraw.Draw(image)
            colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255),
                      (255, 255, 0), (0, 255, 255)]
            for i, color in enumerate(colors):
                draw.rectangle((i * 240, 0, (i + 1) * 240 - 1, 479), fill=color)
                draw.text((i * 240 + 20, 200), f"BAND 1  X={i * 240}", fill="black")
            for x in range(0, 1440, 60):
                for y in range(480, 960, 60):
                    color = (170, 170, 170) if (x // 60 + y // 60) % 2 else (20, 20, 20)
                    draw.rectangle((x, y, x + 59, y + 59), fill=color)
            draw.text((20, 500), "BAND 2 - SQUARE GRID - EACH CELL IS 60 X 60", fill="red")
            image.save(path)
            generate_dataset(path, args.output / args.name, source="demo", title="Display diagnostics",
                             credit="Generated color bars and square grid", gamma=args.gamma)
        return
    path = args.input or ensure_source(src)
    dest = args.output / args.name
    try:
        generate_dataset(path, dest, source="local" if args.input else args.source,
                         title=args.input.stem if args.input else src.title,
                         credit="User-supplied image" if args.input else src.credit,
                         gamma=args.gamma)
    except (OSError, ValueError) as exc:
        sys.exit(str(exc))
    print(f"Inspect {dest / 'contact.png'}, then copy the whole {dest.name}/ directory "
          "into /galaxy/ on the FAT32 card.")


if __name__ == "__main__":
    main()
