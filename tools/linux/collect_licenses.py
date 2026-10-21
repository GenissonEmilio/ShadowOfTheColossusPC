#!/usr/bin/env python3
"""Retain distribution notices and corresponding upstream source snapshots."""
import argparse
import importlib.metadata
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--kind',choices=['native','launcher'],required=True);a=p.parse_args()
out=ROOT/'artifacts/distribution-licenses'/a.kind
out.mkdir(parents=True,exist_ok=True)
for doc in Path('/usr/share/doc').iterdir():
    if (doc/'copyright').is_file():
        target=out/'copyrights'/(doc.name+'.txt');target.parent.mkdir(exist_ok=True);shutil.copy2(doc/'copyright',target)
packages=subprocess.check_output(['dpkg-query','-W','-f','${binary:Package}\t${source:Package}\t${source:Version}\n'],text=True)
(out/'source-packages.tsv').write_text(packages)
if a.kind=='launcher':
    shutil.copy2('/opt/libarchive/COPYING',out/'libarchive-3.8.10.COPYING')
    dist=importlib.metadata.distribution('pyinstaller')
    for file in dist.files or []:
        if file.name in ('COPYING.txt','LICENSE','LICENSE.txt'):
            source=dist.locate_file(file)
            if source.is_file():shutil.copy2(source,out/('PyInstaller-'+file.name))
else:
    deps=ROOT/'build/cmake/_deps'
    names=['elfio','toml11','fmt','libdwarf','rabbitizer','raylib']
    manifest=[]
    sources=ROOT/'artifacts/third-party-sources.tar.gz'
    with tarfile.open(sources,'w:gz',compresslevel=6) as archive:
        for name in names:
            source=deps/(name+'-src')
            revision=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
            remote=subprocess.check_output(['git','-C',str(source),'remote','get-url','origin'],text=True).strip()
            manifest.append({'name':name,'repository':remote,'commit':revision})
            for file in source.iterdir():
                if file.is_file() and any(token in file.name.upper() for token in ('LICENSE','LICENCE','COPYING','COPYRIGHT')):
                    shutil.copy2(file,out/(name+'-'+file.name))
            def selected(info):
                parts=Path(info.name).parts
                if '.git' in parts or '__pycache__' in parts:return None
                if name=='raylib' and len(parts)>1 and parts[1] in ('examples','projects','bindings'):return None
                # toml11's optional test submodules are not library inputs.
                if name=='toml11' and 'extlib' in parts:return None
                return info
            archive.add(source,arcname=name,filter=selected)
    (out/'upstream-sources.json').write_text(json.dumps(manifest,indent=2)+'\n')
(out/'SOURCES.txt').write_text('''Distribution source packages are identified by exact source name/version in source-packages.tsv.
Ubuntu 24.04: https://packages.ubuntu.com/source/noble/
Ubuntu 22.04: https://packages.ubuntu.com/source/jammy/
Ubuntu source pool, including versioned .dsc and source archives: https://archive.ubuntu.com/ubuntu/pool/
GCC source: https://gcc.gnu.org/gcc-13/
PyInstaller source/license: https://github.com/pyinstaller/pyinstaller/tree/v6.22.0
libarchive source/license: https://github.com/libarchive/libarchive/tree/v3.8.10
The statically linked recompiler dependencies are supplied in third-party-sources.tar.gz,
with exact repositories/revisions in native/upstream-sources.json.
The port source and its dependency build recipes are in compiler/source.
Shared libraries can be replaced in the package's library directories.
''')
print(out)
