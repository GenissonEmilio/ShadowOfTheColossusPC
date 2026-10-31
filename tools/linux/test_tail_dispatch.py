#!/usr/bin/env python3
"""Generate and execute a tiny original ELF; no ROM or game code required."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

p=argparse.ArgumentParser()
p.add_argument('--kit',type=Path,default=Path('artifacts/compiler'))
p.add_argument('--compiler',type=Path)
p.add_argument('--out',type=Path,default=Path('artifacts/tail-dispatch'))
a=p.parse_args();root=Path(__file__).resolve().parents[2]
kit=a.kit.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
compiler=(a.compiler or kit/'bin/ps2_recomp').resolve()
fixture=bytearray(0x380)
ident=b'\x7fELF'+bytes([1,1,1])+bytes(9)
struct.pack_into('<16sHHIIIIIHHHHHH',fixture,0,ident,2,8,1,0x100000,52,0x300,0x20924001,52,32,1,40,3,2)
struct.pack_into('<8I',fixture,52,1,0x100,0x100000,0x100000,0x10c,0x10c,5,16)
# J target; ADDIU sp,sp,-16. Target: ADDIU v0,zero,42; JR ra; NOP.
struct.pack_into('<II',fixture,0x100,0x08040040,0x27bdfff0)
struct.pack_into('<III',fixture,0x200,0x2402002a,0x03e00008,0)
names=b'\0.text\0.shstrtab\0';fixture[0x280:0x280+len(names)]=names
struct.pack_into('<10I',fixture,0x328,1,1,6,0x100000,0x100,0x10c,0,0,16,0)
struct.pack_into('<10I',fixture,0x350,7,3,0,0,0x280,len(names),0,0,1,0)
(out/'fixture.elf').write_bytes(fixture)
(out/'functions.csv').write_text('name,start,end,size\ntail_entry,0x00100000,0x00100008,8\ntail_target,0x00100100,0x0010010C,12\n')
config='[general]\n'+''.join(f'{k} = {json.dumps(str(v))}\n' for k,v in {
    'input':out/'fixture.elf','ghidra_output':out/'functions.csv','output':out/'output'}.items())
config+='single_file_output = false\noutput_worker_threads = 2\npatch_syscalls = false\npatch_cop0 = false\npatch_cache = false\n'
(out/'config.toml').write_text(config)
env=dict(os.environ,LD_LIBRARY_PATH=str(kit/'lib'))
with (out/'generate.log').open('w') as log:
    subprocess.run([str(compiler),str(out/'config.toml')],env=env,cwd=out,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=30)
sources=sorted((out/'output').glob('*.cpp'));assert sources,'compiler emitted no sources'
cmd=['g++','-std=c++20','-O1','-msse4.1','-fno-strict-aliasing','-ffunction-sections','-fdata-sections',
     '-DPS2_RUNTIME_LOGS=1','-DPS2X_ENABLE_IOP_RPC_TRACE=0',
     '-I'+str(out/'output'),'-I'+str(root/'PS2Recomp/ps2xRuntime/include'),
     '-I'+str(root/'PS2Recomp/ps2xIOP/include'),'-I'+str(kit/'source/raylib'),
     str(root/'tests/native/tail_dispatch.cpp'),*map(str,sources),'-Wl,--gc-sections',
     '-Wl,--start-group',str(kit/'lib/libps2_runtime.a'),str(kit/'lib/libps2_iop.a'),str(kit/'lib/libraylib.a'),'-Wl,--end-group',
     '-L'+str(kit/'lib'),'-Wl,-rpath-link,'+str(kit/'lib'),'-lavcodec','-lavformat','-lavutil',
     '-lswresample','-lswscale','-lGL','-lX11','-pthread','-ldl','-lm','-o',str(out/'check')]
with (out/'compile.log').open('w') as log:
    subprocess.run(cmd,cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
r=subprocess.run([str(out/'check')],env=env,text=True,capture_output=True,timeout=15)
(out/'run.log').write_text(r.stdout+r.stderr)
print(r.stdout+r.stderr,end='')
(out/'report.json').write_text(json.dumps({'passed':r.returncode==0,'compiler':str(compiler),
    'exit_code':r.returncode,'scope':'five executable cases from an original synthetic MIPS ELF, no ROM'},indent=2)+'\n')
raise SystemExit(r.returncode)
