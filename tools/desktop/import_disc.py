#!/usr/bin/env python3
"""Import an owned SotC ISO or nested 7Z/RAR/ZIP, without executing archive contents."""
from __future__ import annotations
from language import translate as tr

import argparse
import ctypes
import ctypes.util
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import stat
import tempfile
import sys
import zipfile

from disc import DiscImage

ROOT = Path(__file__).resolve().parents[2]
MAX_BYTES = 8 * 1024**3
MAX_ENTRIES = 4096
MAX_DEPTH = 3
MAX_CONFIG_BYTES = 4096
BOOT_PATH = re.compile(rb"cdrom0:[\\/]([a-z]{4}_[0-9]{3}\.[0-9]{2})(?:;1)?", re.IGNORECASE)


class ImportError_(ValueError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def safe_name(name: str) -> Path:
    path = PurePosixPath(name.replace("\\", "/"))
    if not name or path.is_absolute() or ".." in path.parts or any(":" in p for p in path.parts):
        raise ImportError_(f"Archive entry outside its folder: {name!r}")
    if not path.parts or "\x00" in name or (os.name == "nt" and any(os.path.isreserved(p) for p in path.parts)):
        raise ImportError_(f"Invalid archive entry: {name!r}")
    return Path(*path.parts)


class Budget:
    def __init__(self, maximum=MAX_BYTES):
        self.remaining = maximum
        self.entries = MAX_ENTRIES

    def entry(self, size):
        self.entries -= 1
        if size < 0 or size > self.remaining or self.entries < 0:
            raise ImportError_("Archive exceeds the import size or entry limit")

    def consume(self, size):
        self.remaining -= size
        if self.remaining < 0:
            raise ImportError_("Archive exceeds the import size limit")


def extract_zip(source, destination, budget):
    seen = set()
    with zipfile.ZipFile(source) as archive:
        for entry in archive.infolist():
            relative = safe_name(entry.filename)
            key = relative.as_posix().casefold()
            if key in seen:
                raise ImportError_(f"Duplicate archive path: {relative}")
            seen.add(key)
            mode = entry.external_attr >> 16
            if stat.S_ISLNK(mode):
                raise ImportError_(f"Archive links are not accepted: {relative}")
            budget.entry(entry.file_size)
            target = destination / relative
            if entry.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry) as stream, target.open("xb") as out:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    budget.consume(len(block))
                    out.write(block)


def libarchive():
    bundled = Path(getattr(sys, "_MEIPASS", "")) / "libarchive.so.13"
    name = str(bundled) if getattr(sys, "frozen", False) and bundled.is_file() else ctypes.util.find_library("archive")
    if os.name == "nt":
        prefix = Path(sys.executable).parent
        name = next(prefix.glob("libarchive-*.dll"), None)
    if not name:
        raise ImportError_("RAR import needs libarchive (Linux: libarchive; Ubuntu: libarchive13). "
                           "Alternatively unpack the RAR yourself and import its IMG/BIN file.")
    lib = ctypes.CDLL(str(name))
    declarations = {
        "archive_read_new": (ctypes.c_void_p, []),
        "archive_read_support_filter_all": (ctypes.c_int, [ctypes.c_void_p]),
        "archive_read_support_format_all": (ctypes.c_int, [ctypes.c_void_p]),
        "archive_read_open_filename": (ctypes.c_int, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]),
        "archive_read_next_header": (ctypes.c_int, [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]),
        "archive_entry_pathname": (ctypes.c_char_p, [ctypes.c_void_p]),
        "archive_entry_size": (ctypes.c_longlong, [ctypes.c_void_p]),
        "archive_entry_filetype": (ctypes.c_uint, [ctypes.c_void_p]),
        "archive_entry_hardlink": (ctypes.c_char_p, [ctypes.c_void_p]),
        "archive_read_data": (ctypes.c_ssize_t, [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]),
        "archive_error_string": (ctypes.c_char_p, [ctypes.c_void_p]),
        "archive_read_free": (ctypes.c_int, [ctypes.c_void_p]),
    }
    if os.name == "nt":
        declarations["archive_read_open_filename_w"] = (ctypes.c_int, [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_size_t])
        declarations["archive_entry_pathname_w"] = (ctypes.c_wchar_p, [ctypes.c_void_p])
    for name, (result, arguments) in declarations.items():
        function = getattr(lib, name)
        function.restype, function.argtypes = result, arguments
    return lib


def extract_archive(source, destination, budget):
    lib = libarchive()
    archive = lib.archive_read_new()
    if not archive:
        raise ImportError_("Cannot allocate archive reader")
    def fail():
        message = lib.archive_error_string(archive)
        raise ImportError_(message.decode("utf-8", "replace") if message else "Cannot read RAR")
    try:
        lib.archive_read_support_filter_all(archive)
        lib.archive_read_support_format_all(archive)
        if os.name == "nt":
            code = lib.archive_read_open_filename_w(archive, str(source), 65536)
        else:
            code = lib.archive_read_open_filename(archive, os.fsencode(source), 65536)
        if code != 0:
            fail()
        entry = ctypes.c_void_p()
        buffer = ctypes.create_string_buffer(1024 * 1024)
        seen = set()
        while True:
            code = lib.archive_read_next_header(archive, ctypes.byref(entry))
            if code == 1:  # ARCHIVE_EOF
                break
            if code != 0:
                fail()
            name = (lib.archive_entry_pathname_w(entry) if os.name == "nt"
                    else lib.archive_entry_pathname(entry))
            if not name:
                raise ImportError_("Archive entry has no name")
            relative = safe_name(os.fsdecode(name))
            key = relative.as_posix().casefold()
            if key in seen:
                raise ImportError_(f"Duplicate archive path: {relative}")
            seen.add(key)
            kind = lib.archive_entry_filetype(entry)
            if kind not in (stat.S_IFREG, stat.S_IFDIR) or lib.archive_entry_hardlink(entry):
                raise ImportError_(f"Archive links or special files are not accepted: {relative}")
            budget.entry(lib.archive_entry_size(entry))
            target = destination / relative
            if kind == stat.S_IFDIR:
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open("xb") as out:
                while True:
                    count = lib.archive_read_data(archive, buffer, len(buffer))
                    if count == 0:
                        break
                    if count < 0:
                        fail()
                    budget.consume(count)
                    out.write(buffer.raw[:count])
    finally:
        lib.archive_read_free(archive)


def candidates(source, work, budget, depth=0):
    if source.suffix.lower() == ".iso":
        yield source
        return
    if source.suffix.lower() not in (".zip", ".rar", ".7z"):
        raise ImportError_(tr("Escolha ISO, ZIP, RAR ou 7Z do Shadow of the Colossus."))
    if depth >= MAX_DEPTH:
        raise ImportError_(tr("Há arquivos compactados demais dentro de outros arquivos."))
    stage = Path(tempfile.mkdtemp(prefix="archive-", dir=work))
    (extract_zip if zipfile.is_zipfile(source) else extract_archive)(source, stage, budget)
    for path in sorted(stage.rglob("*")):
        if path.is_file() and path.suffix.lower() in (".iso", ".zip", ".rar", ".7z"):
            yield from candidates(path, work, budget, depth + 1)


EXPECTED = {
    "SCUS_974.72": (242940, "d16ad76ba8cdf87695d94b1316d51c1f248959ec0749814c31bb7a2d55d9a2e1"),
    "KERNEL.XFF": (415908, "cbc58929d8d48fa3968df25a2e80a6a730d4cd61107c15363d6e435c9681e604"),
    "MANAGER.XFF": (1650876, "0545ecfb199c78229f02df7a1a4304101e86f8bd8f823047787969a087caa6e9"),
    "GAMECORE.XFF": (2141864, "fba772ac8c793281af15be204243098b999e91b229ad3074a39a9773a56676f9"),
}


@dataclass(frozen=True)
class DiscProfile:
    serial: str
    boot: str
    fingerprints: dict[str, tuple[int, str]]
    data_files: dict[str, int]

    @property
    def data_size(self):
        return sum(self.data_files.values())


PROFILES = {
    "SCUS-97472": DiscProfile("SCUS-97472", "SCUS_974.72", EXPECTED, {"NICO.DAT": 2667340678}),
    "SCES-53326": DiscProfile("SCES-53326", "SCES_533.26", {
        "SCES_533.26": (229888, "851f0c4f48b0b4bb61e89fb7e97d4e9b4ac4c2ea0ff143d1d95e18337ea8b57c"),
        "KERNEL.XFF": (419668, "d83a2d4d7631ed740946849b1b4ba32620891fc41817bb16175fedb9f9cd6463"),
        "MANAGER.XFF": (1732264, "2de4d80eed7ff6c89b34b587ee1ced8792921e2a7c80d7e92a4fbb1481f1461a"),
        "GAMECORE.XFF": (2157144, "aa5a72b239b36da0bfc9f435bf31441057b4f4b8a01405b94b0e3b212fd242e0"),
    }, {"NICO.DAT": 1073741824, "XAB.": 1073741824, "XAC.": 1073741824,
        "XAD.": 1073741824, "XAE.": 314316264}),
}


def profile_from_config(configuration: bytes) -> DiscProfile:
    if len(configuration) > MAX_CONFIG_BYTES:
        raise ImportError_(tr("O arquivo SYSTEM.CNF está ausente ou inválido."))
    boot_values = []
    for line in configuration.splitlines():
        name, separator, value = line.partition(b"=")
        if separator and name.strip().upper() == b"BOOT2":
            boot_values.append(value.strip())
    boot = BOOT_PATH.fullmatch(boot_values[0]) if len(boot_values) == 1 else None
    if not boot:
        raise ImportError_(tr("O arquivo SYSTEM.CNF está ausente ou inválido."))
    name = boot[1].decode("ascii").upper()
    serial = name.replace("_", "-").replace(".", "")
    profile = PROFILES.get(serial)
    if not profile:
        raise ImportError_(tr("Edição incompatível. Use SCUS-97472 (USA) ou SCES-53326 (PAL)."))
    return profile


def identify_disc(directory) -> DiscProfile:
    with (Path(directory) / "SYSTEM.CNF").open("rb") as stream:
        return profile_from_config(stream.read(MAX_CONFIG_BYTES + 1))


def incompatible_file(name):
    return ImportError_(tr("{name}: edição incompatível ou arquivo modificado.").format(name=name))


def incompatible_data(name):
    return ImportError_(tr("{name}: dados de disco ausentes ou incompatíveis.").format(name=name))


def validate_disc(directory) -> DiscProfile:
    directory = Path(directory)
    profile = identify_disc(directory)
    for name, (size, digest) in profile.fingerprints.items():
        path = directory / name
        if not path.is_file() or path.stat().st_size != size or sha256(path) != digest:
            raise incompatible_file(name)
    data = directory / "NICO.DAT"
    if not data.is_file() or data.stat().st_size != profile.data_size:
        raise incompatible_data(data.name)
    return profile


def payload_sector(disc, profile):
    start = end = None
    for name, size in profile.data_files.items():
        extent = disc.find(name)
        if not extent or extent[1] != size or (end is not None and extent[0] != end):
            raise incompatible_data(name)
        sector, _ = extent
        if start is None:
            start = sector
        end = sector + (size + 2047) // 2048
        disc.read_sector(end - 1)
    return start


def inspect_disc(source):
    with DiscImage(source) as disc:
        if (disc.sector, disc.skip) != (2048, 0):
            raise ImportError_(tr("Escolha uma imagem DVD ISO com setores de 2048 bytes."))
        cnf = disc.find("SYSTEM.CNF")
        configuration = disc.read(*cnf) if cnf and cnf[1] <= MAX_CONFIG_BYTES else b""
        profile = profile_from_config(configuration)
        for name, (size, digest) in profile.fingerprints.items():
            loc = disc.find(name)
            if not loc or loc[1] != size or hashlib.sha256(disc.read(*loc)).hexdigest() != digest:
                raise incompatible_file(name)
        payload_sector(disc, profile)
        return {"serial": profile.serial, "boot": profile.boot}


def extract_iso(image, destination, profile=None):
    """Copy bounded ISO9660 extents; never interpret archive documents/code."""
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    split_payload = profile.data_files if profile and len(profile.data_files) > 1 else {}
    with DiscImage(image) as disc:
        root = disc.read_sector(16)[156:190]
        budget = Budget()
        import struct

        def copy_file(start, length, target):
            disc.read_sector(start + max(0, (length - 1) // 2048))
            with target.open("xb") as out:
                disc.file.seek(start * 2048)
                remaining = length
                while remaining:
                    chunk = disc.file.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise ImportError_(tr("ISO truncada."))
                    budget.consume(len(chunk))
                    out.write(chunk)
                    remaining -= len(chunk)

        def copy_directory(lba, size, folder, depth=0):
            if depth > 8:
                raise ImportError_(tr("Diretórios ISO profundos demais."))
            for name, start, length, directory in disc._directory(lba, size):
                if name in ("\x00", "\x01") or (depth == 0 and name in split_payload):
                    continue
                target = folder / safe_name(name)
                budget.entry(length)
                if directory:
                    target.mkdir(parents=True, exist_ok=True)
                    copy_directory(start, length, target, depth+1)
                else:
                    copy_file(start, length, target)

        copy_directory(struct.unpack_from("<I",root,2)[0], struct.unpack_from("<I",root,10)[0], destination)
        if split_payload:
            # PAL streams the sectors following NICO.DAT as part of the same archive.
            budget.entry(profile.data_size)
            copy_file(payload_sector(disc, profile), profile.data_size, destination / "NICO.DAT")


def import_disc(source, destination):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".import-", dir=destination.parent) as temporary:
        work = Path(temporary)
        images = list(candidates(source, work, Budget()))
        if len(images) != 1:
            raise ImportError_(tr("Esperado exatamente um disco; encontrados {count}.").format(count=len(images)))
        selected = inspect_disc(images[0])
        extract_iso(images[0], work / "disc", PROFILES[selected["serial"]])
        if destination.exists():
            raise ImportError_(tr("A pasta de destino já existe. Os dados foram preservados."))
        os.replace(work / "disc", destination)
    return destination / selected["boot"]
