"""Exercise preparation, memory limits and cache selection without a disc or compiler."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/desktop"))
from pipeline import (GENERATED_HEADER_PREFIX_BYTES, MIB, PAL_BOOT, Pipeline, PrecompiledHeader,
                      build_workers, cgroup_memory_headroom, executable_name)


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
        self.pipeline = Pipeline(self.pipeline.program, self.pipeline.data, self.pipeline.env)
        self.pipeline.run = self.run_fixture
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
            with (patch("pipeline.available_memory", return_value=3072 * MIB),
                  patch("pipeline.cgroup_memory_headroom", return_value=None)):
                self.assertEqual(build_workers({}), 3)
                self.assertEqual(build_workers({"SOTC_BUILD_JOBS": "6"}), 6)
                self.assertEqual(build_workers({"SOTC_BUILD_JOBS": "bad"}), 3)
            with patch("pipeline.available_memory", return_value=1024 * MIB):
                self.assertEqual(build_workers({}), 1)
            with (patch("pipeline.WINDOWS", False),
                  patch("pipeline.cgroup_memory_headroom", return_value=2304 * MIB)):
                self.assertEqual(build_workers({"SOTC_BUILD_JOBS": "6"}), 2)

    def test_cgroup_limits_follow_mount_roots_and_process_ancestors(self):
        root = self.pipeline.data
        proc = root / "proc"
        proc.mkdir()
        (proc / "cgroup").write_text("1:name=systemd:/ignored\n0::/user.slice/session/build.scope\n")
        mount = root / "with space"
        full_mount = root / "full"
        escaped = str(mount).replace(" ", r"\040")
        (proc / "mountinfo").write_text(
            f"10 8 0:35 /user.slice {escaped} rw - cgroup2 cgroup rw\n"
            f"11 8 0:35 / {full_mount} ro - cgroup2 cgroup rw\n")

        session = mount / "session"
        leaf = session / "build.scope"
        leaf.mkdir(parents=True)
        full_scope = full_mount / "user.slice/session/build.scope"
        full_scope.mkdir(parents=True)

        def memory(directory, high, maximum, current):
            for control, value in (("high", high), ("max", maximum), ("current", current)):
                (directory / f"memory.{control}").write_text(str(value) + "\n")

        memory(leaf, "max", 8192 * MIB, 200 * MIB)
        memory(session, "max", 3072 * MIB, 1024 * MIB)
        memory(mount, "max", "max", 2048 * MIB)
        memory(full_mount, 4096 * MIB, "max", 3072 * MIB)
        self.assertEqual(cgroup_memory_headroom(proc), 1024 * MIB)
        (full_mount / "memory.high").write_text("max\n")
        self.assertEqual(cgroup_memory_headroom(proc), 2048 * MIB)
        # Only inactive file cache is reclaimable for this estimate. Active
        # cache and anonymous memory continue to reduce the effective budget.
        stat = session / "memory.stat"
        stat.write_text(f"active_file {512 * MIB}\ninactive_file {256 * MIB}\n")
        self.assertEqual(cgroup_memory_headroom(proc), 2304 * MIB)
        stat.write_text("inactive_file invalid\n")
        self.assertEqual(cgroup_memory_headroom(proc), 2048 * MIB)
        (session / "memory.max").write_text("max\n")
        (leaf / "memory.max").write_text("max\n")
        self.assertIsNone(cgroup_memory_headroom(proc))

    def test_compiler_selection_preserves_default_cache_and_reuses_own_variant(self):
        clang = self.pipeline.clang
        clang.parent.mkdir(parents=True)
        clang.touch()
        program, data = self.pipeline.program, self.pipeline.data
        normal = Pipeline(program, data, {})
        gcc = Pipeline(program, data, {"SOTC_BUILD_CLANG": "0"})
        self.assertEqual(normal.work, data / "builds/fixture")
        self.assertEqual(gcc.work, normal.work / "gcc")
        self.assertEqual(Pipeline(program, data, {}, boot_name=PAL_BOOT).work, normal.work / "SCES-53326")
        self.assertEqual(Pipeline(program, data, {"SOTC_BUILD_CLANG": "0"}, boot_name=PAL_BOOT).work,
                         normal.work / "SCES-53326/gcc")

        clang_game = normal.work / "gamecore" / executable_name("game")
        clang_game.parent.mkdir()
        clang_game.write_bytes(b"legacy default game")
        clang_marker = normal.work / "complete.json"
        clang_marker.write_text(json.dumps({"sha256": hashlib.sha256(clang_game.read_bytes()).hexdigest()}))
        before = [path.stat().st_mtime_ns for path in (clang_game, clang_marker)]
        with patch.object(normal, "prepare_stages", side_effect=AssertionError("default cache should be reused")):
            self.assertEqual(normal.prepare(data / "disc"), clang_game)

        gcc_game = gcc.work / "gamecore" / executable_name("game")
        gcc_game.parent.mkdir()
        gcc_game.write_bytes(clang_game.read_bytes())
        gcc_marker = gcc.work / "complete.json"
        gcc_marker.write_bytes(clang_marker.read_bytes())

        def build_gcc(*args):
            gcc_game.write_bytes(b"game built with GCC")

        with patch.object(gcc, "prepare_stages", side_effect=build_gcc) as build:
            self.assertEqual(gcc.prepare(data / "disc"), gcc_game)
            self.assertEqual(gcc.prepare(data / "disc"), gcc_game)
            build.assert_called_once()
        self.assertEqual(json.loads(gcc_marker.read_text())["compiler"], "gcc")
        self.assertEqual([path.stat().st_mtime_ns for path in (clang_game, clang_marker)], before)
        replaced = json.loads(gcc_marker.read_text())
        replaced["compiler"] = "clang"
        gcc_marker.write_text(json.dumps(replaced))
        with patch.object(gcc, "prepare_stages", side_effect=build_gcc) as rebuild:
            self.assertEqual(gcc.prepare(data / "disc"), gcc_game)
            rebuild.assert_called_once()

        clang.unlink()
        fallback = Pipeline(program, data, {})
        self.assertEqual(fallback.work, gcc.work)
        with patch.object(fallback, "prepare_stages", side_effect=AssertionError("GCC cache should be reused")):
            self.assertEqual(fallback.prepare(data / "disc"), gcc_game)
