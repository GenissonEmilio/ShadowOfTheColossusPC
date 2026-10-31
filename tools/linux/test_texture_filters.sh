#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
kit="${1:-artifacts/compiler}"
mkdir -p artifacts/texture-filters
source=build/cmake/_deps/raylib-src/src
# No libbase/guest executable or ROM is linked into this check.
g++ -std=c++20 -O1 -ffunction-sections -fdata-sections \
  -IPS2Recomp/ps2xRuntime/include -I"$kit/source/raylib" -I"$source/external/glfw/include" \
  tests/native/texture_filters.cpp -Wl,--gc-sections -Wl,--wrap=glDrawArrays \
  "$kit/lib/libps2_runtime.a" "$kit/lib/libraylib.a" \
  -lGL -lX11 -pthread -ldl -lm -o artifacts/texture-filters/check
xvfb-run -a timeout --kill-after=2s 20s artifacts/texture-filters/check
