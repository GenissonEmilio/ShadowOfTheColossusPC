#!/usr/bin/env python3
"""Exercise the real frozen launcher against a host desktop helper (no ROM)."""
import argparse
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument("--program", type=Path, required=True)
p.add_argument("--out", type=Path, required=True)
a = p.parse_args()
out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
source = out / "helper-source" / "tools"; source.mkdir(parents=True, exist_ok=True)
bin_dir = out / "host-bin"; bin_dir.mkdir(exist_ok=True)
opener = bin_dir / "xdg-open"
opener.write_text('''#!/usr/bin/python3
import json, os, sys
from pathlib import Path
assert "LD_LIBRARY_PATH" not in os.environ, os.environ.get("LD_LIBRARY_PATH")
assert "FONTCONFIG_FILE" not in os.environ
assert "GI_TYPELIB_PATH" not in os.environ
assert "GIO_EXTRA_MODULES" not in os.environ
assert os.environ.get("DBUS_SESSION_BUS_ADDRESS") == "unix:path=/synthetic-session"
Path(os.environ["SOTC_DESKTOP_RESULT"]).write_text(json.dumps({"path":sys.argv[1],"clean":True}))
''')
opener.chmod(0o755)
(source / "funcmap_elf.py").write_text('''from pathlib import Path
import sys
from desktop_services import open_path
from saves import SaveCards, CARD_BYTES, MAGIC
from model import Library
out=Path(sys.argv[1]); first=SaveCards(Library(out/"original-home"))
first.directory.mkdir(parents=True,exist_ok=True)
first.card.write_bytes(MAGIC+bytes(CARD_BYTES-len(MAGIC)))
transfer=out/"Minha aventura.ps2"; first.export_card(transfer)
second=SaveCards(Library(out/"another-home")); second.import_card(transfer)
assert second.card.read_bytes()==first.card.read_bytes()
open_path(second.directory)
''')
env = dict(os.environ)
env.pop("LD_LIBRARY_PATH", None); env.pop("LD_LIBRARY_PATH_ORIG", None)
env.update(PATH=str(bin_dir) + os.pathsep + os.defpath,
           FONTCONFIG_FILE="/nonexistent/frozen-fonts.conf", GI_TYPELIB_PATH="/nonexistent/typelibs",
           GIO_EXTRA_MODULES="/nonexistent/gio", DBUS_SESSION_BUS_ADDRESS="unix:path=/synthetic-session",
           SOTC_DESKTOP_RESULT=str(out / "host-open.json"))
subprocess.run([str(a.program.resolve()), "--pipeline-helper", str(source.parent),
                "funcmap_elf", str(out)], env=env, check=True, timeout=25)
report = json.loads((out / "host-open.json").read_text())
assert report["clean"] and report["path"] == str(out / "another-home/player/memcards")
report.update(passed=True, scope="frozen launcher: card transfer between two homes and host opener environment")
(out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report))
