#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out="${GROUND_SUPPORT_TEST_OUT:-artifacts/ground-support-check}"
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O1 -DSOTC_GROUND_SUPPORT_GEOMETRY_ONLY -Wall -Wextra -Werror -fno-strict-aliasing \
  tests/native/ground_support.cpp game/overrides/decomp_ground_support.cpp \
  -o "$out/check"
timeout --kill-after=3s 15s "$out/check"
