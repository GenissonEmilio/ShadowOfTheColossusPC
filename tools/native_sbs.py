#!/usr/bin/env python3
"""Side by side, large: the emulated picture (the reference) and the native
one of the same moment, for every picture of a paired run.

A run of tools/native_run.sh (SOTC_NATIVE=2, a picture every 60 game frames:
one per second of game) leaves frame_<i>.png (emulated) and native_<i>.png.
For each pair this writes <out>/sbs_<i>.png: emulated | native | signed
difference, each at 2x (1024 x 896), the difference red where the native is
brighter and blue where it is darker (x4), with the mean luminance of both
and their gap; and <out>/gaps.txt, one line per picture.

Usage: tools/native_sbs.py <run dir> [--out dir] [--scale 2] [--only i,j,...]
"""
import os, sys
from PIL import Image, ImageDraw, ImageStat


def luminance(img):
    return ImageStat.Stat(img.convert('L')).mean[0]


def signed_difference(emulated, native):
    e = emulated.convert('L').load()
    n = native.convert('L').load()
    w, h = emulated.size
    out = Image.new('RGB', (w, h))
    o = out.load()
    for y in range(h):
        for x in range(w):
            d = (n[x, y] - e[x, y]) * 4
            o[x, y] = (min(255, d), 0, 0) if d > 0 else (0, 0, min(255, -d))
    return out


def main():
    d = sys.argv[1]
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else os.path.join(d, 'sbs')
    scale = int(sys.argv[sys.argv.index('--scale') + 1]) if '--scale' in sys.argv else 2
    only = None
    if '--only' in sys.argv:
        only = {int(x) for x in sys.argv[sys.argv.index('--only') + 1].split(',')}
    os.makedirs(out, exist_ok=True)
    frames = {}
    if os.path.exists(os.path.join(d, 'index.txt')):
        for line in open(os.path.join(d, 'index.txt')):
            i, f = line.split()
            frames[int(i)] = int(f)
    lines = []
    i = 0
    while os.path.exists(os.path.join(d, f'frame_{i}.png')):
        if (only is None or i in only) and os.path.exists(os.path.join(d, f'native_{i}.png')):
            e = Image.open(os.path.join(d, f'frame_{i}.png')).convert('RGB')
            n = Image.open(os.path.join(d, f'native_{i}.png')).convert('RGB').resize(e.size)
            w, h = e.size
            diff = signed_difference(e, n)
            le, ln = luminance(e), luminance(n)
            sheet = Image.new('RGB', (w * scale * 3, h * scale + 24), (0, 0, 0))
            for k, img in enumerate((e, n, diff)):
                sheet.paste(img.resize((w * scale, h * scale), Image.NEAREST), (k * w * scale, 24))
            text = f'#{i} game frame {frames.get(i, "?")}   emulated {le:.1f}   native {ln:.1f}   gap {ln - le:+.1f}   (red: native brighter, blue: darker, x4)'
            ImageDraw.Draw(sheet).text((8, 6), text, fill=(255, 255, 255))
            sheet.save(os.path.join(out, f'sbs_{i}.png'))
            lines.append(f'{i} {frames.get(i, "?")} {le:.1f} {ln:.1f} {ln - le:+.1f}')
        i += 1
    open(os.path.join(out, 'gaps.txt'), 'w').write('\n'.join(lines) + '\n')
    print(f'{len(lines)} pairs -> {out}')


if __name__ == '__main__':
    main()
