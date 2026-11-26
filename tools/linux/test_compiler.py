#!/usr/bin/env python3
"""Compile/link an original synthetic fixture through the shipped toolchain."""
import argparse
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import subprocess
sys.path.insert(0,str(Path(__file__).resolve().parents[1] / 'desktop'))
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from build_elf import write_elf
from model import Library
from pipeline import BOOT, Pipeline
p=argparse.ArgumentParser();p.add_argument('--program',type=Path,required=True);a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='sotc-compiler-check-') as t:
    library=Library(t,a.program)
    pipeline=Pipeline(library.program,library.directory,library.game_environment(),print)
    stage=pipeline.work / 'synthetic';(stage/'output').mkdir(parents=True)
    (stage/'output/fixture.cpp').write_text('''#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "Stubs/LibC.h"
#include "runtime/host_state.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <thread>

static LONG WINAPI continue_fixture_access_violation(EXCEPTION_POINTERS *exception) {
    const auto &record = *exception->ExceptionRecord;
    return record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters == 2 &&
           record.ExceptionInformation[1] == 0x4321
        ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

static void check_windows_libc_random() {
    R5900Context context{};
    const auto seed = [&](uint32_t value) {
        context.r[4] = _mm_set_epi64x(0, value);
        ps2_stubs::srand(nullptr, &context, nullptr);
    };
    const auto draw = [&]() {
        ps2_stubs::rand(nullptr, &context, nullptr);
        return static_cast<uint32_t>(_mm_extract_epi32(context.r[2], 0));
    };
    const auto require = [](bool ok, const char *message) {
        if (ok) return;
        std::fprintf(stderr, "Windows libc RNG: %s\\n", message);
        std::abort();
    };
    const auto compare = [&](unsigned count) {
        for (unsigned i = 0; i < count; ++i)
            require(draw() == static_cast<uint32_t>(std::rand()), "sequence differs from the bundled CRT");
    };
    std::vector<uint8_t> saved, restored;
    std::string error;
    const auto capture = [&] {
        const bool ok = ps2_stubs::captureLibcState(saved, error);
        require(ok, error.c_str());
    };

    const auto first = draw();
    capture();
    std::srand(1);
    require(first == static_cast<uint32_t>(std::rand()), "default seed differs from the bundled CRT");
    compare(4095);
    for (uint32_t value : {0u, 1u, 2u, 0x7fffffffu, 0x80000000u, 0x80000001u, 0xfffffffeu, 0xffffffffu}) {
        seed(value);
        std::srand(value);
        compare(4096);
    }
    for (uint32_t i = 1; i <= 32; ++i) {
        const uint32_t value = i * 0x9e3779b9u;
        seed(value);
        std::srand(value);
        compare(4096);
    }

    capture();
    require(saved.size() == 12 && std::memcmp(saved.data(), "LBC3", 4) == 0 &&
            ps2_stubs::validateLibcState(saved, error), "Windows state should store one uint32 of RNG state in LBC3");
    std::array<uint32_t, 1024> continuation{};
    for (auto &value : continuation) value = static_cast<uint32_t>(std::rand());
    for (const auto value : continuation)
        require(draw() == value, "capture advanced guest RNG");

    seed(0);
    require(ps2_stubs::restoreLibcState(saved, error), "could not restore LBC3 state");
    auto truncated = saved;
    truncated.pop_back();
    auto trailing = saved;
    trailing.push_back(0);
    auto invalidHandle = saved;
    std::fill_n(invalidHandle.begin() + 4, 4, 0);
    auto invalidMagic = saved;
    invalidMagic[3] = '2';
    for (const auto *bad : {&truncated, &trailing, &invalidHandle, &invalidMagic}) {
        require(!ps2_stubs::validateLibcState(*bad, error) && !ps2_stubs::restoreLibcState(*bad, error),
                "invalid LBC3 state accepted");
        require(ps2_stubs::captureLibcState(restored, error) && restored == saved,
                "rejected state changed live libc state");
    }
    for (const auto value : continuation)
        require(draw() == value, "restored RNG did not resume its saved sequence");

    const std::vector<uint8_t> legacy{'L', 'B', 'C', '1', 7, 0, 0, 0};
    require(ps2_stubs::validateLibcState(legacy, error) && ps2_stubs::restoreLibcState(legacy, error),
            "legacy LBC1 state rejected");
    require(ps2_stubs::captureLibcState(restored, error) && restored.size() == 12 && restored[4] == 7,
            "legacy LBC1 file handle was not restored");
    std::srand(1);
    compare(4096);
}

#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__)
struct FixtureDispatcherTransfer {};

__attribute__((noipa)) static void throw_fixture_transfer() {
    throw FixtureDispatcherTransfer{};
}

__attribute__((noipa)) static void check_nested_win64_frame(PS2Runtime &runtime) {
    // Force a large Win64 frame with a nonvolatile XMM spill, like the
    // frame the startup dump failed to unwind.
    volatile char local[2048];
    local[0] = 0;
    asm volatile("" : : "r"(local) : "xmm7", "memory");
    const auto originalFloor = runtime.m_hostStackFloor;
    runtime.m_hostStackFloor = reinterpret_cast<uintptr_t>(&local[sizeof(local) - 1]);
    const bool atLimit = runtime.hostStackDeep();
    runtime.m_hostStackFloor = originalFloor;
    if (!atLimit)
        std::abort();
    throw_fixture_transfer();
    if (local[0] != 0)
        std::abort();
}
#endif
#endif
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x01000000;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0x00400000;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[0x00400000] = {};
void original_test_function(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
#if defined(_WIN32)
    if (std::getenv("SOTC_TEST_EXCEPTION_LOG")) {
        SetErrorMode(SEM_NOGPFAULTERRORBOX);
        const auto handler = AddVectoredExceptionHandler(0, continue_fixture_access_violation);
        std::thread([] {
            const ULONG_PTR handled[] = {0, 0x4321};
            for (int repeat = 0; repeat < 12; ++repeat)
                RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 2, handled);
        }).join();
        RemoveVectoredExceptionHandler(handler);
        const ULONG_PTR information[] = {0, 0x1234};
        RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 2, information);
    }
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__)
    try {
        check_nested_win64_frame(*runtime);
        std::abort();
    } catch (const FixtureDispatcherTransfer &) {
        ram[0x1001] = 43;
    }
#endif
    check_windows_libc_random();
    ram[0x1002] = 44;
#endif
    ram[0x1000] = 42;
    runtime->dispatchGuestBranch(ram, ctx, 0x100100, 0x100000, 0x100008,
                                 PS2Runtime::GuestBranchKind::IndirectCall, "fixture");
}
struct InstallFixture {
    InstallFixture() { g_ps2RecompiledFunctionTable[0x100000 / 4] = original_test_function; }
} fixture;
''')
    try:
        result=pipeline.compile(stage)
    except Exception:
        print(pipeline.log.read_text(),file=sys.stderr)
        raise
    output=subprocess.check_output([str(result),'--version'],env=library.game_environment(),text=True)
    platform = "Windows" if os.name == "nt" else "Linux"
    assert f"ShadowOfTheColossusPC {platform} runtime" in output, output
    print('Portable compiler and native runtime:',output.strip())
    if os.name == "nt":
        import ctypes

        # Loading an EXE for inspection maps the image without running its entry point.
        image = ctypes.WinDLL(str(result))
        try:
            for symbol in ("NvOptimusEnablement", "AmdPowerXpressRequestHighPerformance"):
                assert ctypes.c_uint32.in_dll(image, symbol).value == 1, symbol
        finally:
            ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(image._handle))
        print("Windows high-performance GPU exports passed")

    # Exercise the same subprocess contract used by the installer, without a
    # display, audio device or game files. The registered EE function stamps RAM
    # before taking the unresolved-module branch that ends a real capture.
    disc = Path(t) / 'fixture-disc'
    disc.mkdir()
    write_elf(disc / BOOT, 0x100000,
              [('.text', 0x100000, 4, struct.pack('<I', 0), 6)])
    if os.name == "nt":
        import winreg
        from gpu import prefer_windows_gpu

        crash = subprocess.run([str(result), str(disc / BOOT)], cwd=stage,
                               env=dict(pipeline.env, PS2X_MODULE_CAPTURE="1", SOTC_TEST_EXCEPTION_LOG="1"),
                               capture_output=True, text=True, errors="replace", timeout=15)
        assert crash.returncode & 0xffffffff == 0xc0000005, crash
        crash_lines = [line for line in crash.stderr.splitlines() if line.startswith("[crash] 0xc0000005 ")]
        assert len(crash_lines) == 3, crash.stderr
        assert "phase=first-chance " in crash_lines[0], crash.stderr
        assert crash_lines[0].endswith("access=0x0 target=0x4321"), crash.stderr
        assert "phase=first-chance " in crash_lines[1], crash.stderr
        assert crash_lines[1].endswith("access=0x0 target=0x1234"), crash.stderr
        assert crash_lines[1].replace("phase=first-chance ", "phase=unhandled ") == crash_lines[2], crash.stderr
        version = ctypes.WinDLL("version.dll")
        ntdll = str(Path(os.environ["SystemRoot"]) / "System32" / "ntdll.dll")
        version_size = version.GetFileVersionInfoSizeW(ntdll, None)
        assert version_size > 0
        version_buffer = ctypes.create_string_buffer(version_size)
        assert version.GetFileVersionInfoW(ntdll, 0, version_size, version_buffer)
        fixed_info = ctypes.c_void_p()
        fixed_size = ctypes.c_uint()
        assert version.VerQueryValueW(version_buffer, "\\", ctypes.byref(fixed_info), ctypes.byref(fixed_size))
        assert fixed_size.value >= 16
        fixed_words = ctypes.cast(fixed_info, ctypes.POINTER(ctypes.c_uint32))
        version_ms, version_ls = fixed_words[2], fixed_words[3]
        ntdll_version = f"{version_ms >> 16}.{version_ms & 0xffff}.{version_ls >> 16}.{version_ls & 0xffff}"
        stacks = []
        for line in crash_lines:
            diagnostic = re.search(
                r" rip=0x([0-9a-f]+) stack=0x([0-9a-f]+) stack_low=0x([0-9a-f]+) stack_high=0x([0-9a-f]+)"
                r" module=[^ ]+\.dll rva=0x[0-9a-f]+ ntdll_version=([0-9.]+) ", line, re.I)
            assert diagnostic, line
            rip, stack, low, high = (int(value, 16) for value in diagnostic.groups()[:4])
            assert rip != 0 and low <= stack < high, line
            assert diagnostic[5] == ntdll_version, line
            stacks.append((low, high))
        assert stacks[0][1] <= stacks[1][0] or stacks[1][1] <= stacks[0][0], crash.stderr
        # Scope to the disposable fixture's absolute path in the runner's hive.
        registry_path = r"Software\Microsoft\DirectX\UserGpuPreferences"
        filename = str(result.resolve())
        assert "requested high performance" in prefer_windows_gpu(result)
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, registry_path, 0,
                            winreg.KEY_QUERY_VALUE | winreg.KEY_SET_VALUE) as key:
            try:
                assert winreg.QueryValueEx(key, filename) == ("GpuPreference=2;", winreg.REG_SZ)
                winreg.SetValueEx(key, filename, 0, winreg.REG_SZ, "GpuPreference=1;")
                assert "kept existing" in prefer_windows_gpu(result)
                assert winreg.QueryValueEx(key, filename)[0] == "GpuPreference=1;"
            finally:
                winreg.DeleteValue(key, filename)
        print("Windows exception logging and per-executable GPU preference passed")
    pipeline.env.pop('DISPLAY', None)
    pipeline.env.pop('WAYLAND_DISPLAY', None)
    try:
        capture = pipeline.capture(result, disc, 'FIXTURE')
    except Exception:
        print(pipeline.log.read_text(errors='replace'), file=sys.stderr)
        raise
    with capture.open('rb') as stream:
        stream.seek(0x1000)
        assert stream.read(1) == b'\x2a', 'Scheduler did not run the fixture before the RAM dump'
        if os.name == 'nt':
            assert stream.read(1) == b'\x2b', 'Win64 stack bound and unwind fixture did not complete'
            assert stream.read(1) == b'\x2c', 'Windows CRT-compatible random state fixture did not complete'
    assert '[capture] core ready without host video or audio' in pipeline.log.read_text(encoding='utf-8')
    print('Headless module capture passed with the portable runtime')
