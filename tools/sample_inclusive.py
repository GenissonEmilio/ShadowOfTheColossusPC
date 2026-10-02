"""Inclusive sample counts per function inside one thread of a macOS `sample` report.
Usage: sample_inclusive.py report.txt ThreadNameSubstring [pattern ...]"""
import re, sys
lines = open(sys.argv[1]).read().splitlines()
thread = sys.argv[2]
pats = sys.argv[3:]
start = next(i for i, l in enumerate(lines) if thread in l and 'Thread_' in l)
base = len(re.match(r'^(\s*)', lines[start]).group(1))
total = int(re.search(r'(\d+) Thread_', lines[start]).group(1))
counts = {p: 0 for p in pats}
stack = []  # (depth, set of patterns already counted on this path)
for l in lines[start + 1:]:
    m = re.match(r'^([ +!:|]*)(\d+) (.*)', l)
    if not m:
        continue
    depth = len(m.group(1))
    if depth <= base:
        break
    n = int(m.group(2)); name = m.group(3)
    while stack and stack[-1][0] >= depth:
        stack.pop()
    active = set(stack[-1][1]) if stack else set()
    for p in pats:
        if p in name and p not in active:
            counts[p] += n
            active.add(p)
    stack.append((depth, active))
print(f"{thread}: {total} samples")
for p in pats:
    print(f"  {counts[p] * 100.0 / total:5.1f}%  {p}")
