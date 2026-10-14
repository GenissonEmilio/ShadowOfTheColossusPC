#!/usr/bin/env python3
"""Compare the route frames of two tools/measure.sh runs (needs Pillow).

Usage: tools/framediff.py <dir before> <dir after> [threshold]

For each gameframe_<n>.ppm or snap_<n>.ppm in both: a pixel counts as changed when no pixel
of the other frame within one pixel of it is within <threshold> (default 24)
in every channel, so sub-pixel camera sway and edge jitter between runs are
not counted, while wrong colours, textures or missing geometry are. Frames
with over 1.5% changed pixels are flagged and get diff_<name>.png next to the
after frame (changed pixels in red over the darkened after frame). Exit
status 1 when any frame is flagged.
"""
import os
import re
import sys

from PIL import Image, ImageChops


def channel_max(image):
    r, g, b = image.split()
    return ImageChops.lighter(ImageChops.lighter(r, g), b)


def min_neighbour_diff(a, b):
    """Per pixel: smallest max-channel difference between a and b shifted by up to one pixel."""
    best = None
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            d = channel_max(ImageChops.difference(a, ImageChops.offset(b, dx, dy)))
            best = d if best is None else ImageChops.darker(best, d)
    return best


def changed_mask(a, b, threshold):
    # Both ways: what a has that b lacks nearby, and the reverse.
    d = ImageChops.lighter(min_neighbour_diff(a, b), min_neighbour_diff(b, a))
    return d.point(lambda v: 255 if v > threshold else 0)


def main():
    before, after = sys.argv[1], sys.argv[2]
    threshold = int(sys.argv[3]) if len(sys.argv) > 3 else 24
    picture = re.compile(r'(gameframe|snap)_\d+\.ppm$')
    names = sorted((n for n in os.listdir(after) if picture.match(n)),
                   key=lambda n: (n[0], int(re.findall(r'\d+', n)[0])))
    flagged = 0
    for name in names:
        ref = os.path.join(before, name)
        if not os.path.exists(ref):
            print('%-22s only in %s' % (name, after))
            continue
        a = Image.open(ref).convert('RGB')
        b = Image.open(os.path.join(after, name)).convert('RGB')
        if a.size != b.size:
            print('%-22s size %dx%d vs %dx%d' % (name, a.size[0], a.size[1], b.size[0], b.size[1]))
            flagged += 1
            continue
        mask = changed_mask(a, b, threshold)
        # Ignore the one-pixel border (the shifts wrap around there).
        w, h = a.size
        mask = mask.crop((1, 1, w - 1, h - 1))
        changed = mask.histogram()[255]
        share = 100.0 * changed / ((w - 2) * (h - 2))
        mean = sum(i * n for i, n in enumerate(channel_max(ImageChops.difference(a, b)).histogram())) / (w * h)
        bad = share > 1.5
        flagged += bad
        print('%-22s mean %5.2f  changed %6.2f%%%s' % (name, mean, share, '  <-- look' if bad else ''))
        if bad:
            dark = b.point(lambda v: v // 3)
            red = Image.new('RGB', (w - 2, h - 2), (255, 0, 0))
            dark.paste(red, (1, 1), mask)
            dark.save(os.path.join(after, 'diff_' + name.replace('.ppm', '.png')))
    for name in sorted(os.listdir(before)):
        if picture.match(name) and name not in names:
            print('%-22s missing in %s' % (name, after))
            flagged += 1
    sys.exit(1 if flagged else 0)


if __name__ == '__main__':
    main()
