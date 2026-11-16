"""Route errors from the native GUI launch to a log the player can read."""
import contextlib
import ctypes
import os
from pathlib import Path
import runpy
import sys
import traceback

from model import data_directory

log_directory = data_directory()
if "--data-dir" in sys.argv:
    index = sys.argv.index("--data-dir") + 1
    if index < len(sys.argv):
        log_directory = Path(sys.argv[index])
log_directory.mkdir(parents=True, exist_ok=True)
log_path = log_directory / "last-launcher.log"
with log_path.open("w", encoding="utf-8") as log, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
    try:
        runpy.run_path(str(Path(__file__).with_name("main.py")), run_name="__main__")
    except Exception:
        traceback.print_exc()
        log.flush()
        if "--screenshot" not in sys.argv:
            ctypes.windll.user32.MessageBoxW(None, f"O launcher encontrou um erro. Consulte:\n{log_path}",
                                             "Shadow of the Colossus PC", 0x10)
        raise SystemExit(1)
