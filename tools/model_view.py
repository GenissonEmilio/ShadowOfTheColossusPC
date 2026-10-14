#!/usr/bin/env python3
"""Read SotC models straight from a guest RAM dump (SOTC_MODEL_DUMP) and draw
their vertices, to check the model format in docs/native_renderer_map.md.

Usage: tools/model_view.py logs/model_dump [--dump N] [--out file.png]
"""
import struct, sys, math

ram = None

def u32(a): return struct.unpack_from('<I', ram, a & 0x1FFFFFC)[0]
def f32(a): return struct.unpack_from('<f', ram, a & 0x1FFFFFC)[0]

def vif_commands(addr, size):
    """Yield (cmd, code, data address, data words) for a VIF code stream."""
    end = addr + size
    p = addr
    while p < end:
        code = u32(p); p += 4
        cmd = (code >> 24) & 0x7F
        num = (code >> 16) & 0xFF
        imm = code & 0xFFFF
        words = 0
        if (cmd & 0x60) == 0x60:  # UNPACK
            vn, vl = (cmd >> 2) & 3, cmd & 3
            n = num if num else 256
            bits = n * (vn + 1) * (32 >> vl)
            words = (bits + 31) // 32
        elif cmd == 0x20: words = 1          # STMASK
        elif cmd in (0x30, 0x31): words = 4  # STROW, STCOL
        elif cmd == 0x4A: words = (num if num else 256) * 2  # MPG
        elif cmd in (0x50, 0x51): words = (imm if imm else 65536) * 4  # DIRECT
        yield cmd, code, p, words
        p += words * 4

UNPACK_NAMES = {0x60: 'S-32', 0x61: 'S-16', 0x62: 'S-8', 0x64: 'V2-32', 0x65: 'V2-16', 0x66: 'V2-8',
                0x68: 'V3-32', 0x69: 'V3-16', 0x6A: 'V3-8', 0x6C: 'V4-32', 0x6D: 'V4-16', 0x6E: 'V4-8', 0x6F: 'V4-5'}

def describe(addr, size):
    out = []
    for cmd, code, data, words in vif_commands(addr, size):
        if (cmd & 0x60) == 0x60:
            name = UNPACK_NAMES.get(cmd & 0x6F, hex(cmd))
            out.append('UNPACK %s x%d -> %d%s%s' % (name, (code >> 16) & 0xFF, code & 0x3FF,
                                                   ' usn' if code & 0x4000 else '', ' mask' if cmd & 0x10 else ''))
        else:
            out.append({0: 'NOP', 1: 'STCYCL %x' % (code & 0xFFFF), 5: 'STMOD %d' % (code & 3), 0x14: 'MSCAL %x' % (code & 0xFFFF),
                        0x17: 'MSCNT', 0x10: 'FLUSHE', 0x11: 'FLUSH', 0x13: 'FLUSHA', 0x30: 'STROW', 0x31: 'STCOL', 0x20: 'STMASK',
                        2: 'OFFSET', 3: 'BASE', 4: 'ITOP'}.get(cmd, 'cmd %x' % cmd))
    return out

def load(d):
    global ram
    ram = open(d + '/ram.bin', 'rb').read()
    calls = []
    for line in open(d + '/calls.txt'):
        parts = line.split()
        kv = dict(p.split('=') for p in parts[1:4])
        i = parts.index('model'); j = parts.index('camera', i)
        calls.append({'instance': int(kv['instance'], 16), 'header': int(kv['header'], 16),
                      'model': [float(x) for x in parts[i + 1:i + 17]], 'camera': [float(x) for x in parts[j + 1:j + 17]]})
    return calls

def packets(header):
    base, count = u32(header + 0x60), u32(header + 0x64)
    for k in range(count):
        a = base + k * 32
        yield {'size': u32(a), 'lod': u32(a + 4), 'surface': u32(a + 8), 'part': u32(a + 12), 'vertices': u32(a + 16)}

def visible_packets(instance, header):
    """The packets modelDisp draws: the instance's LOD, parts the clip left visible."""
    word = struct.unpack_from('<Q', ram, (instance + 0x250) & 0x1FFFFF8)[0]
    lod = (word >> 40) & 7
    if (u32(header + 0x18) & 1) and lod >= u32(header + 0x1c):
        lod = u32(header + 0x1c) - 1
    table = u32(header + 0x30 + lod * 4)
    for pk in packets(header):
        if pk['lod'] != lod:
            continue
        if pk['part'] != 0xFFFFFFFF and table:
            if u32(table + pk['part'] * 0x30 + 0x2c) not in (0, 2):
                continue
        yield pk

if __name__ == '__main__':
    d = sys.argv[1]
    calls = load(d)
    if '--dump' in sys.argv:
        c = calls[int(sys.argv[sys.argv.index('--dump') + 1])]
        h = c['header']
        print('header %08x surfaces %d packets %d textures %d flags %x lods %d' % (h, u32(h + 0x54), u32(h + 0x64), u32(h + 0x44), u32(h + 0x18), u32(h + 0x1c)))
        for k, pk in enumerate(packets(h)):
            if k >= 3: break
            print('packet', pk)
            for line in describe(pk['vertices'], pk['size']): print('   ', line)

def batches(addr, size):
    """The world batches of a packet, one per MSCNT: GIF tag words, positions, ST, colours."""
    tag = None; pos = []; st = []; col = []
    for cmd, code, data, words in vif_commands(addr, size):
        if cmd in (0x14, 0x15, 0x17):  # MSCAL / MSCALF / MSCNT: the batch goes to VU1
            if pos:
                yield tag, pos, st, col
            tag = None; pos = []; st = []; col = []
            continue
        if (cmd & 0x60) != 0x60:
            continue
        kind, n = cmd & 0x6F, (code >> 16) & 0xFF
        if kind == 0x6C and tag is None:
            tag = [u32(data + 4 * i) for i in range(4)]
        elif kind == 0x68:
            pos = [(f32(data + 12 * i), f32(data + 12 * i + 4), f32(data + 12 * i + 8)) for i in range(n)]
        elif kind == 0x65:
            st = [struct.unpack_from('<hh', ram, (data + 4 * i) & 0x1FFFFFF) for i in range(n)]
        elif kind == 0x6E:
            col = [tuple(ram[(data + 4 * i + k) & 0x1FFFFFF] for k in range(4)) for i in range(n)]
    if pos:
        yield tag, pos, st, col

def batch(addr, size):
    """The first batch of a packet (format checks)."""
    for b in batches(addr, size):
        return b
    return None, [], [], []

def mul(m, v):  # column-major 4x4 times (x, y, z, 1)
    return [m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3] for r in range(4)]

def project(call, p):
    world = mul(call['model'], (p[0], p[1], p[2], 1.0))
    clip = mul(call['camera'], world)
    return clip

def render(calls, w=512, h=448, flip=True):
    from PIL import Image, ImageDraw
    im = Image.new('RGB', (w, h))
    dr = ImageDraw.Draw(im)
    for c in calls:
        for pk in packets(c['header']):
            tag, pos, st, col = batch(pk['vertices'], pk['size'])
            pts = []
            for p in pos:
                cl = project(c, p)
                if cl[3] <= 0.1:
                    pts.append(None); continue
                x = (cl[0] / cl[3] * 0.5 + 0.5) * w
                y = (cl[1] / cl[3] * 0.5 + 0.5) * h
                pts.append((x, h - y if flip else y))
            for i in range(2, len(pts)):
                tri = pts[i - 2:i + 1]
                if None in tri: continue
                if max(abs(a[0]) + abs(a[1]) for a in tri) > 1e5: continue
                cc = col[i] if i < len(col) else (255, 255, 255, 0)
                dr.polygon(tri, outline=(cc[0], cc[1], cc[2]))
    return im

def clip_near(tri, near):
    out = []
    for k in range(len(tri)):
        a, b = tri[k], tri[(k + 1) % len(tri)]
        ina, inb = a[3] >= near, b[3] >= near
        if ina:
            out.append(a)
        if ina != inb:
            t = (near - a[3]) / (b[3] - a[3])
            out.append([a[i] + (b[i] - a[i]) * t for i in range(4)])
    return out

def raster(p0, p1, p2, cc, depth, color, w, h):
    (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = p0, p1, p2
    area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
    if abs(area) < 1e-9:
        return
    minx, maxx = max(0, int(min(x0, x1, x2))), min(w - 1, int(max(x0, x1, x2)) + 1)
    miny, maxy = max(0, int(min(y0, y1, y2))), min(h - 1, int(max(y0, y1, y2)) + 1)
    for y in range(miny, maxy + 1):
        for x in range(minx, maxx + 1):
            px, py = x + 0.5, y + 0.5
            a = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area
            b = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area
            c2 = 1.0 - a - b
            if a < 0 or b < 0 or c2 < 0:
                continue
            z = a * z0 + b * z1 + c2 * z2
            k = y * w + x
            if z > depth[k]:
                depth[k] = z
                color[k] = cc

def zbuffer_render(calls, w=256, h=224, only=None):
    """Rasterize the visible packets with a depth buffer (1/w), flat colour per triangle."""
    from PIL import Image
    depth = [0.0] * (w * h)
    color = [(0, 0, 0)] * (w * h)
    for idx, c in enumerate(calls):
        if only is not None and idx not in only:
            continue
        for pk in visible_packets(c['instance'], c['header']):
          for tag, pos, st, col in batches(pk['vertices'], pk['size']):
            clips = [project(c, p) for p in pos]
            for i in range(2, len(clips)):
                cc = col[i] if i < len(col) else (255, 255, 255, 0)
                # Clip the triangle to w >= near in clip space, then project.
                poly = clip_near(clips[i - 2:i + 1], 1.0)
                pts = [((v[0] / v[3] * 0.5 + 0.5) * w, (-v[1] / v[3] * 0.5 + 0.5) * h, 1.0 / v[3]) for v in poly]
                for j in range(1, len(pts) - 1):
                    raster(pts[0], pts[j], pts[j + 1], (cc[0], cc[1], cc[2]), depth, color, w, h)
    im = Image.new('RGB', (w, h))
    im.putdata(color)
    return im

# ---- GS swizzle (from runtime/gs/ps2_gs_psm*.h) -------------------------------
BLOCK32 = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23], [8, 9, 12, 13, 24, 25, 28, 29], [10, 11, 14, 15, 26, 27, 30, 31]]
COLUMN32 = [[0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15], [16, 17, 20, 21, 24, 25, 28, 29], [18, 19, 22, 23, 26, 27, 30, 31],
            [32, 33, 36, 37, 40, 41, 44, 45], [34, 35, 38, 39, 42, 43, 46, 47], [48, 49, 52, 53, 56, 57, 60, 61], [50, 51, 54, 55, 58, 59, 62, 63]]

def addr32(bw, x, y):
    page = (y >> 5) * max(bw, 1) + (x >> 6)
    return (page << 13) + BLOCK32[(y >> 3) & 3][(x >> 3) & 7] * 256 + COLUMN32[y & 7][x & 7] * 4

def _c_table(path, name):
    import re, os
    text = open(os.path.join(os.path.dirname(__file__), '..', 'PS2Recomp/ps2xRuntime/include/runtime/gs', path)).read()
    body = text[text.index(name):]
    body = body[body.index('=') + 1:body.index('};')]
    return [[int(v) for v in re.findall(r'\d+', row)] for row in re.findall(r'\{([^{}]*)\}', body)]

BLOCK4 = _c_table('ps2_gs_psmt4.h', 'blockTable4'); COLUMN4 = _c_table('ps2_gs_psmt4.h', 'columnTable4')
BLOCK8 = _c_table('ps2_gs_psmt8.h', 'blockTable8'); COLUMN8 = _c_table('ps2_gs_psmt8.h', 'columnTable8')

def addr4(bw, x, y):  # nibble address
    page = (y >> 7) * max(bw >> 1, 1) + (x >> 7)
    return (page << 14) + BLOCK4[(y >> 4) & 7][(x >> 5) & 3] * 512 + COLUMN4[y & 15][x & 31]

def addr8(bw, x, y):
    page = (y >> 6) * max(bw >> 1, 1) + (x >> 7)
    return (page << 13) + BLOCK8[(y >> 4) & 3][(x >> 4) & 7] * 256 + COLUMN8[y & 15][x & 15]

def texture(header):
    """An NTO2 texture as RGBA rows (level 0). Swizzled data (uploaded as 32-bit) is unswizzled."""
    w0 = u32(header + 0x1c)
    psm, W, H = w0 & 0x3f, 1 << ((w0 >> 16) & 0xf), 1 << ((w0 >> 20) & 0xf)
    pix, clut = header + u32(header + 0x14), header + u32(header + 0x18)
    swizzled = (ram[(header + 0x1f) & 0x1FFFFFF] & 1) != 0
    entries = 16 if psm in (0x14, 0x24, 0x2c) else 256
    pal = [tuple(ram[(clut + 4 * i + k) & 0x1FFFFFF] for k in range(4)) for i in range(entries)]
    if entries == 256:  # CSM1: entries 8-15 and 16-23 of each 32 swapped
        pal = [pal[(i & ~0x18) | ((i & 8) << 1) | ((i & 16) >> 1)] for i in range(256)]
    out = []
    if swizzled and psm in (0x13, 0x14):
        # The game uploads it as PSMCT32 (W/2 x H/2 for 8-bit, W/2 x H/4 for 4-bit) and reads it back as 8/4-bit.
        cw, ch = W // 2, (H // 2 if psm == 0x13 else H // 4)
        mem = bytearray(max(1 << 20, cw * ch * 8))
        for i in range(cw * ch):
            a = addr32(max(cw // 64, 1), i % cw, i // cw)
            mem[a:a + 4] = ram[(pix + 4 * i) & 0x1FFFFFF:((pix + 4 * i) & 0x1FFFFFF) + 4]
        tbw = max(W // 64, 2)
        for y in range(H):
            for x in range(W):
                if psm == 0x14:
                    n = addr4(tbw, x, y); b = mem[n >> 1]; idx = (b >> 4) if n & 1 else (b & 15)
                else:
                    idx = mem[addr8(tbw, x, y)]
                out.append(pal[idx])
    else:
        for y in range(H):
            for x in range(W):
                if psm == 0x14:
                    b = ram[(pix + (y * W + x) // 2) & 0x1FFFFFF]; idx = (b >> 4) if x & 1 else (b & 15)
                elif psm == 0x13:
                    idx = ram[(pix + y * W + x) & 0x1FFFFFF]
                else:
                    idx = 0
                out.append(pal[idx])
    return W, H, out
