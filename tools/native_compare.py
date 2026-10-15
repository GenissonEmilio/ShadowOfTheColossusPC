#!/usr/bin/env python3
"""Compare the native renderer's pictures with the emulated ones of the same run.

A run with SOTC_NATIVE=2 and PS2X_SNAPSHOT_FRAMES=<n> leaves, every n game
frames, frame_<i>.png (the emulated GS picture, the reference) and
native_<i>.png (the native renderer), plus index.txt (<i> <game frame>).
This ranks the pairs by difference and writes contact sheets of the worst
(reference above, native below, difference map at the bottom, red where
they differ most), with the difference per screen region, so each problem
can be looked at where and when it happens.

Usage: tools/native_compare.py shots/run [--out dir] [--worst 24]
"""
import os, sys
from PIL import Image, ImageChops, ImageDraw, ImageStat

W, H = 256, 224   # analysis size (the picture is 512 x 448)
GRID = 4          # regions per side


def load(path):
    return Image.open(path).convert('RGB').resize((W, H), Image.BILINEAR)


def regions(diff):
    out = []
    for gy in range(GRID):
        for gx in range(GRID):
            box = (gx * W // GRID, gy * H // GRID, (gx + 1) * W // GRID, (gy + 1) * H // GRID)
            out.append(sum(ImageStat.Stat(diff.crop(box)).mean) / 3.0)
    return out


def main():
    d = sys.argv[1]
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else os.path.join(d, 'compare')
    worst = int(sys.argv[sys.argv.index('--worst') + 1]) if '--worst' in sys.argv else 24
    os.makedirs(out, exist_ok=True)
    frames = {}
    if os.path.exists(os.path.join(d, 'index.txt')):
        for line in open(os.path.join(d, 'index.txt')):
            i, f = line.split()
            frames[int(i)] = int(f)
    rows = []
    i = 0
    while os.path.exists(os.path.join(d, 'frame_%d.png' % i)):
        ref, nat = os.path.join(d, 'frame_%d.png' % i), os.path.join(d, 'native_%d.png' % i)
        if os.path.exists(nat):
            a, b = load(ref), load(nat)
            diff = ImageChops.difference(a, b)
            score = sum(ImageStat.Stat(diff).mean) / 3.0
            bright = sum(ImageStat.Stat(b).mean) / 3.0 - sum(ImageStat.Stat(a).mean) / 3.0
            rows.append((score, i, frames.get(i, -1), bright, regions(diff), a, b, diff))
        i += 1
    if not rows:
        print('no frame_/native_ pairs in', d)
        return
    with open(os.path.join(out, 'report.txt'), 'w') as rep:
        rep.write('index frame diff brightness(native-ref) worst-region\n')
        for score, i, f, bright, reg, *_ in rows:
            k = max(range(len(reg)), key=lambda r: reg[r])
            rep.write('%4d %7d %6.1f %+6.1f r%d,%d=%.0f\n' % (i, f, score, bright, k % GRID, k // GRID, reg[k]))
    ranked = sorted(rows, key=lambda r: -r[0])[:worst]
    per_sheet = 6
    for s in range(0, len(ranked), per_sheet):
        chunk = ranked[s:s + per_sheet]
        sheet = Image.new('RGB', (W * len(chunk), H * 3 + 16), (0, 0, 0))
        dr = ImageDraw.Draw(sheet)
        for c, (score, i, f, bright, reg, a, b, diff) in enumerate(chunk):
            x = c * W
            sheet.paste(a, (x, 16))
            sheet.paste(b, (x, 16 + H))
            heat = diff.convert('L').point(lambda v: min(255, v * 4))
            sheet.paste(Image.merge('RGB', (heat, heat.point(lambda v: v // 3), heat.point(lambda v: v // 3))), (x, 16 + 2 * H))
            dr.text((x + 4, 2), '#%d f%d d%.0f %+.0f' % (i, f, score, bright), fill=(255, 255, 0))
        sheet.save(os.path.join(out, 'worst_%d.png' % (s // per_sheet)))
    mean = sum(r[0] for r in rows) / len(rows)
    print('%d pairs, mean diff %.1f; worst: %s' % (len(rows), mean, ', '.join('#%d(f%d)=%.0f' % (r[1], r[2], r[0]) for r in ranked[:8])))
    print('report and sheets in', out)


if __name__ == '__main__':
    main()
