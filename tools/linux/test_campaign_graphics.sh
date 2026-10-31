#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
kit="${1:-artifacts/compiler}"
out="${SOTC_GRAPHICS_TEST_OUT:-artifacts/campaign-graphics}"
mkdir -p "$out"
extra=()
if [[ -n "${SOTC_NATIVE_TEST_SOURCE:-}" ]]; then
  extra+=("-DSOTC_NATIVE_TEST_SOURCE=\"$SOTC_NATIVE_TEST_SOURCE\"")
fi
g++ -std=c++20 -O1 -msse4.1 -fno-strict-aliasing -ffunction-sections -fdata-sections \
  -DPS2_RUNTIME_LOGS=1 -DPS2X_ENABLE_IOP_RPC_TRACE=0 "${extra[@]}" \
  -IPS2Recomp/ps2xRuntime/include -IPS2Recomp/ps2xIOP/include -I"$kit/source/raylib" \
  tests/native/campaign_graphics.cpp -Wl,--gc-sections \
  -Wl,--wrap=_ZN15native_renderer13uploadTextureEmjjPKhj \
  -Wl,--wrap=_ZN15native_renderer10hasTextureEm \
  -Wl,--wrap=_ZN15native_renderer4drawEmPKfRKNS_9DrawStateE \
  -Wl,--start-group "$kit/lib/libps2_runtime.a" "$kit/lib/libps2_iop.a" "$kit/lib/libraylib.a" -Wl,--end-group \
  -L"$kit/lib" -Wl,-rpath-link,"$kit/lib" \
  -lavcodec -lavformat -lavutil -lswresample -lswscale -lGL -lX11 -pthread -ldl -lm -o "$out/check"
timeout --kill-after=3s 30s "$out/check"
