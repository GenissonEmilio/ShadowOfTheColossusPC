"""Parser for Team Ico's XFF2 relocatable module format (Shadow of the Colossus).

Layout based on research from github.com/Fantaskink/SOTC (tools/parse_xff_relocs.py).
All *_rel fields are file offsets; the non-_rel twins are pointers fixed up at load time.
"""
import struct
from dataclasses import dataclass, field

R_MIPS_NONE, R_MIPS_16, R_MIPS_32, R_MIPS_REL32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = range(7)
SHN_ABS = 0xFFF1
SHN_COMMON = 0xFFF2


@dataclass
class Section:
    index: int
    name: str
    mem_pt: int
    file_pt: int
    size: int
    align: int
    type: int      # ELF sh_type (1 = PROGBITS, 8 = NOBITS)
    flags: int     # ELF sh_flags
    moved: int
    offs: int      # file offset of contents (bss: none)


@dataclass
class Symbol:
    index: int
    name: str
    value: int
    size: int
    type: int      # STT_*
    bind: int      # STB_*
    other: int
    shndx: int


@dataclass
class Reloc:
    offset: int    # offset within target section
    type: int
    sym: int
    addend: int    # original instruction/word stored by the converter


@dataclass
class RelocTable:
    type: int
    sect: int
    relocs: list = field(default_factory=list)


class Xff:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        d = self.data
        if d[:4] != b"xff2":
            raise ValueError(f"{path}: not xff2")
        u = lambda o: struct.unpack_from("<I", d, o)[0]
        self.u32 = u
        self.hdr_unk0c = u(0x0C)
        self.file_size = u(0x14)
        self.imp_count = u(0x1C)
        self.sym_count = u(0x24)
        self.reloc_count = u(0x38)
        self.sect_count = u(0x40)
        self.entry_ptr = u(0x44) if False else None
        self.imp_off = u(0x50)
        self.sym_off = u(0x54)
        self.str_off = u(0x58)
        self.sect_off = u(0x5C)
        self.reloc_off = u(0x64)
        self.sname_offs = u(0x68)
        self.sname_base = u(0x6C)

        self.sections = []
        for i in range(self.sect_count):
            o = self.sect_off + i * 0x20
            mem, fpt, size, align, ty, fl, moved, offs = struct.unpack_from("<8I", d, o)
            name = self.cstr(self.sname_base + u(self.sname_offs + i * 4))
            self.sections.append(Section(i, name, mem, fpt, size, align, ty, fl, moved, offs))

        self.symbols = []
        for i in range(self.sym_count):
            o = self.sym_off + i * 0x10
            no, val, size, info, other, shndx = struct.unpack_from("<IIIBBH", d, o)
            self.symbols.append(Symbol(i, self.cstr(self.str_off + no), val, size,
                                       info & 0xF, info >> 4, other, shndx))

        self.imports = [u(self.imp_off + i * 4) for i in range(self.imp_count)]

        self.reloc_tables = []
        for i in range(self.reloc_count):
            o = self.reloc_off + i * 0x1C
            ty, n, sect, _a, _b, addr_rel, inst_rel = struct.unpack_from("<7I", d, o)
            t = RelocTable(ty, sect)
            for j in range(n):
                off, info = struct.unpack_from("<II", d, addr_rel + j * 8)
                add, _ = struct.unpack_from("<II", d, inst_rel + j * 8)
                t.relocs.append(Reloc(off, info & 0xFF, info >> 8, add))
            self.reloc_tables.append(t)

    def cstr(self, o):
        e = self.data.index(b"\0", o)
        return self.data[o:e].decode("latin-1")

    def section_bytes(self, s):
        if s.type == 8:
            return bytes(s.size)
        return self.data[s.offs:s.offs + s.size]
