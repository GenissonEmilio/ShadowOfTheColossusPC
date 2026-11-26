"""Prepare Gledson999's Brazilian Portuguese campaign as a separate USA disc view."""
from __future__ import annotations

from contextlib import ExitStack
import hashlib
import os
from pathlib import Path
import shutil
import sys
import tempfile
import zlib

from import_disc import ImportError_, PROFILES, incompatible_file, sha256, validate_disc
from language import translate as tr
from pipeline import build_lock

PORTUGUESE_PATCH_PAGE = "https://www.retro-jogos.com/ps2-shadow-of-the-colossus/"
PORTUGUESE_PATCH_CREDITS = "Gledson999 · Mumm-Ra (STR Brasil) · v1.0"
PATCH_SIZE = 1812074
PATCH_SHA256 = "d6a849c552b346d8f3357b18bc52ee7458245fc6e0ec8975ff89c1f6ca546546"
ISO_SIZE = 2693955584
SECTOR_SIZE = 2048
ISO_EXTENTS = {
    "MODULES2/MCMAN.IRX": (291, 96181),
    "MODULES2/MCSERV.IRX": (338, 7385),
    "MODULES/DBCMAN.IRX": (342, 15653),
    "MODULES/DS1O_D.IRX": (350, 10469),
    "MODULES/LIBSD.IRX": (356, 30085),
    "MODULES/MC2_D.IRX": (371, 20269),
    "MODULES/PL2303.IRX": (381, 4269),
    "MODULES/SIO2D.IRX": (384, 11289),
    "MODULES/SIO2MAN.IRX": (390, 6641),
    "MODULES/USBD.IRX": (394, 34993),
    "GAMECORE.XFF": (412, 2141864),
    "IOPRP300.IMG": (1458, 275345),
    "KERNEL.XFF": (1593, 415908),
    "MANAGER.XFF": (1797, 1650876),
    "NICO.DAT": (2604, 2667340678),
    "SCUS_974.72": (1305017, 242940),
    "SG2IOPM1.IRX": (1305136, 27661),
    "STARTUP.XFF": (1305150, 26),
    "SYSTEM.CNF": (1305151, 57),
}
USA = PROFILES["SCUS-97472"]
BOOT = USA.boot
MARKER = ".ptbr-patch"
REPLACEMENTS = {
    "GAMECORE.XFF": "48ec03d95b9ad2051f504f5a139ea67de392e9ebc5297e1f6f6f8dd7257c9f9a",
    "NICO.DAT": "11828b8608438630a7064a496043277befcb265be34e846033db03d03908a166",
}
# The first window also copies ISO filesystem metadata before the first file.
# Its checksum after zero-filling those unavailable sectors is pinned separately.
FIRST_WINDOW_CHECKSUM = 0xF93FBBAF
MARKER_VALUE = f"1 {PATCH_SHA256}"
MAX_WINDOW_SIZE = 8 * 1024**2
# The largest source segment in the published patch is 69,946,601 bytes.
MAX_SOURCE_SIZE = 70 * 1024**2
BLOCK_SIZE = 4096
REQUIRED_SIZES = {name: size for name, (size, _) in USA.fingerprints.items()}
REQUIRED_SIZES.update(USA.data_files)


def is_portuguese_ready(target):
    target = Path(target)
    try:
        return ((target / MARKER).read_text(encoding="ascii").strip() == MARKER_VALUE
                and (target / "SYSTEM.CNF").is_file()
                and all((target / name).is_file() and (target / name).stat().st_size == size
                        for name, size in REQUIRED_SIZES.items()))
    except (OSError, UnicodeError):
        return False


def _load_patch(path):
    try:
        with Path(path).open("rb") as source:
            patch = source.read(PATCH_SIZE + 1)
    except OSError as error:
        raise ImportError_(tr("Não foi possível abrir a tradução integrada.")) from error
    if len(patch) != PATCH_SIZE or hashlib.sha256(patch).hexdigest() != PATCH_SHA256:
        raise ImportError_(tr("O arquivo da tradução não corresponde à versão verificada."))
    return patch


def _number(data, position):
    value = 0
    while True:
        byte = data[position]
        position += 1
        value = (value << 7) | (byte & 127)
        if byte < 128:
            return value, position


def _code_table():
    # The RFC 3284 default table. Entries contain (operation, length, COPY mode).
    table = [(("run", 0, 0),)]
    table.extend((("add", size, 0),) for size in range(18))
    for mode in range(9):
        table.extend((("copy", size, mode),) for size in (0, *range(4, 19)))
    for mode in range(6):
        for add in range(1, 5):
            table.extend((("add", add, 0), ("copy", size, mode)) for size in range(4, 7))
    for mode in range(6, 9):
        for add in range(1, 5):
            table.append((("add", add, 0), ("copy", 4, mode)))
    table.extend((("copy", 4, mode), ("add", 1, 0)) for mode in range(9))
    return table


CODE_TABLE = _code_table()


def _invalid_patch():
    return ImportError_(tr("Não foi possível aplicar a tradução ao disco original."))


def _decode_window(source, literals, instructions, addresses, length):
    # Unchanged windows typically consist of one full-length COPY.
    if not literals and instructions[:1] == b"\x13" and addresses == b"\0":
        size, end = _number(instructions, 1)
        if size == length and end == len(instructions) and size <= len(source):
            return source[:size]

    output = bytearray()
    near = [0] * 4
    same = [0] * 768
    next_near = literal_pos = instruction_pos = address_pos = 0
    while instruction_pos < len(instructions):
        commands = CODE_TABLE[instructions[instruction_pos]]
        instruction_pos += 1
        for command, size, mode in commands:
            if not size:
                size, instruction_pos = _number(instructions, instruction_pos)
            if size > length - len(output):
                raise _invalid_patch()
            if command == "add":
                output += literals[literal_pos:literal_pos + size]
                literal_pos += size
            elif command == "run":
                output += literals[literal_pos:literal_pos + 1] * size
                literal_pos += 1
            else:
                if mode >= 6:
                    address = same[(mode - 6) * 256 + addresses[address_pos]]
                    address_pos += 1
                else:
                    address, address_pos = _number(addresses, address_pos)
                    if mode == 1:
                        address = len(source) + len(output) - address
                    elif mode >= 2:
                        address += near[mode - 2]
                if not 0 <= address < len(source) + len(output):
                    raise _invalid_patch()
                near[next_near] = address
                next_near = (next_near + 1) % len(near)
                same[address % len(same)] = address
                from_source = max(0, min(size, len(source) - address))
                if from_source:
                    output += source[address:address + from_source]
                    address += from_source
                    size -= from_source
                if size:
                    target_address = address - len(source)
                    previous = output[target_address:target_address + size]
                    if len(previous) < size:
                        previous = (previous * ((size + len(previous) - 1) // len(previous)))[:size]
                    output += previous
    if (len(output), literal_pos, address_pos) != (length, len(literals), len(addresses)):
        raise _invalid_patch()
    return output


def _read_extent(file, offset, length):
    file.seek(offset)
    data = file.read(length)
    if len(data) != length:
        raise incompatible_file(Path(file.name).name)
    return data


def _read_source(files, position, size):
    """Read a source range across imported ISO file extents and zero-filled gaps."""
    chunks = []
    end = position + size
    for start, stop, file in files:
        if stop <= position:
            continue
        if start >= end:
            break
        if position < start:
            chunks.append(bytes(start - position))
            position = start
        length = min(stop, end) - position
        chunks.append(_read_extent(file, position - start, length))
        position += length
    if position < end:
        chunks.append(bytes(end - position))
    return b"".join(chunks)


def _patch_windows(files, patch, ranges):
    # This pinned xdelta uses the default code table, an application header,
    # Adler-32 per window, and no secondary compression.
    if patch[:5] != b"\xd6\xc3\xc4\x00\x04":
        raise _invalid_patch()
    header_size, offset = _number(patch, 5)
    offset += header_size
    target_position = 0
    while offset < len(patch):
        flags = patch[offset]
        offset += 1
        if flags not in (4, 5):
            raise _invalid_patch()
        source_size = source_position = 0
        if flags & 1:
            source_size, offset = _number(patch, offset)
            source_position, offset = _number(patch, offset)
            if source_size > MAX_SOURCE_SIZE or source_position + source_size > ISO_SIZE:
                raise _invalid_patch()
        delta_size, offset = _number(patch, offset)
        end = offset + delta_size
        length, offset = _number(patch, offset)
        if (end > len(patch) or not 0 < length <= MAX_WINDOW_SIZE
                or target_position + length > ISO_SIZE or patch[offset] != 0):
            raise _invalid_patch()
        offset += 1
        data_size, offset = _number(patch, offset)
        instruction_size, offset = _number(patch, offset)
        address_size, offset = _number(patch, offset)
        checksum = int.from_bytes(patch[offset:offset + 4], "big")
        data_start = offset + 4
        instruction_start = data_start + data_size
        address_start = instruction_start + instruction_size
        if address_start + address_size != end:
            raise _invalid_patch()
        if any(start < target_position + length and target_position < stop for start, stop in ranges):
            source = _read_source(files, source_position, source_size)
            decoded = _decode_window(source, patch[data_start:instruction_start],
                                     patch[instruction_start:address_start], patch[address_start:end], length)
            if zlib.adler32(decoded) != (checksum if target_position else FIRST_WINDOW_CHECKSUM):
                raise _invalid_patch()
            yield target_position, decoded
        target_position += length
        offset = end
    if target_position != ISO_SIZE:
        raise _invalid_patch()


def _clone_nico(original, target):
    if not sys.platform.startswith("linux"):
        return False
    import fcntl

    # Linux FICLONE is copy-on-write. Neither edits nor cleanup change the imported disc.
    try:
        with original.open("rb") as source, target.open("xb") as output:
            fcntl.ioctl(output.fileno(), 0x40049409, source.fileno())
    except OSError:
        target.unlink(missing_ok=True)
        return False
    return True


def _apply_patch(original_disc, patch, disc, compare_nico, progress):
    with ExitStack() as stack:
        files = []
        ranges = {}
        for name, (sector, size) in ISO_EXTENTS.items():
            path = original_disc / name
            if not path.is_file() or path.stat().st_size != size:
                raise incompatible_file(name)
            start = sector * SECTOR_SIZE
            stop = start + size
            files.append((start, stop, stack.enter_context(path.open("rb"))))
            if name in REPLACEMENTS:
                ranges[name] = (start, stop)
        files.sort()
        outputs = {
            name: stack.enter_context((disc / name).open("r+b" if name == "NICO.DAT" and compare_nico else "xb"))
            for name in REPLACEMENTS
        }
        last_progress = -10
        for position, data in _patch_windows(files, patch, ranges.values()):
            for name, (start, stop) in ranges.items():
                left, right = max(start, position), min(stop, position + len(data))
                if left >= right:
                    continue
                window = memoryview(data)[left - position:right - position]
                file = outputs[name]
                if name == "NICO.DAT" and compare_nico:
                    file.seek(left - start)
                    original = file.read(len(window))
                    if original != window:
                        for offset in range(0, len(window), BLOCK_SIZE):
                            block = window[offset:offset + BLOCK_SIZE]
                            if original[offset:offset + BLOCK_SIZE] != block:
                                file.seek(left - start + offset)
                                file.write(block)
                else:
                    file.write(window)
            percent = min(99, (position + len(data)) * 100 // ISO_SIZE)
            if percent >= last_progress + 10:
                progress(tr("Aplicando tradução brasileira… {percent}%").format(percent=percent))
                last_progress = percent
    for name, digest in REPLACEMENTS.items():
        path = disc / name
        if path.stat().st_size != ISO_EXTENTS[name][1] or sha256(path) != digest:
            raise _invalid_patch()
    progress(tr("Aplicando tradução brasileira… {percent}%").format(percent=100))


def prepare_portuguese(original_disc, target, patch_path, progress=lambda _: None):
    """Create and validate a private translated disc from the imported USA files."""
    original_disc, target = Path(original_disc), Path(target)
    if original_disc.is_file():
        original_disc = original_disc.parent
    target.parent.mkdir(parents=True, exist_ok=True)
    with build_lock(target.parent / ".campaign-ptbr.lock"):
        if is_portuguese_ready(target):
            return target / BOOT
        if target.exists() or target.is_symlink():
            raise ImportError_(tr("A pasta de destino já existe. Os dados foram preservados."))
        progress(tr("Verificando a mídia original da campanha…"))
        if validate_disc(original_disc) != USA:
            raise ImportError_(tr("A tradução precisa da edição americana SCUS-97472."))
        patch = _load_patch(patch_path)
        with tempfile.TemporaryDirectory(prefix=".campaign-ptbr-", dir=target.parent) as temporary:
            work = Path(temporary)
            disc = work / "disc"
            shutil.copytree(original_disc, disc,
                            ignore=shutil.ignore_patterns(*REPLACEMENTS, "mc0", "mc1"))
            compare_nico = _clone_nico(original_disc / "NICO.DAT", disc / "NICO.DAT")
            _apply_patch(original_disc, patch, disc, compare_nico, progress)
            (disc / MARKER).write_text(MARKER_VALUE + "\n", encoding="ascii")
            os.replace(disc, target)
    return target / BOOT
