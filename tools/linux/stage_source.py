"""Stage the compiler kit's corresponding source without retaining removed files."""
from pathlib import Path
import os
import shutil
import tempfile


def stage_source(root, build, output):
    """Replace only output/source after a complete successful copy.

    output/source is dedicated to regenerable corresponding source. The only
    replacement/deletion target is derived here, never supplied by the caller;
    other kit directories and source inputs are untouched. Caller must exclude
    kit readers and other writers during publication: two renames are not an
    atomic exchange.
    """
    root, build, output = Path(root), Path(build), Path(output)
    if output.is_symlink() or not output.is_dir():
        raise ValueError('output must be an existing real directory')
    output = output.resolve()
    destination = output/'source'  # no caller-supplied arbitrary deletion path
    if destination.is_symlink() or (destination.exists() and not destination.is_dir()):
        raise ValueError('source destination must be a real directory or absent')
    pending = Path(tempfile.mkdtemp(prefix='.source-stage-', dir=output))
    previous = None
    committed = False
    try:
        def copy(source, target):
            target.parent.mkdir(parents=True, exist_ok=True)
            if source.is_dir():
                shutil.copytree(source, target,
                                ignore=shutil.ignore_patterns('__pycache__', '.git', 'out'))
            else:
                shutil.copy2(source, target, follow_symlinks=True)

        for name in ('PS2Recomp', 'game', 'tools', 'tests', 'docs',
                     'LICENSE', 'README.md', 'celosia'):
            copy(root/name, pending/name)
        for name in ('raylib.h', 'raymath.h', 'rlgl.h'):
            copy(build/'_deps/raylib-src/src'/name, pending/'raylib'/name)
        # Recheck at publication; do not follow a replaced destination symlink.
        if destination.is_symlink() or (destination.exists() and not destination.is_dir()):
            raise ValueError('source destination changed to a non-directory')
        if destination.exists():
            previous = Path(tempfile.mkdtemp(prefix='.source-previous-', dir=output))
            previous.rmdir()  # our own freshly created empty placeholder only
            os.replace(destination, previous)
        try:
            os.replace(pending, destination)
        except BaseException:
            if previous is not None:
                # If rollback fails, preserve previous for recovery; never delete it.
                os.replace(previous, destination)
            raise
        committed = True
    finally:
        if pending.exists():
            shutil.rmtree(pending)  # only our private unfinished copy
        if committed and previous is not None:
            shutil.rmtree(previous)  # old corresponding source, after successful commit
    return destination
