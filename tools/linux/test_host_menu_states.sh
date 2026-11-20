#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
kit="${1:-artifacts/compiler}"
mkdir -p artifacts/state-menu-check
g++ -std=c++20 -O1 -ffunction-sections -fdata-sections \
  -IPS2Recomp/ps2xRuntime/include -I"$kit/source/raylib" \
  tests/native/host_menu_states.cpp -Wl,--gc-sections -pthread \
  -o artifacts/state-menu-check/check
timeout --kill-after=3s 15s artifacts/state-menu-check/check
g++ -std=c++20 -O1 -msse4.1 -ffunction-sections -fdata-sections \
  -DPS2_RUNTIME_LOGS=1 -DPS2X_ENABLE_IOP_RPC_TRACE=0 \
  -IPS2Recomp/ps2xRuntime/include -IPS2Recomp/ps2xIOP/include -I"$kit/source/raylib" \
  tests/native/widescreen.cpp PS2Recomp/ps2xRuntime/src/lib/desktop_video.cpp \
  -Wl,--gc-sections -o artifacts/state-menu-check/widescreen
timeout --kill-after=3s 15s artifacts/state-menu-check/widescreen
