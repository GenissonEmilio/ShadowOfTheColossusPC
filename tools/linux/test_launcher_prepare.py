#!/usr/bin/env python3
"""ROM-free smoke test of the launcher's compile and play actions."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


PROBE = r'''
import argparse
from pathlib import Path
import subprocess
import sys
import threading
import time
import traceback

from main import Gtk, GLib, Launcher
from pipeline import PreparationProgress
import model


def settle(predicate, description):
    deadline = time.monotonic() + 5
    context = GLib.MainContext.default()
    while time.monotonic() < deadline:
        while context.pending():
            context.iteration(False)
        if predicate():
            return
        time.sleep(.01)
    raise AssertionError(description)


def probe(out):
    data = out / 'profile'
    args = argparse.Namespace(data_dir=data, page='home', rom=None, test_game=False,
                              screenshot=None, no_update_check=True)
    model.atomic_json(data / 'launcher-preferences.json', {'language': 'en', 'ui_sounds': False})
    app = Launcher(args)
    GLib.source_remove(app.gamepad_timer)
    try:
        assert not app.compile_button.is_sensitive(), 'Compile enabled without a disc'
    finally:
        app.window.destroy()

    boot = data / 'discs' / 'fixture' / 'SCUS_974.72'
    boot.parent.mkdir(parents=True)
    boot.write_bytes(b'synthetic boot file')
    (boot.parent / 'SYSTEM.CNF').write_bytes(b'BOOT2 = cdrom0:\\SCUS_974.72;1\n')
    model.atomic_json(data / 'launcher.json', {'disc': str(boot.relative_to(data)), 'name': 'fixture.iso'})
    executable = data / 'fixture-game'
    executable.touch()

    class Attempt:
        def __init__(self, error=None, ready=False):
            self.started = threading.Event()
            self.release = threading.Event()
            if ready:
                self.release.set()
            self.error = error

        def prepare(self):
            self.started.set()
            if not self.release.wait(10):
                raise RuntimeError('Test did not release the compiler')
            if self.error:
                raise self.error
            return executable

    success = Attempt()
    failure = Attempt(subprocess.CalledProcessError(1, ['fixture-compiler']), ready=True)
    retry = Attempt(ready=True)
    play = Attempt(ready=True)
    close = Attempt()
    attempts = iter((success, failure, retry, play, close))
    build_calls = []
    launches = []
    errors = []
    focus_requests = []
    quit_calls = []
    main_thread = threading.get_ident()

    class Compiler:
        def __init__(self, program, directory, environment, progress, *, boot_name):
            assert boot_name == boot.name
            self.progress = progress

        def prepare(self, disc):
            build_calls.append((disc, threading.get_ident()))
            self.progress(PreparationProgress('Step 3/4 · MANAGER',
                                              '45/80 batches\nEstimated time for this step: 2 min', .62))
            return next(attempts).prepare()

    class Game:
        returncode = None

        def poll(self):
            return self.returncode

    game = Game()

    def popen(command, **kwargs):
        launches.append(command)
        return game

    app = Launcher(args)
    GLib.source_remove(app.gamepad_timer)
    from import_disc import identify_disc
    app.library.validated_disc = lambda path: identify_disc(path) if path == boot.parent else None
    app.error = lambda *error: errors.append(error)
    app.window.present = lambda: focus_requests.append(True)
    original_pipeline, model.Pipeline = model.Pipeline, Compiler
    original_popen, model.subprocess.Popen = model.subprocess.Popen, popen
    original_quit, Gtk.main_quit = Gtk.main_quit, lambda: quit_calls.append(True)
    try:
        assert app.compile_button.get_visible() and app.compile_button.is_sensitive()
        app.compile_button.clicked()
        settle(lambda: success.started.is_set() and app.status.get_text() == 'Step 3/4 · MANAGER',
               'Compile action did not start the worker or show its progress')
        assert app.busy and app.progress.get_visible() and app.prepare_detail.get_visible()
        assert app.progress.get_fraction() == .62 and not app.pulse()
        assert 'Estimated time for this step: 2 min' in app.prepare_detail.get_text()
        assert all(not button.is_sensitive() for button in (app.compile_button, app.play_button, app.choose_button))
        app.compile_button.clicked()
        app.play_button.clicked()
        app.presentation_button.grab_focus()
        assert app.window.get_focus() == app.presentation_button
        success.release.set()
        settle(lambda: not app.busy, 'Compile action did not complete')
        assert app.status.get_style_context().has_class('ready')
        assert not app.progress.get_visible() and app.window.get_visible()
        assert app.window.get_focus() == app.presentation_button
        assert not focus_requests, 'Compile requested window focus'
        assert len(build_calls) == 1 and not launches and app.process is None

        app.compile_button.clicked()
        settle(lambda: failure.started.is_set() and not app.busy, 'Compile failure was not handled')
        assert app.status.get_style_context().has_class('error')
        assert app.compile_button.is_sensitive() and app.play_button.is_sensitive()
        assert not errors, 'Compile opened a modal error dialog'
        assert not launches and app.process is None
        app.compile_button.clicked()
        settle(lambda: retry.started.is_set() and not app.busy, 'Compile retry did not complete')
        assert app.status.get_style_context().has_class('ready')
        assert app.compile_button.is_sensitive() and app.play_button.is_sensitive()
        assert not launches and app.process is None

        app.play_button.clicked()
        settle(lambda: app.process is game, 'Play did not launch the fixture explicitly')
        assert launches == [[str(executable), str(boot)]]
        assert not app.compile_button.is_sensitive()
        game.returncode = 0
        settle(lambda: app.process is None, 'Launcher did not recover after the game exited')
        assert app.compile_button.is_sensitive()

        app.compile_button.clicked()
        settle(close.started.is_set, 'Compile did not start before closing')
        app.window.close()
        settle(lambda: app.closing, 'Window close did not wait for compilation')
        assert not app.window.get_visible() and not quit_calls
        close.release.set()
        settle(lambda: quit_calls, 'Launcher did not finish closing after compilation')
        assert len(launches) == 1 and app.process is None
        assert len(build_calls) == 5 and all(disc == boot.parent and tid != main_thread for disc, tid in build_calls)
    finally:
        success.release.set()
        close.release.set()
        model.Pipeline = original_pipeline
        model.subprocess.Popen = original_popen
        Gtk.main_quit = original_quit
        app.gamepad.suspend()
        app.window.destroy()


out = Path(sys.argv[1])
try:
    probe(out)
except Exception:
    (out / 'error.log').write_text(traceback.format_exc(), encoding='utf-8')
    raise SystemExit(1)
(out / 'passed').touch()
'''


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--program', type=Path)
    mode.add_argument('--source', action='store_true')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.out) as temporary:
        out = Path(temporary).resolve()
        source = out / 'source' / 'tools'
        source.mkdir(parents=True)
        script = source / 'funcmap_elf.py'
        script.write_text(PROBE, encoding='utf-8')
        program = args.program.resolve() if args.program else Path(__file__).resolve().parents[1] / 'desktop/main.py'
        command = [str(program)] if args.program else [sys.executable, str(program)]
        # The native Windows launcher uses --data-dir to place its log alongside the fixture.
        result = subprocess.run([*command, '--pipeline-helper', str(source.parent), 'funcmap_elf', str(out),
                                 '--data-dir', str(out)], timeout=45)
        if result.returncode or not (out / 'passed').is_file():
            for log in ('error.log', 'last-launcher.log'):
                if (out / log).is_file():
                    print((out / log).read_text(encoding='utf-8'), file=sys.stderr)
            raise SystemExit('Compile and play smoke test failed')
    print('Passed: compile readiness, progress, focus, failure, retry, explicit play and close')
