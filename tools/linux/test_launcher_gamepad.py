#!/usr/bin/env python3
"""Launcher input logic and optional GTK + virtual SDL controller smoke test.

python3 tools/linux/test_launcher_gamepad.py
xvfb-run -a /usr/bin/python3 tools/linux/test_launcher_gamepad.py --source --out DIR
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

    def test_file_parent_is_an_edge_button(self):
        nav = Navigation()
        nav.update(set(), 0)
        self.assertEqual(nav.update({'parent'}, .1), ['parent'])
        self.assertEqual(nav.update({'parent'}, 1), [])

    def test_hotplug_waits_for_next_scheduled_scan(self):
        from unittest.mock import Mock
        now = [10.0]
        pad = Gamepad.__new__(Gamepad)
        pad.clock = lambda: now[0]
        pad.sdl = Mock()
        pad.controller = None
        pad.running = True
        pad.next_scan = 11.0
        pad.navigation = Navigation()
        pad.sdl.SDL_NumJoysticks.return_value = 1
        pad.sdl.SDL_IsGameController.return_value = 1
        pad.sdl.SDL_GameControllerOpen.return_value = 123
        pad.sdl.SDL_GameControllerGetButton.return_value = 0
        pad.sdl.SDL_GameControllerGetAxis.return_value = 0
        self.assertEqual(pad.poll(True), [])
        self.assertFalse(pad.connected)
        pad.sdl.SDL_NumJoysticks.assert_not_called()
        now[0] = 11.0
        self.assertEqual(pad.poll(True), [])
        self.assertTrue(pad.connected)
        pad.sdl.SDL_GameControllerOpen.assert_called_once_with(0)

    def test_absent_runtime_is_optional_and_suspend_safe(self):
        from unittest.mock import patch
        with patch('gamepad.load_sdl', return_value=None):
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
from main import Launcher, controller_focus
out = Path(sys.argv[1])
args = argparse.Namespace(data_dir=out/'synthetic-home',page='home',rom=None,test_game=False,screenshot=None,
                          no_update_check=True)
launcher = Launcher(args)
GLib.source_remove(launcher.gamepad_timer)
def settle(predicate, description):
    deadline=time.monotonic()+2
    context=GLib.MainContext.default()
    while time.monotonic()<deadline:
        while context.pending():
            context.iteration(False)
        if predicate():
            return
        time.sleep(.01)
    assert predicate(), description() if callable(description) else description
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
if getattr(sys, 'frozen', False):
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
sdl.SDL_GetError.argtypes=[];sdl.SDL_GetError.restype=ctypes.c_char_p
def connected():
    pad.poll(True)
    return pad.connected and pad.navigation.armed
def controller_diagnostic():
    return {'error':'virtual controller did not connect within two seconds',
            'sdl_error':sdl.SDL_GetError().decode('utf-8',errors='replace'),
            'joysticks':sdl.SDL_NumJoysticks(),
            'virtual_index':index,'is_game_controller':bool(is_controller(index)),
            'next_scan':pad.next_scan,'now':time.monotonic(),'running':pad.running}
# GTK focus settling can run the launcher's first (empty) scan before attach.
# Respect the same one-second hotplug scan interval as the real launcher.
settle(connected,controller_diagnostic)
assert sdl.SDL_JoystickSetVirtualButton(stick,0,1) == 0
actions = pad.poll(True)
assert actions==['confirm'], {'actions':actions,'armed':pad.navigation.armed,'held':pad.navigation.previous,
                              'sdl':pad.sdl._name,'error':sdl.SDL_GetError().decode(errors='replace')}
assert pad.poll(True)==[]
sdl.SDL_JoystickSetVirtualButton(stick,0,0);pad.poll(True)
sdl.SDL_JoystickSetVirtualAxis(stick,0,5000);assert pad.poll(True)==[]
sdl.SDL_JoystickSetVirtualAxis(stick,0,25000);assert pad.poll(True)==['right']
sdl.SDL_JoystickSetVirtualAxis(stick,0,0);pad.poll(True)
pad.poll(False);assert not pad.connected and not pad.running
pad.poll(True);assert pad.connected
# Exercise a synchronous GTK modal with real virtual SDL events. The launcher
# timer must continue ticking and wait for A to be released before B is used.
launcher.navigate('graphics')
launcher.fullscreen.grab_focus()
launcher.fullscreen.set_active(0)
launcher.poll_gamepad()
launcher.gamepad_timer = GLib.timeout_add(33, launcher.poll_gamepad)
sdl.SDL_JoystickSetVirtualButton(stick,0,1)
def combo_dialog():
    return next((w for w in Gtk.Window.list_toplevels()
                 if isinstance(w,Gtk.Dialog) and w.get_visible() and w.get_transient_for()==launcher.window),None)
modal = {'seen': False, 'released': False, 'pressed_back': False, 'timed_out': False}
deadline = time.monotonic()+3
def drive_modal():
    dialog = combo_dialog()
    if time.monotonic() >= deadline:
        modal['timed_out'] = True
        if dialog:
            dialog.response(Gtk.ResponseType.CANCEL)
        return False
    if not dialog:
        return not modal['pressed_back']
    if not dialog.is_active():
        dialog.present()
        return True
    modal['seen'] = True
    if not modal['released'] and launcher.gamepad_target == dialog and not pad.navigation.armed:
        sdl.SDL_JoystickSetVirtualButton(stick,0,0)
        modal['released'] = True
    elif modal['released'] and not modal['pressed_back'] and pad.navigation.armed:
        sdl.SDL_JoystickSetVirtualButton(stick,1,1)
        modal['pressed_back'] = True
    return True
GLib.timeout_add(55, drive_modal)
settle(lambda: (modal['pressed_back'] or modal['timed_out']) and combo_dialog() is None, lambda: str(modal))
assert modal['seen'] and modal['released'] and modal['pressed_back'] and not modal['timed_out'], modal
assert launcher.fullscreen.get_active()==0 and controller_focus.focus_widget(launcher.window)==launcher.fullscreen
sdl.SDL_JoystickSetVirtualButton(stick,1,0)
sdl.SDL_JoystickClose(stick)
sdl.SDL_JoystickDetachVirtual(index)
pad.poll(True);assert not pad.connected
# Verify GTK actions against actual launcher widgets without sending desktop keys.
launcher.navigate('home')
settle(lambda: not launcher.stack.get_transition_running()
       and launcher.choose_button.get_allocated_height() > 1, 'home layout did not settle')
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
settle(lambda: launcher.shader_import.get_allocated_height() > 1, 'graphics layout did not settle')
adjustment = launcher.page_scroller.get_vadjustment()
adjustment.set_value(0)
shader_buttons = (launcher.shader_import, launcher.shader_hd)
for _ in range(20):
    if launcher.window.get_focus() in shader_buttons:
        break
    launcher.gamepad_action(launcher.window,'down')
assert launcher.window.get_focus() in shader_buttons, launcher.window.get_focus()
assert adjustment.get_value()>0, 'the graphics page did not scroll to reveal its focused shader button'
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
launcher.watch_game=lambda:False
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
# Navigate the native chooser's file list and go up to its parent directory.
fixture = out/'files'; nested = fixture/'nested'; nested.mkdir(parents=True, exist_ok=True)
(nested/'test.iso').touch()
dialog=Gtk.FileChooserDialog(transient_for=launcher.window,action=Gtk.FileChooserAction.OPEN)
dialog.add_button('Open',Gtk.ResponseType.OK)
dialog.set_default_response(Gtk.ResponseType.OK)
dialog.set_current_folder(str(nested))
launcher.configure_file_chooser(dialog)
dialog.show_all();dialog.present()
settle(lambda: isinstance(dialog.get_focus(), Gtk.TreeView) and dialog.get_focus().get_model() is not None
       and len(dialog.get_focus().get_model()) == 1, 'chooser did not display fixture')
launcher.gamepad_action(dialog,'down')
assert dialog.get_filename()==str(nested/'test.iso'), dialog.get_filename()
launcher.gamepad_action(dialog,'parent')
settle(lambda: dialog.get_current_folder() == str(fixture), 'chooser did not go up to parent')
settle(lambda: dialog.get_filename() == str(nested), 'chooser did not select the previous folder')
launcher.gamepad_action(dialog,'confirm')
settle(lambda: dialog.get_current_folder() == str(nested), 'chooser did not open selected folder')
settle(lambda: dialog.get_filename() == str(nested/'test.iso'), 'chooser did not select the test file')
responses=[]
dialog.connect('response',lambda _,response:responses.append(response))
launcher.gamepad_action(dialog,'confirm')
settle(lambda: responses==[Gtk.ResponseType.OK], 'chooser did not accept selected file')
dialog.destroy()
# The timer must gate SDL polling both on focus and while the game owns input.
launcher.gamepad_window=lambda:launcher.window
launcher.process=object();launcher.poll_gamepad();assert not pad.running
launcher.process=None
launcher.gamepad_window=lambda:None
launcher.poll_gamepad();assert not pad.running
GLib.source_remove(launcher.gamepad_timer)
pad.suspend();sdl.SDL_QuitSubSystem(pad.FLAGS);launcher.window.destroy()
report={'passed':True,'bundled_sdl':bool(getattr(sys,'frozen',False)),'virtual_controller_edges_deadzone_hotplug':True,
        'focus_suspend':True,'gtk_pages_settings_play_back':True,'gtk_modal_neutral_rearm':True,
        'gtk_scroll_follows_focus':True,'gtk_file_chooser_navigation':True}
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
'''

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--program', type=Path)
    mode.add_argument('--source', action='store_true')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(NavigationTest))
    if not result.wasSuccessful():
        raise SystemExit(1)
    if args.program or args.source:
        if not args.out:
            parser.error('--program and --source require --out')
        out = args.out.resolve()
        if args.source:
            old_argv = sys.argv
            try:
                sys.argv = [old_argv[0], str(out)]
                exec(PROBE, {})
            finally:
                sys.argv = old_argv
        else:
            source = out/'helper-source/tools'
            source.mkdir(parents=True, exist_ok=True)
            (source/'funcmap_elf.py').write_text(PROBE)
            subprocess.run([str(args.program.resolve()), '--pipeline-helper', str(source.parent),
                            'funcmap_elf', str(out)], check=True, timeout=30)
        assert json.loads((out/'report.json').read_text())['passed']
