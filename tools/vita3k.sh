#!/bin/bash
# Runs the Vita build in a Vita3K of its own: config.yml and Vita FS under
# $CELOSIA_VITA3K (default ../vita3k-celosia), apart from other ports.
#
# Usage: tools/vita3k.sh [seconds]
#   no argument: install build/vita/.../ps2EntryRunner.vpk and play
#   seconds:     run that long in the background, then stop (logs stay)
# Env: MHZ / CORES (real Vita speed: MHZ=444 CORES=3), PROF=<file> (profile),
#      NOINSTALL=1 (keep the installed app).
# Logs: $CELOSIA_VITA3K/fs/ux0/data/celosia/{log,err}.txt
cd "$(dirname "$0")/.."
V3="${CELOSIA_VITA3K:-$(cd .. && pwd)/vita3k-celosia}"
FS="$V3/fs"
VITA3K="${VITA3K:-$HOME/Documents/workspace/games/Vita3K/build/macos-ninja/bin/Release/Vita3K.app/Contents/MacOS/Vita3K}"
if [ ! -f "$V3/config.yml" ] || [ ! -d "$FS/vs0" ]; then
    echo "no Vita3K tree at $V3 (config.yml + fs/ with firmware)" >&2
    exit 1
fi
VPK=build/vita/ps2xRuntime/ps2EntryRunner.vpk
if [ -z "$NOINSTALL" ]; then
    [ -f "$VPK" ] || { echo "no $VPK (./celosia vita)" >&2; exit 1; }
    rm -rf "$FS/ux0/app/CELO00001" && mkdir -p "$FS/ux0/app/CELO00001"
    unzip -q -o "$VPK" -d "$FS/ux0/app/CELO00001"
fi
mkdir -p "$FS/ux0/data/celosia"
[ -e "$FS/ux0/data/celosia/disc" ] || ln -s "$(pwd)/disc" "$FS/ux0/data/celosia/disc"
rm -f "$FS/ux0/data/celosia/log.txt" "$FS/ux0/data/celosia/err.txt"
run() {
    VITA3K_PROFILE="${PROF:-}" VITA3K_CPU_MHZ="${MHZ:-}" VITA3K_CORES="${CORES:-}" VITA3K_PREF_PATH="$FS/" \
        "$VITA3K" -c "$V3/config.yml" -r CELO00001
}
if [ -z "$1" ]; then
    run
else
    run > "$V3/vita3k-stdout.txt" 2>&1 &
    sleep "$1"
    pkill -f "$V3/config.yml -r CELO00001"
    for _ in 1 2 3 4 5; do pgrep -f "$V3/config.yml -r CELO00001" >/dev/null || break; sleep 1; done
    pkill -9 -f "$V3/config.yml -r CELO00001" # it can ignore SIGTERM
    echo "logs: $FS/ux0/data/celosia/err.txt"
fi
