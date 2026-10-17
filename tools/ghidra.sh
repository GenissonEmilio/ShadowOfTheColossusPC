#!/bin/bash
# Ghidra (headless) over the game: tools/ghidra.sh import   (once: builds build/ghidra from recomp/game/SOTC.elf, ~2 min)
#                                    tools/ghidra.sh <Script.java> args...   (tools/ghidra/*.java; output lines only)
# Needs Ghidra with the Emotion Engine extension at $GHIDRA (default ~/tools/ghidra_12.1.3_PUBLIC).
cd "$(dirname "$0")/.."
GHIDRA=${GHIDRA:-$HOME/tools/ghidra_12.1.3_PUBLIC}
PROJ=$PWD/build/ghidra
if [ "$1" = import ]; then
    rm -rf "$PROJ" && mkdir -p "$PROJ"
    "$GHIDRA/support/analyzeHeadless" "$PROJ" sotc -import recomp/game/SOTC.elf -processor r5900:LE:32:default \
        -scriptPath "$PWD/tools/ghidra" -postScript ApplyRecompNames.java "$PWD/recomp/game/functions.csv" "$PWD/recomp/game/symbols.txt" 2>&1 | grep -E "ERROR|named|Import succeeded" 
    exit
fi
script=$1; shift
"$GHIDRA/support/analyzeHeadless" "$PROJ" sotc -process SOTC.elf -noanalysis -scriptPath "$PWD/tools/ghidra" -postScript "$script" "$@" 2>&1 \
    | grep "> " | grep -v "^INFO  HEADLESS\|Loading\|Searching\|Class search\|Initializing\|Random\|Trust\|extension points\|Opening\|REPORT\|Using log\|execution starts" | sed 's/^INFO  [A-Za-z]*\.java> //; s/ (GhidraScript)$//'
