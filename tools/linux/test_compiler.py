#!/usr/bin/env python3
"""Compile/link an original synthetic fixture through the shipped toolchain."""
import argparse
import os
from pathlib import Path
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
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x01000000;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0x00400000;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[0x00400000] = {};
void original_test_function(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
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

    # Exercise the same subprocess contract used by the installer, without a
    # display, audio device or game files. The registered EE function stamps RAM
    # before taking the unresolved-module branch that ends a real capture.
    disc = Path(t) / 'fixture-disc'
    disc.mkdir()
    write_elf(disc / BOOT, 0x100000,
              [('.text', 0x100000, 4, struct.pack('<I', 0), 6)])
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
    assert '[capture] core ready without host video or audio' in pipeline.log.read_text(encoding='utf-8')
    print('Headless module capture passed with the portable runtime')
