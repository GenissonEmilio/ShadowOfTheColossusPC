#!/usr/bin/env bash
# Resource guard is outside Docker; no nested codex-build in the container.
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ "${1:-}" != --inside-container ]]; then
  out="${SOTC_PROVENANCE_TEST_OUT:-$repo/artifacts/gs-provenance-tests}"
  mkdir -p "$out"
  { cat /proc/meminfo; cat /proc/pressure/memory; ps -eo pid,comm,rss --sort=-rss | sed -n '1,12p'; } >"$out/preflight.log"
  exec /home/genissonemilio/.local/bin/codex-build -- docker run --rm --init --pull=never \
    --cpus=2 --memory=1600m --memory-swap=1600m --network=none \
    --user "$(id -u):$(id -g)" -v "$repo:/src:z" --workdir /src \
    "${SOTC_BUILDER_IMAGE:-sotc-linux-builder}" \
    bash /src/tools/linux/test_gs_provenance.sh --inside-container "$@"
fi
shift
cd "$repo"
out="${SOTC_PROVENANCE_TEST_OUT:-artifacts/gs-provenance-tests}"
mkdir -p "$out"
common=(-std=c++20 -O1 -msse4.1 -ffunction-sections -fdata-sections
  -IPS2Recomp/ps2xRuntime/include -IPS2Recomp/ps2xRuntime/src/lib -IPS2Recomp/ps2xIOP/include)
# Compile the REAL implementation TU, not generated/extracted copies. The
# unrelated interpreter functions are dropped by --gc-sections in this test.
g++ "${common[@]}" tests/native/gs_provenance.cpp PS2Recomp/ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp \
  -Wl,--gc-sections -pthread -o "$out/parsers"
timeout --kill-after=2s 20s "$out/parsers"
if [[ -n "${1:-}" ]]; then
  # Optional integration: supply the FRESH runtime archive separately from
  # dependency-only kit. Never silently pick an installed old runtime.
  runtime="$1"
  kit="${2:-artifacts/compiler}"
  [[ -f "$runtime" ]] || { echo "Missing fresh runtime archive: $runtime" >&2; exit 2; }
  for source in PS2Recomp/ps2xRuntime/src/lib/{ps2_memory.cpp,ps2_gif_events.h,ps2_vif1_compact.h,ps2_vif1_interpreter.cpp} \
      PS2Recomp/ps2xRuntime/include/runtime/ps2_memory.h \
      PS2Recomp/ps2xRuntime/include/runtime/gs/{gs_provenance.h,gs_provenance_ledger.h}; do
    [[ ! "$source" -nt "$runtime" ]] || { echo "Runtime archive predates $source; rebuild first" >&2; exit 2; }
  done
  nm -C --defined-only "$runtime" | grep 'PS2Memory::gsProvenanceQuiescent' >/dev/null || {
    echo 'Runtime lacks the actual provenance implementation; refusing old kit' >&2; exit 2;
  }
  g++ "${common[@]}" -DSOTC_PROVENANCE_MEMORY_TEST -I"$kit/source/raylib" \
    tests/native/gs_provenance.cpp -Wl,--gc-sections \
    -Wl,--wrap=_ZN15native_renderer5shownEv \
    -Wl,--start-group "$runtime" "$kit/lib/libps2_iop.a" "$kit/lib/libraylib.a" -Wl,--end-group \
    -L"$kit/lib" -Wl,-rpath-link,"$kit/lib" \
    -lavcodec -lavformat -lavutil -lswresample -lswscale -lGL -lX11 -pthread -ldl -lm -o "$out/memory"
  for full in 0 1; do
    SOTC_NATIVE=1 PS2X_GS_NODRAW=1 PS2X_MTVU=0 PS2X_VIF1_FULL="$full" \
      timeout --kill-after=2s 20s "$out/memory"
  done
fi
