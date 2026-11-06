#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
kit="${1:-artifacts/compiler}"
out="${SOTC_SKIN_MULTIPASS_TEST_OUT:-artifacts/skinned-multipass}"
mkdir -p "$out"
extra=()
if [[ -n "${SOTC_NATIVE_TEST_SOURCE:-}" ]]; then extra+=("-DSOTC_NATIVE_TEST_SOURCE=\"$SOTC_NATIVE_TEST_SOURCE\""); fi
wraps=(
  _ZN2GS10skipRasterEv
  _ZN15native_renderer7hasMeshEm
  _ZN15native_renderer11gpuSkinningEv
  _ZN15native_renderer4drawEmPKfRKNS_9DrawStateE
  _ZN15native_renderer10uploadMeshEmPKNS_6VertexEjb
  _ZN15native_renderer17uploadSkinnedMeshEmPKNS_10SkinVertexEjPKjj
  _ZN15native_renderer9frameMeshEj
  _ZN15native_renderer16frameMeshIndicesEjRj
  _ZN15native_renderer15commitFrameMeshEmjj
  _ZN15native_renderer12framePaletteEPKfj
)
for symbol in "${wraps[@]}"; do extra+=("-Wl,--wrap=$symbol"); done
g++ -std=c++20 -O1 -msse4.1 -fno-strict-aliasing -ffunction-sections -fdata-sections \
  -DPS2_RUNTIME_LOGS=1 -DPS2X_ENABLE_IOP_RPC_TRACE=0 "${extra[@]}" \
  -Igame/overrides -IPS2Recomp/ps2xRuntime/include -IPS2Recomp/ps2xIOP/include -I"$kit/source/raylib" \
  tests/native/skinned_multipass.cpp -Wl,--gc-sections \
  -Wl,--start-group "$kit/lib/libps2_runtime.a" "$kit/lib/libps2_iop.a" "$kit/lib/libraylib.a" -Wl,--end-group \
  -L"$kit/lib" -Wl,-rpath-link,"$kit/lib" \
  -lavcodec -lavformat -lavutil -lswresample -lswscale -lGL -lX11 -pthread -ldl -lm -o "$out/check"
timeout --kill-after=3s 30s "$out/check"
