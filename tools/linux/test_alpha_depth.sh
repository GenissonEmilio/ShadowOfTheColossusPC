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
  -IPS2Recomp/ps2xIOP/include
  -I"$kit/source/raylib" -I"${SOTC_TEST_GLFW_INCLUDE:-build/cmake/_deps/raylib-src/src/external/glfw/include}")
# Define createGlBackend directly so the archive's GL object is not selected.
# The public API implementation and all remaining dependencies use a fresh kit.
sources=(tests/native/alpha_depth.cpp PS2Recomp/ps2xRuntime/src/lib/native/native_renderer_gl.cpp)
link=(-Wl,--gc-sections -Wl,--wrap=glDrawArrays -Wl,--wrap=glDrawElements "$runtime" "$kit/lib/libraylib.a" -lGL -lX11 -pthread -ldl -lm)
g++ "${common[@]}" "${sources[@]}" "${link[@]}" -o "$out/check"
frames="$(mktemp -d "$out/frames.XXXXXX")"
trap 'rm -f -- "$frames/batched.rgba" "$frames/ordered.rgba"; rmdir -- "$frames"' EXIT
LP_NUM_THREADS=2 SOTC_GL_ALPHA_BATCH=1 SOTC_ALPHA_TEST_FRAME_DUMP="$frames/batched.rgba" \
  xvfb-run -a timeout --kill-after=2s 30s "$out/check" | tee "$out/normal.log"
LP_NUM_THREADS=2 SOTC_GL_ALPHA_BATCH=0 SOTC_GL_STATIC_VAO=0 SOTC_ALPHA_TEST_FRAME_DUMP="$frames/ordered.rgba" \
  xvfb-run -a timeout --kill-after=2s 30s "$out/check" >"$out/ordered.log"
cmp "$frames/batched.rgba" "$frames/ordered.rgba"
awk '
  $1 != "PASS:" || index($NF, "calls=") != 1 {next}
  FNR == NR {batched[$2] = substr($NF, 7); next}
  {
    expected = 0
    if ($2 ~ /^disjoint-mixed-/) expected = 14
    if ($2 ~ /^offscreen-mixed\//) expected = 16
    if ($2 ~ /^overlap-after-batched-range\//) expected = 2
    saved = substr($NF, 7) - batched[$2]
    if (saved != expected) {
      printf "FAIL: %s saved %d GL draws, expected %d\n", $2, saved, expected
      failed = 1
    }
    total += saved
    checked++
  }
  END {
    if (checked == 0 || total <= 0 || failed) exit 1
    printf "PASS: %d frames byte-identical to ordered path, %d fewer GL draws\n", checked, total
  }' "$out/normal.log" "$out/ordered.log"
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
