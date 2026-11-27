"""ROM-free checks for the preparation kit's pins and APK payload rules."""

import argparse
import io
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from stage_on_device_kit import LLVM, PYTHON, native, sha256
from verify_on_device_apk import REQUIRED_KIT_ENTRIES, verify_kit


class KitTests(unittest.TestCase):
    def verify_entries(self, extra=None):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            for name in REQUIRED_KIT_ENTRIES:
                archive.writestr(name, b"")
            if extra:
                archive.writestr(extra, b"")
        with zipfile.ZipFile(buffer) as archive:
            verify_kit(archive)

    def test_kit_allowlist(self):
        self.verify_entries()
        for entry in ("game/NICO.DAT", "lib/libps2EntryRunner.so", "lib/renamed-guest.a",
                      "tools/output/register_functions.cpp", "../payload", "tools\\payload"):
            with self.subTest(entry=entry), self.assertRaises(ValueError):
                self.verify_entries(entry)

    def test_alignment(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "runner.so"
            image = bytearray(120)
            image[:6] = b"\x7fELF\x02\x01"
            struct.pack_into("<HH", image, 16, 3, 183)
            struct.pack_into("<Q14xHH", image, 32, 64, 56, 1)
            struct.pack_into("<IIQQQQQQ", image, 64, 1, 5, 0, 0, 0, 120, 120, 16384)
            path.write_bytes(image)
            native(path)
            with self.assertRaises(ValueError):
                native(path, executable=True)
            struct.pack_into("<Q", image, 112, 4096)
            path.write_bytes(image)
            with self.assertRaises(ValueError):
                native(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--downloads", type=Path)
    parser.add_argument("--staged-kit", type=Path)
    args = parser.parse_args()
    result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(KitTests))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.downloads:
        for filename, pin in (("clang.tar.xz", LLVM), ("python.tar.gz", PYTHON)):
            if sha256(args.downloads / filename) != pin[1]:
                parser.exit(1, f"Download SHA-256 mismatch: {filename}\n")
        print("Compiler and CPython archive pins verified.")
    if args.staged_kit:
        with zipfile.ZipFile(args.staged_kit / "assets/sotc-ondevice.zip") as archive:
            verify_kit(archive)
        print("Embedded kit payload verified.")


if __name__ == "__main__":
    main()
