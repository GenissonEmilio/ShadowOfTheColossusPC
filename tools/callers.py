"""List static (jal/j) callers of a function, recursively up to a depth.
Usage: callers.py <name|0xaddr> [depth]"""
import sys, bisect
sys.path.insert(0, __file__.rsplit("/", 1)[0])
from elfmini import Elf
e = Elf('recomp/game/SOTC.elf')
rows = [l.strip().split(",") for l in open('recomp/game/functions.csv').readlines()[1:]]
starts = [int(r[1], 16) for r in rows]
byname = {r[0]: int(r[1], 16) for r in rows}
def func_of(a):
    i = bisect.bisect_right(starts, a) - 1
    return starts[i], rows[i][0]
callers = {}
for lo, hi in e.exec_ranges():
    for a in range(lo, hi, 4):
        w = e.read32(a)
        if w >> 26 in (2, 3):
            t = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            callers.setdefault(t, set()).add(func_of(a))
q = sys.argv[1]
start = int(q, 16) if q.startswith("0x") else byname[q]
depth = int(sys.argv[2]) if len(sys.argv) > 2 else 3
seen = set()
def show(addr, name, d):
    print("  " * d + f"{name} ({addr:#x})" + ("" if callers.get(addr) else "   <- no static callers (called via pointer?)"))
    if d >= depth or addr in seen:
        return
    seen.add(addr)
    for ca, cn in sorted(callers.get(addr, ())):
        show(ca, cn, d + 1)
show(start, func_of(start)[1], 0)
