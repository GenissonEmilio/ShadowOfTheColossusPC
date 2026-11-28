"""Route errors from the native GUI launch to a log the player can read."""
import argparse
import contextlib
import ctypes
import os
from pathlib import Path
import runpy
import sys
import traceback

from model import Library

main_script = Path(__file__).with_name("main.py")
if sys.argv[1:2] in (["--prepare"], ["--play"]):
    runpy.run_path(str(main_script), run_name="__main__")

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument("--data-dir", type=Path, default=os.environ.get("SOTC_DATA_DIR"))
args, _ = parser.parse_known_args()
try:
    log_directory = Library(args.data_dir).directory
    log_path = log_directory / "last-launcher.log"
    log_directory.mkdir(parents=True, exist_ok=True)
    log = log_path.open("w", encoding="utf-8")
except OSError as error:
    if "--screenshot" not in sys.argv:
        ctypes.windll.user32.MessageBoxW(None, f"Não foi possível abrir a pasta de dados:\n\n{error}",
                                         "Shadow of the Colossus PC", 0x10)
    raise SystemExit(1)
with log, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
    try:
        runpy.run_path(str(main_script), run_name="__main__")
    except Exception:
        traceback.print_exc()
        log.flush()
        if "--screenshot" not in sys.argv:
            ctypes.windll.user32.MessageBoxW(None, f"O launcher encontrou um erro. Consulte:\n{log_path}",
                                             "Shadow of the Colossus PC", 0x10)
        raise SystemExit(1)
