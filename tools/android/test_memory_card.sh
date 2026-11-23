#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ANDROID_NDK_HOME=/path/to/ndk bash tools/android/test_memory_card.sh CMAKE_DEPS_DIR

Use the _deps directory from an existing port build to find SSE2NEON and raylib headers.
The probe writes a synthetic card page and reopens it from another process on an
ARM64 Android device. It uses a new directory in /data/local/tmp/sotc-card-test-work.
Set ANDROID_SERIAL if multiple devices are connected, or ADB for an adb path off PATH.
EOF
}

fail() {
    printf 'test_memory_card: %s\n' "$*" >&2
    exit 1
}

if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
    usage
    exit 0
fi
if [[ $# -ne 1 ]]; then
    usage >&2
    exit 1
fi
[[ -n "${ANDROID_NDK_HOME:-}" ]] || fail 'Set ANDROID_NDK_HOME to the Android NDK directory.'
[[ -d "$1" ]] || fail "Dependency directory not found: $1"
deps=$(cd -- "$1" && pwd -P)
[[ -f "$deps/sse2neon-src/sse2neon.h" && -f "$deps/raylib-src/src/raylib.h" ]] ||
    fail 'Expected sse2neon-src and raylib-src under CMAKE_DEPS_DIR.'
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
runtime="$root/PS2Recomp/ps2xRuntime"

case "$(uname -s)" in
    Linux) compiler="linux-x86_64/bin/clang++" ;;
    MINGW*|MSYS*) compiler="windows-x86_64/bin/clang++.exe" ;;
    Darwin) compiler="darwin-x86_64/bin/clang++" ;;
    *) fail 'Unsupported NDK host.' ;;
esac
compiler="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$compiler"
[[ -x "$compiler" ]] || fail "NDK compiler not found: $compiler"
adb=${ADB:-adb}
command -v "$adb" >/dev/null || fail "ADB was not found: $adb"

device() {
    MSYS2_ARG_CONV_EXCL='*' "$adb" "$@"
}

work=$(mktemp -d)
remote=
cleanup() {
    if [[ -n "$remote" ]]; then
        device shell "rm -f '$remote/card-probe' '$remote/cards/Mcd001.ps2'; rmdir '$remote/cards' '$remote'" \
            >/dev/null 2>&1 || true
    fi
    rm -f -- "$work/card-probe"
    rmdir -- "$work"
}
trap cleanup EXIT

"$compiler" --target=aarch64-linux-android28 -std=c++20 -O2 -fPIE -pie -static-libstdc++ \
    -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,-z,max-page-size=16384 \
    -DUSE_SSE2NEON -DPLATFORM_ANDROID \
    -I"$runtime/include" -I"$runtime/src/lib/Kernel" -I"$root/PS2Recomp/ps2xIOP/include" \
    -I"$deps/sse2neon-src" -I"$deps/raylib-src/src" \
    "$root/tests/android/memory_card.cpp" "$runtime/src/lib/Kernel/Stubs/MemoryCard2.cpp" \
    -o "$work/card-probe"

candidate=$(device shell 'set -e; umask 077; base=/data/local/tmp/sotc-card-test-work; test ! -L "$base"; mkdir -p "$base"; mktemp -d "$base/run.XXXXXXXX"' | tr -d '\r')
[[ "$candidate" =~ ^/data/local/tmp/sotc-card-test-work/run\.[a-zA-Z0-9]{8}$ ]] ||
    fail 'The device returned an unexpected test directory.'
remote=$candidate
executable="$work/card-probe"
if [[ "$compiler" == *.exe ]]; then
    executable=$(cygpath -am "$executable")
fi
device push "$executable" "$remote/card-probe"
device shell "set -e; chmod 700 '$remote/card-probe'; mkdir -m 700 '$remote/cards'"
device shell "'$remote/card-probe' write '$remote/cards'"
device shell "'$remote/card-probe' read '$remote/cards'"
