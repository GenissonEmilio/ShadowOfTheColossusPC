#!/bin/bash
# Measurement gate of docs/native_port_plan.md (Mac): the scripted route
# (title -> Continue -> shrine walk) driven by game frames, so every
# run passes the same moments whatever the speed. Prints one summary line,
# keeps the log and the route's frames in shots/<label>/.
# Usage: [PS2X_...=...] tools/measure.sh <label> [seconds]
#   compare: python3 tools/framediff.py shots/<before> shots/<after>
cd "$(dirname "$0")/.."
LABEL=${1:?usage: tools/measure.sh <label> [seconds]}
SECS=${2:-210}
OUT=shots/$LABEL
# Title -> Load Game -> the save in memcards/Mcd001.ps2 (shrine, 1 colossus
# defeated) -> its wake-up cutscene skipped -> Wander standing at the altar
# (the same picture every run: the section to compare speed on) -> a walk.
# Frames calibrated on the Mac (2026-10-13): the cutscene runs ~2600-3600
# (START lands in it whatever the load time), control from ~3270.
[ -f memcards/Mcd001.ps2 ] || { echo "needs the SotC save in memcards/Mcd001.ps2" >&2; exit 1; }
IN="1656:START:12;1992:START:12;2100:CROSS:8;2200:CROSS:8;3000:START:12;4000:LS_LEFT:60;4060:LS_UP:900"
# Compared frames: title and Wander standing at the altar (the same in every
# run up to his breathing). Walk frames (look_*) differ run to run (the path
# drifts with the moment control starts): only to look at side by side.
SHOTS=1000,3330,3360,3390
LOOK=4200,4500,4800
rm -rf "$OUT" build/measure-mc && mkdir -p "$OUT" build/measure-mc
cp memcards/Mcd001.ps2 build/measure-mc/
PS2X_MEMCARD_DIR=$PWD/build/measure-mc \
PS2X_GS_HW=${PS2X_GS_HW:-1} PS2X_STATS=1 PS2X_INPUT_CLOCK=frames PS2X_INPUT_SCRIPT="$IN" \
    PS2X_SNAPSHOT_FRAMES=$SHOTS,$LOOK ./celosia autotest "$SECS" 100000 >/dev/null 2>&1
cp logs/autotest.log "$OUT/run.log"
mv shots/run/gameframe_*.ppm "$OUT/" 2>/dev/null # autotest saves to shots/run
for f in ${LOOK//,/ }; do [ -f "$OUT/gameframe_$f.ppm" ] && mv "$OUT/gameframe_$f.ppm" "$OUT/look_$f.ppm"; done
python3 - "$OUT/run.log" "$LABEL" <<'EOF'
import re, sys
log = open(sys.argv[1], errors='ignore').read()
# Route sections by game frame: title/menus, Wander still at the altar, walk.
sections = [('title', 300, 1650), ('idle', 3350, 3990), ('walk', 4060, 4960)]
acc = {s[0]: [0, 0] for s in sections}
busy = {s[0]: {} for s in sections}  # thread -> busy % sums over the section's windows
counters = {}
last = 0
end = 0
window_load = None
for line in log.splitlines():
    if line.startswith('[load]'):
        window_load = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)%', line) if k != 'gsWaitsHostGl'}
        continue
    m = re.match(r'\[stats\] t=\d+ .*?frames=(\d+)', line)
    if not m:
        continue
    f = int(m[1])
    for name, a, b in sections:
        if a <= last and f <= b:
            acc[name][0] += f - last
            acc[name][1] += 5
            for k, v in (window_load or {}).items():
                busy[name][k] = busy[name].get(k, 0) + v
    for k in ('direct', 'mscnt', 'vu1Instr', 'xgkick'):
        c = re.search(r' %s=(\d+)' % k, line)
        if c and last >= 300:
            counters[k] = counters.get(k, 0) + int(c[1])
    last = end = f
fps = ' '.join('%s %4.1f' % (n, acc[n][0] / acc[n][1] if acc[n][1] else 0) for n, _, _ in sections)
# Busy ms per game frame on the still scene: thread busy share x window / frames.
ms = ' '.join('%s=%.1f' % (k, v * 0.05 / acc['idle'][0] * 1000.0) for k, v in busy['idle'].items()) if acc['idle'][0] else '-'
frames = max(end - 300, 1)
cnt = ' '.join('%s/frame=%d' % (k, v / frames) for k, v in counters.items())
done = 'route done' if end >= 4960 else 'route NOT done (frame %d)' % end
print('%-14s fps: %s | idle ms/frame: %s | %s | %s' % (sys.argv[2], fps, ms, cnt, done))
EOF
