#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
kit="${1:-artifacts/compiler}"
mkdir -p artifacts/state-components-check/files
g++ -std=c++20 -O1 -msse4.1 -fno-strict-aliasing \
  -IPS2Recomp/ps2xRuntime/include -IPS2Recomp/ps2xIOP/include -IPS2Recomp/ps2xIOP/src \
  tests/native/save_state_components.cpp \
  -Wl,--start-group "$kit/lib/libps2_runtime.a" "$kit/lib/libps2_iop.a" "$kit/lib/libraylib.a" -Wl,--end-group \
  -L"$kit/lib" -Wl,-rpath-link,"$kit/lib" \
  -lavcodec -lavformat -lavutil -lswresample -lswscale -lGL -lX11 -pthread -ldl -lm \
  -o artifacts/state-components-check/check
timeout --kill-after=3s 30s artifacts/state-components-check/check artifacts/state-components-check/files
