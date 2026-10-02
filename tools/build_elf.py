"""Combine the boot ELF and loaded XFF modules into one ELF for PS2Recomp,
and emit the matching function map CSV + symbol list.

Usage: build_elf.py out_dir boot_elf boot_syms XFF@hdr@ram_dump [...]

hdr is the module header address, or "auto" to locate it in the dump.

Each module is captured from a RAM dump taken when the game jumps to its entry.
"""
import sys, struct
from elfmini import Elf
from modimage import LoadedModule, find_header
import funcmap_elf

SHF_WRITE, SHF_ALLOC, SHF_EXEC = 1, 2, 4


def write_elf(path, entry, sections):
    """sections: list of (name, addr, size, data|None, flags)."""
    shstr = b"\0" + b"".join(n.encode() + b"\0" for n, *_ in sections) + b".shstrtab\0"
    progs = [s for s in sections if s[3] is not None]
    ehsize, phsize, shsize = 52, 32, 40
    off = ehsize + phsize * len(progs)
    blobs, phdrs, shdrs = [], [], [(0,) * 10]
    name_off = 1
    for name, addr, size, data, flags in sections:
        if data is not None:
            off = (off + 15) & ~15
            pflags = 4 | (1 if flags & SHF_EXEC else 0) | (2 if flags & SHF_WRITE else 0)
            phdrs.append((1, off, addr, addr, len(data), len(data), pflags, 16))
            blobs.append((off, data))
            shdrs.append((name_off, 1, flags, addr, off, size, 0, 0, 16, 0))
            off += len(data)
        else:
            shdrs.append((name_off, 8, flags, addr, off, size, 0, 0, 16, 0))
        name_off += len(name) + 1
    shstr_off = off
    off += len(shstr)
    shdrs.append((name_off, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0))
    shoff = (off + 3) & ~3
    out = bytearray(shoff + shsize * len(shdrs))
    out[0:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", out, 16, 2, 8, 1, entry, ehsize, shoff, 0x20924001,
                     ehsize, phsize, len(phdrs), shsize, len(shdrs), len(shdrs) - 1)
    for i, p in enumerate(phdrs):
        struct.pack_into("<8I", out, ehsize + i * phsize, *p)
    for o, d in blobs:
        out[o:o + len(d)] = d
    out[shstr_off:shstr_off + len(shstr)] = shstr
    for i, s in enumerate(shdrs):
        struct.pack_into("<10I", out, shoff + i * shsize, *s)
    open(path, "wb").write(out)


def main(out_dir, boot_path, boot_syms, mods):
    boot = Elf(boot_path)
    sections = []
    for s in boot.sections:
        if s["flags"] & SHF_ALLOC and s["size"]:
            data = None if s["type"] == 8 else boot.data[s["offset"]:s["offset"] + s["size"]]
            sections.append((s["name"], s["addr"], s["size"], data, s["flags"]))

    # boot ELF function map from heuristics
    boot_csv = f"{out_dir}/boot_functions.csv"
    funcmap_elf.main(boot_path, boot_syms, boot_csv)
    rows = [l.strip().split(",") for l in open(boot_csv).readlines()[1:]]
    funcs = [(int(r[1], 16), int(r[2], 16), r[0]) for r in rows]
    syms = [l.split() for l in open(boot_syms)]
    dynamic = set()

    for spec in mods:
        path, hdr, ram_path = spec.split("@")
        ram = open(ram_path, "rb").read()
        m = LoadedModule(path, ram, find_header(path, ram) if hdr == "auto" else int(hdr, 0))
        tag = m.name.split(".")[0]
        for s in m.sections:
            flags = SHF_ALLOC | (SHF_EXEC if s.exec else SHF_WRITE)
            sections.append((f"{s.name}.{tag}", s.addr, s.size, s.data, flags))
        t = m.text()
        fmap = m.functions()
        # unnamed (static) functions: jal targets, code pointers, prologues
        words = {t.addr + o: struct.unpack_from("<I", t.data, o)[0] for o in range(0, t.size, 4)}
        in_text = lambda a, t=t: t.addr <= a < t.addr + t.size
        blobs = [s.data for s in m.sections if s.data is not None and not s.exec]
        for tgt in funcmap_elf.find_starts(words, in_text, blobs):
            if tgt not in fmap:
                fmap[tgt] = (0, f"{tag.lower()}_sub_{tgt:08X}")
        starts = sorted(fmap)
        for i, a in enumerate(starts):
            nxt = starts[i + 1] if i + 1 < len(starts) else t.addr + t.size
            funcs.append((a, nxt, fmap[a][1]))
        for a, (sz, n) in fmap.items():
            syms.append([f"{a:08x}", n])
        for a, n in m.data_symbols().items():
            syms.append([f"{a:08x}", n])
        print(f"{m.name}: header {m.base:#x}, {len(fmap)} functions, sections:",
              ", ".join(f"{s.name}@{s.addr:#x}+{s.size:#x}" for s in m.sections))

        # import relocation sites in .text: the guest linker rewrites these at run time
        text_index = next(i for i, sec in enumerate(m.xff.sections) if sec.name == ".text")
        for tab in m.xff.reloc_tables:
            if tab.sect == text_index:
                for r in tab.relocs:
                    if m.xff.symbols[r.sym].shndx == 0:
                        dynamic.add(t.addr + r.offset)

    sections.sort(key=lambda s: s[1])
    with open(f"{out_dir}/dynamic_imm.txt", "w") as f:
        f.write("# instruction addresses relinked by the game's dynamic linker\n")
        for a in sorted(dynamic):
            f.write(f"{a:08x}\n")
    print(f"{len(dynamic)} dynamic immediate sites")
    write_elf(f"{out_dir}/SOTC.elf", boot.entry, sections)
    seen = set()
    with open(f"{out_dir}/functions.csv", "w") as f:
        f.write("Name,Start,End,Size\n")
        for a, e, n in sorted(funcs):
            if a in seen:
                continue
            seen.add(a)
            f.write(f"{n},0x{a:08X},0x{e:08X},{e - a}\n")
    with open(f"{out_dir}/symbols.txt", "w") as f:
        for a, n in sorted(syms):
            f.write(f"{a} {n}\n")
    print(f"wrote {out_dir}/SOTC.elf with {len(seen)} functions")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:])
