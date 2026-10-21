#!/usr/bin/env python3
"""Check that a completed native preparation is reused, including after relocation."""
import argparse
import fcntl
import json
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1] / 'desktop'))
from model import Library
from pipeline import Pipeline
p=argparse.ArgumentParser();p.add_argument('--program',type=Path,required=True);p.add_argument('--data',type=Path,required=True);p.add_argument('--disc',type=Path,required=True)
a=p.parse_args();library=Library(a.data,a.program);events=[]
pipeline=Pipeline(library.program,library.directory,library.game_environment(),events.append)
# Wait for an already-running first preparation without starting another one.
with (library.directory / '.build.lock').open('a') as lock:
    fcntl.flock(lock,fcntl.LOCK_EX)
    executable=pipeline.work / 'gamecore/game';stamp=pipeline.work / 'complete.json'
    if not executable.is_file() or not stamp.is_file():raise RuntimeError('Initial preparation did not complete')
    paths=[executable,stamp,pipeline.log]
    before=[x.stat().st_mtime_ns for x in paths]
result=pipeline.prepare(a.disc)
after=[x.stat().st_mtime_ns for x in paths]
assert before==after,(before,after)
assert result==executable
assert any('cache' in s.lower() for s in events),events
report={'passed':True,'executable_unchanged':True,'prepare_log_unchanged':True,
        'build_id':pipeline.build_id,'messages':events}
(library.directory/'cache-check.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report),flush=True)
