#!/bin/bash
# Measurement gate of docs/native_port_plan.md (Mac): a scripted route
# (title -> Load Game -> shrine -> walk out) that passes the same moments at
# any speed. Prints one summary line, keeps the log and the route's pictures
# in shots/<label>/.
# Usage: [PS2X_...=...] tools/measure.sh <label> [seconds]
#   compare: python3 tools/framediff.py shots/<before> shots/<after>
#   SOTC_FRAME_INTERVAL=4: every frame lasts 4 VBlanks (15 fps), so the game
#   itself runs the same in every run (the walk ends in the same place);
#   compare the busy ms per frame then, not the fps.
cd "$(dirname "$0")/.."
LABEL=${1:?usage: tools/measure.sh <label> [seconds]}
SECS=${2:-210}
OUT=shots/$LABEL
# Timed in VBlanks ("v"), the game's own clock (the intro, the menus and
# Wander move by it): the menus from guest VBlank 2560 (START ends the intro
# demo), the rest from the wake-up cutscene starting (0xd7d824 drops to 0;
# the save in memcards/Mcd001.ps2, shrine with 1 colossus defeated, loads in
# real time). Title -> Load Game -> the save; START 10 s into the cutscene
# skips it; Wander stands at the altar (SNAP 6-8, the pictures compared run
# to run); then a turn and a 15 s walk (SNAP 11-13, to look at; the same
# place only with SOTC_FRAME_INTERVAL).
[ -f memcards/Mcd001.ps2 ] || { echo "needs the SotC save in memcards/Mcd001.ps2" >&2; exit 1; }
IN="2560v:START:12;+760v:START:12"                          # entries 0, 1: intro, title
IN="$IN;+150v:CROSS:8;+150v:CROSS:8"                         # 2, 3: Load Game, the save
IN="$IN;@d7d824=0:WAIT;+600v:START:12"                       # 4, 5: cutscene, skipped
IN="$IN;+700v:SNAP;+60v:SNAP;+60v:SNAP"                      # 6-8: standing
IN="$IN;+1080v:LS_LEFT:60v;+60v:LS_UP:900v"                  # 9, 10: turn, walk
IN="$IN;+300v:SNAP;+300v:SNAP;+600v:SNAP"                    # 11-13: walk pictures
rm -rf "$OUT" build/measure-mc && mkdir -p "$OUT" build/measure-mc
cp memcards/Mcd001.ps2 build/measure-mc/
PS2X_MEMCARD_DIR=$PWD/build/measure-mc \
PS2X_GS_HW=${PS2X_GS_HW:-1} PS2X_STATS=1 PS2X_INPUT_CLOCK=frames PS2X_INPUT_SCRIPT="$IN" \
    ./celosia autotest "$SECS" 100000 >/dev/null 2>&1
cp logs/autotest.log "$OUT/run.log"
mv shots/run/gameframe_*.ppm shots/run/snap_*.ppm "$OUT/" 2>/dev/null # autotest saves to shots/run
for n in 11 12 13; do [ -f "$OUT/snap_$n.ppm" ] && mv "$OUT/snap_$n.ppm" "$OUT/look_$n.ppm"; done
python3 - "$OUT/run.log" "$LABEL" <<'EOF'
import re, sys
log = open(sys.argv[1], errors='ignore').read()
# Script entries' first game frame and VBlank ([input] lines).
entry = {int(i): (int(f), int(v)) for i, f, v in re.findall(r'\[input\] entry (\d+) pressed at frame (\d+) vblank (\d+)', log)}
released = {int(i): (int(f), int(v)) for i, f, v in re.findall(r'\[input\] entry (\d+) released at frame (\d+) vblank (\d+)', log)}
never = (10**9, 10**9)
# Sections: menus (by frame), Wander standing (SNAP 6 -> the turn), the walk.
sections = [('title', (300, 0), (1650, 0)), ('idle', entry.get(6, never), entry.get(9, never)),
            ('walk', entry.get(10, never), released.get(10, never))]
fps = {}
for name, a, b in sections:
    if name != 'title' and b != never and b[1] > a[1]:
        fps[name] = (b[0] - a[0]) / ((b[1] - a[1]) / 59.94) # frames per second of guest time
# Busy ms per frame per thread over each section: [load] windows (5 s) inside it.
busy = {name: {} for name, _, _ in sections}
frames = {name: 0 for name, _, _ in sections}
counters = {}
last = 0
end = 0
window_load = None
title = [0, 0]
for line in log.splitlines():
    if line.startswith('[load]'):
        window_load = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)%', line) if k != 'gsWaitsHostGl'}
        continue
    m = re.match(r'\[stats\] t=\d+ .*?frames=(\d+)', line)
    if not m:
        continue
    f = int(m[1])
    for name, a, b in sections:
        if a[0] <= last and f <= b[0]:
            frames[name] += f - last
            for k, v in (window_load or {}).items():
                busy[name][k] = busy[name].get(k, 0) + v
            if name == 'title':
                title[0] += f - last
                title[1] += 5
    for k in ('direct', 'mscnt', 'vu1Instr', 'xgkick'):
        c = re.search(r' %s=(\d+)' % k, line)
        if c and last >= 300:
            counters[k] = counters.get(k, 0) + int(c[1])
    last = end = f
if title[1]:
    fps['title'] = title[0] / title[1]
fpsText = ' '.join('%s %4.1f' % (n, fps.get(n, 0.0)) for n, _, _ in sections)
# Busy share x 5 s window / frames in the windows = ms per frame.
def ms(name):
    if not frames[name]:
        return '-'
    return ' '.join('%s=%.1f' % (k, v * 0.05 / frames[name] * 1000.0) for k, v in busy[name].items())
total = max(end - 300, 1)
cnt = ' '.join('%s/frame=%d' % (k, v / total) for k, v in counters.items())
done = 'route done' if 13 in entry else 'route NOT done (frame %d)' % end
print('%-14s fps: %s | ms/frame idle: %s | walk: %s | %s | %s' % (sys.argv[2], fpsText, ms('idle'), ms('walk'), cnt, done))
EOF
