"""Resolve addresses to function+offset using a recomp functions.csv."""
import sys, bisect
rows = [l.strip().split(",") for l in open(sys.argv[1]).readlines()[1:]]
starts = [int(r[1], 16) for r in rows]
for a in sys.argv[2:]:
    v = int(a, 16)
    i = bisect.bisect_right(starts, v) - 1
    print(f"{v:#x} = {rows[i][0]}+{v - starts[i]:#x}" if i >= 0 and v < int(rows[i][2], 16) else f"{v:#x} = ?")
