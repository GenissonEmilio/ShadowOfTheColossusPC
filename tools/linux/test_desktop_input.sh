#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p artifacts/keyboard-mouse-check
g++ -std=c++20 -O1 -ffunction-sections -fdata-sections \
  -IPS2Recomp/ps2xRuntime/include -Iartifacts/compiler/source/raylib \
  tests/native/desktop_input.cpp -Wl,--gc-sections -pthread \
  -o artifacts/keyboard-mouse-check/packets
timeout --kill-after=3s 10s artifacts/keyboard-mouse-check/packets artifacts/keyboard-mouse-check/profile.cfg
