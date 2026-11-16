"""Find official release assets and stage verified launcher upgrades."""
from __future__ import annotations

import ctypes
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import ssl
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
import zipfile

from desktop_services import host_environment

NAME = "ShadowOfTheColossusPC"
REPOSITORY = f"GenissonEmilio/{NAME}"
RELEASES = f"https://api.github.com/repos/{REPOSITORY}/releases?per_page=20"
MAX_DOWNLOAD = 3 * 1024**3
MAX_EXPANDED = 8 * 1024**3
MAX_METADATA = 2 * 1024**2
STARTUP_MARKER = "launcher-ready"
LINUX_CA_BUNDLES = (
    Path("/etc/ssl/certs/ca-certificates.crt"),
    Path("/etc/pki/tls/certs/ca-bundle.crt"),
    Path("/etc/ssl/cert.pem"),
    Path("/etc/ssl/ca-bundle.pem"),
)


@dataclass(frozen=True)
class Update:
    version: str
    system: str
    name: str
    url: str
    size: int
    sha256: str


def platform_id():
    if platform.machine().lower() not in ("x86_64", "amd64"):
        return None
    if sys.platform.startswith("linux"):
        return "linux-x86_64"
    if sys.platform == "win32":
        return "windows-x86_64"
    return None


def release_version(tag):
    match = re.fullmatch(r"(?:(?:linux|windows)-)?v?(\d{1,5})\.(\d{1,5})\.(\d{1,5})", tag)
    return tuple(map(int, match.groups())) if match else None


def https_context():
    context = ssl.create_default_context()
    # The portable Linux launcher carries OpenSSL from its build distribution.
    # Its compiled CA paths can differ from the paths on the player's system.
    if sys.platform.startswith("linux") and not (os.environ.get("SSL_CERT_FILE")
                                                  or os.environ.get("SSL_CERT_DIR")):
        for bundle in LINUX_CA_BUNDLES:
            if bundle.is_file():
                context.load_verify_locations(cafile=str(bundle))
                break
    return context


def select_update(releases, current, system):
    old = release_version(current)
    if not old or system not in ("linux-x86_64", "windows-x86_64"):
        return None
    choices = []
    extension = "zip" if system.startswith("windows") else "tar.gz"
    for release in releases:
        if release.get("draft") or release.get("prerelease"):
            continue
        tag = release.get("tag_name", "")
        version = release_version(tag)
        if not version or version <= old:
            continue
        label = ".".join(map(str, version))
        name = f"{NAME}-{label}-{system}.{extension}"
        url = f"https://github.com/{REPOSITORY}/releases/download/{tag}/{name}"
        for asset in release.get("assets", ()):
            if (asset.get("state") != "uploaded" or asset.get("name") != name
                    or asset.get("browser_download_url") != url):
                continue
            digest = re.fullmatch(r"sha256:([0-9a-f]{64})", asset.get("digest") or "")
            size = asset.get("size")
            if digest and type(size) is int and 0 < size <= MAX_DOWNLOAD:
                choices.append((version, Update(label, system, name, url, size, digest[1])))
    return max(choices, default=(None, None), key=lambda choice: choice[0])[1]


def check_for_update(current):
    system = platform_id()
    if not system:
        return None
    request = Request(RELEASES, headers={"Accept": "application/vnd.github+json",
                                        "User-Agent": f"{NAME}/{current}"})
    with urlopen(request, timeout=5, context=https_context()) as response:
        data = response.read(MAX_METADATA + 1)
    if len(data) > MAX_METADATA:
        raise ValueError("GitHub release metadata exceeds the limit")
    releases = json.loads(data)
    if not isinstance(releases, list):
        raise ValueError("Invalid GitHub release response")
    return select_update(releases, current, system)


def download(update, destination, progress):
    request = Request(update.url, headers={"User-Agent": NAME})
    digest = hashlib.sha256()
    total = 0
    with urlopen(request, timeout=20, context=https_context()) as response, destination.open("xb") as output:
        if urlsplit(response.geturl()).scheme != "https":
            raise ValueError("The release download must use HTTPS")
        while block := response.read(1024 * 1024):
            total += len(block)
            if total > update.size:
                raise ValueError("The release download is larger than expected")
            digest.update(block)
            output.write(block)
            progress(total / update.size)
    if total != update.size or digest.hexdigest() != update.sha256:
        raise ValueError("The release download failed its SHA-256 check")


def package_path(name, system):
    parts = name.rstrip("/").split("/")
    invalid = any(part in ("", ".", "..") or "\\" in part or ":" in part or "\0" in part
                  for part in parts)
    if system.startswith("windows"):
        reserved = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)),
                    *(f"LPT{i}" for i in range(1, 10))}
        invalid |= any(part.rstrip(" .") != part or part.split(".")[0].upper() in reserved
                       for part in parts)
    if invalid or parts[0] != NAME:
        raise ValueError(f"Invalid path in update archive: {name}")
    return Path(*parts)


def unpack(archive, directory, system):
    seen = set()
    total = 0

    def copy_entry(name, is_directory, size, open_entry, mode):
        nonlocal total
        relative = package_path(name, system)
        key = relative.as_posix().casefold() if system.startswith("windows") else relative.as_posix()
        total += size
        if total > MAX_EXPANDED or len(seen) >= 100000 or key in seen:
            raise ValueError("The update archive exceeds its limits or contains duplicate entries")
        seen.add(key)
        target = directory / relative
        if is_directory:
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            with open_entry() as source, target.open("xb") as destination:
                shutil.copyfileobj(source, destination, 1024 * 1024)
            if system.startswith("linux"):
                target.chmod(0o755 if mode & 0o111 else 0o644)

    if system.startswith("windows"):
        with zipfile.ZipFile(archive) as bundle:
            for member in bundle.infolist():
                kind = stat.S_IFMT(member.external_attr >> 16)
                if kind not in (0, stat.S_IFDIR, stat.S_IFREG):
                    raise ValueError("The update archive contains a link or special file")
                copy_entry(member.filename, member.is_dir(), member.file_size,
                           lambda: bundle.open(member), 0)
    else:
        with tarfile.open(archive, "r|gz") as bundle:
            for member in bundle:
                if not (member.isfile() or member.isdir()):
                    raise ValueError("The update archive contains a link or special file")
                copy_entry(member.name, member.isdir(), member.size,
                           lambda: bundle.extractfile(member), member.mode)
    return directory / NAME


def launcher_filename(system):
    return NAME + (".exe" if system.startswith("windows") else "")


def installed_package(program, data, current, system):
    if data.is_relative_to(program):
        raise ValueError("Move your launcher data directory outside the application before updating")
    try:
        manifest = json.loads((program / "release.json").read_text(encoding="utf-8"))
    except FileNotFoundError:
        manifest = None
    if manifest != {"version": current, "platform": system}:
        raise ValueError("Install the official portable package before using automatic updates")
    if not (program / launcher_filename(system)).is_file():
        raise ValueError("The installed launcher executable is missing")


def stage_update(program, data, current, update, progress):
    installed_package(program, data, current, update.system)
    work = Path(tempfile.mkdtemp(prefix=".sotc-update-", dir=program.parent))
    try:
        archive = work / update.name
        download(update, archive, progress)
        progress(None)
        content = unpack(archive, work, update.system)
        installed_package(content, data, update.version, update.system)
        archive.unlink()
        return content
    except BaseException:
        shutil.rmtree(work)
        raise


def exchange_directories(first, second):
    # Keep the installation path valid throughout, even with symlinked shortcuts.
    libc = ctypes.CDLL(None, use_errno=True)
    rename = libc.renameat2
    rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    if rename(-100, os.fsencode(first), -100, os.fsencode(second), 2) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))


def restart_with_update(stage, program, data, current, system, assets):
    backup = program.with_name(f".{program.name}.previous-{current}-{stage.parent.name.rsplit('-', 1)[-1]}")
    executable = program / launcher_filename(system)
    environment = host_environment()
    environment["SOTC_DATA_DIR"] = str(data)
    environment["PYINSTALLER_RESET_ENVIRONMENT"] = "1"
    ready = stage.parent / STARTUP_MARKER
    environment["SOTC_UPDATE_READY"] = str(ready)
    if system.startswith("windows"):
        script = stage.parent / "apply-update.ps1"
        try:
            shutil.copyfile(assets / script.name, script)
            receipt = {"target": str(program), "staged": str(stage), "backup": str(backup),
                       "current": current, "lock": str(program.with_name(f".{program.name}.update.lock")),
                       "exe": str(executable), "data": str(data), "pid": os.getpid(),
                       "shimPid": int(os.environ.get("SOTC_LAUNCHER_PID", "0"))}
            (stage.parent / "update.json").write_text(json.dumps(receipt), encoding="utf-8")
            powershell = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
            subprocess.Popen([str(powershell), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                              "-File", str(script)], cwd=program.parent, env=environment,
                             creationflags=subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW)
        except Exception:
            shutil.rmtree(stage.parent, ignore_errors=True)
            raise
        return
    import fcntl
    lock_path = program.with_name(f".{program.name}.update.lock")
    applying = False
    try:
        with lock_path.open("a+b") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            installed_package(program, data, current, system)
            applying = True
            apply_linux_update(stage, program, backup, executable, environment, ready)
    except Exception:
        if not applying:
            shutil.rmtree(stage.parent, ignore_errors=True)
        raise


def apply_linux_update(stage, program, backup, executable, environment, ready):
    swapped = False
    try:
        exchange_directories(program, stage)
        swapped = True
        stage.rename(backup)
        child = subprocess.Popen([str(executable)], cwd=program.parent, env=environment, start_new_session=True)
        deadline = time.monotonic() + 30
        while not ready.is_file():
            if child.poll() is not None:
                raise RuntimeError("The updated launcher exited during startup")
            if time.monotonic() >= deadline:
                child.terminate()
                try:
                    child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
                raise RuntimeError("The updated launcher did not finish starting")
            time.sleep(0.1)
    except Exception:
        if swapped:
            # If rollback itself fails, preserve both directories for recovery.
            previous = backup if backup.exists() else stage
            exchange_directories(program, previous)
            if previous == backup:
                backup.rename(stage)
        shutil.rmtree(stage.parent, ignore_errors=True)
        raise
    shutil.rmtree(stage.parent, ignore_errors=True)


def confirm_startup(program):
    raw = os.environ.pop("SOTC_UPDATE_READY", "")
    if raw:
        marker = Path(raw).resolve()
        if (marker.name == STARTUP_MARKER and marker.parent.parent == program.parent
                and marker.parent.name.startswith(".sotc-update-")):
            with marker.open("x", encoding="ascii") as stream:
                stream.write("ready\n")
