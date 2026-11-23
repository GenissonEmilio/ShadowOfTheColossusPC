#!/usr/bin/env python3
"""Prepare locally recompiled EE sources for the Android native build."""

from __future__ import annotations

import argparse
from contextlib import contextmanager, redirect_stderr, redirect_stdout
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "desktop"))
from import_disc import EXPECTED, import_disc, inspect_disc, sha256
from pipeline import BOOT, MODULES, helper

RAM_SIZE = 32 * 1024 * 1024
EXE = ".exe" if os.name == "nt" else ""


@contextmanager
def lock_work_directory(work):
    work.mkdir(parents=True, exist_ok=True)
    with (work / ".prepare.lock").open("a+b") as lock:
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise ValueError(f"Another process is preparing {work}") from error
        yield


def run(command, log, cwd):
    with log.open("w", encoding="utf-8") as stream:
        try:
            subprocess.run(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT, check=True)
        except subprocess.CalledProcessError as error:
            raise ValueError(f"{command[0]} exited with {error.returncode}. See {log}") from error


def run_helper(name, arguments, log):
    original_argv, original_path = sys.argv, sys.path[:]
    try:
        with log.open("w", encoding="utf-8") as stream, redirect_stdout(stream), redirect_stderr(stream):
            helper(ROOT, name, arguments)
    finally:
        sys.argv, sys.path[:] = original_argv, original_path


def capture(runner, disc, output, log):
    pending = output.with_suffix(".partial")
    pending.unlink(missing_ok=True)
    env = dict(os.environ, PS2X_DUMP_RAM_ON_MISSING=str(pending), PS2X_HIDDEN_WINDOW="1",
               PS2X_MEMCARD_DIR=str(output.parent / "memcards"))
    for name in ("SOTC_SHADER", "SOTC_SHADER_STATUS"):
        env.pop(name, None)
    with log.open("w", encoding="utf-8") as stream, subprocess.Popen(
        [runner, disc / BOOT], cwd=output.parent, env=env, stdout=stream, stderr=subprocess.STDOUT
    ) as process:
        try:
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                if pending.is_file() and pending.stat().st_size == RAM_SIZE:
                    break
                if process.poll() is not None:
                    break
                time.sleep(0.25)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    if not pending.is_file() or pending.stat().st_size != RAM_SIZE:
        raise ValueError(f"Could not capture {output.stem.upper()}. See {log}")
    pending.replace(output)


def compile_stage(folder, recompiler, manifest=None):
    config = folder / "config.toml"
    output = folder / "output"
    previous = folder / ".previous-output"
    log = folder / "recompile.log"
    if previous.is_dir() and not output.exists():
        previous.replace(output)
    with tempfile.TemporaryDirectory(prefix=".recompile-", dir=folder) as temp_dir:
        temporary = Path(temp_dir)
        generated = temporary / "output"
        config_file = temporary / config.name
        destination = json.dumps(generated.as_posix(), ensure_ascii=False)
        content = re.sub(r"^output = .*$", f"output = {destination}",
                         config.read_text(encoding="utf-8"), flags=re.M)
        config_file.write_text(content, encoding="utf-8")
        run([recompiler, config_file], log, folder)
        required = ("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h", "register_functions.cpp")
        if not all((generated / name).is_file() for name in required):
            raise ValueError(f"The recompiler produced incomplete sources. See {log}")
        if manifest is not None:
            (generated / "prepare-manifest.json").write_text(
                json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
            )
        if previous.is_dir():
            shutil.rmtree(previous)
        if output.is_dir():
            output.replace(previous)
        try:
            generated.replace(output)
        except OSError:
            if previous.is_dir():
                previous.replace(output)
            raise
        if previous.is_dir():
            shutil.rmtree(previous)
    return output


def host_runner(folder, stage, args):
    binary = args.work / "host-runtime"
    overrides = ROOT / "game" / "overrides"
    run([args.cmake, "-S", ROOT / "PS2Recomp", "-B", binary, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DPS2X_BUILD_RECOMP=OFF", "-DPS2X_BUILD_RUNTIME=ON",
         "-DPS2X_BUILD_ANALYZER=OFF", "-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_STUDIO=OFF",
         "-DPS2X_ENABLE_DEBUG_UI=OFF", "-DPS2X_ENABLE_FFMPEG=OFF",
         "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF", "-DPS2X_ENABLE_IOP_RPC_TRACE=OFF",
         *args.host_cmake_arg, f"-DPS2X_RUNNER_DIRS={stage.as_posix()};{overrides.as_posix()}"],
        folder / "host-configure.log", args.work)
    run([args.cmake, "--build", binary, "--target", "ps2EntryRunner", "--parallel", str(args.jobs)],
        folder / "host-build.log", args.work)
    return binary / "ps2xRuntime" / f"ps2EntryRunner{EXE}"


def recompile_modules(modules, disc, captures, symbols, boot_config, args):
    folder = args.work / modules[-1].lower()
    folder.mkdir(exist_ok=True)
    capture_hashes = {}
    with tempfile.TemporaryDirectory(prefix=".capture-inputs-", dir=folder) as temp_dir:
        specs = []
        for module in modules:
            dump = captures / f"{module.lower()}.bin"
            if not dump.is_file() or dump.stat().st_size != RAM_SIZE:
                raise ValueError(f"Expected a {RAM_SIZE}-byte RAM capture at {dump}")
            snapshot = Path(temp_dir) / dump.name
            shutil.copyfile(dump, snapshot)
            capture_hashes[module] = sha256(snapshot)
            if snapshot.stat().st_size != RAM_SIZE or capture_hashes[module] != sha256(dump):
                raise ValueError(f"The RAM capture changed while being copied: {dump}")
            specs.append(f"{(disc / (module + '.XFF')).as_posix()}@auto@{snapshot.as_posix()}")
        print(f"Recompiling through {modules[-1]}", flush=True)
        run_helper("build_elf", [folder, disc / BOOT, symbols, *specs], folder / "link.log")
    run_helper("gen_config", [folder, boot_config], folder / "config.log")
    manifest = None
    if tuple(modules) == MODULES:
        with (folder / "functions.csv").open(encoding="utf-8") as functions:
            function_count = sum(1 for _ in functions) - 1
        manifest = {
            "serial": "SCUS-97472",
            "disc_files_sha256": {name: digest for name, (_, digest) in EXPECTED.items()},
            "capture_sha256": capture_hashes,
            "combined_elf_sha256": sha256(folder / "SOTC.elf"),
            "config_sha256": sha256(folder / "config.toml"),
            "recompiler_sha256": sha256(args.recompiler),
            "mapped_functions": function_count,
        }
    return compile_stage(folder, args.recompiler, manifest)


def prepare(args):
    work = args.work
    disc = work / "disc"
    if not disc.exists():
        print("Importing the local disc image", flush=True)
        import_disc(args.iso, disc)
    elif args.iso.suffix.lower() == ".iso":
        inspect_disc(args.iso)
    if not (disc / "NICO.DAT").is_file():
        raise ValueError(f"Missing extracted file {disc / 'NICO.DAT'}. Choose a fresh work directory.")
    for name, (size, digest) in EXPECTED.items():
        source = disc / name
        if not source.is_file() or source.stat().st_size != size or sha256(source) != digest:
            raise ValueError(f"Invalid extracted disc file {source}. Choose a fresh work directory.")

    boot = work / "boot"
    boot.mkdir(exist_ok=True)
    symbols = work / "symbols.txt"
    run_helper("symbols", [disc / "KERNEL.XFF", symbols], boot / "symbols.log")
    config = (ROOT / "game" / "scus.toml").read_text(encoding="utf-8")
    executable = json.dumps((disc / BOOT).as_posix(), ensure_ascii=False)
    config = re.sub(r"^input = .*$", f"input = {executable}", config, flags=re.M)
    boot_config = boot / "config.toml"
    boot_config.write_text(config, encoding="utf-8")

    if args.captures_dir:
        return recompile_modules(MODULES, disc, args.captures_dir, symbols, boot_config, args)

    captures = work / "captures"
    captures.mkdir(exist_ok=True)
    run_helper("funcmap_elf", [disc / BOOT, symbols, boot / "functions.csv"], boot / "functions.log")
    print(f"Recompiling {BOOT}", flush=True)
    output = compile_stage(boot, args.recompiler)
    modules = []
    for module in MODULES:
        print(f"Building the host runner and capturing {module}", flush=True)
        runner = host_runner(output.parent, output, args)
        dump = captures / f"{module.lower()}.bin"
        capture(runner, disc, dump, captures / f"{module.lower()}.log")
        modules.append(module)
        output = recompile_modules(modules, disc, captures, symbols, boot_config, args)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", required=True, type=Path, help="owned SCUS-97472 disc image")
    parser.add_argument("--work", required=True, type=Path, help="private directory outside the repository")
    parser.add_argument("--recompiler", required=True, type=Path, help="host ps2_recomp executable")
    parser.add_argument("--captures-dir", type=Path, help="reuse kernel.bin, manager.bin and gamecore.bin")
    parser.add_argument("--cmake", default="cmake", help="host CMake executable")
    parser.add_argument("--host-cmake-arg", action="append", default=[], help="extra host CMake option")
    parser.add_argument("--jobs", default=2, type=int, help="parallel host build jobs")
    args = parser.parse_args()
    args.iso, args.work, args.recompiler = [path.resolve() for path in (args.iso, args.work, args.recompiler)]
    if args.work.is_relative_to(ROOT) or "@" in str(args.work) or ";" in str(args.work):
        parser.error("--work must be outside the repository and its path cannot contain @ or ;")
    if args.captures_dir:
        args.captures_dir = args.captures_dir.resolve()
    else:
        cmake = shutil.which(args.cmake)
        if cmake is None:
            parser.error("CMake was not found. Install it or pass --cmake.")
        args.cmake = Path(cmake).resolve()
    if not args.iso.is_file() or not args.recompiler.is_file() or args.jobs < 1:
        parser.error("--iso and --recompiler must be existing files and --jobs must be positive")
    try:
        with lock_work_directory(args.work):
            output = prepare(args)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Generated Android sources: {output}")


if __name__ == "__main__":
    main()
