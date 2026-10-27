"""Run Python, wire-protocol and actual firmware manifest-parser host checks.

Run after a PlatformIO ESP32 build has installed framework-espidf:
    uv run --no-project tests/check_host.py
Set PLATFORMIO_CORE_DIR when using an isolated PlatformIO package directory.
"""
import argparse
import contextlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(*command):
    print("Running:", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-sanitizers", action="store_true")
    args = parser.parse_args()
    core = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    cjson = core / "packages/framework-espidf/components/json/cJSON"
    if not (cjson / "cJSON.c").is_file():
        parser.error("build the ESP32 PlatformIO project first, or set PLATFORMIO_CORE_DIR")
    cc, cxx = shutil.which("clang") or shutil.which("cc"), shutil.which("clang++") or shutil.which("c++")
    if not cc or not cxx:
        parser.error("native C and C++ compilers are required (Xcode Command Line Tools on macOS)")
    flags = [] if args.no_sanitizers else ["-fsanitize=address,undefined"]
    run(sys.executable, "-m", "unittest", "discover", "-s", "tests", "-v")
    with tempfile.TemporaryDirectory(prefix="galaxy-check-") as td:
        temp = Path(td)
        run(cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", *flags,
            "tests/protocol_test.cpp", "-o", temp / "protocol")
        run(temp / "protocol")
        run(cxx, "-std=c++17", "-Wall", "-Wextra", *flags,
            "-DFS_FATFS_WINDOW_ALIGNMENT=4", "-Ifirmware/shared", "-Ifirmware/rp2040/lib/fatfs",
            "tests/card_mount_test.cpp", "-o", temp / "card-mount")
        run(temp / "card-mount")
        run(cc, "-Wno-deprecated-declarations", *flags, "-c", cjson / "cJSON.c", "-o", temp / "cjson.o")
        run(cxx, "-std=c++17", "-Wall", "-Wextra", *flags, "-I" + str(cjson),
            "-Ifirmware/shared", "tests/manifest_test.cpp", "firmware/esp32/main/manifest.cpp",
            temp / "cjson.o", "-o", temp / "manifest")
        sys.path.insert(0, str(ROOT / "generator"))
        from PIL import Image
        from generate_strip import generate_dataset
        source = temp / "source.png"
        Image.new("RGB", (500, 483), (33, 97, 193)).save(source)
        with contextlib.redirect_stdout(io.StringIO()):
            generate_dataset(source, temp / "fixture", source="fixture", title="Fixture", credit="Test")
        run(temp / "manifest", temp / "fixture/manifest.json")
    print("All host checks passed. Physical display, SD, and UART testing remains necessary.")


if __name__ == "__main__":
    main()
