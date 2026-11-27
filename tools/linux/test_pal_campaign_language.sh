#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
kit="${1:-artifacts/compiler}"
out="$(mktemp -d)"
trap 'rm -f -- "$out/check"; rmdir -- "$out"' EXIT

"${CXX:-g++}" -std=c++20 -O1 -msse4.1 -fno-strict-aliasing -Wall -Wextra -Werror \
  -DPS2_RUNTIME_LOGS=1 -DPS2X_ENABLE_IOP_RPC_TRACE=0 \
  -isystem PS2Recomp/ps2xRuntime/include -isystem PS2Recomp/ps2xIOP/include \
  tests/native/pal_campaign_language.cpp game/overrides/sotc_pal.cpp \
  PS2Recomp/ps2xTest/src/test_function_table.cpp \
  -Wl,--start-group "$kit/lib/libps2_runtime.a" "$kit/lib/libps2_iop.a" "$kit/lib/libraylib.a" \
  -Wl,--end-group -L"$kit/lib" -Wl,-rpath-link,"$kit/lib" \
  -lavcodec -lavformat -lavutil -lswresample -lswscale -lGL -lX11 -pthread -ldl -lm -o "$out/check"
timeout --kill-after=3s 10s "$out/check"
