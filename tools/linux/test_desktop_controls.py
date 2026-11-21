#!/usr/bin/env python3
"""Real frozen GTK editor: key capture, persistent profile, reload and cancel.

Run under xvfb-run for headless CI. No game or desktop input injection.
"""
import argparse
from pathlib import Path
import subprocess
import json

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--program', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
out = args.out.resolve()
source = out/'helper-source/tools'
source.mkdir(parents=True, exist_ok=True)
(source/'funcmap_elf.py').write_text('''import sys, json
from pathlib import Path
import gi
gi.require_version('Gtk','3.0')
gi.require_version('Gdk','3.0')
from gi.repository import Gtk, Gdk, GLib
from model import Library, assets_directory
from controls import Controls, DEFAULTS
from controls_dialog import ControlsDialog
import cairo
out=Path(sys.argv[1]);lib=Library(out/'synthetic-home')
provider=Gtk.CssProvider();provider.load_from_path(str(assets_directory()/'style.css'))
Gtk.StyleContext.add_provider_for_screen(Gdk.Screen.get_default(),provider,Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
Gtk.Settings.get_default().set_property("gtk-application-prefer-dark-theme",True)
parent=Gtk.Window()
dialog=ControlsDialog(parent,lib);dialog.show_all()
failures=[]
def guard(fn):
 def wrapped():
  try: fn()
  except Exception as exc:
   failures.append(repr(exc));Gtk.main_quit()
  return False
 return wrapped
def capture_key(keyval):
 pop=next(w for w in Gtk.Window.list_toplevels() if isinstance(w,Gtk.Dialog) and w is not dialog and w.get_visible())
 event=Gdk.Event.new(Gdk.EventType.KEY_PRESS);event.keyval=keyval;event.state=0
 assert pop.emit('key-press-event',event)
def edit():
 GLib.timeout_add(100,guard(lambda:capture_key(Gdk.KEY_b)))
 dialog.buttons['attack'].clicked()
 assert dialog.values['attack']==66
 GLib.timeout_add(100,guard(lambda:capture_key(Gdk.KEY_Escape)))
 dialog.buttons['start'].clicked()
 assert dialog.values['start']==256
 dialog.invert_y.set_active(True);dialog.sensitivity.set_value(1.8)
 GLib.timeout_add(250,guard(save))
def save():
 surface=cairo.ImageSurface(cairo.FORMAT_ARGB32,dialog.get_allocated_width(),dialog.get_allocated_height())
 dialog.draw(cairo.Context(surface));surface.write_to_png(str(out/'editor.png'))
 dialog.response(Gtk.ResponseType.OK)
 expected=dict(DEFAULTS,attack=66,invert_y=1,sensitivity=1.8)
 assert Controls(lib).load()==expected
 second=ControlsDialog(parent,lib);second.show_all()
 assert second.buttons['attack'].get_label()=='B' and second.invert_y.get_active()
 second.values['attack']=-1;second.response(Gtk.ResponseType.CANCEL)
 assert Controls(lib).load()==expected
 (out/'report.json').write_text(json.dumps({'passed':True,'key_capture':True,'escape_bindable':True,'save_reload':True,'cancel_preserved':True},indent=2)+'\\n')
 Gtk.main_quit()
GLib.timeout_add(300,guard(edit))
Gtk.main()
assert not failures,failures
''')
subprocess.run([str(args.program.resolve()), '--pipeline-helper', str(source.parent),
                'funcmap_elf', str(out)], check=True, timeout=25)
report = json.loads((out/'report.json').read_text())
assert report['passed']
print(json.dumps(report))
