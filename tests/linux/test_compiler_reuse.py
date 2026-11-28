import hashlib
from io import BytesIO
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/linux"))
from reuse_compiler import PRODUCT, reuse_compiler


class CompilerReuseTests(unittest.TestCase):
    build_id = "1" * 24

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.archive = self.directory / "release.tar.gz"
        self.output = self.directory / "artifacts"

    def write_archive(self, extra=None):
        files = {
            "release.json": json.dumps({"version": "1.2.12", "platform": "linux-x86_64"}).encode(),
            "compiler/manifest.json": json.dumps({"build_id": self.build_id}).encode(),
            "compiler/bin/ps2_recomp": b"compiler fixture",
            "licenses/native/SOURCES.txt": b"license fixture",
            "third-party-sources.tar.gz": b"source fixture",
            "data/launcher.json": b"not part of the kit",
        }
        if extra:
            files.update(extra)
        with tarfile.open(self.archive, "w:gz") as archive:
            for path, contents in files.items():
                member = tarfile.TarInfo(f"{PRODUCT}/{path}")
                member.size = len(contents)
                member.mode = 0o755 if path.endswith("/ps2_recomp") else 0o644
                archive.addfile(member, BytesIO(contents))
        return hashlib.sha256(self.archive.read_bytes()).hexdigest()

    def test_extracts_only_verified_kit_and_preserves_executable(self):
        reuse_compiler(self.archive, "v1.2.12", self.write_archive(), self.build_id, self.output)
        compiler = self.output / "compiler/bin/ps2_recomp"
        self.assertEqual(compiler.read_bytes(), b"compiler fixture")
        self.assertEqual(compiler.stat().st_mode & 0o777, 0o755)
        self.assertTrue((self.output / "distribution-licenses/native/SOURCES.txt").is_file())
        self.assertFalse((self.output / "data").exists())
        self.assertFalse((self.output / "release.json").exists())

    def test_rejects_mismatched_download_and_compiler_id(self):
        digest = self.write_archive()
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            reuse_compiler(self.archive, "v1.2.12", "0" * 64, self.build_id, self.output)
        with self.assertRaisesRegex(ValueError, "build_id mismatch"):
            reuse_compiler(self.archive, "v1.2.12", digest, "2" * 24, self.output)
        self.assertFalse((self.output / "compiler").exists())

    def test_rejects_escape_from_kit(self):
        digest = self.write_archive({"compiler/../../unexpected": b"unexpected"})
        with self.assertRaisesRegex(ValueError, "Unexpected link or path"):
            reuse_compiler(self.archive, "v1.2.12", digest, self.build_id, self.output)
        self.assertEqual(list(self.output.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
