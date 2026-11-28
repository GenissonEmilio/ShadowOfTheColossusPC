"""Headless entry points used by RecompLauncher and the portable package tests."""
import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import sys

from graphics import Graphics
from language import activate, preferences, translate as tr
from model import Library
from pipeline import CompilationError, WINDOWS


def run(arguments):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--prepare", action="store_true")
    mode.add_argument("--play", action="store_true")
    parser.add_argument("--rom", type=Path)
    parser.add_argument("--data-dir", type=Path, default=os.environ.get("SOTC_DATA_DIR"))
    args = parser.parse_args(arguments)
    if (args.prepare and args.rom is None) or (args.play and args.rom is not None):
        parser.error("--prepare requires --rom. --play uses the prepared copy.")

    try:
        library = Library(args.data_dir)
    except OSError as error:
        print(error, file=sys.stderr, flush=True)
        return 74
    activate(preferences(library.directory)["language"])
    graphics = Graphics(library)
    try:
        if args.prepare:
            def progress(message):
                fraction = 0.05 + 0.94 * getattr(message, "fraction", 0.0)
                detail = " · ".join(str(message).splitlines())
                print(f"progress {round(fraction, 3)} {detail}", flush=True)

            library.prepare(args.rom, progress)
            graphics.normalize_external_settings()
            graphics.save_display(*graphics.display())
            print("progress 1 Ready to play", flush=True)
            return 0

        process = library.launch(attached=True)
        status = process.wait()
        if WINDOWS:
            return ctypes.c_int32(status).value
        return status if status >= 0 else 128 - status
    except (CompilationError, OSError, subprocess.SubprocessError) as error:
        print(f"{error}\n{library.directory / 'last-prepare.log'}", file=sys.stderr, flush=True)
        return 74
    except ValueError as error:
        print(error, file=sys.stderr, flush=True)
        return 65
