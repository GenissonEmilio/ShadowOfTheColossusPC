"""Decode textures from a GS VRAM dump using the GS swizzle (PSMT4/PSMT8/PSMCT32)."""
import sys, re
from PIL import Image

src = open("PS2Recomp/ps2xRuntime/include/runtime/gs/ps2_gs_psmt4.h").read()
def table(name, rows, cols):
    body = src[src.index(name):]
    nums = list(map(int, re.findall(r"\d+", body[body.index("{"):])))[: rows * cols]
    return [nums[r * cols:(r + 1) * cols] for r in range(rows)]
bt4 = table("blockTable4[8][4]", 8, 4)
ct4 = table("columnTable4[16][32]", 16, 32)

def addr_t4(block, width, x, y):
    ppr = max(width >> 1, 1)
    page = (block >> 5) + (y >> 7) * ppr + (x >> 7)
    bid = (block & 31) + bt4[(y >> 4) & 7][(x >> 5) & 3]
    return (page << 14) + ((bid >> 5) << 14) + (bid & 31) * 512 + ct4[y & 15][x & 31]

def decode_t4(vram, tbp, tbw, w, h):
    img = Image.new("L", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            n = addr_t4(tbp, tbw, x, y) & 0x7FFFFF
            b = vram[n >> 1]
            px[x, y] = ((b >> 4) if n & 1 else (b & 15)) * 17
    return img

if __name__ == "__main__":
    vram = open(sys.argv[1], "rb").read()
    tbp, tbw, w, h = map(int, sys.argv[2:6])
    decode_t4(vram, tbp, tbw, w, h).save(sys.argv[6])
