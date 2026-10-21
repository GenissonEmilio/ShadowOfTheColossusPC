#!/usr/bin/env python3
"""Compile/link an original synthetic fixture through the shipped toolchain."""
import argparse
from pathlib import Path
import sys
import tempfile
import subprocess
sys.path.insert(0,str(Path(__file__).resolve().parents[1] / 'desktop'))
from model import Library
from pipeline import Pipeline
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
void original_test_function(uint8_t*, R5900Context* ctx, PS2Runtime*) { SET_GPR_U32(ctx, 2, 42); }
''')
    try:
        result=pipeline.compile(stage)
    except Exception:
        print(pipeline.log.read_text(),file=sys.stderr)
        raise
    output=subprocess.check_output([str(result),'--version'],env=library.game_environment(),text=True)
    assert 'ShadowOfTheColossusPC Linux runtime' in output,output
    print('Portable compiler and native runtime:',output.strip())
