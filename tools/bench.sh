#!/usr/bin/env bash
# Scripted New Game run (title -> intro -> shrine) printing the average game
# fps per section. Extra environment variables are passed to the runner.
# Usage: [VAR=value ...] tools/bench.sh [label]
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
rm -rf build/autotest-memcards
# START skips the intro movie; CROSS is pressed until New Game starts.
INPUT="35:START:0.2;45:START:0.2"
for t in $(seq 56 2 90); do INPUT="$INPUT;$t:CROSS:0.2"; done
PS2X_STATS=1 PS2X_INPUT_SCRIPT="$INPUT" ./celosia autotest 190 ${BENCH_SHOTS:-60} >/dev/null
grep -a '\[stats\]' logs/autotest.log | awk -v label="${1:-run}" -v load="$(sysctl -n vm.loadavg 2>/dev/null | awk '{print $2}')" '
{ split($2, t, "="); split($3, f, "="); s = t[2] + 0; v = f[2] + 0
  if (s >= 20 && s < 60)  { a += v; na++ }
  if (s >= 60 && s < 110) { b += v; nb++ }
  if (s >= 130)           { c += v; nc++ } }
END { printf "%-12s title/intro=%5.1f  bridge=%5.1f  shrine=%5.1f  (load %s)\n", label, a/na, b/nb, c/nc, load }'
