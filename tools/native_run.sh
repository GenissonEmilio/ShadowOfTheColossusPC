#!/bin/bash
# Paired native/emulated run along a quick route, for tools/native_compare.py.
# Usage: [ENV=...] tools/native_run.sh <out dir> [last game frame]
#
# The route (game frames, PS2X_INPUT_CLOCK=frames): START through the logos
# (title at ~540), Load Game and the save in memcards/Mcd001.ps2 (CROSS
# pressed four times: the card prompt comes at varying times), the wake-up
# cutscene skipped (START 600 VBlanks after the first 3D), Wander at the
# altar from ~1950, out between the pillars (~2100), down the stairs past the
# shrine's front (~2500, the tutorial lines ~2700) and onto the plain, where
# Agro comes (~4000). The run ends by itself at the last game frame
# (PS2X_EXIT_AT_FRAME, default 4800), under two minutes. Pictures every 60
# game frames: frame_N.png (emulated), native_N.png (SOTC_NATIVE=2), index.txt.
cd "$(dirname "$0")/.."
OUT=${1:?usage: tools/native_run.sh <out dir> [last game frame]}
LAST=${2:-4800}
[ -f memcards/Mcd001.ps2 ] || { echo "needs the SotC save in memcards/Mcd001.ps2" >&2; exit 1; }
IN="300v:START:12;+120v:START:12;+120v:START:12;+120v:START:12"          # 0-3: logos, title, Load Game
IN="$IN;+150v:CROSS:8;+150v:CROSS:8;+150v:CROSS:8;+150v:CROSS:8"         # 4-7: the save, Yes
IN="$IN;@3d:WAIT;+300v@8:CROSS:8;+600v@8:START:12"                       # 8: first 3D; 9: late card prompt; 10: skip
IN="$IN;+500v:LS_LEFT:60v;+60v:LS_UP:900v"                               # 11, 12: turn, out between the pillars
IN="$IN;+950v:LS_RIGHT:40v;+40v:LS_UP:3000v"                             # 13, 14: down to the plain
rm -rf build/run-mc && mkdir -p build/run-mc && cp memcards/Mcd001.ps2 build/run-mc/
SOTC_NATIVE=${SOTC_NATIVE:-2} PS2X_SNAPSHOT_FRAMES=${PS2X_SNAPSHOT_FRAMES:-60} PS2X_EXIT_AT_FRAME=$LAST \
PS2X_MEMCARD_DIR=$PWD/build/run-mc PS2X_INPUT_CLOCK=frames PS2X_INPUT_SCRIPT="$IN" \
    ./celosia autotest 400 5 >/dev/null 2>&1
rm -rf "$OUT" && mkdir -p "$(dirname "$OUT")" && mv shots/run "$OUT"
cp logs/autotest.log "$OUT/run.log"
echo "$OUT: $(($(wc -l <"$OUT/index.txt"))) pictures, last game frame $(tail -1 "$OUT/index.txt" | cut -d' ' -f2)"
