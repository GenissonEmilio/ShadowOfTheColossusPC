"""ISO9660 reader adapted from the ForbiddenMemoriesPC desktop importer."""
import os
import struct
from pathlib import Path

class GameFilesError(ValueError):
    pass

class DiscImage:
    """A CD image of 2352-byte raw sectors or 2048-byte ones, read through
    its ISO 9660 directories."""

    def __init__(self, path):
        self.path = Path(path)
        self.file = open(self.path, "rb")
        size = os.fstat(self.file.fileno()).st_size
        for sector, skip in ((2352, 24), (2352, 16), (2048, 0)):
            if size % sector or size < 17 * sector:
                continue
            self.sector, self.skip = sector, skip
            head = self.read_sector(16)
            if head[1:6] == b"CD001":
                break
        else:
            self.file.close()
            raise GameFilesError(f"{self.path.name} is not a CD image this editor can read "
                                 "(a raw .bin of 2352-byte sectors, or an ISO)")

    def close(self):
        self.file.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def read_sector(self, lba: int) -> bytes:
        self.file.seek(lba * self.sector + self.skip)
        data = self.file.read(2048)
        if len(data) != 2048:
            raise GameFilesError(f"{self.path.name} ends at sector {lba}")
        return data

    def read(self, lba: int, size: int) -> bytes:
        if size < 0 or size > 16 * 1024 * 1024:
            raise GameFilesError("Disc directory or executable exceeds its size limit")
        out = bytearray()
        for i in range((size + 2047) // 2048):
            out += self.read_sector(lba + i)
        return bytes(out[:size])

    def _directory(self, lba: int, length: int):
        data = self.read(lba, length)
        at = 0
        while at < len(data):
            record = data[at]
            if not record:
                at = (at // 2048 + 1) * 2048
                continue
            if record < 34 or at + record > len(data) or data[at + 32] > record - 33:
                raise GameFilesError("Malformed ISO directory record")
            name = data[at + 33:at + 33 + data[at + 32]].decode("ascii", "replace")
            start, size = struct.unpack_from("<I", data, at + 2)[0], struct.unpack_from("<I", data, at + 10)[0]
            flags = data[at + 25]
            yield name.split(";")[0].upper(), start, size, bool(flags & 2)
            at += record

    def find(self, path: str):
        """(lba, size) of a file such as "DATA/WA_MRG.MRG", or None."""
        root = self.read_sector(16)[156:156 + 34]
        lba, length = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
        parts = [p.upper() for p in path.replace("\\", "/").split("/") if p]
        for i, part in enumerate(parts):
            for name, start, size, is_dir in self._directory(lba, length):
                if name == part:
                    if i == len(parts) - 1:
                        return start, size
                    if is_dir:
                        lba, length = start, size
                        break
            else:
                return None
        return None
