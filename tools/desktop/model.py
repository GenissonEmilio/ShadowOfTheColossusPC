"""SotC launcher state/process boundary, adapted from ForbiddenMemoriesPC (MIT)."""
from __future__ import annotations
import ctypes
import hashlib
import json
import os
import shutil
from pathlib import Path, PureWindowsPath
import stat
import subprocess
import sys
import tempfile
from import_disc import PROFILES, import_disc, validate_disc
from language import PREFERENCES, preferences, translate as tr
from pipeline import CompilationError, PAL_BOOT, Pipeline, PROCESS_FLAGS, WINDOWS, compiler_tool, executable_name

VERSION = "1.2.14"
SOURCE_ROOT = Path(__file__).resolve().parents[2]
APPLICATION_ID = "io.github.GenissonEmilio.ShadowOfTheColossusPC"
PORTABLE_MARKER_NAME = "portable.ini"
PORTABLE_DATA_NAME = "data"
PORTABLE_DISABLED = "disabled"
SAVE_STATES_AVAILABLE = WINDOWS or sys.platform.startswith("linux")
SERIAL_BY_BOOT = {profile.boot: profile.serial for profile in PROFILES.values()}
PAL_CAMPAIGN_LANGUAGES = {"en": "English", "fr": "Français", "de": "Deutsch",
                          "it": "Italiano", "es": "Español"}


def assets_directory():
    return Path(getattr(sys, "_MEIPASS", Path(__file__).parent)) / "assets"


def program_directory():
    if WINDOWS and (directory := os.environ.get("SOTC_PROGRAM_DIR")):
        return Path(directory).resolve()
    return Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else SOURCE_ROOT


def data_directory():
    if WINDOWS:
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local"))
        return base / "ShadowOfTheColossusPC"
    base = Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share"))).expanduser()
    if not base.is_absolute():
        base = Path.home() / ".local/share"
    return base / "ShadowOfTheColossusPC"


def check_portable_path(path, kind):
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return False
    if (stat.S_IFMT(metadata.st_mode) != kind
            or getattr(metadata, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT):
        raise OSError(tr("Caminho inválido para o modo portátil") + f": {path}")
    return True


def atomic_json(path, value):
    atomic_text(path, json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def atomic_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".launcher-", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(text)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def publish_profile(source, target):
    if WINDOWS:
        source.rename(target)  # Windows fails if the target already exists.
        return
    # Unlike os.rename on POSIX, RENAME_NOREPLACE also preserves an empty
    # profile that appeared while another launcher was copying the old one.
    rename = ctypes.CDLL(None, use_errno=True).renameat2
    rename.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
    rename.restype = ctypes.c_int
    if rename(-100, os.fsencode(source), -100, os.fsencode(target), 1):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(target))


def copy_profile_entry(source, target):
    try:
        info = source.lstat()
    except FileNotFoundError:
        return
    kind = stat.S_IFMT(info.st_mode)
    if kind not in (stat.S_IFREG, stat.S_IFDIR) or getattr(
            info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
        raise OSError(tr("Caminho inválido para o modo portátil") + f": {source}")
    if kind == stat.S_IFDIR:
        target.mkdir(parents=True, exist_ok=True)
        for child in source.iterdir():
            copy_profile_entry(child, target / child.name)
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        after = source.stat()
        if (info.st_size, info.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise OSError(tr("O perfil anterior mudou durante a cópia") + f": {source}")


def copy_completed_builds(program, source, target):
    manifest_path = program / "compiler/manifest.json"
    try:
        build_id = json.loads(manifest_path.read_text(encoding="utf-8"))["build_id"]
    except (FileNotFoundError, KeyError, TypeError, ValueError):
        return
    if not isinstance(build_id, str) or not build_id or PureWindowsPath(build_id).name != build_id or build_id == "..":
        return
    for edition in (Path(), Path(SERIAL_BY_BOOT[PAL_BOOT])):
        for compiler, folder in (("clang", edition), ("gcc", edition / "gcc")):
            relative = Path("builds", build_id, folder)
            work = source / relative
            if not all(check_portable_path(source / parent, stat.S_IFDIR)
                       for parent in (*reversed(relative.parents), relative) if parent != Path()):
                continue
            stamp = work / "complete.json"
            binary = work / "gamecore" / executable_name("game")
            if not (check_portable_path(stamp, stat.S_IFREG)
                    and check_portable_path(binary.parent, stat.S_IFDIR)
                    and check_portable_path(binary, stat.S_IFREG)):
                continue
            try:
                manifest = json.loads(stamp.read_text(encoding="utf-8"))
            except (FileNotFoundError, ValueError):
                continue
            if (not isinstance(manifest, dict) or manifest.get("build_id", build_id) != build_id
                    or manifest.get("compiler", "clang") != compiler):
                continue
            copied = target / relative / "gamecore" / binary.name
            copy_profile_entry(binary, copied)
            digest = hashlib.sha256()
            with copied.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            if digest.hexdigest() == manifest.get("sha256"):
                atomic_json(target / relative / stamp.name, manifest)
            else:
                copied.unlink()


class Library:
    def __init__(self, directory=None, program=None):
        self.program = Path(program or program_directory()).resolve()
        self.portable_marker = self.program / PORTABLE_MARKER_NAME
        self.portable_directory = self.program / PORTABLE_DATA_NAME
        self.has_data_override = directory is not None
        self.packaged = (self.program / "release.json").is_file()
        self.portable_error = None
        self.portable = not self.has_data_override and self.portable_enabled()
        if self.portable:
            check_portable_path(self.portable_directory, stat.S_IFDIR)
        if directory is None:
            directory = self.portable_directory if self.portable else data_directory()
        self.load_profile(directory)

    @classmethod
    def for_launch(cls, directory=None, program=None):
        try:
            library = cls(directory, program)
            if library.portable:
                if (os.environ.get("SOTC_UPDATE_READY") and not library.directory.exists()
                        and data_directory().is_dir()):
                    # The updater waits for GTK to acknowledge startup. Leave
                    # the old profile available until the next ordinary launch.
                    library.portable = False
                    library.load_profile(data_directory())
                else:
                    library.initialize_portable()
            return library
        except OSError as error:
            if directory is not None:
                raise
            library = cls(data_directory(), program)
            library.has_data_override = False
            library.portable_error = error
            return library

    def load_profile(self, directory):
        self.directory = Path(directory).resolve()
        self.config = self.directory / "launcher.json"
        self.state = {}
        try:
            state = json.loads(self.config.read_text(encoding="utf-8"))
            if isinstance(state, dict):
                self.state = state
        except (OSError, ValueError):
            pass

    def portable_enabled(self):
        if check_portable_path(self.portable_marker, stat.S_IFREG):
            value = self.portable_marker.read_text(encoding="utf-8-sig", errors="replace")
            return value.strip().lower() != PORTABLE_DISABLED
        return self.packaged

    def initialize_portable(self):
        if not check_portable_path(self.portable_directory, stat.S_IFDIR):
            legacy = data_directory().resolve()
            with tempfile.TemporaryDirectory(prefix=".sotc-profile-", dir=self.program) as temporary:
                staged = Path(temporary) / PORTABLE_DATA_NAME
                staged.mkdir()
                if self.packaged and legacy.is_dir():
                    check_portable_path(legacy / "player", stat.S_IFDIR)
                    for entry in (
                        "launcher.json", PREFERENCES, "controllers", "shader.json", "shaders",
                        "texture-mods.json", "textures", "discs", "campaign-languages",
                        "player/celosia_settings.cfg", "player/memcards", "player/savestates",
                        "player/mc0", "player/mc1",
                    ):
                        copy_profile_entry(legacy / entry, staged / entry)
                    copy_completed_builds(self.program, legacy, staged)
                    previous = Library(legacy, self.program)
                    migrated = Library(staged, self.program)
                    migrated.portable = True
                    boot = migrated.disc
                    if previous.disc and not boot:
                        raise OSError(tr("Não foi possível copiar o disco do perfil anterior"))
                    if boot:
                        migrated.state["disc"] = boot.relative_to(staged).as_posix()
                        atomic_json(migrated.config, migrated.state)
                try:
                    publish_profile(staged, self.portable_directory)
                except FileExistsError:
                    check_portable_path(self.portable_directory, stat.S_IFDIR)
        with tempfile.TemporaryFile(dir=self.portable_directory):
            pass
        if self.portable:
            self.load_profile(self.portable_directory)

    def set_portable(self, enabled):
        if self.has_data_override:
            raise ValueError("The current profile was selected externally")
        check_portable_path(self.portable_marker, stat.S_IFREG)
        if enabled:
            self.initialize_portable()
        atomic_text(self.portable_marker, "" if enabled else PORTABLE_DISABLED + "\n")

    @property
    def disc(self):
        path = self.state.get("disc")
        if not isinstance(path, str):
            return None
        value = Path(path)
        if self.portable:
            stored = PureWindowsPath(path)
            if stored.anchor:
                # Older profiles may contain absolute paths to the managed disc.
                # Recover the relative location without consulting the old profile.
                parts = stored.parts[-3:]
                if parts[:1] != ("discs",):
                    return None
                stored = Path(*parts)
            value = self.directory.joinpath(*stored.parts)
            if not value.resolve().is_relative_to(self.directory):
                return None
        elif not value.is_absolute():
            value = self.directory / value
        return value if value.is_file() else None

    @property
    def player_directory(self):
        return self.directory / "player"

    @property
    def is_pal(self):
        boot = self.disc
        return boot is not None and boot.name == PAL_BOOT

    @property
    def campaign_patch(self):
        return assets_directory() / "campaign/pt-BR.xdelta"

    @property
    def portuguese_directory(self):
        boot = self.disc
        if boot and boot.name == "SCUS_974.72":
            return self.directory / "campaign-languages" / boot.parent.name / "pt-BR"
        return None

    def portuguese_boot(self):
        from campaign_locale import is_portuguese_ready
        directory = self.portuguese_directory
        return directory / "SCUS_974.72" if directory and is_portuguese_ready(directory) else None

    def campaign_language_options(self):
        boot = self.disc
        if not boot:
            return {}
        if boot.name == PAL_BOOT:
            return {"original": tr("Menu original do jogo"), "auto": tr("Automático"),
                    **PAL_CAMPAIGN_LANGUAGES}
        if boot.name != "SCUS_974.72":
            return {}
        options = {"auto": tr("Automático"), "en": "English"}
        if self.campaign_patch.is_file() or self.portuguese_boot():
            options["pt-BR"] = "Português (Brasil)"
        return options

    def campaign_language_choice(self, options, selected_preferences):
        if not options:
            return None
        serial = SERIAL_BY_BOOT[self.disc.name]
        selected = selected_preferences["campaign_languages"].get(serial)
        if selected is None and serial == "SCUS-97472":
            from graphics import Graphics
            legacy = Graphics(self).settings().get("campaign_language")
            selected = {"1": "auto", "2": "en", "3": "pt-BR"}.get(legacy)
        return selected if isinstance(selected, str) and selected in options else next(iter(options))

    def campaign_language(self):
        options = self.campaign_language_options()
        selected_preferences = preferences(self.directory)
        selected = self.campaign_language_choice(options, selected_preferences)
        if selected != "auto":
            return selected
        interface_language = selected_preferences["language"]
        detected = "pt-BR" if interface_language == "pt" else interface_language
        return detected if detected in options else "en"

    def game_environment(self, test=False, *, boot=None):
        environment = dict(os.environ)
        original_ld = environment.get("LD_LIBRARY_PATH_ORIG")
        for name in ("LD_LIBRARY_PATH", "LD_LIBRARY_PATH_ORIG", "GI_TYPELIB_PATH", "GTK_DATA_PREFIX",
                     "GTK_EXE_PREFIX", "GDK_PIXBUF_MODULE_FILE", "GDK_PIXBUF_MODULEDIR", "GIO_EXTRA_MODULES",
                     "FONTCONFIG_FILE", "DOTNET_ROOT", "DOTNET_ROOT_X64",
                     "SOTC_CAMPAIGN_PT_BR_BOOT", "SOTC_CAMPAIGN_SYSTEM_PT",
                     "SOTC_CAMPAIGN_PAL_LANGUAGE"):
            environment.pop(name, None)
        if original_ld:
            environment["LD_LIBRARY_PATH"] = original_ld
        boot = boot or self.disc
        pal = boot is not None and boot.name == PAL_BOOT
        environment["SOTC_NATIVE"] = "0" if pal else "1"
        environment["SOTC_NATIVE_GL"] = "1"
        if pal:
            environment["PS2X_NATIVE_IOP_BOOT"] = "0"
            environment.setdefault("PS2X_GS_HW", "1")
        selected_preferences = preferences(self.directory)
        environment["SOTC_BUTTON_PROMPTS"] = selected_preferences["button_prompts"]
        environment["SOTC_INFINITE_HEALTH"] = "1" if selected_preferences["infinite_health"] else "0"
        environment["SOTC_INFINITE_GRIP"] = "1" if selected_preferences["infinite_grip"] else "0"
        environment["PS2X_GS_THREADS"] = "2"
        environment.setdefault("PS2X_STATS", "1")
        environment["PS2X_MEMCARD_DIR"] = str(self.player_directory / "memcards")
        states = self.player_directory / "savestates"
        if pal:
            states /= SERIAL_BY_BOOT[PAL_BOOT]
        environment["PS2X_STATE_DIR"] = str(states)
        if WINDOWS:
            binary = self.program / "compiler/toolchain/bin"
            environment["PATH"] = str(binary) + os.pathsep + environment.get("PATH", "")
        else:
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
            raise ValueError(tr("O arquivo escolhido não foi encontrado."))
        stamp = f"{source}\0{source.stat().st_size}\0{source.stat().st_mtime_ns}"
        identifier = hashlib.sha256(stamp.encode()).hexdigest()[:20]
        destination = self.directory / "discs" / identifier
        progress(tr("Lendo e validando o disco…"))
        profile = self.validated_disc(destination)
        if profile:
            boot = destination / profile.boot
        else:
            if destination.exists():
                shutil.rmtree(destination)
            boot = import_disc(source, destination)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.player_directory.mkdir(parents=True, exist_ok=True)
        try:
            executable = Pipeline(self.program, self.directory, self.game_environment(boot=boot),
                                  progress, boot_name=boot.name).prepare(destination)
        except subprocess.CalledProcessError as exc:
            raise CompilationError(tr("A compilação não terminou. Consulte {path}").format(path=self.directory / "last-prepare.log")) from exc
        disc_path = boot.relative_to(self.directory)
        if self.portable:
            disc_path = disc_path.as_posix()
            executable = executable.relative_to(self.directory).as_posix()
        state = {"disc": str(disc_path), "name": source.name, "source": str(source),
                 "serial": SERIAL_BY_BOOT[boot.name], "executable": str(executable)}
        atomic_json(self.config, state)
        self.state = state
        return boot

    def validated_disc(self, directory):
        try:
            return validate_disc(directory)
        except (OSError, ValueError):
            return None

    def verify(self):
        boot = self.disc
        profile = self.validated_disc(boot.parent) if boot else None
        if not profile or boot.name != profile.boot:
            raise ValueError(tr("Escolha sua ROM para continuar. Os arquivos locais estão ausentes ou incompletos."))
        return boot

    def prepare_game(self, progress=lambda text: None):
        boot = self.verify()
        self.player_directory.mkdir(parents=True, exist_ok=True)
        return Pipeline(self.program, self.directory, self.game_environment(boot=boot),
                        progress, boot_name=boot.name).prepare(boot.parent)

    def launch(self, test=False, progress=lambda text: None, *, attached=False):
        boot = self.disc
        executable = self.prepare_game(progress)
        from graphics import Graphics
        graphics = Graphics(self)
        graphics.normalize_external_settings()
        graphics.save_display(*graphics.display())
        environment = self.game_environment(test)
        campaign_language = self.campaign_language()
        if campaign_language == "pt-BR":
            from campaign_locale import prepare_portuguese
            boot = self.portuguese_boot() or prepare_portuguese(
                boot.parent, self.portuguese_directory, self.campaign_patch, progress)
            environment["PS2X_STATE_DIR"] = str(Path(environment["PS2X_STATE_DIR"]) / "pt-BR")
        elif boot.name == PAL_BOOT and campaign_language in PAL_CAMPAIGN_LANGUAGES:
            environment["SOTC_CAMPAIGN_PAL_LANGUAGE"] = campaign_language
            if campaign_language != "en":
                environment["PS2X_STATE_DIR"] = str(Path(environment["PS2X_STATE_DIR"]) / campaign_language)
        from texture_mods import TextureMods
        environment.update(TextureMods(self).environment())
        gpu_status = None
        if WINDOWS:
            from gpu import prefer_windows_gpu
            gpu_status = prefer_windows_gpu(executable)
        elif sys.platform.startswith("linux"):
            from gpu import prefer_discrete_gpu
            prefer_discrete_gpu(environment)
        self.player_directory.mkdir(parents=True, exist_ok=True)
        if test:
            shots = self.directory / "game-test"
            shots.mkdir(exist_ok=True)
            environment.update(PS2X_SNAPSHOT_DIR=str(shots), PS2X_SNAPSHOT_SECONDS="5",
                               PS2X_EXIT_AT_FRAME="600")
        with (self.directory / "last-game.log").open("w", encoding="utf-8", errors="backslashreplace") as output:
            print(f"[launcher] version={VERSION} disc={SERIAL_BY_BOOT[boot.name]} "
                  f"prompts={environment['SOTC_BUTTON_PROMPTS']} f1Language={graphics.menu_language()} "
                  f"campaignLanguage={campaign_language}",
                  file=output, flush=True)
            if gpu_status:
                print(f"[gpu] {gpu_status}", file=output, flush=True)
            try:
                return subprocess.Popen([str(executable), str(boot)], cwd=self.player_directory,
                                        env=environment, stdout=output, stderr=subprocess.STDOUT,
                                        start_new_session=not WINDOWS and not attached, creationflags=PROCESS_FLAGS)
            except OSError as error:
                winerror = getattr(error, "winerror", None)
                code = f" winerror={winerror}" if winerror is not None else ""
                print(f"[launcher] could not start {executable}{code}: {error}", file=output, flush=True)
                raise

    def install_desktop_entry(self, applications=None):
        if WINDOWS:
            destination = Path(applications) if applications else (
                Path(os.environ["APPDATA"]) / "Microsoft/Windows/Start Menu/Programs")
            destination.mkdir(parents=True, exist_ok=True)
            link = destination / "Shadow of the Colossus PC.lnk"
            executable = self.program / "ShadowOfTheColossusPC.exe"
            if not executable.is_file():
                raise FileNotFoundError("ShadowOfTheColossusPC.exe")
            script = ("$shell = New-Object -ComObject WScript.Shell;"
                      "$link = $shell.CreateShortcut($env:SOTC_SHORTCUT);"
                      "$link.TargetPath = $env:SOTC_TARGET;"
                      "$link.WorkingDirectory = $env:LOCALAPPDATA;"
                      "$link.Save()")
            try:
                subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
                               env=dict(os.environ, SOTC_SHORTCUT=str(link), SOTC_TARGET=str(executable)),
                               creationflags=PROCESS_FLAGS, check=True, timeout=15)
            except subprocess.SubprocessError as exc:
                raise OSError("Não foi possível criar o atalho no menu Iniciar.") from exc
            return link
        destination = Path(applications) if applications else Path(
            os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share"))) / "applications"
        destination.mkdir(parents=True, exist_ok=True)
        icon = assets_directory() / "icon.png"
        if getattr(sys, "frozen", False):
            executable = Path(sys.executable).resolve()
            command = [str(executable)]
            canonical = Path.home() / ".local/opt/ShadowOfTheColossusPC"
            # Keep installed shortcuts on the update link, but only when that
            # link belongs to this exact running program. Portable copies and
            # other installed versions must retain their own entry point.
            try:
                entry = canonical / executable.name
                if (canonical.is_symlink() and canonical.resolve(strict=True) == executable.parent
                        and entry.resolve(strict=True) == executable):
                    command = [str(entry)]
                    try:
                        icon = canonical / icon.resolve().relative_to(executable.parent)
                    except ValueError:
                        pass  # one-file or externally supplied assets
            except (OSError, RuntimeError):
                pass  # absent, broken or cyclic installation link
        else:
            command = [sys.executable, str(SOURCE_ROOT / "tools/desktop/main.py")]
        def quote(value):
            return '"' + value.replace('%', '%%').replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$') + '"'
        path = destination / (APPLICATION_ID + ".desktop")
        path.write_text("[Desktop Entry]\nType=Application\nName=Shadow of the Colossus PC\n"
                        "Comment=Sua próxima aventura começa aqui\nTerminal=false\nCategories=Game;ActionGame;\n"
                        f"Exec={' '.join(map(quote, command))}\nIcon={icon}\n"
                        "StartupWMClass=shadow-of-the-colossus-pc\n")
        path.chmod(0o755)
        return path
