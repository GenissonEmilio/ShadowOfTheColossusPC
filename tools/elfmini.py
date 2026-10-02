"""Minimal ELF32 little-endian reader (sections + program headers)."""
import struct


class Elf:
    def __init__(self, path):
        self.data = d = open(path, "rb").read()
        (self.entry, phoff, shoff) = struct.unpack_from("<III", d, 0x18)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", d, 0x2A)
        self.segments = []
        for i in range(phnum):
            ty, off, vaddr, paddr, filesz, memsz, flags, align = struct.unpack_from("<8I", d, phoff + i * phentsize)
            if ty == 1:
                self.segments.append((vaddr, off, filesz, memsz, flags))
        self.sections = []
        if shoff and shnum:
            raw = [struct.unpack_from("<10I", d, shoff + i * shentsize) for i in range(shnum)]
            stroff = raw[shstrndx][4]
            for r in raw:
                name = d[stroff + r[0]:d.index(b"\0", stroff + r[0])].decode()
                self.sections.append(dict(name=name, type=r[1], flags=r[2], addr=r[3], offset=r[4], size=r[5]))

    def read32(self, addr):
        for vaddr, off, filesz, memsz, flags in self.segments:
            if vaddr <= addr < vaddr + filesz:
                return struct.unpack_from("<I", self.data, off + addr - vaddr)[0]
        return None

    def exec_ranges(self):
        secs = [(s["addr"], s["addr"] + s["size"]) for s in self.sections if s["flags"] & 4 and s["size"]]
        return secs or [(v, v + fs) for v, o, fs, ms, fl in self.segments if fl & 1]
