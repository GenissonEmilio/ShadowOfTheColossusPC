#!/usr/bin/env python3
"""ROM-free GTK smoke test for launcher focus motion and the side menu.

xvfb-run -a /usr/bin/python3 tools/linux/test_launcher_motion.py --source --out DIR
xvfb-run -a python3 tools/linux/test_launcher_motion.py --program PATH --out DIR
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


PROBE = r'''
import argparse
from pathlib import Path
import sys
import time
import traceback

from main import Gdk, GLib, Gtk, Launcher, controller_focus
from model import atomic_json


def settle(predicate, description):
    deadline = time.monotonic() + 3
    context = GLib.MainContext.default()
    while time.monotonic() < deadline:
        while context.pending():
            context.iteration(False)
            if predicate():
                return
        if predicate():
            return
        time.sleep(.005)
    raise AssertionError(description)


def key(window, keyval, state=0):
    event = Gdk.Event.new(Gdk.EventType.KEY_PRESS)
    event.key.window = window.get_window()
    event.key.send_event = True
    event.key.keyval = keyval
    event.key.state = state
    return window.emit('key-press-event', event)


def pointer(widget, kind):
    window = widget.get_event_window() if isinstance(widget, Gtk.Button) else widget.get_window()
    event = Gdk.Event.new(kind)
    event.window = window
    event.send_event = True
    event.set_device(widget.get_display().get_default_seat().get_pointer())
    if kind in (Gdk.EventType.ENTER_NOTIFY, Gdk.EventType.LEAVE_NOTIFY):
        event.crossing.mode = Gdk.CrossingMode.NORMAL
        event.crossing.detail = Gdk.NotifyType.NONLINEAR
    else:
        event.button.button = 1
        event.button.x = widget.get_allocated_width() / 2
        event.button.y = widget.get_allocated_height() / 2
    widget.event(event)


def probe(out):
    data = out / 'profile'
    args = argparse.Namespace(data_dir=data, page='home', rom=None, test_game=False,
                              screenshot=None, no_update_check=True)
    atomic_json(data / 'launcher-preferences.json', {'language': 'en', 'ui_sounds': False})
    Gtk.Settings.get_default().set_property('gtk-enable-animations', True)
    app = Launcher(args)
    GLib.source_remove(app.gamepad_timer)
    try:
        window = app.window
        window.present()
        app.choose_button.grab_focus()
        card = app.menu_button
        settle(lambda: window.is_active() and card.get_mapped() and card.progress == 0,
               'Launcher did not acquire focus with the unfocused card at rest')
        size = card.get_allocation()
        card.grab_focus()
        settle(lambda: 0 < card.progress < 1, 'Focused card did not animate through an intermediate state')
        settle(lambda: card.progress == 1, 'Focused card did not reach its final size')
        assert card.get_allocation().equal(size), 'The animation shifted the card hitbox or layout'
        app.choose_button.grab_focus()
        settle(lambda: 0 < card.progress < 1, 'Unfocused card did not animate through an intermediate state')
        settle(lambda: card.progress == 0, 'Unfocused card did not return to its resting size')

        pointer(card, Gdk.EventType.ENTER_NOTIFY)
        assert window.get_focus() == app.choose_button, 'Hover moved keyboard focus'
        settle(lambda: card.progress == 1, 'Pointer hover did not grow the card')
        pointer(card, Gdk.EventType.LEAVE_NOTIFY)
        settle(lambda: card.progress == 0, 'Pointer leave did not shrink the card')
        pointer(app.menu_button, Gdk.EventType.ENTER_NOTIFY)
        pointer(app.menu_button, Gdk.EventType.BUTTON_PRESS)
        pointer(app.menu_button, Gdk.EventType.BUTTON_RELEASE)
        settle(lambda: app.menu_open and app.menu_revealer.get_child_revealed()
               and app.menu_overlay.get_child_revealed(),
               'Pointer click did not open the side menu')
        assert window.get_focus() == app.menu_buttons['home'], 'Opening the menu did not focus the current page'
        shade = app.menu_overlay.get_child()
        pointer(shade, Gdk.EventType.BUTTON_PRESS)
        pointer(shade, Gdk.EventType.BUTTON_RELEASE)
        settle(lambda: not app.menu_open and not app.menu_overlay.get_child_revealed(),
               'Clicking the dimmed background did not close the side menu')

        app.choose_button.grab_focus()
        assert key(window, Gdk.KEY_F1), 'The launcher did not handle the menu shortcut'
        assert app.menu_open and app.menu_revealer.get_reveal_child()
        assert not app.menu_revealer.get_child_revealed(), 'Menu skipped its opening animation'
        settle(app.menu_revealer.get_child_revealed, 'Side menu did not finish opening')
        assert window.get_focus() == app.menu_buttons['home']
        assert not app.content_root.is_sensitive(), 'The background stayed interactive while the menu was open'
        assert app.choose_button not in controller_focus.focusables(window)
        app.choose_button.grab_focus()
        assert window.get_focus() == app.menu_buttons['home'], 'Focus escaped to the menu background'
        assert key(window, Gdk.KEY_Down), 'The launcher did not handle Down in the menu'
        assert window.get_focus() == app.menu_buttons['graphics'], 'Down did not navigate within the menu'
        assert key(window, Gdk.KEY_Escape), 'The launcher did not handle Escape in the menu'
        assert not app.menu_open and not app.menu_revealer.get_reveal_child()
        assert app.menu_panel.get_mapped(), 'Menu skipped its closing animation'
        assert not set(app.menu_buttons.values()).intersection(controller_focus.focusables(window)), \
            'The closing menu still accepted navigation focus'
        settle(lambda: not app.menu_revealer.get_child_revealed() and not app.menu_overlay.get_child_revealed(),
               'Side menu did not finish closing')
        assert window.get_focus() == app.choose_button, 'Canceling the menu did not restore the previous focus'
        assert app.stack.get_visible_child_name() == 'home', 'Canceling the menu selected a page'

        assert key(window, Gdk.KEY_F1)
        settle(app.menu_revealer.get_child_revealed, 'Side menu did not reopen')
        assert key(window, Gdk.KEY_Down)
        assert key(window, Gdk.KEY_Return), 'The launcher did not handle Enter in the menu'
        settle(lambda: not app.menu_open and app.stack.get_visible_child_name() == 'graphics',
               'Enter did not open the selected page and dismiss the menu')
        assert window.get_focus() not in app.menu_buttons.values(), 'Focus remained on a hidden menu item'
        assert key(window, Gdk.KEY_Escape), 'The launcher did not handle Escape on the graphics page'
        assert app.stack.get_visible_child_name() == 'home'
        assert app.gamepad_hint.get_text(), 'The launcher did not show input hints'

        pointer(card, Gdk.EventType.LEAVE_NOTIFY)
        settle(lambda: card.progress == 0 and not card._tick, 'Menu button did not return to rest')
        Gtk.Settings.get_default().set_property('gtk-enable-animations', False)
        card.grab_focus()
        assert card.progress == 1 and not card._tick, 'Reduced motion still animated focus'
        app.choose_button.grab_focus()
        assert card.progress == 0 and not card._tick, 'Reduced motion still animated unfocus'
        assert key(window, Gdk.KEY_F1)
        assert app.menu_revealer.get_child_revealed() and app.menu_overlay.get_child_revealed(), \
            'Reduced motion still animated the menu opening'
        assert key(window, Gdk.KEY_Escape)
        assert not app.menu_revealer.get_child_revealed() and not app.menu_overlay.get_child_revealed(), \
            'Reduced motion still animated the menu closing'
    finally:
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
        (source / 'funcmap_elf.py').write_text(PROBE, encoding='utf-8')
        program = args.program.resolve() if args.program else Path(__file__).resolve().parents[1] / 'desktop/main.py'
        command = [str(program)] if args.program else [sys.executable, str(program)]
        result = subprocess.run([*command, '--pipeline-helper', str(source.parent), 'funcmap_elf', str(out),
                                 '--data-dir', str(out)], timeout=45)
        if result.returncode or not (out / 'passed').is_file():
            for log in ('error.log', 'last-launcher.log'):
                if (out / log).is_file():
                    print((out / log).read_text(encoding='utf-8'), file=sys.stderr)
            raise SystemExit('Launcher focus motion and side menu smoke test failed')
    print('Passed: focus and hover animation, pointer activation, menu navigation and reduced motion')
