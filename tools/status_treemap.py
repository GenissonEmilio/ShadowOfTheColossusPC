#!/usr/bin/env python3
"""Draws docs/port_status.json as a treemap (SVG) for the README.

One map per "map" (renderer, hardware), split in areas, one tile per feature,
coloured by status (native / partial / emulated). Run it after editing the
JSON: tools/status_treemap.py [docs/port_status.json] [docs/port_status.svg]
"""
import html
import json
import os
import sys
from collections import OrderedDict

ROOT = os.path.join(os.path.dirname(__file__), '..')
COLOURS = {'native': '#2f9e5b', 'partial': '#d9a521', 'emulated': '#c8553d'}
TITLES = {'renderer': 'Native renderer', 'hardware': 'PS2 hardware'}


def squarify(items, x, y, w, h):
    """items: [(weight, payload)] -> [(x, y, w, h, payload)] (squarified)."""
    out = []
    items = sorted(items, key=lambda t: -t[0])
    total = sum(t[0] for t in items) or 1.0
    scale = w * h / total
    rest = [(wt * scale, p) for wt, p in items]
    while rest:
        short = min(w, h)
        row = [rest[0]]
        def worst(r):
            s = sum(a for a, _ in r)
            return max(max(short * short * a / (s * s), (s * s) / (short * short * a)) for a, _ in r)
        i = 1
        while i < len(rest) and worst(row + [rest[i]]) <= worst(row):
            row.append(rest[i])
            i += 1
        rest = rest[i:]
        s = sum(a for a, _ in row)
        if w >= h:  # a column on the left
            cw = s / h
            cy = y
            for a, p in row:
                ch = a / cw
                out.append((x, cy, cw, ch, p))
                cy += ch
            x += cw
            w -= cw
        else:  # a row on top
            rh = s / w
            cx = x
            for a, p in row:
                cw2 = a / rh
                out.append((cx, y, cw2, rh, p))
                cx += cw2
            y += rh
            h -= rh
    return out


def wrap(text, width_px, size):
    chars = max(4, int(width_px / (size * 0.56)))
    words, lines, line = text.split(), [], ''
    for word in words:
        if len(line) + len(word) + (1 if line else 0) <= chars:
            line = f'{line} {word}' if line else word
        else:
            if line:
                lines.append(line)
            line = word[:chars]
    if line:
        lines.append(line)
    return lines


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'docs/port_status.json')
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'docs/port_status.svg')
    features = json.load(open(src))
    maps = OrderedDict()
    for f in features:
        maps.setdefault(f['map'], OrderedDict()).setdefault(f['area'], []).append(f)

    width, map_h, gap, head = 1000, 420, 14, 34
    total_h = 70 + len(maps) * (map_h + head + gap) + 40
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{total_h}" viewBox="0 0 {width} {total_h}" '
         'font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">',
         f'<rect width="{width}" height="{total_h}" fill="#0d1117"/>']
    counts = {k: sum(1 for f in features if f['status'] == k) for k in COLOURS}
    native = counts['native']
    s.append(f'<text x="16" y="34" fill="#e6edf3" font-size="22" font-weight="600">Celosia: what is native</text>')
    s.append(f'<text x="16" y="56" fill="#8b949e" font-size="13">{native} of {len(features)} features native '
             f'({100 * native // max(1, len(features))}%). Generated from docs/port_status.json by tools/status_treemap.py.</text>')
    lx = width - 300
    for i, (k, c) in enumerate(COLOURS.items()):
        s.append(f'<rect x="{lx + i * 95}" y="24" width="12" height="12" rx="2" fill="{c}"/>'
                 f'<text x="{lx + i * 95 + 17}" y="35" fill="#c9d1d9" font-size="12">{k} ({counts[k]})</text>')
    y = 70
    for name, areas in maps.items():
        n = sum(len(v) for v in areas.values())
        done = sum(1 for v in areas.values() for f in v if f['status'] == 'native')
        s.append(f'<text x="16" y="{y + 22}" fill="#e6edf3" font-size="16" font-weight="600">{html.escape(TITLES.get(name, name))}'
                 f'<tspan fill="#8b949e" font-weight="400" font-size="13">  {done}/{n} native</tspan></text>')
        y += head
        for ax, ay, aw, ah, (area, feats) in squarify([(len(v), (a, v)) for a, v in areas.items()], 16, y, width - 32, map_h):
            s.append(f'<rect x="{ax:.1f}" y="{ay:.1f}" width="{aw:.1f}" height="{ah:.1f}" fill="#161b22" stroke="#0d1117" stroke-width="3"/>')
            s.append(f'<text x="{ax + 6:.1f}" y="{ay + 15:.1f}" fill="#8b949e" font-size="11" font-weight="600">{html.escape(area.upper())}</text>')
            inner = squarify([(1.0, f) for f in feats], ax + 3, ay + 21, aw - 6, ah - 24)
            for fx, fy, fw, fh, f in inner:
                colour = COLOURS.get(f['status'], '#6e7681')
                tip = html.escape(f"{f['feature']}: {f['status']} since {f.get('since', '?')} ({f.get('commit', '')}). {f.get('note', '')}")
                s.append(f'<g><title>{tip}</title><rect x="{fx + 1:.1f}" y="{fy + 1:.1f}" width="{max(0, fw - 2):.1f}" '
                         f'height="{max(0, fh - 2):.1f}" rx="3" fill="{colour}"/>')
                size = 11 if fw > 90 else 9
                lines = wrap(f['feature'], fw - 8, size)
                max_lines = max(0, int((fh - 6) // (size + 2)))
                for li, line in enumerate(lines[:max_lines]):
                    s.append(f'<text x="{fx + 5:.1f}" y="{fy + 4 + (li + 1) * (size + 2):.1f}" fill="#ffffff" font-size="{size}">{html.escape(line)}</text>')
                s.append('</g>')
        y += map_h + gap
    s.append('</svg>')
    open(dst, 'w').write('\n'.join(s) + '\n')
    print(f'{dst}: {len(features)} features, {native} native')


if __name__ == '__main__':
    main()
