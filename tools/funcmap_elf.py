"""Function-start discovery for MIPS code, plus a CSV builder for stripped ELFs.

Function starts come from: known symbols, JAL targets, lui/addiu code pointers,
data words pointing at prologues/post-return code, and stack-frame prologues
following a return.
"""
import sys, struct
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
            for k in range(4):
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


def main(elf_path, sym_path, out_csv):
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
    with open(out_csv, "w") as f:
        f.write("Name,Start,End,Size\n")
        for i, st in enumerate(starts):
            hi = next(h for l, h in ranges if l <= st < h)
            end = min(starts[i + 1], hi) if i + 1 < len(starts) else hi
            n = names.get(st, f"sub_{st:08X}")
            f.write(f"{n},0x{st:08X},0x{end:08X},{end - st}\n")
    print(f"{len(starts)} functions ({len(names)} named) -> {out_csv}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None, sys.argv[3])
