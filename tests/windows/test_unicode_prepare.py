#!/usr/bin/env python3
"""Run the Windows preparation toolchain with non-ASCII paths and no game files."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import tomllib
from types import SimpleNamespace
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tools/desktop")]
from build_elf import main as build_module_elf, module_specs, write_elf
from model import Library
from modimage import LoadedModule
from pipeline import BOOT, CompilationError, Pipeline, relative_path


def check_interrupted_object(pipeline, stage):
    source = sorted((stage / "output").glob("*.cpp"))[0]
    original = source.read_bytes()
    obj = stage / "objects/batch-0000.o"
    stamp = obj.with_suffix(".key")
    assert obj.is_file() and stamp.is_file()

    def interrupted(*args, **kwargs):
        obj.write_bytes(b"partial object from interrupted compiler")
        raise RuntimeError("synthetic compiler interruption")

    source.write_bytes(original + b"\n// force a different object cache key\n")
    try:
        with patch.object(pipeline, "run", side_effect=interrupted):
            try:
                pipeline.compile(stage, capture=True)
            except RuntimeError as error:
                assert str(error) == "synthetic compiler interruption"
            else:
                raise AssertionError("The interrupted compile was unexpectedly accepted")
        assert not stamp.exists(), "A partial object retained the previous build's key"
    finally:
        source.write_bytes(original)

    with patch.object(pipeline, "run", wraps=pipeline.run) as commands:
        pipeline.compile(stage, capture=True)
    assert any("-c" in call.args[0] for call in commands.call_args_list), "The restored source reused a partial object"


def check_module_paths(program, temporary):
    """Exercise prepare's helper protocol across volumes, without a ROM."""
    data = temporary / "Modules @ Eudásio 文字"
    data.mkdir()
    pipeline = Pipeline(program, data, os.environ, lambda event: None)
    # The paths need not exist: the fake helper consumes arguments instead
    # of reading game data. C: is a real volume on both Windows CI and users' PCs.
    drive = "D:" if data.drive.casefold() == "c:" else "C:"
    disc = Path(drive + "/External disc @ edição 文字")
    calls = []

    def helper(name, arguments, cwd=None):
        if name == "build_elf":
            specs = module_specs(arguments[3:])
            calls.append(specs)
            for index, (xff, load, ram) in enumerate(specs):
                expected = ("KERNEL", "MANAGER", "GAMECORE")[index]
                assert Path(xff) == disc / (expected + ".XFF"), specs
                assert load == "auto", specs
                actual_capture = (Path(cwd) / ram).resolve()
                expected_capture = (pipeline.work / "captures" / (expected.lower() + ".bin")).resolve()
                assert actual_capture == expected_capture, (
                    f"{expected}: RAM path {ram!r} from {cwd} resolves to {actual_capture}, expected {expected_capture}"
                )

    def stage(folder, *, step, precompiled=None):
        result = folder / "game.exe"
        result.write_bytes(b"MZ synthetic helper-protocol fixture")
        return result

    def capture(executable, directory, name):
        assert directory == disc
        return pipeline.work / "captures" / (name.lower() + ".bin")

    with patch.object(pipeline, "helper", side_effect=helper), \
         patch.object(pipeline, "stage", side_effect=stage), \
         patch.object(pipeline, "capture", side_effect=capture):
        pipeline.prepare(disc)
    assert [len(specs) for specs in calls] == [1, 2, 3], calls
    config = tomllib.loads((pipeline.work / "boot/config.toml").read_text(encoding="utf-8"))
    assert Path(config["general"]["input"]) == disc / BOOT


def check_module_names(temporary):
    folder = temporary / "Módulos.v1, @ Eudásio 文字"
    folder.mkdir()
    module = folder / "KERNEL.XFF"
    forward_path = module.as_posix()
    paths = (forward_path, forward_path.replace("/", "\\"), folder.as_posix() + "\\" + module.name)
    with patch("modimage.Xff", return_value=SimpleNamespace(sections=[], symbols=[])):
        for path in paths:
            assert LoadedModule(path, b"xff2", 0).name == "KERNEL.XFF", path

    # An original XFF with one unnamed function and a matching relocated RAM image.
    code = struct.pack("<II", 0x03E00008, 0)  # jr $ra; nop
    xff = bytearray(0xB4)
    xff[:4] = b"xff2"
    struct.pack_into("<I", xff, 0x14, len(xff))
    struct.pack_into("<I", xff, 0x40, 1)  # section count
    struct.pack_into("<I", xff, 0x5C, 0x80)  # section table offset
    struct.pack_into("<II", xff, 0x68, 0xA0, 0xA4)  # section name offsets and string table
    struct.pack_into("<8I", xff, 0x80, 0, 0, len(code), 4, 1, 6, 0, 0xAC)
    xff[0xA4:0xAA] = b".text\0"
    xff[0xAC:] = code
    module.write_bytes(xff)
    ram = bytearray(0x208)
    ram[:len(xff)] = xff
    struct.pack_into("<I", ram, 0x80, 0x200)
    ram[0x200:] = code
    dump = folder / "captura @ 文字.bin"
    dump.write_bytes(ram)
    boot = folder / BOOT
    write_elf(boot, 0x100000, [(".text", 0x100000, len(code), code, 6)])
    symbols = folder / "boot-symbols.txt"
    symbols.touch()

    filenames = ("SOTC.elf", "functions.csv", "symbols.txt", "dynamic_imm.txt", "merged_entries.txt")
    artifacts = []
    for index, path in enumerate(paths):
        output = folder / f"helper-{index}"
        output.mkdir()
        arguments = ["--modules-json", json.dumps([[path, "auto", str(dump)]])]
        build_module_elf(output, boot, symbols, arguments)
        artifacts.append(tuple((output / filename).read_bytes() for filename in filenames))
    assert b".text.KERNEL\0" in artifacts[0][0]
    assert b"kernel_sub_00000200," in artifacts[0][1]
    assert all(artifact == artifacts[0] for artifact in artifacts[1:]), "Module artifacts depend on path separators"


def native_environment(program, scratch):
    binary = program / "compiler/toolchain/bin"
    environment = dict(os.environ, PATH=str(binary) + os.pathsep + os.environ.get("PATH", ""),
                       TMPDIR=str(scratch), TMP=str(scratch), TEMP=str(scratch))
    # The launcher normally starts outside MSYS2. A UTF-8 shell locale can
    # otherwise mask the kit's mismatched executable code pages.
    for variable in ("LC_ALL", "LC_CTYPE", "LANG"):
        environment.pop(variable, None)
    return environment


def check_link_response_file(program, temporary):
    """Isolate a UTF-8 response file's object paths from the assembler."""
    scratch = temporary / "link-scratch"
    scratch.mkdir()
    toolchain = program / "compiler/toolchain"
    environment = native_environment(program, scratch)
    gcc = [str(toolchain / "bin/g++.exe"), "--sysroot=" + toolchain.as_posix(),
           "-B" + (toolchain / "lib").as_posix() + "/"]
    source = scratch / "fixture.cpp"
    source.write_text("int main() { return 0; }\n", encoding="utf-8")
    obj = scratch / "batch-0000.o"
    subprocess.run([*gcc, "-c", source, "-o", obj], cwd=scratch, env=environment, check=True)
    build = temporary / "Eudásio" / "boot"
    objects = build / "objects"
    objects.mkdir(parents=True)
    obj = obj.replace(objects / obj.name)
    assert obj.is_file()
    response = build / "link.rsp"
    response.write_text(f'"{obj.as_posix()}"\n', encoding="utf-8")
    game = scratch / "fixture.exe"
    linker = subprocess.run([*gcc, "@" + str(response), "-static-libgcc", "-o", game], cwd=scratch,
                            env=environment, capture_output=True)
    assert linker.returncode == 0, linker.stderr.decode("utf-8", errors="replace")
    with game.open("rb") as stream:
        assert stream.read(2) == b"MZ"


def check_prepare(program, temporary):
    profile = temporary / "Eudásio 文字"
    scratch = profile / "Arquivos temporários"
    scratch.mkdir(parents=True)
    environment = native_environment(program, scratch)
    environment["SOTC_BUILD_JOBS"] = "1"
    pipeline = Pipeline(program, profile, environment, print)
    stage = pipeline.work / "boot"
    stage.mkdir()
    disc = profile / "Disco 文字"
    disc.mkdir()
    guest = disc / BOOT
    write_elf(guest, 0x100000, [(".text", 0x100000, 8, struct.pack("<II", 0x03E00008, 0), 6)])
    symbols = stage / "symbols.txt"
    symbols.write_text("", encoding="utf-8")
    functions = stage / "functions.csv"
    pipeline.helper("funcmap_elf", [guest, symbols, functions])
    assert str(functions) in pipeline.log.read_text(encoding="utf-8")
    config = stage / "config.toml"
    path = json.dumps(relative_path(guest, stage).as_posix(), ensure_ascii=False)
    config.write_text(f'[general]\ninput = {path}\noutput = "output"\nghidra_output = "functions.csv"\n',
                      encoding="utf-8")
    try:
        executable = pipeline.stage(stage, step=1)
    except Exception:
        print(pipeline.log.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
        raise
    with executable.open("rb") as stream:
        assert stream.read(2) == b"MZ"
    check_interrupted_object(pipeline, stage)

    for code in (4551, 577):
        with patch("pipeline.subprocess.Popen", side_effect=ctypes.WinError(code)):
            try:
                pipeline.capture(executable, disc, "FIXTURE")
            except CompilationError as error:
                assert f"Windows blocked the compiled game from starting (error {code})" in str(error), error
            else:
                raise AssertionError("Expected a Windows application control error")
        assert f"[WinError {code}]" in pipeline.log.read_text(encoding="utf-8")


def check_launch_error(program, temporary):
    profile = temporary / "Execução 文字"
    profile.mkdir()
    boot = profile / BOOT
    boot.touch()
    library = Library(profile, program)
    library.state["disc"] = str(boot)
    executable = profile / "Jogo recompilado 文字.exe"
    missing = FileNotFoundError(2, "fixture executable missing")
    for error in (ctypes.WinError(4551), ctypes.WinError(577), missing):
        with (
            patch.object(library, "prepare_game", return_value=executable),
            patch("gpu.prefer_windows_gpu", return_value=None),
            patch("model.subprocess.Popen", side_effect=error),
        ):
            try:
                library.launch()
            except OSError as raised:
                assert raised is error
            else:
                raise AssertionError("Expected the original startup error")
        log = (profile / "last-game.log").read_text(encoding="utf-8")
        assert str(executable) in log and str(error) in log, log
        if error is not missing:
            assert f"winerror={error.winerror}" in log, log


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    assert relative_path(Path("D:/discs/KERNEL.XFF"), Path("C:/builds/boot")) == Path("D:/discs/KERNEL.XFF")
    assert relative_path(Path("C:/discs/KERNEL.XFF"), Path("C:/builds/boot")) == Path("../../discs/KERNEL.XFF")
    specs = [["D:/discos @ edição/KERNEL.XFF", "auto", "C:/builds @ 文字/kernel.bin"]]
    assert module_specs(["--modules-json", json.dumps(specs)]) == specs
    assert module_specs(["KERNEL.XFF@auto@kernel.bin"]) == [["KERNEL.XFF", "auto", "kernel.bin"]]
    with tempfile.TemporaryDirectory(prefix="unicode-", dir=args.work_dir) as folder:
        check_module_paths(args.program.resolve(), Path(folder))
        check_module_names(Path(folder))
        check_link_response_file(args.program.resolve(), Path(folder))
        check_prepare(args.program.resolve(), Path(folder))
        check_launch_error(args.program.resolve(), Path(folder))
    print("Windows preparation passed with Unicode data and scratch paths")


if __name__ == "__main__":
    main()
