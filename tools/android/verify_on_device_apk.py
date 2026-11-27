#!/usr/bin/env python3
"""Verify the native files and ROM-free preparation kit in a portable APK."""

import argparse
import re
from pathlib import Path, PurePosixPath
import shutil
import struct
import sys
import tempfile
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from stage_on_device_kit import ANDROID_LIBS, EXECUTABLES, PYTHON_LIBRARIES, TOOLS, native


RUNNER_ARCHIVES = {
    "libbase.a", "libps2_runtime.a", "libps2_iop.a", "libraylib.a", "libavcodec.a",
    "libavutil.a", "libswscale.a", "libclang_rt.builtins-aarch64-android.a", "libatomic.a", "libunwind.a",
}
APK_LIBRARIES = {"libSotcBootstrap.so", *EXECUTABLES, *PYTHON_LIBRARIES}
DISC_FILES = {"scus_974.72", "nico.dat", "kernel.xff", "manager.xff", "gamecore.xff"}
FORBIDDEN_EXTENSIONS = (".iso", ".xff", ".elf", ".sotcplay")
REQUIRED_KIT_ENTRIES = {
    "tools/prepare_stage.py", "game/scus.toml", "game/extra_stubs.txt",
    "python/lib/python3.14/encodings/__init__.py", "include/ps2_runtime.h",
    "include/Stubs/Unimplemented.h", "include/sse2neon.h",
    "PS2Recomp/ps2xRuntime/include/ps2_call_list.h",
    *(f"tools/{tool}" for tool in TOOLS if tool.endswith(".py")),
    *(f"lib/{archive}" for archive in RUNNER_ARCHIVES),
    *(f"sysroot/usr/lib/aarch64-linux-android/28/{library}"
      for library in (*ANDROID_LIBS, "crtbegin_so.o", "crtend_so.o")),
    "sysroot/usr/lib/aarch64-linux-android/libc++_static.a",
    "sysroot/usr/lib/aarch64-linux-android/libc++abi.a",
}


def checked_names(archive):
    names = archive.namelist()
    if len(names) != len(set(names)):
        raise ValueError("Duplicate archive entries")
    for name in names:
        path = PurePosixPath(name)
        if "\\" in name or path.is_absolute() or ".." in path.parts:
            raise ValueError(f"Unsafe archive path: {name}")
        item = path.name.casefold()
        if item.endswith(FORBIDDEN_EXTENSIONS) or item in DISC_FILES:
            raise ValueError(f"Found a game disc or runner input: {name}")
    return {name for name in names if not name.endswith("/")}


def verify_kit(archive):
    entries = checked_names(archive)
    if REQUIRED_KIT_ENTRIES - entries:
        raise ValueError(f"Incomplete preparation kit: {sorted(REQUIRED_KIT_ENTRIES - entries)}")
    headers = ("include/", "resource/include/", "sysroot/usr/include/")
    for name in sorted(entries - REQUIRED_KIT_ENTRIES):
        if name == "tools/stub_deny.txt":
            continue
        if name.startswith(headers) and not name.endswith((".cpp", ".so", ".o", ".a", ".bin", ".dat", ".zip")):
            continue
        if name.startswith("python/lib/python3.14/") and name.endswith(".py"):
            continue
        if name.startswith("PS2Recomp/ps2xRuntime/src/lib/Kernel/") and name.endswith(".cpp"):
            if b"TODO_NAMED(" in archive.read(name):
                continue
        raise ValueError(f"Unexpected preparation kit entry: {name}")


def verify_apk(apk):
    with zipfile.ZipFile(apk) as archive:
        entries = checked_names(archive)
        expected_native = {f"lib/arm64-v8a/{name}" for name in APK_LIBRARIES}
        if {name for name in entries if name.startswith("lib/")} != expected_native:
            raise ValueError("APK must contain exactly the ARM64 bootstrap and preparation tools")
        identity = archive.read("assets/sotc-ondevice.id")
        if not re.fullmatch(b"[0-9a-f]{64}\\n", identity):
            raise ValueError("Invalid preparation kit version")
        with tempfile.TemporaryDirectory(prefix="sotc-apk-") as scratch:
            root = Path(scratch)
            for name in sorted(APK_LIBRARIES):
                target = root / name
                with archive.open(f"lib/arm64-v8a/{name}") as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output)
                native(target, executable=name in EXECUTABLES)
            target = root / "kit.zip"
            with archive.open("assets/sotc-ondevice.zip") as source, target.open("wb") as output:
                shutil.copyfileobj(source, output)
            with zipfile.ZipFile(target) as kit:
                verify_kit(kit)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    args = parser.parse_args()
    try:
        verify_apk(args.apk)
    except (OSError, ValueError, KeyError, struct.error, zipfile.BadZipFile) as error:
        parser.exit(1, f"APK verification failed: {error}\n")
    print("ARM64 tools, 16 KiB ELF alignment and ROM-free preparation kit verified.")


if __name__ == "__main__":
    main()
