"""Exercise precompiled-header reuse and fallback without a disc or GCC."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/desktop"))
from pipeline import GENERATED_HEADER_PREFIX_BYTES, MIB, Pipeline, PrecompiledHeader, build_workers, executable_name


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
        self.link_command = None
        self.fail_on_pch = False
        self.pipeline.run = self.run_fixture

    def run_fixture(self, args, cwd=None, env=None):
        if "-c" in args or "c++-header" in args:
            self.commands.append(args)
            if self.fail_on_pch and ("-include" in args or "-include-pch" in args):
                raise subprocess.CalledProcessError(1, args)
            Path(args[-1]).touch()
        else:
            self.link_command = args

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

    def test_clang_compiles_and_gcc_links(self):
        toolchain = self.pipeline.bundle / "toolchain"
        clang = toolchain / "bin" / executable_name("clang++")
        clang.parent.mkdir(parents=True)
        clang.touch()
        (toolchain / "include/c++/16/x86_64-w64-mingw32").mkdir(parents=True)
        final = self.stage("gamecore")
        self.pipeline.compile(final, precompiled=self.header)

        commands = [command for command in self.commands if "-c" in command]
        self.assertTrue(all(Path(command[0]) == clang for command in commands))
        self.assertIn("-include-pch", commands[0])
        self.assertIn("-O2", commands[0])
        self.assertNotIn("-include-pch", commands[1])
        self.assertEqual(Path(self.link_command[0]).name, executable_name("g++"))

    def test_worker_budget_and_override(self):
        with patch("pipeline.os.process_cpu_count", return_value=8, create=True):
            with patch("pipeline.available_memory", return_value=3072 * MIB):
                self.assertEqual(build_workers({}), 3)
                self.assertEqual(build_workers({"SOTC_BUILD_JOBS": "6"}), 6)
                self.assertEqual(build_workers({"SOTC_BUILD_JOBS": "bad"}), 3)
            with patch("pipeline.available_memory", return_value=1024 * MIB):
                self.assertEqual(build_workers({}), 1)
