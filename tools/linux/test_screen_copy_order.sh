#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
kit="${1:-artifacts/compiler}"
out="${SOTC_SCREEN_COPY_ORDER_TEST_OUT:-artifacts/screen-copy-order}"
mkdir -p "$out"
source=build/cmake/_deps/raylib-src/src
# One compiler process; run this script through codex-build like other heavy checks.
# No guest executable or ROM is linked into this check; kit can select a baseline.
g++ -std=c++20 -O1 -ffunction-sections -fdata-sections \
  -IPS2Recomp/ps2xRuntime/include -I"$kit/source/raylib" -I"$source/external/glfw/include" \
  tests/native/screen_copy_order.cpp -Wl,--gc-sections \
  "$kit/lib/libps2_runtime.a" "$kit/lib/libraylib.a" \
  -lGL -lX11 -pthread -ldl -lm -o "$out/check"
OMP_NUM_THREADS=1 LP_NUM_THREADS=1 xvfb-run -a timeout --kill-after=3s 30s "$out/check"
