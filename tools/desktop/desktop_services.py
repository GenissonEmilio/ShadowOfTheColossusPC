"""Launch host desktop applications without PyInstaller's private libraries."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys


def host_environment():
    env = dict(os.environ)
    if getattr(sys, "frozen", False) or "LD_LIBRARY_PATH_ORIG" in env:
        original = env.pop("LD_LIBRARY_PATH_ORIG", "")
        env.pop("LD_LIBRARY_PATH", None)
        if original:
            env["LD_LIBRARY_PATH"] = original
        # The GI hook and our font setup refer to the bundle, not the host.
        for name in ("GI_TYPELIB_PATH", "GIO_EXTRA_MODULES", "GTK_DATA_PREFIX",
                     "GTK_EXE_PREFIX", "GTK_PATH", "GTK_MODULES", "GDK_PIXBUF_MODULE_FILE",
                     "GDK_PIXBUF_MODULEDIR", "FONTCONFIG_FILE", "FONTCONFIG_PATH"):
            env.pop(name, None)
        bundle = Path(sys._MEIPASS).resolve() if hasattr(sys, "_MEIPASS") else None
        if bundle:
            env["PATH"] = os.pathsep.join(p for p in env.get("PATH", "").split(os.pathsep)
                                          if p and not Path(p).resolve().is_relative_to(bundle))
    return env


def open_path(path):
    """Try both desktop standards; propagate failures instead of a silent click."""
    path = Path(path).resolve(strict=True)
    env = host_environment()
    errors = []
    for program, arguments in (("xdg-open", [str(path)]), ("gio", ["open", path.as_uri()])):
        executable = shutil.which(program, path=env.get("PATH", os.defpath))
        if not executable:
            continue
        try:
            result = subprocess.run([executable, *arguments], env=env, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                    text=True, timeout=12, check=False)
            if result.returncode == 0:
                return
            errors.append(result.stderr.strip()[-500:])
        except (OSError, subprocess.TimeoutExpired) as exc:
            errors.append(str(exc))
    detail = "\n".join(filter(None, errors))
    raise OSError("Não foi possível abrir o aplicativo padrão deste computador.\n"
                  f"O arquivo continua disponível em:\n{path}" + ("\n\n" + detail if detail else ""))
