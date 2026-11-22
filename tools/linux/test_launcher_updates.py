#!/usr/bin/env python3
"""Test update placement and the first check in the frozen GTK launcher."""
import argparse
from pathlib import Path
import subprocess


PROBE = r'''
import argparse
import json
from pathlib import Path
import sys
import time

import main
from main import GLib, Gtk, Launcher
from updates import Update

out = Path(sys.argv[1])
data = out / 'profile'
data.mkdir(exist_ok=True)
(data / 'launcher-preferences.json').write_text(json.dumps({'language': 'pt'}))

release = Update('0.4.2', 'linux-x86_64', 'fixture', 'https://github.com/fixture', 1, 'a' * 64)
responses = iter((release, None, OSError('TLS test failure')))
calls = []

def check(version):
    calls.append(version)
    result = next(responses)
    if isinstance(result, Exception):
        raise result
    return result

def settle(predicate):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        while Gtk.events_pending():
            Gtk.main_iteration_do(False)
        if predicate():
            return
        time.sleep(.01)
    raise AssertionError('Launcher did not reach the expected update state')

main.check_for_update = check
args = argparse.Namespace(data_dir=data, page='home', rom=None, test_game=False, screenshot=None)
app = Launcher(args)
try:
    body = app.stack.get_parent()
    assert all(getattr(body, 'get_margin_' + edge)() == 0
               for edge in ('top', 'bottom', 'start', 'end'))
    padding = body.get_style_context().get_padding(Gtk.StateFlags.NORMAL)
    assert min(padding.left, padding.top, padding.right, padding.bottom) > 0
    assert not app.update_button.get_visible()
    settle(lambda: app.update_button.get_visible())
    assert calls == [main.VERSION], calls
    assert app.update_button.get_label() == 'Atualizar para 0.4.2'
    assert app.update_status_label.get_text() == 'Versão 0.4.2 disponível.'
    assert app.check_button.is_ancestor(app.navigation_rail) and app.check_button.is_visible()
    assert app.update_button.is_ancestor(app.status_strip)
    app.navigate('more')
    assert not app.check_button.is_visible() and app.update_button.is_visible()
    app.navigate('home')

    app.check_button.clicked()
    settle(lambda: app.update_status_label.get_text() == 'O launcher já está atualizado.')
    assert not app.update_button.get_visible()
    assert app.check_button.get_sensitive()

    app.check_button.clicked()
    settle(lambda: app.update_status_label.get_tooltip_text() == 'TLS test failure')
    assert 'Tente novamente' in app.update_status_label.get_text()
    assert app.check_button.get_sensitive()
    assert calls == [main.VERSION] * 3
finally:
    GLib.source_remove(app.gamepad_timer)
    if app.update_timer is not None:
        GLib.source_remove(app.update_timer)
    app.gamepad.suspend()
    app.window.destroy()
print('Passed: startup, Home placement, update available, up to date and error recovery')
'''


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    source = out / "source/tools"
    source.mkdir(parents=True, exist_ok=True)
    (source / "funcmap_elf.py").write_text(PROBE, encoding="utf-8")
    subprocess.run([str(args.program.resolve()), "--pipeline-helper", str(source.parent),
                    "funcmap_elf", str(out)], check=True, timeout=20)
