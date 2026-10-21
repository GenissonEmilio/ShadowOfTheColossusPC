#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
docker build --cpuset-cpus=0,1 -t sotc-linux-builder -f tools/linux/Dockerfile .
docker run --rm --init --cpus=2 -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -e CELOSIA_CMAKE_ARGS='-DPS2X_ENABLE_DEBUG_UI=OFF -DPS2X_ENABLE_IOP_RPC_TRACE=OFF -DCMAKE_CXX_FLAGS=-msse4.1' \
  -v "$root:/src:z" sotc-linux-builder bash -c \
  './celosia configure && nice -n 10 cmake --build build/cmake --target ps2_runtime ps2_recomp -j2 && nice -n 10 python3 tools/linux/prepare_compiler.py'
# The GTK packager is intentionally separate from the C++ compilation kit.
docker build --cpuset-cpus=0,1 -t sotc-desktop-builder -f tools/desktop/release/Dockerfile .
docker run --rm --init --cpus=2 -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$root:/src:z" \
  sotc-desktop-builder python3 -m PyInstaller --noconfirm --clean \
  --distpath artifacts/frozen --workpath artifacts/pyinstaller tools/desktop/release/launcher.spec
mkdir -p artifacts/compiler-check
ln -sfn ../compiler artifacts/compiler-check/compiler
docker run --rm --init --cpus=2 --network=none -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$root:/src:z" \
  sotc-linux-builder python3 tools/linux/test_compiler.py --program /src/artifacts/compiler-check
python3 tools/desktop/release/package_linux.py
