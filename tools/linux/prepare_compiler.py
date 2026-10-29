#!/usr/bin/env python3
"""Stage a ROM-free, relocatable GCC + PS2Recomp compilation kit."""
from pathlib import Path
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from compiler_key import compiler_key

ROOT=Path(__file__).resolve().parents[2]
BUILD=ROOT / 'build/cmake'
OUT=ROOT / 'artifacts/compiler'
OUT.mkdir(parents=True,exist_ok=True)
for name in ('libstdc++.so.6','libgcc_s.so.1'):
    (OUT / 'lib' / name).unlink(missing_ok=True)

def copy(src,dst):
    src,dst=Path(src),Path(dst)
    dst.parent.mkdir(parents=True,exist_ok=True)
    if src.is_dir(): shutil.copytree(src,dst,dirs_exist_ok=True,ignore=shutil.ignore_patterns('__pycache__','.git','out'))
    else: shutil.copy2(src,dst,follow_symlinks=True)

def deps(binary,directory,skip_system=True):
    result=subprocess.run(['ldd',str(binary)],capture_output=True,text=True).stdout
    for line in result.splitlines():
        match=re.search(r'=> (/\S+)',line)
        if not match: continue
        src=Path(match[1])
        if src.name in ('libc.so.6','libm.so.6','libpthread.so.0','libdl.so.2','librt.so.1','libresolv.so.2') or (skip_system and src.name in ('libstdc++.so.6','libgcc_s.so.1')):
            continue
        copy(src,directory / src.name)

for name in ('PS2Recomp','game','tools','tests'):
    copy(ROOT / name,OUT / 'source' / name)
for name in ('raylib.h','raymath.h','rlgl.h'):
    copy(BUILD / '_deps/raylib-src/src' / name,OUT / 'source/raylib' / name)
for name in ('LICENSE','README.md','celosia'):
    copy(ROOT / name,OUT / 'source' / name)
copy(ROOT / 'docs/SHADERS.md',OUT / 'source/docs/SHADERS.md')
for report in (ROOT / 'docs').glob('FIDELITY-*.md'):
    copy(report, OUT / 'source/docs' / report.name)
copy(ROOT / 'PS2Recomp/ps2xRuntime/third_party/gamecontrollerdb/LICENSE',
     OUT / 'licenses/SDL_GameControllerDB.LICENSE')
copy(BUILD / '_deps/raylib-src/LICENSE',OUT / 'licenses/raylib.LICENSE')
copy(BUILD / 'ps2xRecomp/ps2_recomp',OUT / 'bin/ps2_recomp')
deps(OUT / 'bin/ps2_recomp',OUT / 'lib')
# The same presentation code validates user shaders on the local GPU, without
# loading a ROM or recompiling the game.
subprocess.run(['g++','-std=c++20','-O2','-static-libstdc++','-static-libgcc',
    '-I'+str(ROOT / 'PS2Recomp/ps2xRuntime/include'),'-I'+str(BUILD / '_deps/raylib-src/src'),
    str(ROOT / 'tools/linux/video_tool.cpp'), str(ROOT / 'PS2Recomp/ps2xRuntime/src/lib/desktop_video.cpp'),
    str(next(BUILD.rglob('libraylib.a'))),'-lGL','-lX11','-ldl','-pthread','-lm',
    '-o',str(OUT / 'bin/sotc-video-tool')],check=True)
subprocess.run(['g++', '-std=c++20', '-O2', '-static-libstdc++', '-static-libgcc',
    '-I'+str(BUILD / '_deps/raylib-src/src'),
    '-I'+str(BUILD / '_deps/raylib-src/src/external/glfw/include'),
    str(ROOT / 'tools/linux/input_tool.cpp'), str(next(BUILD.rglob('libraylib.a'))),
    '-lGL', '-lX11', '-ldl', '-pthread', '-lm',
    '-o', str(OUT / 'bin/sotc-input-tool')], check=True)

includes=ROOT / 'PS2Recomp/ps2xRuntime/include'
base_dir=ROOT / 'artifacts/base'
base_dir.mkdir(parents=True,exist_ok=True)
sources=[ROOT / 'PS2Recomp/ps2xRuntime/src/main.cpp',*sorted((ROOT / 'game/overrides').glob('*.cpp'))]
flags=['g++','-std=c++20','-O2','-DNDEBUG','-msse4.1','-fno-strict-aliasing','-DPS2_RUNTIME_LOGS=1',
       '-DPS2X_ENABLE_IOP_RPC_TRACE=0','-DPS2X_VU1_TRUST_FPU=1','-I'+str(includes),'-I'+str(ROOT / 'PS2Recomp/ps2xIOP/include'),
       '-I'+str(BUILD / '_deps/raylib-src/src')]
def compile_source(item):
    i,src=item; obj=base_dir / f'base-{i}.o'
    subprocess.run([*flags,'-c',str(src),'-o',str(obj)],check=True)
    return str(obj)
base_hash=hashlib.sha256(repr(flags).encode()+subprocess.check_output(['g++','--version']))
for folder in (ROOT / 'game/overrides',includes,ROOT / 'PS2Recomp/ps2xIOP/include'):
    for src in sorted(folder.rglob('*')):
        if src.is_file():base_hash.update(src.read_bytes())
base_hash.update(sources[0].read_bytes())
base_key=base_dir / 'key.txt'
objects=[str(base_dir / f'base-{i}.o') for i in range(len(sources))]
if not (base_key.is_file() and base_key.read_text()==base_hash.hexdigest() and all(Path(p).is_file() for p in objects)):
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        objects=list(pool.map(compile_source,enumerate(sources)))
    base_key.write_text(base_hash.hexdigest())
base=OUT / 'lib/libbase.a';base.parent.mkdir(exist_ok=True)
base.unlink(missing_ok=True)
subprocess.run(['ar','rcs',str(base),*objects],check=True)
link_inputs=['lib/libbase.a']
for pattern in ('libps2_runtime.a','libps2_iop.a','libraylib.a'):
    found=list(BUILD.rglob(pattern))
    if len(found)!=1: raise ValueError((pattern,found))
    copy(found[0],OUT / 'lib' / pattern)
    link_inputs.append('lib/'+pattern)
for name in ('libavcodec.so.60','libavformat.so.60','libavutil.so.58','libswresample.so.4','libswscale.so.7','libX11.so.6'):
    source=Path('/usr/lib/x86_64-linux-gnu') / name
    copy(source,OUT / 'lib' / name);deps(source,OUT / 'lib')
    link_inputs.append('lib/'+name)
# GL is resolved against the user's GPU driver when the game runs. This copy
# is only a linker input; it is outside the runtime's library search path.
copy('/usr/lib/x86_64-linux-gnu/libGL.so.1',OUT / 'toolchain/sysroot/usr/lib/x86_64-linux-gnu/libGL.so')
deps('/usr/lib/x86_64-linux-gnu/libGL.so.1',OUT / 'toolchain/sysroot/usr/lib/x86_64-linux-gnu')
link_inputs.append('toolchain/sysroot/usr/lib/x86_64-linux-gnu/libGL.so')

tc=OUT / 'toolchain'
# Link-time inputs for FFmpeg's C++ dependencies; never in the game's runtime path.
for name in ('libstdc++.so.6','libgcc_s.so.1'):
    copy('/usr/lib/x86_64-linux-gnu/'+name,tc / 'lib' / name)
for original,name in [('/usr/bin/x86_64-linux-gnu-g++-13','g++'),('/usr/bin/x86_64-linux-gnu-as','as'),('/usr/bin/x86_64-linux-gnu-ld.bfd','ld')]:
    copy(original,tc / 'bin' / name);deps(tc / 'bin' / name,tc / 'lib',skip_system=False)
copy('/usr/lib/gcc/x86_64-linux-gnu/13',tc / 'lib/gcc/x86_64-linux-gnu/13')
for name in ('cc1plus','collect2'):
    copy('/usr/libexec/gcc/x86_64-linux-gnu/13/'+name,tc / 'lib/gcc/x86_64-linux-gnu/13' / name)
    deps(tc / 'lib/gcc/x86_64-linux-gnu/13' / name,tc / 'lib',skip_system=False)
copy('/usr/include/c++/13',tc / 'include/c++/13')
copy('/usr/include/x86_64-linux-gnu/c++/13',tc / 'include/x86_64-linux-gnu/c++/13')
copy('/usr/include',tc / 'sysroot/usr/include')
for pattern in ('crt*.o','Scrt*.o','Mcrt*.o','libc.so','libm.so','libmvec.so','libc_nonshared.a','libpthread.a','libdl.a','librt.a','libutil.a'):
    for src in Path('/usr/lib/x86_64-linux-gnu').glob(pattern):copy(src,tc / 'sysroot/usr/lib/x86_64-linux-gnu' / src.name)
for name in ('libc.so.6','libm.so.6','libmvec.so.1','ld-linux-x86-64.so.2'):
    copy('/usr/lib/x86_64-linux-gnu/'+name,tc / 'sysroot/lib/x86_64-linux-gnu' / name)
copy('/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2',tc / 'sysroot/lib64/ld-linux-x86-64.so.2')
# The complete corresponding port source and third-party license texts travel
# with this kit. Package names/versions identify the distro source packages.
for doc in Path('/usr/share/doc').iterdir():
    if (doc / 'copyright').is_file():copy(doc / 'copyright',OUT / 'licenses' / (doc.name+'.copyright'))
(OUT / 'packages.txt').write_text(subprocess.check_output(['dpkg-query','-W'],text=True))
(OUT / 'manifest.json').write_text(json.dumps({'build_id':compiler_key(OUT), 'link_inputs':link_inputs},indent=2)+'\n')
print('Compiler staged:',OUT,flush=True)
