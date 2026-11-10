#!/usr/bin/env python3
"""Launcher input logic; optional frozen GTK + virtual SDL controller smoke test.

python3 tools/linux/test_launcher_gamepad.py
xvfb-run -a python3 tools/linux/test_launcher_gamepad.py --program PATH --out DIR
The optional probe changes only a synthetic data directory. No global input.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'desktop'))
from gamepad import Gamepad, Navigation


class NavigationTest(unittest.TestCase):
    def test_neutral_resume_and_edges(self):
        nav = Navigation()
        self.assertEqual(nav.update({'confirm'}, 0), [])
        self.assertEqual(nav.update(set(), .1), [])
        self.assertEqual(nav.update({'confirm'}, .2), ['confirm'])
        self.assertEqual(nav.update({'confirm'}, 2), [])
        nav.reset()
        self.assertEqual(nav.update({'confirm'}, 3), [])

    def test_direction_repeat_bounded_after_delay(self):
        nav = Navigation(); nav.update(set(), 0)
        self.assertEqual(nav.update({'down'}, .1), ['down'])
        self.assertEqual(nav.update({'down'}, .49), [])
        self.assertEqual(nav.update({'down'}, .5), ['down'])
        self.assertEqual(nav.update({'down'}, 10), ['down'])
        self.assertEqual(nav.update({'right'}, 10.01), ['right'])

    def test_absent_runtime_is_optional_and_suspend_safe(self):
        from unittest.mock import patch
        with patch('gamepad.ctypes.CDLL', side_effect=OSError):
            pad = Gamepad('/nonexistent', '/nonexistent')
        self.assertEqual(pad.poll(True), [])
        pad.suspend()
        self.assertFalse(pad.connected)


PROBE = r'''
import argparse, ctypes, json, sys, time
from pathlib import Path
import gi
gi.require_version('Gtk','3.0')
from gi.repository import Gtk, GLib
from gamepad import Gamepad
from main import Launcher
out = Path(sys.argv[1])
args = argparse.Namespace(data_dir=out/'synthetic-home',page='home',rom=None,test_game=False,screenshot=None)
launcher = Launcher(args)
def settle(predicate, description):
    deadline=time.monotonic()+2
    context=GLib.MainContext.default()
    while time.monotonic()<deadline:
        while context.pending():
            context.iteration(False)
        if predicate():
            return
        time.sleep(.01)
    assert predicate(), description
# show_all creates widgets but the X focus events have not reached GTK yet.
# Button.child_focus on an inactive toplevel grabs the same button again;
# test real Dpad traversal only after the isolated Xvfb window has focus.
launcher.window.present()
launcher.focus_gamepad_page()
settle(lambda: launcher.window.is_active() and launcher.choose_button.has_focus(),
       'launcher did not acquire actual GTK focus in isolated Xvfb')
pad = launcher.gamepad
assert pad.sdl, 'SDL2 not loaded'
bundled = Path(sys.executable).resolve().parent/'_internal/libSDL2-2.0.so.0'
assert bundled.is_file() and Path(pad.sdl._name).resolve() == bundled.resolve(), pad.sdl._name
assert any(p.name == 'gamecontrollerdb.txt' and p.is_file() for p in pad.mapping_paths)
sdl = pad.sdl
assert sdl.SDL_InitSubSystem(pad.FLAGS) == 0
sdl.SDL_JoystickAttachVirtual.argtypes=[ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int]
sdl.SDL_JoystickAttachVirtual.restype=ctypes.c_int
sdl.SDL_JoystickOpen.argtypes=[ctypes.c_int];sdl.SDL_JoystickOpen.restype=ctypes.c_void_p
sdl.SDL_JoystickClose.argtypes=[ctypes.c_void_p]
sdl.SDL_JoystickSetVirtualButton.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_uint8]
sdl.SDL_JoystickSetVirtualAxis.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_int16]
index=sdl.SDL_JoystickAttachVirtual(1,6,15,0)
assert index >= 0
stick=sdl.SDL_JoystickOpen(index)
# Ignore any real controllers on the test host; exercise the virtual pad only.
is_controller=sdl.SDL_IsGameController
sdl.SDL_IsGameController=lambda candidate: candidate == index and is_controller(candidate)
assert stick
pad.poll(True); assert pad.connected
sdl.SDL_JoystickSetVirtualButton(stick,0,1)
assert pad.poll(True)==['confirm']
assert pad.poll(True)==[]
sdl.SDL_JoystickSetVirtualButton(stick,0,0);pad.poll(True)
sdl.SDL_JoystickSetVirtualAxis(stick,0,5000);assert pad.poll(True)==[]
sdl.SDL_JoystickSetVirtualAxis(stick,0,25000);assert pad.poll(True)==['right']
sdl.SDL_JoystickSetVirtualAxis(stick,0,0);pad.poll(True)
pad.poll(False);assert not pad.connected and not pad.running
pad.poll(True);assert pad.connected
sdl.SDL_JoystickClose(stick)
sdl.SDL_JoystickDetachVirtual(index)
pad.poll(True);assert not pad.connected
# Verify GTK actions against actual launcher widgets without sending desktop keys.
launcher.focus_gamepad_page();assert launcher.window.get_focus() == launcher.choose_button
launcher.gamepad_action(launcher.window,'down')
assert launcher.window.get_focus() is not None and launcher.window.get_focus() != launcher.choose_button
launcher.gamepad_action(launcher.window,'up')
assert launcher.window.get_focus() == launcher.choose_button and launcher.choose_button.has_focus()
launcher.gamepad_action(launcher.window,'next')
assert launcher.stack.get_visible_child_name()=='graphics'
launcher.fullscreen.grab_focus();launcher.fullscreen.set_active(0)
launcher.gamepad_action(launcher.window,'right');assert launcher.fullscreen.get_active()==1
launcher.gamepad_action(launcher.window,'left');assert launcher.fullscreen.get_active()==0
launcher.gamepad_action(launcher.window,'next');assert launcher.stack.get_visible_child_name()=='controls'
launcher.gamepad_action(launcher.window,'back');assert launcher.stack.get_visible_child_name()=='home'
called=[];launcher.play_button.set_sensitive(True)
class FakeGame:
    def poll(self): return None
process=FakeGame()
def launch(*args):
    called.append(True)
    return process
launcher.library.launch=launch
launcher.gamepad_action(launcher.window,'play')
from gi.repository import GLib
end=time.monotonic()+2
while launcher.process is None and time.monotonic()<end:
    GLib.MainContext.default().iteration(False)
    time.sleep(.01)
assert called==[True] and launcher.process is process and not pad.running
launcher.process=None
dialog=Gtk.Dialog(transient_for=launcher.window);responses=[]
dialog.connect('response',lambda _,response:responses.append(response))
launcher.gamepad_action(dialog,'back');assert responses==[Gtk.ResponseType.CANCEL]
dialog.destroy()
# The timer must gate SDL polling both on focus and while the game owns input.
launcher.gamepad_window=lambda:launcher.window
launcher.process=object();launcher.poll_gamepad();assert not pad.running
launcher.process=None
launcher.gamepad_window=lambda:None
launcher.poll_gamepad();assert not pad.running
pad.suspend();sdl.SDL_QuitSubSystem(pad.FLAGS);launcher.window.destroy()
report={'passed':True,'bundled_sdl':True,'virtual_controller_edges_deadzone_hotplug':True,
        'focus_suspend':True,'gtk_pages_settings_play_back':True}
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
'''

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--program', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(NavigationTest))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.program:
        if not args.out:
            parser.error('--program requires --out')
        out = args.out.resolve(); source = out/'helper-source/tools'
        source.mkdir(parents=True, exist_ok=True)
        (source/'funcmap_elf.py').write_text(PROBE)
        subprocess.run([str(args.program.resolve()), '--pipeline-helper', str(source.parent),
                        'funcmap_elf', str(out)], check=True, timeout=30)
        assert json.loads((out/'report.json').read_text())['passed']
