#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
kit="${1:-artifacts/compiler}"
mkdir -p artifacts/chara-state
source_args=()
if [[ -n "${CHARA_SOURCE:-}" ]]; then source_args+=("-DCHARA_SOURCE=\"$CHARA_SOURCE\""); fi
g++ -std=c++20 -O1 -msse4.1 -fno-strict-aliasing -ffunction-sections -fdata-sections \
  -DPS2_RUNTIME_LOGS=1 -DPS2X_ENABLE_IOP_RPC_TRACE=0 "${source_args[@]}" \
  -Igame/overrides -IPS2Recomp/ps2xRuntime/include -IPS2Recomp/ps2xIOP/include -I"$kit/source/raylib" \
  tests/native/chara_state.cpp -Wl,--gc-sections \
  -Wl,--start-group "$kit/lib/libps2_runtime.a" "$kit/lib/libps2_iop.a" \
  "$kit/lib/libraylib.a" -Wl,--end-group \
  -L"$kit/lib" -Wl,-rpath-link,"$kit/lib" \
  -lavcodec -lavformat -lavutil -lswresample -lswscale -lGL -lX11 -pthread -ldl -lm \
  -o artifacts/chara-state/check
timeout --kill-after=2s 15s artifacts/chara-state/check
