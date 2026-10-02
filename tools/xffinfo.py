import sys, collections
from xff import Xff
for p in sys.argv[1:]:
    x = Xff(p)
    print(f"=== {p}: size={x.file_size:#x} syms={x.sym_count} imports={x.imp_count} relocTabs={x.reloc_count} unk0c={x.hdr_unk0c}")
    for s in x.sections:
        print(f"  [{s.index:2}] {s.name:12} mem={s.mem_pt:#010x} filePt={s.file_pt:#010x} size={s.size:#9x} align={s.align:3} type={s.type} flags={s.flags:#x} moved={s.moved:#x} offs={s.offs:#x}")
    for t in x.reloc_tables:
        c = collections.Counter(r.type for r in t.relocs)
        print(f"  reloc tab type={t.type} sect={t.sect}({x.sections[t.sect].name}) n={len(t.relocs)} types={dict(c)}")
    shn = collections.Counter(s.shndx for s in x.symbols)
    print("  sym shndx:", dict(shn))
    und = [s for s in x.symbols if s.shndx == 0 and s.name]
    print(f"  undefined named syms: {len(und)} e.g. {[s.name for s in und[:15]]}")
    print(f"  imports (first 10): {[(i, x.symbols[i].name) for i in x.imports[:10]]}")
    gl = [s for s in x.symbols if s.bind == 1 and s.shndx not in (0,)]
    print(f"  global defined: {len(gl)} e.g. {[s.name for s in gl[:10]]}")
