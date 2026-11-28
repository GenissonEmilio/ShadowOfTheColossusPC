"""Find official release assets and stage verified launcher upgrades."""
from __future__ import annotations

import ctypes
from dataclasses import dataclass
import hashlib
from http.client import IncompleteRead
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
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen
import zipfile

from desktop_services import host_environment
from model import PORTABLE_DATA_NAME, PORTABLE_MARKER_NAME

NAME = "ShadowOfTheColossusPC"
REPOSITORY = f"GenissonEmilio/{NAME}"
RELEASES = f"https://api.github.com/repos/{REPOSITORY}/releases?per_page=20"
MANIFEST = f"https://github.com/{REPOSITORY}/releases/latest/download/update-manifest.json"
MANIFEST_HOSTS = ("github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com")
MAX_DOWNLOAD = 3 * 1024**3
DOWNLOAD_ATTEMPTS = 3
DOWNLOAD_BLOCK = 1024 * 1024
MAX_EXPANDED = 8 * 1024**3
MAX_METADATA = 2 * 1024**2
STARTUP_MARKER = "launcher-ready"
PORTABLE_ENTRIES = {PORTABLE_DATA_NAME: stat.S_IFDIR, PORTABLE_MARKER_NAME: stat.S_IFREG}
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


class RateLimitedError(RuntimeError):
    pass


def platform_id():
    if platform.machine().lower() not in ("x86_64", "amd64"):
        return None
    if sys.platform.startswith("linux"):
        return "linux-x86_64"
    if sys.platform == "win32":
        return "windows-x86_64"
    return None


def release_version(tag):
    if not isinstance(tag, str):
        return None
    match = re.fullmatch(r"(?:(?:linux|windows)-)?v?([0-9]{1,5})\.([0-9]{1,5})\.([0-9]{1,5})", tag)
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


def release_package(release, version, system, *, strict=False):
    label = ".".join(map(str, version))
    extension = "zip" if system.startswith("windows") else "tar.gz"
    name = f"{NAME}-{label}-{system}.{extension}"
    url = f"https://github.com/{REPOSITORY}/releases/download/{release['tag_name']}/{name}"
    assets = release.get("assets")
    if not isinstance(assets, list):
        return None
    candidates = [asset for asset in assets if isinstance(asset, dict) and asset.get("name") == name]
    if strict and len(candidates) > 1:
        raise ValueError("GitHub update manifest contains duplicate packages")
    for asset in candidates:
        digest = asset.get("digest")
        size = asset.get("size")
        if (asset.get("state") == "uploaded" and asset.get("browser_download_url") == url
                and isinstance(digest, str) and re.fullmatch(r"sha256:[0-9a-f]{64}", digest)
                and type(size) is int and 0 < size <= MAX_DOWNLOAD):
            return Update(label, system, name, url, size, digest.removeprefix("sha256:"))
    if strict and candidates:
        raise ValueError("GitHub update manifest contains invalid package metadata")
    return None


def select_update(releases, current, system):
    old = release_version(current)
    if not old or system not in ("linux-x86_64", "windows-x86_64"):
        return None
    choices = []
    for release in releases:
        if not isinstance(release, dict) or release.get("draft") or release.get("prerelease"):
            continue
        version = release_version(release.get("tag_name"))
        if not version or version <= old:
            continue
        package = release_package(release, version, system)
        if package:
            choices.append((version, package))
    return max(choices, default=(None, None), key=lambda choice: choice[0])[1]


def github_metadata(url, current, context, *, accept, hosts):
    request = Request(url, headers={"Accept": accept, "User-Agent": f"{NAME}/{current}"})
    try:
        with urlopen(request, timeout=5, context=context) as response:
            origin = urlsplit(response.geturl())
            if origin.scheme != "https" or origin.netloc not in hosts:
                raise ValueError("GitHub release metadata must come from GitHub over HTTPS")
            data = response.read(MAX_METADATA + 1)
    except HTTPError as error:
        with error:
            if (error.code == 429 or (error.code == 403 and (
                    error.headers.get("X-RateLimit-Remaining") == "0"
                    or error.headers.get("Retry-After")
                    or b"rate limit" in error.read(4096).lower()))):
                raise RateLimitedError(f"GitHub rate limit reached (HTTP {error.code})") from error
        raise
    if len(data) > MAX_METADATA:
        raise ValueError("GitHub release metadata exceeds the limit")
    return json.loads(data)


def check_for_update(current):
    system = platform_id()
    installed = release_version(current)
    if not system or not installed:
        return None
    context = https_context()
    try:
        latest = github_metadata(MANIFEST, current, context, accept="application/json", hosts=MANIFEST_HOSTS)
    except HTTPError as error:
        if error.code != 404:
            raise
    else:
        version = release_version(latest.get("tag_name")) if isinstance(latest, dict) else None
        if (not version or latest.get("draft") is not False or latest.get("prerelease") is not False
                or not isinstance(latest.get("assets"), list)
                or not all(isinstance(asset, dict) for asset in latest["assets"])):
            raise ValueError("Invalid GitHub update manifest")
        package = release_package(latest, version, system, strict=True)
        if version <= installed:
            return None
        if package:
            return package
    releases = github_metadata(RELEASES, current, context,
                               accept="application/vnd.github+json", hosts=("api.github.com",))
    if not isinstance(releases, list):
        raise ValueError("Invalid GitHub release response")
    return select_update(releases, current, system)


def download(update, destination, progress):
    context = https_context()
    digest = hashlib.sha256()
    total = reported = 0
    allow_range = True
    with destination.open("xb") as output:
        for attempt in range(DOWNLOAD_ATTEMPTS):
            last_attempt = attempt + 1 == DOWNLOAD_ATTEMPTS
            offset = total if allow_range and total < update.size else 0
            headers = {"User-Agent": NAME, "Accept-Encoding": "identity"}
            if offset:
                headers["Range"] = f"bytes={offset}-"
            request = Request(update.url, headers=headers)
            try:
                with urlopen(request, timeout=20, context=context) as response:
                    if urlsplit(response.geturl()).scheme != "https":
                        raise ValueError("The release download must use HTTPS")
                    if response.headers.get("Content-Encoding", "identity").lower() != "identity":
                        raise ValueError("Unexpected encoding in the release download")
                    if response.status == 206:
                        expected_range = f"bytes {offset}-{update.size - 1}/{update.size}"
                        if not offset or response.headers.get("Content-Range") != expected_range:
                            if not offset or last_attempt:
                                raise ValueError("Invalid Content-Range in the release download")
                            allow_range = False
                            continue
                    elif response.status == 200:
                        if total:
                            # A server may ignore Range and send the entire asset.
                            output.seek(0)
                            output.truncate()
                            digest = hashlib.sha256()
                            total = reported = 0
                            progress(0)
                        allow_range = True
                    else:
                        raise ValueError(f"Unexpected HTTP status in the release download: {response.status}")

                    # read1 keeps bytes already received if the following read times out.
                    while block := response.read1(DOWNLOAD_BLOCK):
                        if total + len(block) > update.size:
                            raise ValueError("The release download is larger than expected")
                        output.write(block)
                        digest.update(block)
                        total += len(block)
                        if not reported or total - reported >= DOWNLOAD_BLOCK or total == update.size:
                            progress(total / update.size)
                            reported = total
                if total != update.size:
                    raise EOFError(f"The release download ended after {total} of {update.size} bytes")
                break
            except HTTPError as error:
                with error:
                    if last_attempt:
                        raise
                    if error.code == 416 and offset:
                        allow_range = False
                    elif error.code not in (408, 500, 502, 503, 504):
                        raise
            except (ConnectionError, EOFError, IncompleteRead, TimeoutError, URLError, ssl.SSLEOFError) as error:
                if last_attempt or (isinstance(error, URLError)
                                    and isinstance(error.reason, ssl.SSLCertVerificationError)):
                    raise
            time.sleep(2**attempt)
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
    if len(parts) > 1:
        entry = parts[1].casefold() if system.startswith("windows") else parts[1]
        if entry in PORTABLE_ENTRIES:
            raise ValueError(f"The update archive contains a reserved portable path: {name}")
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
    if data.is_relative_to(program) and not data.is_relative_to(program / PORTABLE_DATA_NAME):
        raise ValueError("Move your launcher data to the portable data directory or outside the application before updating")
    try:
        manifest = json.loads((program / "release.json").read_text(encoding="utf-8"))
    except FileNotFoundError:
        manifest = None
    if manifest != {"version": current, "platform": system}:
        raise ValueError("Install the official portable package before using automatic updates")
    if not (program / launcher_filename(system)).is_file():
        raise ValueError("The installed launcher executable is missing")


def portable_entries(directory, *, require_empty=False):
    entries = []
    for name, kind in PORTABLE_ENTRIES.items():
        path = directory / name
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            continue
        if require_empty:
            raise ValueError(f"The staged update contains a reserved portable path: {path}")
        if (stat.S_IFMT(metadata.st_mode) != kind
                or getattr(metadata, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT):
            raise ValueError(f"Invalid portable path in the installation: {path}")
        entries.append(name)
    return entries


def stage_update(program, data, current, update, progress):
    installed_package(program, data, current, update.system)
    portable_entries(program)
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


def linux_rename(first, second, flags):
    libc = ctypes.CDLL(None, use_errno=True)
    rename = libc.renameat2
    rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    if rename(-100, os.fsencode(first), -100, os.fsencode(second), flags) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), first, None, second)


def exchange_directories(first, second):
    # Keep the installation path valid throughout, even with symlinked shortcuts.
    linux_rename(first, second, 2)  # RENAME_EXCHANGE


def move_without_replacing(first, second):
    linux_rename(first, second, 1)  # RENAME_NOREPLACE


def restart_with_update(stage, program, data, current, system, assets, *, preserve_data_override=True):
    backup = program.with_name(f".{program.name}.previous-{current}-{stage.parent.name.rsplit('-', 1)[-1]}")
    executable = program / launcher_filename(system)
    environment = host_environment()
    if preserve_data_override:
        environment["SOTC_DATA_DIR"] = str(data)
    else:
        environment.pop("SOTC_DATA_DIR", None)
    environment["PYINSTALLER_RESET_ENVIRONMENT"] = "1"
    ready = stage.parent / STARTUP_MARKER
    environment["SOTC_UPDATE_READY"] = str(ready)
    if system.startswith("windows"):
        script = stage.parent / "apply-update.ps1"
        try:
            portable_entries(stage, require_empty=True)
            shutil.copyfile(assets / script.name, script)
            receipt = {"target": str(program), "staged": str(stage), "backup": str(backup),
                       "current": current, "lock": str(program.with_name(f".{program.name}.update.lock")),
                       "exe": str(executable), "data": str(data), "pid": os.getpid(),
                       "portableData": PORTABLE_DATA_NAME, "portableMarker": PORTABLE_MARKER_NAME,
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
            entries = portable_entries(program)
            portable_entries(stage, require_empty=True)
            applying = True
            apply_linux_update(stage, program, backup, executable, environment, ready, entries)
    except Exception:
        if not applying:
            shutil.rmtree(stage.parent, ignore_errors=True)
        raise


def apply_linux_update(stage, program, backup, executable, environment, ready, entries):
    swapped = False
    previous = stage
    moved = []
    child = None
    try:
        exchange_directories(program, stage)
        swapped = True
        for name in entries:
            move_without_replacing(stage / name, program / name)
            moved.append(name)
        move_without_replacing(stage, backup)
        previous = backup
        child = subprocess.Popen([str(executable)], cwd=program.parent, env=environment, start_new_session=True)
        deadline = time.monotonic() + 30
        while not ready.is_file():
            if child.poll() is not None:
                raise RuntimeError("The updated launcher exited during startup")
            if time.monotonic() >= deadline:
                raise RuntimeError("The updated launcher did not finish starting")
            time.sleep(0.1)
    except Exception:
        if swapped:
            if child is not None:
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()
                # The replacement may have created a previously absent profile.
                moved = portable_entries(program)
            # If rollback itself fails, preserve both directories for recovery.
            for name in reversed(moved):
                move_without_replacing(program / name, previous / name)
            exchange_directories(program, previous)
            if previous == backup:
                move_without_replacing(backup, stage)
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
