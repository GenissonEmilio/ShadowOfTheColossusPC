"""Function-start discovery for MIPS code, plus a CSV builder for stripped ELFs.

Function starts come from: known symbols, JAL targets, lui/addiu code pointers,
data words pointing at prologues/post-return code, and stack-frame prologues
following a return.
"""
import bisect, sys, struct
from elfmini import Elf

JR_RA = 0x03E00008


def is_prologue(w):
    return w is not None and (w >> 16) == 0x27BD and (w & 0x8000)


def is_terminal(w):
    """Unconditional control transfer that never falls through: jr, j, b."""
    if w is None:
        return False
    return (w >> 26 == 0 and (w & 0x3F) == 8) or w >> 26 == 2 or (w >> 16) == 0x1000


def dest_reg(w):
    """GPR written by an instruction (approximate; None if none)."""
    op = w >> 26
    if op == 0:
        return (w >> 11) & 31 if (w & 0x3F) not in (8, 0x18, 0x19, 0x1A, 0x1B, 0x11, 0x13) else None
    if op == 3:
        return 31
    if op in (8, 9, 10, 11, 12, 13, 14, 15, 0x18, 0x19, 0x1A, 0x1B, 0x1E,
              0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x37):
        return (w >> 16) & 31
    if op == 0x1C:  # MMI
        return (w >> 11) & 31
    return None


def after_return(words, t):
    """True if t follows a terminal jump and its delay slot (plus nop padding)."""
    p = t - 4
    while words.get(p) == 0:
        p -= 4
    # p is the delay slot of a terminal jump, or the jump itself when its
    # delay slot was a nop that we skipped as padding.
    return is_terminal(words.get(p - 4)) or (is_terminal(words.get(p)) and p + 4 < t)


def find_starts(words, in_text, data_blobs):
    """words: {addr: insn} for code; data_blobs: [bytes] of non-code sections."""
    starts = set()
    for a, w in words.items():
        op = w >> 26
        if op == 3:  # jal
            t = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if in_text(t):
                starts.add(t)
        if is_terminal(w):
            p = a + 8
            padded = words.get(p) == 0
            while words.get(p) == 0:
                p += 4
            # GCC pads between functions with nops up to 8-byte alignment.
            if padded and p % 8 == 0 and words.get(p) is not None:
                starts.add(p)
            # the frame setup may be scheduled a few slots into the function
            # (GCC hoists address loads above it: lui, lui, lw, lui, addiu sp)
            for k in range(8):
                wk = words.get(p + 4 * k)
                if wk is None or (wk >> 26) in (1, 2, 3, 4, 5, 6, 7) or is_terminal(wk):
                    break
                if is_prologue(wk):
                    starts.add(p)
                    break
        elif op == 0x0F:  # lui: follow the base register to addiu/ori uses (code pointers)
            rt, hi = (w >> 16) & 31, (w & 0xFFFF) << 16
            for k in range(1, 96):
                w2 = words.get(a + 4 * k)
                if w2 is None:
                    break
                op2 = w2 >> 26
                if op2 in (9, 13) and ((w2 >> 21) & 31) == rt:
                    lo = w2 & 0xFFFF
                    t = (hi | lo) if op2 == 13 else (hi + struct.unpack("<h", struct.pack("<H", lo))[0]) & 0xFFFFFFFF
                    if in_text(t) and t % 4 == 0:
                        starts.add(t)
                if dest_reg(w2) == rt or is_terminal(words.get(a + 4 * (k - 1))):
                    break
    for blob in data_blobs:
        for o in range(0, len(blob) - 3, 4):
            t = struct.unpack_from("<I", blob, o)[0]
            if in_text(t) and t % 4 == 0 and (is_prologue(words.get(t)) or after_return(words, t)):
                starts.add(t)
    return starts


def branch_targets(words):
    """Targets of every direct branch and j in the code."""
    out = set()
    for a, w in words.items():
        op = w >> 26
        if op in (4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or (op == 1 and ((w >> 16) & 31) in (0, 1, 2, 3, 16, 17, 18, 19)):
            out.add((a + 4 + (struct.unpack("<h", struct.pack("<H", w & 0xFFFF))[0] << 2)) & 0xFFFFFFFF)
        elif op == 2:
            out.add(((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2))
    return out


def branch_edges(words):
    """(source, target) of every PC-relative branch: beq/bne/blez/bgtz and
    their likely forms, REGIMM (bltz, bgez, the -al forms), COP0/1/2 bc."""
    out = []
    for a, w in words.items():
        op = w >> 26
        if (op in (4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17)
                or (op == 1 and ((w >> 16) & 31) in (0, 1, 2, 3, 16, 17, 18, 19))
                or (op in (0x10, 0x11, 0x12) and ((w >> 21) & 31) == 8)):
            out.append((a, (a + 4 + (struct.unpack("<h", struct.pack("<H", w & 0xFFFF))[0] << 2)) & 0xFFFFFFFF))
    return out


def merge_split_starts(words, starts, strong, sizes):
    """Drops the starts that fall inside another function's body: the
    heuristics above cut functions at switch cases (a jump table's targets
    follow a jump), at labels GCC aligned with a nop after a jump, at a
    prologue-like store in the middle of a body. A split makes the jumps
    between the pieces leave the function at run time (a nested host call
    each) instead of staying a goto.

    starts: candidate starts; strong: starts kept whatever happens (named
    symbols, the ELF entry, addresses the overrides hook); sizes: {start:
    size} for symbols that give one (the XFF modules' functions). A start is
    dropped when it lies inside a sized symbol's extent, or when a branch
    crosses it (from before it to it or past it, or back over it) without
    crossing a strong start (a branch to another function's body). Returns
    (kept starts, dropped starts); the dropped ones stay callable as entry
    points into the merged function."""
    starts = sorted(set(starts))
    dropped = set()
    for a, size in sizes.items():
        if size > 4:
            i = bisect.bisect_right(starts, a)
            while i < len(starts) and starts[i] < a + size:
                if starts[i] not in strong:
                    dropped.add(starts[i])
                i += 1
    strong_sorted = sorted(strong)
    for src, tgt in branch_edges(words):
        lo, hi = (src, tgt) if src < tgt else (tgt, src)
        if hi - lo > 0x20000 or tgt not in words:
            continue
        # starts s with lo < s <= hi: a forward branch reaches s or runs past
        # it; a backward one goes back over it (or to it from inside it: s ==
        # src's own start is not crossed, s <= src is; lo < s keeps tgt's own
        # function start out).
        j = bisect.bisect_right(strong_sorted, lo)
        if j < len(strong_sorted) and strong_sorted[j] <= hi:
            continue
        i = bisect.bisect_right(starts, lo)
        while i < len(starts) and starts[i] <= hi:
            dropped.add(starts[i])
            i += 1
    dropped -= set(strong)
    return [s for s in starts if s not in dropped], sorted(dropped)


def hidden_entries(words, starts):
    """Code right after a jump and its delay slot, with no nop padding, that
    no branch reaches and that starts no known function: a leaf GCC packed
    against the previous function (gamecore 0x1357888, 'v0 = 1; *t0 = v0')
    or a switch case. Called through a pointer, it needs an entry of its own."""
    targets = branch_targets(words)
    out = []
    for a, w in words.items():
        t = a + 8
        if is_terminal(w) and t not in starts and t not in targets and words.get(t) not in (None, 0):
            out.append(t)
    return sorted(out)


def main(elf_path, sym_path, out_csv, merge=False, pinned=()):
    """Writes the function map; with merge, false splits are merged
    (merge_split_starts) and the dropped starts are returned."""
    elf = Elf(elf_path)
    ranges = elf.exec_ranges()
    in_text = lambda a: any(lo <= a < hi for lo, hi in ranges)
    names = {}
    if sym_path:
        for line in open(sym_path):
            a, n = line.split()
            a = int(a, 16)
            if in_text(a):
                names[a] = n
    words = {a: elf.read32(a) for lo, hi in ranges for a in range(lo, hi, 4)}
    blobs = [elf.data[s["offset"]:s["offset"] + s["size"]] for s in elf.sections
             if s["type"] == 1 and not s["flags"] & 4 and s["size"]]
    starts = sorted(set(names) | {elf.entry} | find_starts(words, in_text, blobs))
    dropped = []
    if merge:
        strong = set(names) | {elf.entry} | {p for p in pinned if p in set(starts)}
        starts, dropped = merge_split_starts(words, starts, strong, {})
    with open(out_csv, "w") as f:
        f.write("Name,Start,End,Size\n")
        for i, st in enumerate(starts):
            hi = next(h for l, h in ranges if l <= st < h)
            end = min(starts[i + 1], hi) if i + 1 < len(starts) else hi
            n = names.get(st, f"sub_{st:08X}")
            f.write(f"{n},0x{st:08X},0x{end:08X},{end - st}\n")
    print(f"{len(starts)} functions ({len(names)} named, {len(dropped)} false splits merged) -> {out_csv}")
    return dropped


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None, sys.argv[3])
