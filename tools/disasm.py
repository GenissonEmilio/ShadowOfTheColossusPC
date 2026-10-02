"""Disassemble a function (or address range) of the combined game ELF.
Usage: disasm.py <name|0xstart> [0xend]"""
import os, sys, bisect
import capstone
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfmini import Elf

GAME = os.path.join(os.environ.get("CELOSIA_BUILD", "build"), "recomp/game")
elf = Elf(f"{GAME}/SOTC.elf")
rows = [l.strip().split(",") for l in open(f"{GAME}/functions.csv").readlines()[1:]]
starts = [int(r[1], 16) for r in rows]
byname = {r[0]: (int(r[1], 16), int(r[2], 16)) for r in rows}
names = {int(r[1], 16): r[0] for r in rows}
for l in open(f"{GAME}/symbols.txt"):
    a, n = l.split()[:2]
    names.setdefault(int(a, 16), n)

q = sys.argv[1]
if q.startswith("0x"):
    lo = int(q, 16)
    hi = int(sys.argv[2], 16) if len(sys.argv) > 2 else lo + 0x100
else:
    lo, hi = byname[q]
code = b"".join(elf.read32(a).to_bytes(4, "little") for a in range(lo, hi, 4))
md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 | capstone.CS_MODE_LITTLE_ENDIAN)
md.skipdata = True  # EE-only (MMI) opcodes show as .byte
for ins in md.disasm(code, lo):
    note = ""
    if ins.mnemonic in ("jal", "j"):
        t = int(ins.op_str, 16)
        note = f"  ; {names.get(t, '?')}"
    label = f"{names[ins.address]}:\n" if ins.address in names and ins.address != lo else ""
    print(f"{label}  {ins.address:08x}: {ins.mnemonic:8} {ins.op_str}{note}")
