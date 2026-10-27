import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "generator"))
import generate_strip as g
from validate_dataset import validate


class GeneratorTests(unittest.TestCase):
    def test_pillow_and_vips_crops_agree(self):
        try:
            import pyvips
        except (ImportError, OSError):
            self.skipTest("optional pyvips not installed")
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "image.tif"
            image = Image.new("RGB", (500, 481), (17, 65, 129))
            image.save(path)
            vips = pyvips.Image.new_from_file(str(path), access="random")
            with Image.open(path) as pil:
                a = g.crop_band_at(pil, 480, 480, 500, 481, 1, False)
            b = g.crop_band_at(vips, 480, 480, 500, 481, 1, True)
            self.assertEqual(a.tobytes(), b.tobytes())
            self.assertEqual(b.getpixel((20, 0)), (17, 65, 129))
            self.assertEqual(b.getpixel((20, 1)), (0, 0, 0))

    def test_rgb565_column_order_and_preview(self):
        band = Image.new("RGB", (480, 480))
        band.paste((255, 0, 0), (0, 0, 1, 480))
        band.paste((0, 255, 0), (1, 0, 2, 480))
        band.paste((0, 0, 255), (2, 0, 3, 480))
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
            path = Path(td) / "band.dat"
            g.write_dat(band, path)
            self.assertEqual(path.stat().st_size, 480 * 480 * 2)
            self.assertEqual(path.read_bytes()[:960], b"\xf8\x00" * 480)
            self.assertEqual(path.read_bytes()[960:1920], b"\x07\xe0" * 480)
            self.assertEqual(path.read_bytes()[1920:2880], b"\x00\x1f" * 480)
            preview = Path(td) / "preview.png"
            g.build_preview(path, preview, 480)
            with Image.open(preview) as im:
                self.assertEqual(im.size, (480, 480))
                self.assertEqual(im.getpixel((1, 100)), (0, 255, 0))

    def test_full_coverage_padding_chunks_checksums_and_publish(self):
        # Force many small files at non-Bayer-aligned boundaries. No GiB fixture.
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stdout(io.StringIO()):
            base = Path(td)
            image = Image.new("RGB", (487, 483), (103, 71, 127))
            image.save(base / "input.png")
            with patch.object(g, "FAT32_MAX_BYTES", 113 * 960):
                m = g.generate_dataset(base / "input.png", base / "test",
                                       source="test", title="Test", credit="Test")
            self.assertEqual(len(m["bands"]), 2)
            self.assertEqual(m["bands"][1]["valid_height"], 3)
            validate(base / "test")
            encoded = b"".join((base / "test" / c["file"]).read_bytes() for c in m["bands"][0]["chunks"])
            complete = io.BytesIO()
            g.pack_columns(image.crop((0, 0, 487, 480)), complete)
            self.assertEqual(encoded, complete.getvalue())
            last = b"".join((base / "test" / c["file"]).read_bytes() for c in m["bands"][1]["chunks"])
            self.assertEqual(last[6:960], b"\0" * 954)
            g.generate_dataset(base / "input.png", base / "test", source="test", title="New", credit="Test")
            self.assertTrue((base / "test.previous" / "manifest.json").exists())
            before = (base / "test" / "manifest.json").read_bytes()
            with self.assertRaises(FileExistsError):
                g.generate_dataset(base / "input.png", base / "test", source="test", title="Third", credit="Test")
            self.assertEqual((base / "test" / "manifest.json").read_bytes(), before)
            chunk = base / "test" / m["bands"][0]["chunks"][0]["file"]
            with chunk.open("r+b") as f:
                f.write(b"\0\0")
            with self.assertRaisesRegex(ValueError, "checksum"):
                validate(base / "test")

    def test_invalid_image_or_gamma_cannot_publish(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "small.png"
            Image.new("RGB", (100, 100)).save(path)
            for gamma in (0, -1, float("nan"), float("inf")):
                with self.assertRaises(ValueError):
                    g.generate_dataset(path, Path(td) / "bad", source="x", title="x", credit="x", gamma=gamma)
            with self.assertRaises(ValueError):
                g.generate_dataset(path, Path(td) / "bad", source="x", title="x", credit="x")
            self.assertFalse((Path(td) / "bad").exists())


if __name__ == "__main__":
    unittest.main()
