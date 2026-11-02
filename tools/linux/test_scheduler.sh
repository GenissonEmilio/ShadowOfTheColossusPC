#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
kit="${1:-artifacts/compiler}"
mkdir -p artifacts/scheduler
g++ -std=c++20 -O1 -msse4.1 -fno-strict-aliasing \
  -DPS2_RUNTIME_LOGS=1 -DPS2X_ENABLE_IOP_RPC_TRACE=0 \
  -IPS2Recomp/ps2xRuntime/include -IPS2Recomp/ps2xRuntime/src/lib -IPS2Recomp/ps2xRuntime/src/lib/Kernel \
  -IPS2Recomp/ps2xIOP/include -IPS2Recomp/ps2xTest/include -I"$kit/source/raylib" \
  tests/native/scheduler.cpp PS2Recomp/ps2xTest/src/ps2_runtime_kernel_tests.cpp \
  PS2Recomp/ps2xTest/src/ps2_runtime_interrupt_tests.cpp \
  PS2Recomp/ps2xTest/src/ee_save_state_tests.cpp \
  PS2Recomp/ps2xTest/src/test_function_table.cpp \
  -Wl,--start-group "$kit/lib/libps2_runtime.a" "$kit/lib/libps2_iop.a" \
  "$kit/lib/libraylib.a" -Wl,--end-group \
  -L"$kit/lib" -Wl,-rpath-link,"$kit/lib" \
  -lavcodec -lavformat -lavutil -lswresample -lswscale -lGL -lX11 -pthread -ldl -lm \
  -o artifacts/scheduler/check
timeout --kill-after=3s 60s artifacts/scheduler/check
