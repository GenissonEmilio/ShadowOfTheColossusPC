#!/usr/bin/env python3
"""Stage the ROM-free toolkit for -PsotcOnDeviceKit=<output>."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import shutil
import struct
import tarfile
import urllib.request
import zipfile

REPO = Path(__file__).resolve().parents[2]
LLVM_PATH = "android-ndk-r28c/toolchains/llvm/prebuilt/linux-arm64"
LLVM = (
    "https://github.com/Apex-Studio-Dev/apex-toolchains/releases/download/ndk-r28c/"
    "custom-android-ndk-r28c-aarch64-linux-android.tar.xz",
    "b3e465fa6ee8721deadb07f0765f94086bded46ed2e4a813f6a63b3ddc383fee",
)
PYTHON = (
    "https://www.python.org/ftp/python/3.14.10/python-3.14.10-aarch64-linux-android.tar.gz",
    "423c80b957330cf0ebb92b9b20f2294c292144a441bdb0ceb8c69d14928e569b",
)
PYTHON_PATH = "./prefix/lib/python3.14"
EXECUTABLES = ("libSotcRecompile.so", "libSotcPrepare.so", "libSotcClang.so", "libSotcLld.so")
PYTHON_MODULES = ("_struct", "_bisect", "_json", "math")
PYTHON_LIBRARIES = ("libpython3.14.so", *(f"libpy_{module}.so" for module in PYTHON_MODULES))
ANDROID_LIBS = (
    "libandroid.so", "libc.so", "libc++.a", "libdl.so", "liblog.so", "libm.so", "libEGL.so",
    "libGLESv2.so", "libGLESv3.so", "libOpenSLES.so", "libz.so",
)
TOOLS = ("build_elf.py", "elfmini.py", "funcmap_elf.py", "gen_config.py", "modimage.py", "xff.py", "stub_deny.txt")
RECOMPILER_NOTICES = {
    "elfio": "LICENSE.txt",
    "fmt": "LICENSE",
    "libdwarf": "COPYING",
    "rabbitizer": "LICENSE",
    "toml11": "LICENSE",
}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def download(directory, name, pin):
    url, expected = pin
    target = directory / name
    if not target.exists():
        temporary = target.with_suffix(".part")
        try:
            with urllib.request.urlopen(url, timeout=120) as stream, temporary.open("wb") as output:
                shutil.copyfileobj(stream, output)
            if sha256(temporary) != expected:
                raise ValueError(f"Download SHA-256 mismatch: {url}")
            temporary.replace(target)
        finally:
            temporary.unlink(missing_ok=True)
    if sha256(target) != expected:
        raise ValueError(f"Cached SHA-256 mismatch: {target}")
    return target


def native(path, executable=False):
    """Reject non-Android ELF binaries, chunks misaligned for 16 KiB pages or ET_EXEC."""
    with path.open("rb") as stream:
        header = stream.read(64)
        if header[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<HH", header, 16) != (3, 183):
            raise ValueError(f"Expected an ARM64 PIE or shared library: {path}")
        phoff, phsize, count = struct.unpack_from("<Q14xHH", header, 32)
        stream.seek(phoff)
        has_interp = False
        has_load = False
        for _ in range(count):
            kind, _, offset, address, _, _, _, align = struct.unpack("<IIQQQQQQ", stream.read(phsize))
            if kind == 1:
                has_load = True
                if align < 16384 or (address - offset) % 16384 != 0:
                    raise ValueError(f"ELF segment lacks 16 KiB alignment: {path}")
            elif kind == 3:
                cursor = stream.tell()
                stream.seek(offset)
                if stream.read(len(b"/system/bin/linker64\0")) != b"/system/bin/linker64\0":
                    raise ValueError(f"Wrong Android program interpreter: {path}")
                has_interp = True
                stream.seek(cursor)
        if not has_load or executable != has_interp:
            raise ValueError(f"Invalid Android {'executable' if executable else 'library'}: {path}")


def tar_copy(archive, name, target):
    with archive.extractfile(name) as stream, target.open("wb") as output:
        shutil.copyfileobj(stream, output)


def zip_file(zipper, source, destination):
    # A stable timestamp lets identical kits reuse the on-device build.
    item = zipfile.ZipInfo(str(destination).replace("\\", "/"), date_time=(2026, 1, 1, 0, 0, 0))
    item.compress_type = zipfile.ZIP_DEFLATED
    with source.open("rb") as stream, zipper.open(item, "w") as output:
        shutil.copyfileobj(stream, output)


def zip_tree(zipper, source, destination):
    for file in sorted(source.rglob("*")):
        if file.is_file():
            zip_file(zipper, file, destination / file.relative_to(source))


def copy_recompiler_notices(build, destination):
    cache = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("FETCHCONTENT_") and "=" in line:
            property_name, value = line.split("=", 1)
            cache[property_name.split(":", 1)[0]] = value
    deps = Path(cache.get("FETCHCONTENT_BASE_DIR") or build / "_deps")
    for name, notice in RECOMPILER_NOTICES.items():
        source = Path(cache.get(f"FETCHCONTENT_SOURCE_DIR_{name.upper()}") or deps / f"{name}-src")
        shutil.copyfile(source / notice, destination / f"Sotc-Recompiler-{name}.txt")


def stage(args):
    host = args.ndk / "toolchains/llvm/prebuilt"
    llvm, = [directory for directory in host.iterdir() if (directory / "sysroot").is_dir()]
    sysroot = llvm / "sysroot"
    abi_libs = sysroot / "usr/lib/aarch64-linux-android"
    resource = llvm / "lib/clang/19"
    if "28.2.13676358" not in (args.ndk / "source.properties").read_text():
        raise ValueError("Use Android NDK 28.2.13676358")

    args.downloads.mkdir(parents=True, exist_ok=True)
    llvm_archive = download(args.downloads, "clang.tar.xz", LLVM)
    python_archive = download(args.downloads, "python.tar.gz", PYTHON)
    destination = args.output
    destination.mkdir(parents=True, exist_ok=True)
    jni = destination / "jniLibs/arm64-v8a"
    assets = destination / "assets"
    licenses = assets / "licenses"
    jni.mkdir(parents=True, exist_ok=True)
    licenses.mkdir(parents=True, exist_ok=True)
    identity = assets / "sotc-ondevice.id"
    identity.unlink(missing_ok=True)

    with tarfile.open(llvm_archive) as archive:
        for source, target in (("clang-19", "libSotcClang.so"), ("lld", "libSotcLld.so")):
            tar_copy(archive, f"{LLVM_PATH}/bin/{source}", jni / target)
        tar_copy(archive, f"{LLVM_PATH}/NOTICE", licenses / "Sotc-LLVM-NOTICE.txt")

    for tool in ("libSotcRecompile.so", "libSotcPrepare.so"):
        shutil.copyfile(args.runtime_stage / "tools" / tool, jni / tool)
    for name in EXECUTABLES:
        native(jni / name, executable=True)

    with tarfile.open(python_archive) as package, zipfile.ZipFile(assets / "sotc-ondevice.zip", "w") as zipper:
        tar_copy(package, "./prefix/lib/libpython3.14.so", jni / "libpython3.14.so")
        tar_copy(package, f"{PYTHON_PATH}/LICENSE.txt", licenses / "Sotc-Python-PSF.txt")
        for module in PYTHON_MODULES:
            tar_copy(package, f"{PYTHON_PATH}/lib-dynload/{module}.cpython-314-aarch64-linux-android.so",
                     jni / f"libpy_{module}.so")
        for name in PYTHON_LIBRARIES:
            native(jni / name)

        for member in package:
            relative = member.name.removeprefix(PYTHON_PATH + "/")
            if relative == member.name or not relative.endswith(".py") or relative.startswith(("test/", "site-packages/")):
                continue
            item = zipfile.ZipInfo("python/lib/python3.14/" + relative, date_time=(2026, 1, 1, 0, 0, 0))
            item.compress_type = zipfile.ZIP_DEFLATED
            with package.extractfile(member) as stream:
                zipper.writestr(item, stream.read())

        zip_tree(zipper, args.runtime_stage / "lib", Path("lib"))
        zip_tree(zipper, args.runtime_stage / "include", Path("include"))
        kernel_headers = REPO / "PS2Recomp/ps2xRuntime/src/lib/Kernel"
        for header in sorted(kernel_headers.rglob("*")):
            if header.is_file() and header.suffix in (".h", ".hpp", ".inc"):
                zip_file(zipper, header, Path("include") / header.relative_to(kernel_headers))
        zip_tree(zipper, sysroot / "usr/include", Path("sysroot/usr/include"))
        zip_tree(zipper, resource / "include", Path("resource/include"))
        for name in (*ANDROID_LIBS, "crtbegin_so.o", "crtend_so.o"):
            zip_file(zipper, abi_libs / "28" / name, Path(f"sysroot/usr/lib/aarch64-linux-android/28/{name}"))
        for name in ("libc++_static.a", "libc++abi.a"):
            zip_file(zipper, abi_libs / name, Path(f"sysroot/usr/lib/aarch64-linux-android/{name}"))
        builtins = "libclang_rt.builtins-aarch64-android.a"
        zip_file(zipper, resource / "lib/linux" / builtins, Path("lib") / builtins)
        for name in ("libatomic.a", "libunwind.a"):
            zip_file(zipper, resource / "lib/linux/aarch64" / name, Path("lib") / name)

        for name in TOOLS:
            file = REPO / "tools" / name
            if file.exists():
                zip_file(zipper, file, Path("tools") / name)
        zip_file(zipper, REPO / "tools/android/kit/prepare_stage.py", Path("tools/prepare_stage.py"))
        for name in ("scus.toml", "extra_stubs.txt"):
            zip_file(zipper, REPO / "game" / name, Path("game") / name)
        runtime = Path("PS2Recomp/ps2xRuntime")
        calls = runtime / "include/ps2_call_list.h"
        zip_file(zipper, REPO / calls, calls)
        # gen_config scans TODO_NAMED declarations to exclude unimplemented stubs.
        for implementation in sorted((REPO / runtime / "src/lib").rglob("*.cpp")):
            if b"TODO_NAMED(" in implementation.read_bytes():
                zip_file(zipper, implementation, implementation.relative_to(REPO))

    for license_file in (args.runtime_stage / "licenses").iterdir():
        shutil.copyfile(license_file, licenses / f"Sotc-kit-{license_file.name}")
    copy_recompiler_notices(args.runtime_stage.parent.parent, licenses)
    shutil.copyfile(sysroot / "NOTICE", licenses / "Sotc-NDK-sysroot-NOTICE.txt")
    (licenses / "Sotc-toolkit-sources.txt").write_text(
        f"LLVM r28c ARM64 Android, Apache-2.0 with LLVM Exceptions and bundled notices\n{LLVM[0]}\n"
        "LLVM build scripts: https://github.com/Apex-Studio-Dev/apex-toolchains/tree/ndk-r28c\n"
        "Android NDK r28c sysroot: https://github.com/android/ndk/releases/tag/r28c\n"
        f"CPython 3.14.10 ARM64 Android, Python Software Foundation License\n{PYTHON[0]}\n"
        "CPython source: https://www.python.org/downloads/release/python-31410/\n",
        encoding="utf-8",
    )
    digest = hashlib.sha256()
    for source in sorted((*jni.glob("*.so"), assets / "sotc-ondevice.zip")):
        digest.update(bytes.fromhex(sha256(source)))
    logic = REPO / "PS2Recomp/android/app/src/main/java/com/genisson/sotc/OnDevicePreparation.java"
    digest.update(bytes.fromhex(sha256(logic)))
    identity.write_bytes((digest.hexdigest() + "\n").encode("ascii"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("ndk", "runtime-stage", "downloads", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    args = parser.parse_args()
    stage(args)
    print(args.output)


if __name__ == "__main__":
    main()
