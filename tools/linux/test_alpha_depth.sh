#!/usr/bin/env bash
# Invoke inside the root's protected Docker window; no nested resource guard.
set -euo pipefail
cd "$(dirname "$0")/../.."
kit="${1:-artifacts/compiler}"
runtime="${2:-build/cmake/ps2xRuntime/libps2_runtime.a}"
out="${SOTC_ALPHA_TEST_OUT:-artifacts/alpha-depth}"
mkdir -p "$out"
[[ -f "$runtime" ]] || { echo "Missing fresh runtime archive: $runtime" >&2; exit 2; }
for source in PS2Recomp/ps2xRuntime/include/runtime/native_renderer.h PS2Recomp/ps2xRuntime/src/lib/native/native_renderer.cpp; do
  [[ ! "$source" -nt "$runtime" ]] || { echo "Runtime predates $source; rebuild first" >&2; exit 2; }
done
common=(-std=c++20 -O1 -msse4.1 -ffunction-sections -fdata-sections
  -IPS2Recomp/ps2xRuntime/include -IPS2Recomp/ps2xRuntime/src/lib/native
  -I"$kit/source/raylib" -Ibuild/cmake/_deps/raylib-src/src/external/glfw/include)
# Define createGlBackend directly so the archive's GL object is not selected.
# The public API implementation and all remaining dependencies use a fresh kit.
sources=(tests/native/alpha_depth.cpp PS2Recomp/ps2xRuntime/src/lib/native/native_renderer_gl.cpp)
link=(-Wl,--gc-sections "$runtime" "$kit/lib/libraylib.a" -lGL -lX11 -pthread -ldl -lm)
g++ "${common[@]}" "${sources[@]}" "${link[@]}" -o "$out/check"
LP_NUM_THREADS=2 xvfb-run -a timeout --kill-after=2s 30s "$out/check" | tee "$out/normal.log"
if [[ ${SOTC_ALPHA_BASELINE:-0} == 1 ]]; then
  g++ "${common[@]}" -DSOTC_ALPHA_LEGACY_TEST "${sources[@]}" "${link[@]}" -o "$out/baseline"
  set +e
  LP_NUM_THREADS=2 xvfb-run -a timeout --kill-after=2s 30s "$out/baseline" >"$out/baseline.log" 2>&1
  status=$?
  set -e
  cat "$out/baseline.log"
  [[ $status == 1 ]] || { echo "Expected assertion failure from legacy alpha/depth path, got $status" >&2; exit 1; }
  grep -Eq '^FAIL: fur-scar-glow/.*/depth-probe' "$out/baseline.log"
  grep -Eq '^FAIL: [0-9]+ alpha/depth GPU checks; [1-9][0-9]* failures' "$out/baseline.log"
fi
