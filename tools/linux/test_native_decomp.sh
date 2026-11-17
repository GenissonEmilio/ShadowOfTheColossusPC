#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
kit="${1:-artifacts/compiler}"
out="${SOTC_NATIVE_DECOMP_TEST_OUT:-artifacts/native-decomp}"
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O1 -msse4.1 -fno-strict-aliasing -ffunction-sections -fdata-sections \
  -DPS2_RUNTIME_LOGS=1 -DPS2X_ENABLE_IOP_RPC_TRACE=0 \
  -IPS2Recomp/ps2xRuntime/include -IPS2Recomp/ps2xIOP/include -I"$kit/source/raylib" \
  tests/native/native_decomp.cpp game/overrides/sotc_decomp.cpp game/overrides/decomp_math.cpp \
  game/overrides/decomp_curve.cpp game/overrides/decomp_texsearch.cpp game/overrides/decomp_heap120.cpp \
  -Wl,--gc-sections -Wl,--start-group "$kit/lib/libps2_runtime.a" "$kit/lib/libps2_iop.a" \
  "$kit/lib/libraylib.a" -Wl,--end-group -L"$kit/lib" -Wl,-rpath-link,"$kit/lib" \
  -lavcodec -lavformat -lavutil -lswresample -lswscale -lGL -lX11 -pthread -ldl -lm -o "$out/check"
env -u SOTC_DECOMP -u SOTC_DECOMP_SKIP -u SOTC_DIFFTEST "$out/check"
env -u SOTC_DECOMP -u SOTC_DIFFTEST SOTC_DECOMP_SKIP=curveEval "$out/check" skip
