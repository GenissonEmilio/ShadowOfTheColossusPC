#!/usr/bin/env python3
"""Collect a scripted new-game run for visual review; not a campaign-completion test."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
sys.path.insert(0,str(Path(__file__).resolve().parents[1] / 'desktop'))
from model import Library
from pipeline import Pipeline
p=argparse.ArgumentParser();p.add_argument('--program',type=Path,required=True);p.add_argument('--data',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--seconds',type=int,default=160)
a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
library=Library(a.data,a.program);boot=library.verify()
executable=Pipeline(library.program,library.directory,library.game_environment()).prepare(boot.parent)
route='35:START:0.3;45:START:0.3'
for second in range(56,91,2):route+=f';{second}:CROSS:0.3'
route+=';110:START:0.3;125:LS_UP:20;146:TRIANGLE:0.4;149:SQUARE:0.4'
env=library.game_environment()
env.update(PS2X_INPUT_SCRIPT=os.environ.get('PS2X_INPUT_SCRIPT',route),PS2X_STATS='1',
           PS2X_SNAPSHOT_DIR=str(a.out),PS2X_SNAPSHOT_SECONDS='10',
           PS2X_MEMCARD_DIR=str(a.out / 'memcards'),PS2X_SPU2_WAV=str(a.out / 'mix.pcm'),
           SOTC_TRACE_CALLS='0x1c3628',SOTC_TRACE_EVERY='60')
started=time.monotonic()
with (a.out/'game.log').open('w') as stream:
    process=subprocess.Popen([str(executable),str(boot)],cwd=a.out,env=env,stdout=stream,stderr=subprocess.STDOUT)
    try:
        code=process.wait(timeout=a.seconds)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:code=process.wait(timeout=5)
        except subprocess.TimeoutExpired:process.kill();code=process.wait()
(a.out/'capture.json').write_text(json.dumps({'scope':'scripted new game and input; requires visual review',
 'elapsed':round(time.monotonic()-started,1),'exit_code':code,'screenshots':len(list(a.out.glob('frame_*.png')))},indent=2)+'\n')
print(a.out)
