#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
kit="${1:-artifacts/compiler}"
out="${SOTC_COLOUR_TEST_OUT:-artifacts/renderer-colours}"
mkdir -p "$out"
g++ -std=c++20 -O1 -ffunction-sections -fdata-sections \
  -IPS2Recomp/ps2xRuntime/include -I"$kit/source/raylib" -Ibuild/cmake/_deps/raylib-src/src/external/glfw/include \
  tests/native/renderer_colours.cpp -Wl,--gc-sections \
  "$kit/lib/libps2_runtime.a" "$kit/lib/libraylib.a" -lGL -lX11 -pthread -ldl -lm -o "$out/check"
xvfb-run -a timeout --kill-after=2s 20s "$out/check"
