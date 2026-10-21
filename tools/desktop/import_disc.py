#!/usr/bin/env python3
"""Import an owned SotC USA ISO or nested 7Z/RAR/ZIP, without executing archive contents."""
from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tempfile
import sys
import zipfile

from disc import DiscImage, GameFilesError

ROOT = Path(__file__).resolve().parents[2]
MAX_BYTES = 8 * 1024**3
MAX_ENTRIES = 4096
MAX_DEPTH = 3


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
    if not path.parts or "\x00" in name:
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
    if not name:
        raise ImportError_("RAR import needs libarchive (Linux: libarchive; Ubuntu: libarchive13). "
                           "Alternatively unpack the RAR yourself and import its IMG/BIN file.")
    lib = ctypes.CDLL(name)
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
        if lib.archive_read_open_filename(archive, os.fsencode(source), 65536) != 0:
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
            name = lib.archive_entry_pathname(entry)
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
        raise ImportError_("Escolha ISO, ZIP, RAR ou 7Z do Shadow of the Colossus USA.")
    if depth >= MAX_DEPTH:
        raise ImportError_("Há arquivos compactados demais dentro de outros arquivos.")
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


def inspect_disc(source):
    with DiscImage(source) as disc:
        if (disc.sector, disc.skip) != (2048, 0):
            raise ImportError_("Escolha uma imagem DVD ISO com setores de 2048 bytes.")
        cnf = disc.find("SYSTEM.CNF")
        if not cnf or b"SCUS_974.72" not in disc.read(*cnf):
            raise ImportError_("Edição incompatível. É necessário SCUS-97472 (USA).")
        for name, (size, digest) in EXPECTED.items():
            loc = disc.find(name)
            if not loc or loc[1] != size or hashlib.sha256(disc.read(*loc)).hexdigest() != digest:
                raise ImportError_(f"{name}: edição incompatível ou arquivo modificado.")
        data = disc.find("NICO.DAT")
        if not data or data[1] != 2667340678:
            raise ImportError_("Os dados NICO.DAT estão ausentes ou incompatíveis.")
        disc.read_sector(data[0] + (data[1]-1)//2048)
        return {"serial": "SCUS-97472"}


def extract_iso(image, destination):
    """Copy bounded ISO9660 extents; never interpret archive documents/code."""
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    with DiscImage(image) as disc:
        root = disc.read_sector(16)[156:190]
        budget = Budget()
        import struct
        def copy_directory(lba, size, folder, depth=0):
            if depth > 8:
                raise ImportError_("Diretórios ISO profundos demais.")
            for name, start, length, directory in disc._directory(lba, size):
                if name in ("\x00", "\x01"):
                    continue
                target = folder / safe_name(name)
                budget.entry(length)
                if directory:
                    target.mkdir(parents=True, exist_ok=True)
                    copy_directory(start, length, target, depth+1)
                else:
                    disc.read_sector(start + max(0, (length-1)//2048))
                    with target.open("xb") as out:
                        disc.file.seek(start*2048)
                        remaining = length
                        while remaining:
                            chunk = disc.file.read(min(1024*1024, remaining))
                            if not chunk:
                                raise ImportError_("ISO truncada.")
                            budget.consume(len(chunk))
                            out.write(chunk)
                            remaining -= len(chunk)
        copy_directory(struct.unpack_from("<I",root,2)[0], struct.unpack_from("<I",root,10)[0], destination)


def import_disc(source, destination):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".import-", dir=destination.parent) as temporary:
        work = Path(temporary)
        images = list(candidates(source, work, Budget()))
        if len(images) != 1:
            raise ImportError_(f"Esperado exatamente um disco; encontrados {len(images)}.")
        inspect_disc(images[0])
        extract_iso(images[0], work / "disc")
        if destination.exists():
            raise ImportError_("A pasta de destino já existe. Os dados foram preservados.")
        os.replace(work / "disc", destination)
    return destination / "SCUS_974.72"
