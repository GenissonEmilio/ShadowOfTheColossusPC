"""Exercise precompiled-header reuse and fallback without a disc or GCC."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/desktop"))
from pipeline import GENERATED_HEADER_PREFIX_BYTES, Pipeline, PrecompiledHeader


class PreparePipelineTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        available = root / "program/compiler"
        available.mkdir(parents=True)
        (available / "manifest.json").write_text(json.dumps({"build_id": "fixture", "link_inputs": ["lib/fixture.a"]}))
        self.pipeline = Pipeline(root / "program", root / "data",
                                 {"SOTC_BUILD_JOBS": "1", "SOTC_BUILD_BATCH_SIZE": "1"})
        self.header = PrecompiledHeader(root)
        self.commands = []
        self.fail_on_pch = False
        self.pipeline.run = self.run_fixture

    def run_fixture(self, args, cwd=None, env=None):
        if "-c" in args or "c++-header" in args:
            self.commands.append(args)
            if "-include" in args and self.fail_on_pch:
                raise subprocess.CalledProcessError(1, args)
            Path(args[-1]).touch()

    def stage(self, name):
        directory = self.pipeline.work / name
        generated = directory / "output"
        generated.mkdir(parents=True)
        (generated / "a.cpp").write_bytes(GENERATED_HEADER_PREFIX_BYTES.replace(b"\n", b"\r\n"))
        (generated / "b.cpp").write_text("// special source with a different header order\n")
        return directory

    def test_only_matching_uncached_batches_share_the_header(self):
        boot = self.stage("boot")
        kernel = self.stage("kernel")
        self.pipeline.compile(boot)
        self.commands.clear()
        self.pipeline.compile(boot, precompiled=self.header)
        self.assertEqual(self.commands, [])
        self.assertIsNone(self.header.ready)

        self.pipeline.compile(kernel, precompiled=self.header)
        (boot / "objects/batch-0000.key").unlink()
        self.pipeline.compile(boot, precompiled=self.header)
        self.assertEqual(sum("c++-header" in command for command in self.commands), 1)
        self.assertEqual(["-include" in command for command in self.commands if "-c" in command],
                         [True, False, True])

    def test_failure_retries_without_header_and_keeps_result(self):
        boot = self.stage("boot")
        self.fail_on_pch = True
        self.pipeline.compile(boot, precompiled=self.header)
        self.assertEqual(["-include" in command for command in self.commands if "-c" in command],
                         [True, False, False])
        self.assertFalse(self.header.ready)
        self.assertTrue((boot / "objects/batch-0000.key").is_file())
        self.assertIn("precompiled-header-fallback", self.pipeline.log.read_text(encoding="utf-8"))
