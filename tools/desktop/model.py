"""SotC launcher state/process boundary, adapted from ForbiddenMemoriesPC (MIT)."""
from __future__ import annotations
import hashlib
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
from import_disc import import_disc, EXPECTED
from pipeline import Pipeline

VERSION = "0.2.7"
SOURCE_ROOT = Path(__file__).resolve().parents[2]
APPLICATION_ID = "io.github.GenissonEmilio.ShadowOfTheColossusPC"


def assets_directory():
    return Path(getattr(sys, "_MEIPASS", Path(__file__).parent)) / "assets"


def program_directory():
    return Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else SOURCE_ROOT


def data_directory():
    base = Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share"))).expanduser()
    if not base.is_absolute():
        base = Path.home() / ".local/share"
    return base / "ShadowOfTheColossusPC"


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".launcher-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


class Library:
    def __init__(self, directory=None, program=None):
        self.directory = Path(directory or data_directory()).resolve()
        self.program = Path(program or program_directory()).resolve()
        self.config = self.directory / "launcher.json"
        self.state = {}
        try:
            state = json.loads(self.config.read_text())
            if isinstance(state, dict):
                self.state = state
        except (OSError, ValueError):
            pass

    @property
    def disc(self):
        path = self.state.get("disc")
        if not isinstance(path, str):
            return None
        value = Path(path)
        value = value if value.is_absolute() else self.directory / value
        return value if value.is_file() else None

    @property
    def player_directory(self):
        return self.directory / "player"

    def game_environment(self, test=False):
        environment = dict(os.environ)
        original_ld = environment.get("LD_LIBRARY_PATH_ORIG")
        for name in ("LD_LIBRARY_PATH", "LD_LIBRARY_PATH_ORIG", "GI_TYPELIB_PATH", "GTK_DATA_PREFIX",
                     "GTK_EXE_PREFIX", "GDK_PIXBUF_MODULE_FILE", "GDK_PIXBUF_MODULEDIR", "GIO_EXTRA_MODULES",
                     "FONTCONFIG_FILE", "DOTNET_ROOT", "DOTNET_ROOT_X64"):
            environment.pop(name, None)
        if original_ld:
            environment["LD_LIBRARY_PATH"] = original_ld
        environment["SOTC_NATIVE"] = "1"
        environment["SOTC_NATIVE_GL"] = "1"
        environment["PS2X_GS_THREADS"] = "2"
        environment["PS2X_MEMCARD_DIR"] = str(self.player_directory / "memcards")
        environment["LD_LIBRARY_PATH"] = str(self.program / "compiler/lib") + (
            os.pathsep + original_ld if original_ld else "")
        environment.setdefault("LP_NUM_THREADS", "2")
        mapping = self.directory / "controllers/gamecontrollerdb.txt"
        if mapping.is_file():
            environment["PS2X_GAMEPAD_MAPPINGS"] = str(mapping)
        # The native backend supplies the same defaults when no profile exists.
        from controls import Controls
        environment["PS2X_DESKTOP_INPUT"] = str(Controls(self).path)
        from graphics import Graphics
        selected = Graphics(self).selected()
        environment["SOTC_SHADER"] = str(selected) if selected else ""
        environment["SOTC_SHADER_STATUS"] = str(self.directory / "shader-status.txt")
        return environment

    def prepare(self, source, progress=lambda text: None):
        source = Path(source).expanduser().resolve()
        if not source.is_file():
            raise ValueError("O arquivo escolhido não foi encontrado.")
        stamp = f"{source}\0{source.stat().st_size}\0{source.stat().st_mtime_ns}"
        identifier = hashlib.sha256(stamp.encode()).hexdigest()[:20]
        destination = self.directory / "discs" / identifier
        boot = destination / "SCUS_974.72"
        progress("Lendo e validando o disco…")
        if not self.valid_extracted(destination):
            if destination.exists():
                shutil.rmtree(destination)
            boot = import_disc(source, destination)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.player_directory.mkdir(parents=True, exist_ok=True)
        try:
            executable = Pipeline(self.program, self.directory, self.game_environment(), progress).prepare(destination)
        except subprocess.CalledProcessError as exc:
            raise ValueError(f"A compilação não terminou. Consulte os detalhes em:\n{self.directory / 'last-prepare.log'}") from exc
        state = {"disc": str(boot.relative_to(self.directory)), "name": source.name, "serial": "SCUS-97472", "executable": str(executable)}
        atomic_json(self.config, state)
        self.state = state
        return boot

    def valid_extracted(self, directory):
        try:
            for name,(size,digest) in EXPECTED.items():
                path = directory / name
                if path.stat().st_size != size or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                    return False
            return (directory / "NICO.DAT").stat().st_size == 2667340678
        except OSError:
            return False

    def verify(self):
        boot = self.disc
        if not boot or not self.valid_extracted(boot.parent):
            raise ValueError("Escolha sua ROM para continuar. Os arquivos locais estão ausentes ou incompletos.")
        return boot

    def launch(self, test=False, progress=lambda text: None):
        boot = self.verify()
        from graphics import Graphics
        graphics = Graphics(self)
        graphics.save_display(*graphics.display())
        environment = self.game_environment(test)
        executable = Pipeline(self.program, self.directory, environment, progress).prepare(boot.parent)
        self.player_directory.mkdir(parents=True, exist_ok=True)
        if test:
            shots = self.directory / "game-test"
            shots.mkdir(exist_ok=True)
            environment.update(PS2X_SNAPSHOT_DIR=str(shots), PS2X_SNAPSHOT_SECONDS="5",
                               PS2X_EXIT_AT_FRAME="600")
        with (self.directory / "last-game.log").open("w") as output:
            return subprocess.Popen([str(executable), str(boot)], cwd=self.player_directory,
                                    env=environment, stdout=output, stderr=subprocess.STDOUT,
                                    start_new_session=True)

    def install_desktop_entry(self, applications=None):
        destination = Path(applications) if applications else Path(
            os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share"))) / "applications"
        destination.mkdir(parents=True, exist_ok=True)
        command = [str(Path(sys.executable).resolve())] if getattr(sys, "frozen", False) else [
            sys.executable, str(SOURCE_ROOT / "tools/desktop/main.py")]
        def quote(value):
            return '"' + value.replace('%', '%%').replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$') + '"'
        path = destination / (APPLICATION_ID + ".desktop")
        path.write_text("[Desktop Entry]\nType=Application\nName=Shadow of the Colossus PC\n"
                        "Comment=Sua próxima aventura começa aqui\nTerminal=false\nCategories=Game;ActionGame;\n"
                        f"Exec={' '.join(map(quote, command))}\nIcon={assets_directory() / 'icon.png'}\n"
                        "StartupWMClass=shadow-of-the-colossus-pc\n")
        path.chmod(0o755)
        return path
