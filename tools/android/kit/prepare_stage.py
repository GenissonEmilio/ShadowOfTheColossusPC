"""Run the desktop recompiler's input helpers against Android's private disc."""

import importlib.machinery
import importlib.util
import sys


def register_extensions(directory):
    # Android extracts only files named lib*.so from the APK's native library
    # directory. Import extensions using their original PyInit_ names.
    for name in ("_struct", "_bisect", "_json", "math"):
        path = f"{directory}/libpy_{name}.so"
        loader = importlib.machinery.ExtensionFileLoader(name, path)
        spec = importlib.util.spec_from_file_location(name, path, loader=loader)
        module = importlib.util.module_from_spec(spec)
        loader.exec_module(module)
        sys.modules[name] = module


if sys.platform == "android":
    register_extensions(sys.argv.pop(1))

import json
import re
import runpy
from pathlib import Path
from xff import SHN_ABS, Xff

ROOT = Path(__file__).resolve().parent.parent
MODULES = ("KERNEL", "MANAGER", "GAMECORE")
BOOT = "SCUS_974.72"


def run_helper(name, arguments):
    path = ROOT / "tools" / f"{name}.py"
    original = sys.argv
    try:
        sys.argv = [str(path), *map(str, arguments)]
        runpy.run_path(str(path), run_name="__main__")
    finally:
        sys.argv = original


def main(disc, work, stage_number):
    boot = work / "boot"
    boot_config = boot / "config.toml"
    symbols = boot / "symbols.txt"
    stage = work / ("boot" if stage_number == 0 else MODULES[stage_number - 1].lower())
    if stage_number == 0:
        kernel = Xff(disc / "KERNEL.XFF")
        absolute_symbols = sorted({(s.value, s.name) for s in kernel.symbols if s.shndx == SHN_ABS and s.name})
        symbols.write_text("".join(f"{address:08x} {name}\n" for address, name in absolute_symbols))
        run_helper("funcmap_elf", [disc / BOOT, symbols, boot / "functions.csv"])
        config = (ROOT / "game" / "scus.toml").read_text()
        config = re.sub(r"^input = .*$", lambda _: "input = " + json.dumps(str(disc / BOOT)), config, flags=re.M)
        boot_config.write_text(config)
    else:
        modules = [[str(disc / f"{name}.XFF"), "auto", str(work / f"{name.lower()}.bin")]
                   for name in MODULES[:stage_number]]
        run_helper("build_elf", [stage, disc / BOOT, symbols, "--modules-json", json.dumps(modules)])
        run_helper("gen_config", [stage, boot_config])


if __name__ == "__main__":
    main(Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3]))
