#!/usr/bin/env python3
"""Names the samples of PS2X_PROFILE (the guest sampling profiler).

The runtime writes "pc ra samples" lines (1000 samples a second of the
running EE context; "idle" while the EE waits). Each pc is attributed to
the recompiled function that contains it (recomp/game/functions.csv), its
ra to the caller. Time spent in host code called from a hook counts for the
hooked function (the dispatcher sets pc to it).

Usage: tools/guest_profile.py <profile file> [--top 40] [--callers 3] [--module]
"""
import bisect
import os
import sys
from collections import Counter, defaultdict

ROOT = os.path.join(os.path.dirname(__file__), '..')


def load_functions():
    starts, entries = [], []
    for line in open(os.path.join(ROOT, 'recomp/game/functions.csv')):
        p = line.strip().split(',')
        try:
            start, end = int(p[1], 16), int(p[2], 16)
        except (IndexError, ValueError):
            continue
        entries.append((start, end, p[0]))
    entries.sort()
    starts = [e[0] for e in entries]
    return starts, entries


def module(address):
    if address < 0x1000000:
        return 'boot/KERNEL'
    if address < 0x1170000:
        return 'MANAGER'
    return 'GAMECORE+'


def main():
    path = sys.argv[1]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 40
    ncallers = int(sys.argv[sys.argv.index('--callers') + 1]) if '--callers' in sys.argv else 3
    starts, entries = load_functions()

    def name(address):
        i = bisect.bisect_right(starts, address) - 1
        if i >= 0 and entries[i][0] <= address < entries[i][1]:
            return entries[i][2], entries[i][0]
        return '?%08x' % address, address

    total = idle = 0
    self_samples = Counter()
    callers = defaultdict(Counter)
    modules = Counter()
    for line in open(path):
        p = line.split()
        if p[0] == 'total':
            total = int(p[1])
            continue
        if p[0] == 'idle':
            idle = int(p[1])
            continue
        pc, ra, n = int(p[0], 16), int(p[1], 16), int(p[2])
        fn, start = name(pc)
        self_samples[(fn, start)] += n
        callers[(fn, start)][name(ra)[0] if ra else '-'] += n
        modules[module(pc)] += n
    busy = max(1, total - idle)
    print(f'{total} samples ({total / 1000:.0f} s), EE busy {100.0 * busy / max(1, total):.1f}%')
    if '--module' in sys.argv:
        for m, n in modules.most_common():
            print(f'  {m:12s} {100.0 * n / busy:5.1f}%')
    print(f'{"busy %":>7} {"samples":>8}  function (callers)')
    for (fn, start), n in self_samples.most_common(top):
        by = ', '.join(f'{c} {100.0 * k / n:.0f}%' for c, k in callers[(fn, start)].most_common(ncallers))
        print(f'{100.0 * n / busy:7.2f} {n:8d}  {fn} @{start:08x}  ({by})')


if __name__ == '__main__':
    main()
