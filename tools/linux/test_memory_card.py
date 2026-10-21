#!/usr/bin/env python3
"""Verify native card-page persistence across two processes, without game data."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1] / 'desktop'))
from model import Library
from pipeline import Pipeline
p=argparse.ArgumentParser();p.add_argument('--program',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
library=Library(a.out,a.program)
class Probe(Pipeline):
    def run(self,args,cwd=None,env=None):
        args=list(args)
        if '-o' in args and '-c' not in args:args.append('-Wl,--wrap=main')
        return super().run(args,cwd,env)
pipeline=Probe(library.program,library.directory,library.game_environment(),print)
stage=pipeline.work/'card-probe';(stage/'output').mkdir(parents=True,exist_ok=True)
(stage/'output/probe.cpp').write_text(r'''
#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"
#include <vector>
#include <cstring>
#include <cstdlib>
#include <iostream>
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x01000000;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0x00400000;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[0x00400000] = {};
namespace ps2_stubs {
void _sceMcCoreInit(uint8_t*, R5900Context*, PS2Runtime*);
void _sceMcCoreEnd(uint8_t*, R5900Context*, PS2Runtime*);
void _sceMcCoreCreateSocket(uint8_t*, R5900Context*, PS2Runtime*);
void _sceMcCoreWritePage(uint8_t*, R5900Context*, PS2Runtime*);
void _sceMcCoreReadPage2(uint8_t*, R5900Context*, PS2Runtime*);
}
extern "C" int __real_main(int,char**);
extern "C" int __wrap_main(int argc,char**argv) {
    std::vector<uint8_t> ram(PS2_RAM_SIZE);
    R5900Context ctx{};
    ps2_stubs::_sceMcCoreInit(ram.data(),&ctx,nullptr);
    uint32_t port=2;std::memcpy(ram.data()+0x2004,&port,4);
    SET_GPR_U32(&ctx,4,0x2000);
    ps2_stubs::_sceMcCoreCreateSocket(ram.data(),&ctx,nullptr);
    uint32_t socket=GPR_U32(&ctx,2);
    if (socket>1) return 10;
    uint8_t pattern[512];for(unsigned i=0;i<512;++i) pattern[i]=(i*17+31)&255;
    SET_GPR_U32(&ctx,4,socket);SET_GPR_U32(&ctx,5,0x1000);
    SET_GPR_U32(&ctx,6,128);SET_GPR_U32(&ctx,7,1);
    const bool write=std::getenv("SOTC_CARD_WRITE")!=nullptr;
    if(write) {
        std::memcpy(ram.data()+0x1000,pattern,512);
        ps2_stubs::_sceMcCoreWritePage(ram.data(),&ctx,nullptr);
    } else {
        ps2_stubs::_sceMcCoreReadPage2(ram.data(),&ctx,nullptr);
    }
    if(GPR_U32(&ctx,2)!=1) return 11;
    if(!write && std::memcmp(pattern,ram.data()+0x1000,512)) return 12;
    ps2_stubs::_sceMcCoreEnd(ram.data(),&ctx,nullptr);
    std::cout << (write ? "native card written\n" : "native card reopened and verified\n");
    return __real_main(argc,argv);
}
''')
exe=pipeline.compile(stage)
env=library.game_environment();env['PS2X_MEMCARD_DIR']=str(a.out/'cards')
write=subprocess.check_output([str(exe),'--version'],env=dict(env,SOTC_CARD_WRITE='1'),text=True)
read=subprocess.check_output([str(exe),'--version'],env=env,text=True)
assert 'reopened and verified' in read
report={'passed':True,'scope':'native PS2 memory-card page I/O, persistence across two processes; not an in-game save-menu test','card_bytes':(a.out/'cards/Mcd001.ps2').stat().st_size,'write':write.strip(),'read':read.strip()}
(a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
