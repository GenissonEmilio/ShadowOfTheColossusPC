#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out="${GROUND_SUPPORT_ADAPTER_TEST_OUT:-artifacts/ground-support-adapter-check}"
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O1 -msse4.1 -Wall -Wextra -Werror -fno-strict-aliasing \
  -ffunction-sections -fdata-sections -DPS2_RUNTIME_LOGS=1 -DPS2X_ENABLE_IOP_RPC_TRACE=0 \
  -isystem PS2Recomp/ps2xRuntime/include -isystem PS2Recomp/ps2xIOP/include \
  tests/native/ground_support_adapter.cpp -Wl,--gc-sections \
  -o "$out/check"
timeout --kill-after=3s 15s "$out/check"
