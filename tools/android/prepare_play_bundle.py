#!/usr/bin/env python3
"""Build a private Android .sotcplay bundle from an owned North American PS2 ISO."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

from prepare_game import (
    BOOT, EXPECTED, MODULES, RAM_SIZE, ROOT, inspect_disc, lock_work_directory, prepare, run, sha256,
)

NDK_VERSION = "28.2.13676358"
CMAKE_VERSION = "3.31.6"
EXE = ".exe" if os.name == "nt" else ""
RUNNER = "libps2EntryRunner.so"
BUNDLE = "ShadowOfTheColossus.sotcplay"
MAX_RUNNER_BYTES = 256 * 1024**2
ANDROID_LIBRARIES = {
    "libEGL.so", "libGLESv2.so", "libGLESv3.so", "libOpenSLES.so", "libaaudio.so",
    "libandroid.so", "libc.so", "libdl.so", "liblog.so", "libm.so", "libz.so",
}
RUNNER_EXPORTS = {"ANativeActivity_onCreate", "avcodec_find_decoder"} | {
    "Java_com_genisson_sotc_NativeGameActivity_native" + method
    for method in ("HasGame", "Configure", "SetInput", "ReleaseInput", "Pause", "SetWidescreen")
}


def validate_runner(runner: Path, readelf: Path):
    if not runner.is_file() or not 0 < runner.stat().st_size <= MAX_RUNNER_BYTES:
        raise ValueError("The runner must be a nonempty Android library up to 256 MiB.")
    result = subprocess.run([readelf, "-hldn", "--dyn-syms", "--wide", runner],
                            capture_output=True, encoding="utf-8", errors="replace",
                            env=dict(os.environ, LC_ALL="C"))
    if result.returncode:
        raise ValueError(f"Could not read the runner ELF: {result.stderr.strip()}")
    report = result.stdout
    if not all(re.search(pattern, report, re.M) for pattern in (
        r"^\s*Class:\s+ELF64$", r"^\s*Data:.*little endian$",
        r"^\s*Machine:\s+AArch64$", r"^\s*Type:\s+DYN\b",
    )):
        raise ValueError("The runner must be an ARM64 Android shared library.")
    android_api = re.search(r"NT_ANDROID_TYPE_IDENT\s*\n\s*description data: ((?:[0-9a-f]{2} ){3}[0-9a-f]{2})",
                            report)
    if android_api is None or not 21 <= int.from_bytes(bytes.fromhex(android_api[1]), "little") <= 28:
        raise ValueError("The runner must target Android API 28 or earlier.")
    loads = [line.split() for line in report.splitlines() if line.strip().startswith("LOAD ")]
    if not loads or any(int(load[-1], 16) < 0x4000 or
                        (int(load[1], 16) - int(load[2], 16)) % 0x4000 for load in loads):
        raise ValueError("The runner must support Android devices with 16 KiB memory pages.")
    soname = re.findall(r"\(SONAME\).*?\[([^\]]+)\]", report)
    if soname != [RUNNER] or re.search(r"\((?:RPATH|RUNPATH)\)", report):
        raise ValueError(f"Expected an Android library with SONAME {RUNNER} and no runtime search path.")
    needed = set(re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", report))
    if not needed or needed - ANDROID_LIBRARIES:
        raise ValueError("The runner must use only system shared libraries. "
                         f"Unexpected dependencies: {', '.join(sorted(needed - ANDROID_LIBRARIES))}")
    exports = set(re.findall(r"^\s*\d+:.*?\bFUNC\s+(?:GLOBAL|WEAK)\s+DEFAULT\s+\d+\s+(\w+)",
                             report, re.M))
    if RUNNER_EXPORTS - exports:
        raise ValueError(f"The runner is missing exports: {', '.join(sorted(RUNNER_EXPORTS - exports))}")


def preparation_inputs(recompiler: Path, captures: Path):
    dumps = [captures / f"{module.lower()}.bin" for module in MODULES]
    if not all(dump.is_file() and dump.stat().st_size == RAM_SIZE for dump in dumps):
        return None
    helpers = sorted((ROOT / "tools").glob("*.py"))
    helpers.extend(ROOT / path for path in (
        "game/scus.toml", "tools/android/prepare_game.py", "tools/desktop/disc.py",
        "tools/desktop/import_disc.py", "tools/desktop/pipeline.py",
    ))
    return {
        "recompiler": sha256(recompiler),
        "captures": {dump.name: sha256(dump) for dump in dumps},
        "helpers": {path.relative_to(ROOT).as_posix(): sha256(path) for path in helpers},
    }


def generated_sources(args, cmake: Path):
    captures = args.captures_dir or args.work / "captures"
    inputs = preparation_inputs(args.recompiler, captures)
    if args.captures_dir and inputs is None:
        raise ValueError("--captures-dir must contain kernel.bin, manager.bin and gamecore.bin (32 MiB each).")
    generated = args.work / "gamecore" / "output"
    stamp = generated / "play-bundle-inputs.json"
    expected_stamp = json.dumps(inputs, sort_keys=True) if inputs is not None else None
    required = ("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h", "register_functions.cpp",
                "prepare-manifest.json")
    if expected_stamp and stamp.is_file() and stamp.read_text(encoding="utf-8") == expected_stamp and all(
        (generated / name).is_file() for name in required
    ):
        print(f"Reusing generated sources in {generated}", flush=True)
        return generated
    print("Generating ARM64 build inputs from the local disc", flush=True)
    host_args = argparse.Namespace(**vars(args))
    host_args.cmake = str(cmake)
    host_args.captures_dir = captures if inputs is not None else None
    output = prepare(host_args)
    inputs = inputs or preparation_inputs(args.recompiler, captures)
    manifest = json.loads((output / "prepare-manifest.json").read_text(encoding="utf-8"))
    if (inputs is None or manifest["recompiler_sha256"] != inputs["recompiler"] or
            any(manifest["capture_sha256"][module] != inputs["captures"][f"{module.lower()}.bin"]
                for module in MODULES)):
        raise ValueError("The private generation inputs changed during preparation. Retry the command.")
    (output / stamp.name).write_text(json.dumps(inputs, sort_keys=True), encoding="utf-8")
    return output


def build_runner(args, sdk: Path, ndk: Path):
    bin_dir = sdk / "cmake" / CMAKE_VERSION / "bin"
    cmake, ninja = (bin_dir / (name + EXE) for name in ("cmake", "ninja"))
    toolchain = ndk / "build" / "cmake" / "android.toolchain.cmake"
    if not all(path.is_file() for path in (cmake, ninja, toolchain)):
        raise ValueError(f"Install Android SDK CMake {CMAKE_VERSION} and NDK {NDK_VERSION}.")
    if not (args.ffmpeg_root / "include" / "libavcodec" / "avcodec.h").is_file() or not all(
        (args.ffmpeg_root / "lib" / f"lib{component}.a").is_file()
        for component in ("avcodec", "avutil", "swscale")
    ):
        raise ValueError("--ffmpeg-root must be the ARM64 static FFmpeg install from build_ffmpeg.sh.")
    # The earlier host capture stages also use Ninja. Prefer the installed SDK copy.
    os.environ["PATH"] = str(bin_dir) + os.pathsep + os.environ.get("PATH", "")
    generated = generated_sources(args, cmake)
    build = args.work / "android-runtime"
    runner_dirs = f"{generated.as_posix()};{(ROOT / 'game' / 'overrides').as_posix()}"
    print(f"Building Android ARM64 runner in {build}", flush=True)
    run([cmake, "-S", ROOT / "PS2Recomp", "-B", build, "-G", "Ninja",
         *args.android_cmake_arg, f"-DCMAKE_MAKE_PROGRAM={ninja.as_posix()}",
         f"-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}", "-DCMAKE_BUILD_TYPE=Release",
         "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-28", "-DANDROID_STL=c++_static",
         "-DPS2X_BUILD_RUNTIME=ON", "-DSOTC_HAS_GAME=ON", "-DSOTC_ANDROID_CAPTURE_MODULES=OFF",
         "-DPS2X_ENABLE_DEBUG_UI=OFF", "-DPS2X_ENABLE_SCCACHE=OFF", "-DPS2X_ENABLE_CCACHE=OFF",
         "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF", "-DPS2X_ENABLE_IOP_RPC_TRACE=OFF",
         "-DPS2X_ENABLE_FFMPEG=ON", f"-DPS2X_ANDROID_FFMPEG_ROOT={args.ffmpeg_root.as_posix()}",
         "-DPS2X_RUNNER_UNITY_BUILD_BATCH_SIZE=8", f"-DPS2X_RUNNER_DIRS={runner_dirs}"],
        args.work / "android-configure.log", args.work)
    run([cmake, "--build", build, "--target", "ps2EntryRunner", "--parallel", str(args.jobs)],
        args.work / "android-build.log", args.work)
    return build / "ps2xRuntime" / RUNNER


def write_bundle(runner: Path, output: Path, readelf: Path):
    if not runner.is_file() or not 0 < runner.stat().st_size <= MAX_RUNNER_BYTES:
        raise ValueError("The runner must be a nonempty Android library up to 256 MiB.")
    with tempfile.TemporaryDirectory(prefix=".sotcplay-", dir=output.parent) as path:
        staging = Path(path)
        snapshot = staging / RUNNER
        shutil.copyfile(runner, snapshot)
        validate_runner(snapshot, readelf)
        with zipfile.ZipFile(staging / BUNDLE, "w", zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
            digest = hashlib.sha256()
            runner_bytes = 0
            with snapshot.open("rb") as source, archive.open(RUNNER, "w") as target:
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    target.write(block)
                    digest.update(block)
                    runner_bytes += len(block)
            manifest = {
                "format": 1, "serial": "SCUS-97472", "bootSha256": EXPECTED[BOOT][1],
                "runnerSha256": digest.hexdigest(), "runnerBytes": runner_bytes,
            }
            archive.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        (staging / BUNDLE).replace(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True, help="owned SCUS-97472 DVD ISO")
    parser.add_argument("--work", type=Path, required=True, help=f"private work directory for {BUNDLE}")
    parser.add_argument("--sdk", type=Path, default=os.environ.get("ANDROID_HOME") or
                        os.environ.get("ANDROID_SDK_ROOT"), help="Android SDK path (default: ANDROID_HOME)")
    parser.add_argument("--runner", type=Path, help="reuse a compatible, locally built ARM64 game library")
    parser.add_argument("--recompiler", type=Path, help="ps2_recomp from the desktop compiler kit")
    parser.add_argument("--ffmpeg-root", type=Path, help="ARM64 static FFmpeg install from build_ffmpeg.sh")
    parser.add_argument("--captures-dir", type=Path, help="reuse the three private 32 MiB module captures")
    parser.add_argument("--host-cmake-arg", action="append", default=[], help="extra host capture CMake option")
    parser.add_argument("--android-cmake-arg", action="append", default=[], help="extra Android CMake option")
    parser.add_argument("--jobs", type=int, choices=(1, 2), default=2, help="parallel build jobs (default: 2)")
    args = parser.parse_args()
    for name in ("iso", "work", "sdk", "runner", "recompiler", "ffmpeg_root", "captures_dir"):
        path = getattr(args, name)
        if path is not None:
            setattr(args, name, path.resolve())
    if args.work.is_relative_to(ROOT) or any(char in str(args.work) for char in "@;"):
        parser.error("--work must be outside the source checkout and its path cannot contain @ or ;")
    if not args.iso.is_file() or args.iso.suffix.lower() != ".iso":
        parser.error("--iso must name an existing .iso file")
    if args.sdk is None:
        parser.error("set ANDROID_HOME or pass --sdk")
    if args.runner is None and (args.recompiler is None or not args.recompiler.is_file() or args.ffmpeg_root is None):
        parser.error("pass --recompiler and --ffmpeg-root to build the runner, or --runner to reuse one")
    ndk = args.sdk / "ndk" / NDK_VERSION
    readelf = next(ndk.glob(f"toolchains/llvm/prebuilt/*/bin/llvm-readelf{EXE}"), None)
    if readelf is None:
        parser.error(f"install Android NDK {NDK_VERSION} in the selected SDK")
    try:
        inspect_disc(args.iso)
        with lock_work_directory(args.work):
            runner = args.runner or build_runner(args, args.sdk, ndk)
            output = args.work / BUNDLE
            write_bundle(runner, output, readelf)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Prepared private Android play bundle: {output}")


if __name__ == "__main__":
    main()
