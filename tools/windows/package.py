"""Create the relocatable Windows compiler kit and native launcher ZIP."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
PRODUCT = "ShadowOfTheColossusPC"
PLATFORM = "windows-x86_64"
UCRT_PACKAGE = "mingw-w64-ucrt-x86_64-"
COMPILER_PACKAGES = ("gcc", "crt", "headers", "binutils", "winpthreads", "windows-default-manifest")
sys.path[:0] = [str(ROOT / "tools" / part) for part in ("desktop", "linux")]
from compiler_manifest import enable_utf8
from model import VERSION
from stage_source import stage_source


def copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def run(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True, encoding="utf-8")


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def exactly_one(directory, name):
    matches = list(directory.rglob(name))
    if len(matches) != 1:
        raise ValueError(f"Expected one {name} in {directory}, found {matches}")
    return matches[0]


def compiler_files(prefix):
    files = run("pacman", "-Qql", *(UCRT_PACKAGE + package for package in COMPILER_PACKAGES))
    tools = {"g++.exe", "as.exe", "ld.exe", "windres.exe"}
    for filename in files.splitlines():
        if not filename.startswith("/ucrt64/") or filename.endswith("/"):
            continue
        relative = Path(filename.removeprefix("/ucrt64/"))
        if (relative.parts[0] == "include"
            or relative.parts[:2] in (("lib", "gcc"), ("x86_64-w64-mingw32", "include"),
                                      ("x86_64-w64-mingw32", "lib"))
            or (relative.parts[0] == "lib" and relative.suffix in (".o", ".a"))
            or (relative.parent == Path("bin") and relative.name in tools)):
            source = prefix / relative
            if source.is_file():
                yield source


def dependency_closure(root, prefix):
    """Inspect native PE imports. Dependencies outside UCRT64 are system DLLs."""
    bin_dir = prefix / "bin"
    dlls = {path.name.casefold(): path for path in bin_dir.glob("*.dll")}
    pending = [path for path in root.rglob("*") if path.suffix.lower() in (".exe", ".dll", ".pyd")]
    processed = set()
    objdump = bin_dir / "objdump.exe"
    while pending:
        binary = pending.pop()
        identity = binary.resolve()
        if identity in processed:
            continue
        processed.add(identity)
        imports = re.findall(r"DLL Name: (\S+)", run(objdump, "-p", binary))
        for name in imports:
            source = dlls.get(name.casefold())
            if not source:
                if name.casefold().startswith(("msys-", "cygwin")):
                    raise ValueError(f"{binary} imports unexpected POSIX DLL {name}")
                continue
            destination = root / "compiler/toolchain/bin" / source.name
            if not destination.exists():
                copy(source, destination)
                pending.append(destination)


def stage_launcher(root, build):
    copy(exactly_one(build, f"{PRODUCT}.exe"), root / f"{PRODUCT}.exe")
    shutil.copytree(ROOT / "tools/desktop", root / "desktop",
                    ignore=shutil.ignore_patterns("release", "__pycache__", "*.pyc"))
    copy(ROOT / "tools/windows/windows_bootstrap.py", root / "desktop/windows_bootstrap.py")
    copy(ROOT / "tools/windows/LEIA-ME.txt", root / "LEIA-ME.txt")
    copy(ROOT / "LICENSE", root / "LICENSE")
    copy(ROOT / "recomplauncher.json", root / "recomplauncher.json")
    controller_db = ROOT / "PS2Recomp/ps2xRuntime/third_party/gamecontrollerdb"
    copy(controller_db / "gamecontrollerdb.txt", root / "desktop/assets/gamecontrollerdb.txt")


def reuse_compiler(root, archive, expected_hash, expected_build_id):
    if sha256(archive) != expected_hash:
        raise ValueError(f"Previous Windows package SHA-256 mismatch: {archive}")
    with zipfile.ZipFile(archive) as source:
        release = json.loads(source.read(f"{PRODUCT}/release.json"))
        manifest = json.loads(source.read(f"{PRODUCT}/compiler/manifest.json"))
        if release["platform"] != PLATFORM or manifest["build_id"] != expected_build_id:
            raise ValueError("Previous Windows package platform or compiler build_id mismatch")
        files = [info for info in source.infolist() if info.filename.startswith(f"{PRODUCT}/compiler/")]
        source.extractall(root.parent, files)
    print(f"Reused compiler build_id {expected_build_id} from {archive.name}", flush=True)


def package(build, prefix, destination, previous=None):
    destination.mkdir(parents=True, exist_ok=True)
    root = destination / PRODUCT
    root.mkdir()
    stage_launcher(root, build)
    if previous:
        reuse_compiler(root, *previous)
    else:
        stage_compiler(build, prefix, root)
    write_archive(root)


def stage_compiler(build, prefix, root):
    compiler = root / "compiler"
    compiler.mkdir()
    kit = compiler / "toolchain"
    kit.mkdir()
    stage_source(ROOT, build, compiler)
    runner = compiler / "source/PS2Recomp/ps2xRuntime/src/runner"
    extra = [path for path in runner.iterdir() if path.name != "register_functions.cpp"]
    if extra:
        raise ValueError(f"Generated runner files must be outside the source tree: {extra}")

    for executable in ("ps2_recomp", "sotc-video-tool", "sotc-input-tool"):
        copy(exactly_one(build, executable + ".exe"), compiler / "bin" / (executable + ".exe"))
    link_inputs = []
    for library in ("libsotc-windows-base.a", "libps2_runtime.a", "libps2_iop.a", "libraylib.a"):
        relative = "lib/" + library
        copy(exactly_one(build, library), compiler / relative)
        link_inputs.append(relative)
    for library in ("libavcodec", "libavformat", "libavutil", "libswresample", "libswscale"):
        relative = "lib/" + library + ".dll.a"
        copy(prefix / relative, compiler / relative)
        link_inputs.append(relative)
        dlls = list((prefix / "bin").glob(library.removeprefix("lib") + "-*.dll"))
        if not dlls:
            dlls = list((prefix / "bin").glob(library + "-*.dll"))
        if len(dlls) != 1:
            raise ValueError(f"Expected one {library} DLL, found {dlls}")
        copy(dlls[0], kit / "bin" / dlls[0].name)

    subprocess.run([str(prefix / "bin/windres.exe"), "-I", str(ROOT / "tools/windows"),
                    str(ROOT / "tools/windows/utf8.rc"), str(compiler / "lib/utf8.o")], check=True)
    for source in compiler_files(prefix):
        copy(source, kit / source.relative_to(prefix))
    for name in ("bin/g++.exe", "bin/as.exe", "bin/ld.exe", "bin/windres.exe",
                 "include/stdint.h", "lib/crt2.o", "lib/libstdc++.a"):
        if not (kit / name).is_file():
            raise FileNotFoundError(f"Toolchain file {name} was not staged")
    # The GCC driver emits UTF-8 arguments. Its assembler and linker helpers
    # must use the same code page when Windows turns those arguments into paths.
    tools = [kit / "bin" / name for name in ("g++.exe", "as.exe", "ld.exe")]
    tools.extend(exactly_one(kit / "lib/gcc", name) for name in ("cc1plus.exe", "collect2.exe"))
    for executable in tools:
        try:
            enable_utf8(executable)
        except Exception as error:
            raise RuntimeError(f"Could not configure the UTF-8 manifest in {executable}") from error
    for name in ("SDL2.dll", "python.exe"):
        copy(prefix / "bin" / name, kit / "bin" / name)
    # sdl2-compat can load SDL3 by name at runtime instead of importing it.
    if (prefix / "bin/SDL3.dll").is_file():
        copy(prefix / "bin/SDL3.dll", kit / "bin/SDL3.dll")
    archives = list((prefix / "bin").glob("libarchive-*.dll"))
    if len(archives) != 1:
        raise ValueError(f"Expected one libarchive DLL, found {archives}")
    copy(archives[0], kit / "bin" / archives[0].name)

    # Preserve MSYS2's native Python layout, including its C extensions.
    py_lib = Path(sys.base_prefix) / "lib" / f"python{sys.version_info.major}.{sys.version_info.minor}"
    shutil.copytree(py_lib, kit / "lib" / py_lib.name,
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "test", "tests", "ensurepip"))
    for sub in ("lib/girepository-1.0", "share/glib-2.0/schemas"):
        shutil.copytree(prefix / sub, kit / sub)
    for library in ("libgtk-3-0.dll", "libgdk-3-0.dll", "libgdk_pixbuf-2.0-0.dll",
                    "libgirepository-1.0-1.dll", "libgirepository-2.0-0.dll",
                    "libpango-1.0-0.dll", "libpangocairo-1.0-0.dll"):
        # Typelibs open these by name even when no .pyd declares the dependency.
        source = prefix / "bin" / library
        if source.is_file():
            copy(source, kit / "bin" / library)

    controller_db = ROOT / "PS2Recomp/ps2xRuntime/third_party/gamecontrollerdb"
    copy(controller_db / "LICENSE", compiler / "licenses/SDL_GameControllerDB.LICENSE")
    fsr_license = ROOT / "PS2Recomp/ps2xRuntime/third_party/amd-fsr/LICENSE"
    if fsr_license.is_file():
        copy(fsr_license, compiler / "licenses/AMD-FSR.LICENSE")
    dependency_closure(root, prefix)

    licenses = compiler / "licenses"
    upstream = {}
    upstream_dir = compiler / "upstream"
    upstream_dir.mkdir()
    for source in sorted((build / "_deps").glob("*-src")):
        if not (source / ".git").is_dir():
            continue
        name = source.name.removesuffix("-src")
        revision = run("git", "-C", source, "rev-parse", "HEAD").strip()
        repository = run("git", "-C", source, "remote", "get-url", "origin").strip()
        upstream[name] = {"repository": repository, "commit": revision}
        selected = ["src", "cmake", "LICENSE", "CMakeLists.txt"] if name == "raylib" else []
        subprocess.run(["git", "-C", str(source), "archive", "--format=tar.gz",
                        "--output", str(upstream_dir / (name + ".tar.gz")),
                        "--prefix=" + name + "/", revision, *selected], check=True)
        for path in source.iterdir():
            if path.is_file() and path.name.upper().startswith(("LICENSE", "LICENCE", "COPYING", "COPYRIGHT")):
                copy(path, licenses / name / path.name)
    (upstream_dir / "sources.json").write_text(json.dumps(upstream, indent=2) + "\n", encoding="utf-8")
    shutil.copytree(prefix / "share/licenses", licenses / "MSYS2")
    packages = run("pacman", "-Q").splitlines()
    (compiler / "packages.txt").write_text(
        "\n".join(f"{line} https://packages.msys2.org/packages/{line.split()[0]}"
                  for line in packages if line.startswith(UCRT_PACKAGE)) + "\n",
        encoding="utf-8",
    )

    # Launcher/python/GTK updates need not invalidate the recompiled game.
    versions = run("pacman", "-Q", *(UCRT_PACKAGE + name for name in (*COMPILER_PACKAGES, "ffmpeg")))
    fingerprint = hashlib.sha256(versions.encode("utf-8"))
    helpers = ("tools/desktop/pipeline.py", "tools/build_elf.py", "tools/elfmini.py",
               "tools/funcmap_elf.py", "tools/gen_config.py", "tools/xff.py", "tools/modimage.py",
               "game/scus.toml", "game/sces.toml",
               "tools/windows/compiler_manifest.py", "tools/windows/utf8.manifest")
    for relative in ("bin/ps2_recomp.exe", "lib/utf8.o", *link_inputs,
                     *("source/" + helper for helper in helpers)):
        fingerprint.update(bytes.fromhex(sha256(compiler / relative)))
    (compiler / "manifest.json").write_text(json.dumps({
        "build_id": fingerprint.hexdigest()[:24], "link_inputs": link_inputs,
    }) + "\n", encoding="utf-8")


def write_archive(root):
    (root / "release.json").write_text(json.dumps({
        "version": VERSION, "platform": PLATFORM,
    }) + "\n", encoding="utf-8")
    destination = root.parent
    archive = destination / f"{PRODUCT}-{VERSION}-{PLATFORM}.zip"
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=5, strict_timestamps=False) as output:
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            if (path.suffix.casefold() in {".iso", ".7z", ".rar", ".xff", ".elf", ".ps2", ".sotcstate"}
                or path.name.casefold().rstrip(".") in {
                    "nico.dat", "scus_974.72", "sces_533.26", "xab", "xac", "xad", "xae", "complete.json"
                }
                or path.name.startswith(("func_", "vu1rec_"))):
                raise ValueError(f"Unexpected game-derived file in package: {path}")
            output.write(path, path.relative_to(destination).as_posix())
    (archive.with_suffix(".zip.sha256")).write_text(f"{sha256(archive)}  {archive.name}\n", encoding="ascii")
    print(archive, flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--reuse-compiler", type=Path)
    parser.add_argument("--compiler-sha256")
    parser.add_argument("--compiler-build-id")
    args = parser.parse_args()
    previous = (args.reuse_compiler, args.compiler_sha256, args.compiler_build_id)
    if any(previous) and not all(previous):
        parser.error("--reuse-compiler, --compiler-sha256 and --compiler-build-id must be supplied together")
    package(args.build.resolve(), args.prefix.resolve(), args.destination.resolve(), previous if all(previous) else None)
