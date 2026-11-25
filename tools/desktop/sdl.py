"""Locate the launcher SDL2 runtime in a release or a source checkout."""
import ctypes
import os
from pathlib import Path


def load_sdl(program):
    program = Path(program).resolve()
    if os.name == "nt":
        candidates = (program / "compiler/toolchain/bin/SDL2.dll", "SDL2.dll")
    else:
        candidates = (program / "_internal/libSDL2-2.0.so.0",
                      program / "compiler/lib/libSDL2-2.0.so.0", "libSDL2-2.0.so.0")
    for candidate in candidates:
        try:
            return ctypes.CDLL(str(candidate))
        except OSError:
            continue
    return None
