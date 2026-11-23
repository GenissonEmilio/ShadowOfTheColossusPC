#!/usr/bin/env bash
set -euo pipefail

version=7.1.5
source_url=https://ffmpeg.org/releases/ffmpeg-${version}.tar.xz
source_sha256=de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f

usage() {
    cat <<'EOF'
Usage: ANDROID_NDK_HOME=/path/to/ndk bash tools/android/build_ffmpeg.sh SOURCE_DIR INSTALL_PREFIX

Build FFmpeg 7.1.5 static PIC libraries for Android arm64-v8a, API 28.
Use a clean extraction of the release tarball. The build runs in SOURCE_DIR.
On Windows, run from Git Bash with the LLVM-MinGW host compiler on PATH.

Optional environment variables:
  FFMPEG_JOBS       Parallel build jobs (default: 2)
  FFMPEG_HOST_CC    Host compiler on PATH (default: cc on Linux,
                    x86_64-w64-mingw32-clang on Windows)
EOF
}

fail() {
    printf 'build_ffmpeg: %s\n' "$*" >&2
    exit 1
}

if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
    usage
    exit 0
fi
if [[ $# -ne 2 || -z "$1" || -z "$2" ]]; then
    usage >&2
    exit 1
fi

[[ -n "${ANDROID_NDK_HOME:-}" ]] || fail 'Set ANDROID_NDK_HOME to the Android NDK directory.'
[[ -d "$ANDROID_NDK_HOME" ]] || fail "NDK directory not found: $ANDROID_NDK_HOME"
[[ -d "$1" ]] || fail "FFmpeg source directory not found: $1"
source_dir=$(cd -- "$1" && pwd -P)
ndk_dir=$(cd -- "$ANDROID_NDK_HOME" && pwd -P)
[[ -f "$source_dir/configure" && -f "$source_dir/RELEASE" && -f "$source_dir/COPYING.LGPLv2.1" ]] ||
    fail "Use an extracted FFmpeg $version release as SOURCE_DIR."
[[ "$(tr -d '\r\n' < "$source_dir/RELEASE")" == "$version" ]] || fail "Expected FFmpeg $version."

jobs=${FFMPEG_JOBS:-2}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail 'FFMPEG_JOBS must be a positive integer.'

case "$(uname -s)" in
    Linux)
        host_tag=linux-x86_64
        host_cc=${FFMPEG_HOST_CC:-cc}
        exe_suffix=
        ;;
    MINGW*|MSYS*)
        host_tag=windows-x86_64
        host_cc=${FFMPEG_HOST_CC:-x86_64-w64-mingw32-clang}
        exe_suffix=.exe
        ;;
    *) fail 'Run this script on Linux or in Git Bash on Windows.' ;;
esac

toolchain="$ndk_dir/toolchains/llvm/prebuilt/$host_tag"
[[ -x "$toolchain/bin/clang$exe_suffix" && -f "$ndk_dir/source.properties" ]] ||
    fail "The NDK toolchain for $host_tag was not found in $ndk_dir."
export PATH="$toolchain/bin:$ndk_dir/prebuilt/$host_tag/bin:$PATH"
command -v "$host_cc" >/dev/null || fail "Host compiler not found on PATH: $host_cc"
command -v make >/dev/null || fail 'GNU make was not found on PATH.'

mkdir -p -- "$2"
install_dir=$(cd -- "$2" && pwd -P)
configure_prefix=$install_dir
if [[ "$host_tag" == windows-x86_64 ]]; then
    configure_prefix=$(cygpath -am "$install_dir")
fi

configure_args=(
    "--prefix=$configure_prefix"
    --target-os=android --arch=aarch64 --cpu=armv8-a --enable-cross-compile
    '--cc=clang --target=aarch64-linux-android28'
    '--cxx=clang++ --target=aarch64-linux-android28'
    '--as=clang --target=aarch64-linux-android28'
    '--ld=clang --target=aarch64-linux-android28'
    "--host-cc=$host_cc" --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm --strip=llvm-strip
    --enable-static --disable-shared --enable-pic --optflags=-O2
    --disable-autodetect --disable-programs --disable-doc --disable-network --disable-everything
    --disable-avformat --disable-avdevice --disable-avfilter --disable-swresample --disable-postproc
    --disable-x86asm --enable-decoder=mpeg2video --enable-parser=mpegvideo
)

cd -- "$source_dir"
./configure "${configure_args[@]}"
make -j"$jobs"
make install

for archive in avcodec avutil swscale; do
    [[ -f "$install_dir/lib/lib$archive.a" ]] || fail "Missing installed archive: lib$archive.a"
done
cp -- COPYING.LGPLv2.1 "$install_dir/"
{
    printf 'FFmpeg %s for Android arm64-v8a, API 28\n' "$version"
    printf 'Release source: %s\nRelease tarball SHA-256: %s\n' "$source_url" "$source_sha256"
    sed -n 's/^Pkg\.Revision[[:space:]]*=[[:space:]]*/Android NDK revision: /p' "$ndk_dir/source.properties"
    printf 'Configure: ./configure'
    printf ' %q' "${configure_args[@]}"
    printf '\nBuild: make -j%s && make install\n' "$jobs"
    printf 'License: LGPL version 2.1 or later. See COPYING.LGPLv2.1.\n'
} > "$install_dir/BUILD-FFMPEG.txt"
