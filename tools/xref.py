"""Find code referencing a string or address in the combined ELF (lui/addiu|ori pairs).

Usage: xref.py SOTC.elf functions.csv <string|0xaddr>
"""
import sys, struct, bisect
sys.path.insert(0, __file__.rsplit("/", 1)[0])
from elfmini import Elf

elf = Elf(sys.argv[1])
rows = [l.strip().split(",") for l in open(sys.argv[2]).readlines()[1:]]
starts = [int(r[1], 16) for r in rows]
q = sys.argv[3]
targets = set()
if q.startswith("0x"):
    targets.add(int(q, 16))
else:
    needle = q.encode()
    for s in elf.sections:
        if s["type"] == 1:
            blob = elf.data[s["offset"]:s["offset"] + s["size"]]
            i = blob.find(needle)
            while i >= 0:
                # back up to the start of the C string
                j = i
                while j > 0 and blob[j - 1] != 0:
                    j -= 1
                targets.add(s["addr"] + j)
                end = blob.find(bytes(1), i)
                print(f"string at {s['addr'] + j:#x}: {blob[j:end]!r}")
                i = blob.find(needle, i + 1)
for lo, hi in elf.exec_ranges():
    for a in range(lo, hi, 4):
        w = elf.read32(a)
        if w >> 26 != 0x0F:
            continue
        rt, up = (w >> 16) & 31, (w & 0xFFFF) << 16
        for k in range(1, 12):
            w2 = elf.read32(a + 4 * k)
            if w2 is None:
                break
            op = w2 >> 26
            if ((w2 >> 21) & 31) == rt and op in (9, 13, 0x23, 0x2B, 0x21, 0x25, 0x20, 0x24):
                imm = w2 & 0xFFFF
                val = (up | imm) if op == 13 else (up + struct.unpack("<h", struct.pack("<H", imm))[0]) & 0xFFFFFFFF
                if val in targets:
                    i = bisect.bisect_right(starts, a) - 1
                    print(f"  ref at {a:#x} in {rows[i][0]}+{a - starts[i]:#x}")
                break
