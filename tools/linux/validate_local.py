#!/usr/bin/env python3
"""Exercise the exact portable compiler against an owned extracted disc."""
import argparse
import json
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1] / 'desktop'))
from model import Library
from pipeline import Pipeline
p=argparse.ArgumentParser();p.add_argument('--disc',type=Path,required=True);p.add_argument('--program',type=Path,required=True);p.add_argument('--data',type=Path,required=True)
a=p.parse_args()
library=Library(a.data,a.program);library.directory.mkdir(parents=True,exist_ok=True)
if not library.valid_extracted(a.disc):raise ValueError('Incompatible disc')
compiler=Pipeline(library.program,library.directory,library.game_environment(),lambda s:print(s,flush=True))
result=compiler.prepare(a.disc)
print(json.dumps({'executable':str(result),'build_id':compiler.build_id}),flush=True)
