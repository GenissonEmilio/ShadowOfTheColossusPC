#!/usr/bin/env python3
"""Shadow of the Colossus PC desktop launcher."""
from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
import subprocess
from pathlib import Path
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parent))
from model import Library, SAVE_STATES_AVAILABLE, VERSION, assets_directory, atomic_json, program_directory
from graphics import GRAPHICS_PROFILES, LEGACY_PROFILES, Graphics, WINDOW_RESOLUTIONS
from gamepad import Gamepad
from language import LANGUAGES, PREFERENCES, PROMPTS, activate, preferences, translate as tr
from launcher_audio import LauncherAudio
from pipeline import CompilationError
from texture_mods import TextureMods
from updates import RateLimitedError, check_for_update, confirm_startup, restart_with_update, stage_update

if len(sys.argv) > 1 and sys.argv[1] == "--pipeline-helper":
    from pipeline import helper
    helper(sys.argv[2], sys.argv[3], sys.argv[4:])
    raise SystemExit(0)

if __name__ == "__main__" and sys.argv[1:2] in (["--prepare"], ["--play"]):
    from recomp_cli import run
    raise SystemExit(run(sys.argv[1:]))

if sys.platform.startswith("linux"):
    if getattr(sys, "frozen", False):
        os.environ.pop("GTK_MODULES", None)
        os.environ["FONTCONFIG_FILE"] = str(assets_directory() / "fonts.conf")
    # Register the shipped fonts by absolute path. Older fontconfig resolves
    # relative font directories differently on newer Linux desktops.
    fontconfig = ctypes.CDLL("libfontconfig.so.1")
    fontconfig.FcConfigGetCurrent.restype = ctypes.c_void_p
    fontconfig.FcConfigAppFontAddFile.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    fontconfig.FcConfigAppFontAddFile.restype = ctypes.c_int
    for font_file in sorted((assets_directory() / "fonts").iterdir()):
        if font_file.suffix not in (".ttf", ".otf"):
            continue
        if not fontconfig.FcConfigAppFontAddFile(fontconfig.FcConfigGetCurrent(), os.fsencode(font_file)):
            print("Could not register bundled font", file=sys.stderr)
elif sys.platform == "win32":
    register_font = ctypes.windll.gdi32.AddFontResourceExW
    register_font.argtypes = [ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_void_p]
    register_font(str(assets_directory() / "fonts/SotCLauncherJapanese-Regular.otf"), 0x10, None)

import gi
gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_foreign("cairo")
from gi.repository import Gdk, GdkPixbuf, GLib, Gtk, Pango
import cairo

import controller_focus
from motion import FocusButton


def style(widget, *names):
    for name in names:
        widget.get_style_context().add_class(name)
    return widget


def label(text, *names, wrap=False):
    widget = style(Gtk.Label(label=text), *names)
    widget.set_xalign(0)
    widget.set_line_wrap(wrap)
    return widget


def box(orientation=Gtk.Orientation.VERTICAL, spacing=0, margin=0, classes=()):
    widget = style(Gtk.Box(orientation=orientation, spacing=spacing), *classes)
    for name in ("top", "bottom", "start", "end"):
        getattr(widget, "set_margin_" + name)(margin)
    return widget


class Artwork(Gtk.DrawingArea):
    """Responsive illustration with transparent artwork for the brand."""
    _images = {}

    def __init__(self, small=False):
        super().__init__()
        self.small = small
        self.set_size_request(48 if small else 0, 64 if small else 260)
        name = "icon.png" if small else "hero.png"
        if name not in self._images:
            self._images[name] = GdkPixbuf.Pixbuf.new_from_file(
                str(assets_directory() / name))
        self.picture = self._images[name]
        self.connect("draw", self.draw)
        self.get_accessible().set_name(
            tr("Wander erguendo a espada sobre um colosso de olhos azuis"))

    def draw(self, widget, cr):
        w, h = self.get_allocated_width(), self.get_allocated_height()
        iw, ih = self.picture.get_width(), self.picture.get_height()
        cr.save()
        if self.small:
            scale = min(w / iw, h / ih)
            cr.translate((w - iw * scale) / 2, (h - ih * scale) / 2)
            cr.scale(scale, scale)
            Gdk.cairo_set_source_pixbuf(cr, self.picture, 0, 0)
            cr.paint()
        else:
            radius = 13
            for cx, cy, start in ((w-radius, radius, -math.pi/2),
                                  (w-radius, h-radius, 0),
                                  (radius, h-radius, math.pi/2),
                                  (radius, radius, math.pi)):
                cr.arc(cx, cy, radius, start, start + math.pi/2)
            cr.close_path()
            cr.clip()
            # Cover without distortion. Right alignment keeps Wander and the
            # glowing eyes visible when the user makes the window narrower.
            scale = max(w / iw, h / ih)
            cr.save()
            cr.translate(w - iw * scale, (h - ih * scale) / 2)
            cr.scale(scale, scale)
            Gdk.cairo_set_source_pixbuf(cr, self.picture, 0, 0)
            cr.paint()
            cr.restore()
            # Keep copy readable even when a narrow window crops away the
            # quiet left half of the wide illustration.
            shade = cairo.LinearGradient(0, 0, w * .71, 0)
            for at, alpha in ((0, .93), (.45, .79), (1, 0)):
                shade.add_color_stop_rgba(at, .035, .065, .058, alpha)
            cr.set_source(shade)
            cr.paint()
            shade = cairo.LinearGradient(0, h * .56, 0, h)
            shade.add_color_stop_rgba(0, .035, .065, .058, 0)
            shade.add_color_stop_rgba(1, .035, .065, .058, .68)
            cr.set_source(shade)
            cr.paint()
        cr.restore()
        return False


class Launcher:
    def __init__(self, args):
        self.args = args
        self.library = Library(args.data_dir)
        self.preferences = preferences(self.library.directory)
        self.last_input_style = "keyboard"
        activate(self.preferences["language"])
        self.graphics = Graphics(self.library)
        self.texture_mods = TextureMods(self.library)
        self.feedback = LauncherAudio(self.library.program, enabled=self.preferences["ui_sounds"])
        self.presentation_fullscreen = self.preferences["launcher_fullscreen"]
        self.busy = False; self.process = None; self.closing = False; self.failed = False
        self.prepare_only = False
        self.disposed = False
        self.controller_process = None
        self.update = None
        self.checking = False
        self.updating = False
        self.restart_language = False
        self.update_timer = None
        self.menu_open = False
        self.menu_restore_focus = None
        provider = Gtk.CssProvider(); provider.load_from_path(str(assets_directory() / "style.css"))
        Gtk.StyleContext.add_provider_for_screen(Gdk.Screen.get_default(), provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        settings = Gtk.Settings.get_default()
        settings.set_property("gtk-application-prefer-dark-theme", True)
        self.window = Gtk.Window(title="Shadow of the Colossus PC")
        self.window.set_wmclass("shadow-of-the-colossus-pc", "ShadowOfTheColossusPC")
        self.window.set_default_size(1180, 770); self.window.set_size_request(900, 600)
        self.window.set_position(Gtk.WindowPosition.CENTER)
        self.window.set_icon_from_file(str(assets_directory() / "icon.png"))
        self.window.connect("delete-event", self.close)
        self.window.connect("destroy", lambda *_: self.feedback.close())
        self.window.connect("key-press-event", self.presentation_key)
        self.window.connect("button-press-event", self.pointer_input)
        self.header = Gtk.HeaderBar()
        self.header.set_show_close_button(True)
        self.header.set_title("Shadow of the Colossus PC")
        self.window.set_titlebar(self.header)
        root = self.content_root = box(classes=("launcher-root",))
        self.shell = Gtk.Overlay()
        self.shell.add(root)
        self.window.add(self.shell)
        navigation = box(Gtk.Orientation.HORIZONTAL, 20, classes=("top-bar",))
        brand = Artwork(True)
        brand.set_size_request(46, 44)
        navigation.pack_start(brand, False, False, 0)
        tabs = box(Gtk.Orientation.HORIZONTAL, 7)
        tabs.set_homogeneous(True)
        self.nav = {}
        self.stack = Gtk.Stack()
        self.stack.set_transition_type(Gtk.StackTransitionType.SLIDE_LEFT_RIGHT)
        self.stack.set_transition_duration(220)
        self.stack.set_hhomogeneous(False); self.stack.set_vhomogeneous(False)
        for key, title in (("home", "Início"), ("graphics", "Gráficos"), ("textures", "Texturas"),
                           ("controls", "Controles"),
                           ("settings", "Configurações"), ("about", "Sobre o port")):
            button = style(FocusButton(label=tr(title)), "nav")
            button.connect("clicked", lambda _, name=key: self.navigate(name))
            tabs.pack_start(button, True, True, 0)
            self.nav[key] = button
        navigation.pack_start(tabs, True, True, 0)
        system = "Windows" if sys.platform == "win32" else "Linux"
        navigation.pack_end(label(f"v{VERSION}  ·  {system}", "build-label"), False, False, 0)
        root.pack_start(navigation, False, False, 0)

        body = box(spacing=18, classes=("main-content",))
        self.page_scroller = style(Gtk.ScrolledWindow(), "main-scroll")
        self.page_scroller.set_shadow_type(Gtk.ShadowType.NONE)
        self.page_scroller.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self.page_scroller.add(body)
        root.pack_start(self.page_scroller, True, True, 0)
        body.pack_start(self.stack, True, True, 0)
        self.home_page = self.home()
        self.stack.add_named(self.home_page, "home")
        self.stack.add_named(self.graphics_page(),"graphics")
        self.stack.add_named(self.textures_page(), "textures")
        self.stack.add_named(self.controls(),"controls")
        self.stack.add_named(self.settings_page(),"settings")
        self.stack.add_named(self.about(),"about")
        footer = box(Gtk.Orientation.HORIZONTAL, 9, classes=("footer",))
        self.menu_button = style(FocusButton(label=tr("Menu")), "footer-action")
        self.menu_button.set_tooltip_text(tr("Abrir menu · F1"))
        self.menu_button.connect("clicked", self.toggle_menu)
        footer.pack_start(self.menu_button, False, False, 0)
        self.gamepad_hint = label(tr("F1 abre os ajustes durante o jogo."), "input-hint", wrap=True)
        footer.pack_start(self.gamepad_hint, True, True, 0)
        self.saves_button = style(Gtk.Button(label=tr("Meus saves")), "footer-action")
        self.saves_button.connect("clicked", self.open_saves)
        footer.pack_start(self.saves_button, False, False, 0)
        self.presentation_button = style(Gtk.Button(), "footer-action")
        self.presentation_button.set_tooltip_text("F11")
        self.presentation_button.connect("clicked", self.toggle_presentation)
        footer.pack_start(self.presentation_button, False, False, 0)
        self.exit_button = style(Gtk.Button(label=tr("Sair do launcher")), "footer-action")
        self.exit_button.connect("clicked", self.close)
        footer.pack_start(self.exit_button, False, False, 0)
        root.pack_end(footer, False, False, 0)
        self.build_menu()
        self.connect_feedback(self.shell)
        self.window.connect("set-focus", self.focus_feedback)
        self.window.show_all()
        self.navigate(args.page)
        self.set_presentation(self.presentation_fullscreen)
        self.progress.hide()
        if self.library.disc:
            self.set_ready()
        else:
            self.play_button.set_sensitive(False)
            self.status.set_text(tr("Escolha sua ROM para começar."))
        self.refresh_prepare_controls()
        target = Gtk.TargetEntry.new("text/uri-list",0,0)
        self.drop.drag_dest_set(Gtk.DestDefaults.ALL,[target],Gdk.DragAction.COPY)
        self.drop.connect("drag-data-received", self.dropped)
        self.gamepad = Gamepad(self.library.program, self.library.directory / "controllers/gamecontrollerdb.txt")
        self.gamepad_timer = GLib.timeout_add(33, self.poll_gamepad)
        if args.rom:
            GLib.timeout_add(300,lambda: self.prepare(args.rom) or False)
        elif args.test_game:
            GLib.timeout_add(300,self.play)
        elif args.screenshot:
            GLib.timeout_add(700,self.capture_and_exit)
        if (not (args.test_game or args.screenshot or getattr(args, "no_update_check", False))
                and (self.library.program / "release.json").is_file()):
            GLib.idle_add(self.check_updates)
            self.update_timer = GLib.timeout_add_seconds(30 * 60, self.periodic_check)
        error_file = self.library.directory / "launcher-update-error.txt"
        if error_file.is_file():
            detail = error_file.read_text(encoding="utf-8-sig", errors="replace")
            error_file.unlink()
            GLib.idle_add(self.error, tr("A atualização falhou"), detail)

    def connect_feedback(self, widget):
        if isinstance(widget, Gtk.Button):
            widget.connect("button-press-event", self.pointer_feedback)
            widget.connect("enter-notify-event", self.hover_feedback)
        if isinstance(widget, Gtk.Container):
            for child in widget.get_children():
                self.connect_feedback(child)

    def focus_feedback(self, window, widget):
        if widget is not None and window.is_active():
            self.feedback.play("focus")

    def pointer_feedback(self, widget, event):
        self.last_input_style = "keyboard"
        if event.button == 1 and widget.is_sensitive():
            self.feedback.play("confirm")
        return False

    def pointer_input(self, _widget, _event):
        self.last_input_style = "keyboard"
        return False

    def hover_feedback(self, widget, event):
        if event.detail != Gdk.NotifyType.INFERIOR and widget.is_sensitive():
            self.feedback.play("focus")
        return False

    def presentation_key(self, _window, event):
        self.last_input_style = "keyboard"
        if event.keyval == Gdk.KEY_F11:
            self.toggle_presentation()
            return True
        if event.keyval == Gdk.KEY_F1:
            self.toggle_menu()
            return True
        if not self.gamepad_available():
            return False
        focus = controller_focus.focus_widget(self.window)
        if isinstance(focus, (Gtk.Entry, Gtk.TextView)):
            return False
        # Let an open native dropdown consume its own navigation keys.
        if isinstance(focus, Gtk.ComboBox) and focus.get_property("popup-shown"):
            return False
        action = {Gdk.KEY_Up: "up", Gdk.KEY_Down: "down",
                  Gdk.KEY_Left: "left", Gdk.KEY_Right: "right",
                  Gdk.KEY_Return: "confirm", Gdk.KEY_KP_Enter: "confirm",
                  Gdk.KEY_Escape: "back", Gdk.KEY_Page_Up: "previous",
                  Gdk.KEY_Page_Down: "next"}.get(event.keyval)
        if action:
            self.gamepad_action(self.window, action)
            return True
        return False

    def build_menu(self):
        self.menu_overlay = Gtk.Revealer()
        self.menu_overlay.set_transition_type(Gtk.RevealerTransitionType.CROSSFADE)
        self.menu_overlay.set_transition_duration(180)
        self.menu_overlay.set_halign(Gtk.Align.FILL)
        self.menu_overlay.set_valign(Gtk.Align.FILL)
        dim = style(Gtk.EventBox(), "menu-shade")
        dim.connect("button-press-event", self.dismiss_menu)
        self.menu_overlay.add(dim)
        self.menu_revealer = Gtk.Revealer()
        self.menu_revealer.set_transition_type(Gtk.RevealerTransitionType.SLIDE_RIGHT)
        self.menu_revealer.set_transition_duration(220)
        self.menu_revealer.set_halign(Gtk.Align.START)
        self.menu_revealer.set_valign(Gtk.Align.FILL)
        self.menu_panel = box(spacing=9, classes=("side-menu",))
        self.menu_panel.set_size_request(290, -1)
        self.menu_panel.pack_start(label("Shadow of the Colossus", "panel-title"), False, False, 16)
        self.menu_buttons = {}
        for name, title in (("home", "Início"), ("graphics", "Gráficos"), ("textures", "Texturas"),
                           ("controls", "Controles"),
                            ("settings", "Configurações"), ("about", "Sobre o port")):
            button = style(FocusButton(label=tr(title)), "menu-item")
            button.get_child().set_xalign(0)
            button.connect("clicked", lambda _, page=name: self.menu_navigate(page))
            self.menu_panel.pack_start(button, False, False, 0)
            self.menu_buttons[name] = button
        saves = style(FocusButton(label=tr("Meus saves")), "menu-item")
        saves.get_child().set_xalign(0)
        saves.connect("clicked", self.menu_saves)
        self.menu_panel.pack_start(saves, False, False, 0)
        self.menu_panel.pack_start(Gtk.Box(), True, True, 0)
        close = style(FocusButton(label=tr("Fechar menu")), "secondary")
        close.connect("clicked", self.toggle_menu)
        self.menu_panel.pack_start(close, False, False, 0)
        panel_surface = Gtk.EventBox()
        panel_surface.add(self.menu_panel)
        self.menu_revealer.add(panel_surface)
        for revealer in (self.menu_overlay, self.menu_revealer):
            revealer.set_no_show_all(True)
            revealer.get_child().show_all()
            revealer.connect("notify::child-revealed", self.hide_closed_menu_layer)
            self.shell.add_overlay(revealer)

    def hide_closed_menu_layer(self, revealer, *_):
        # A collapsed revealer can retain an input window even when its
        # GtkOverlay child is marked pass-through. Unmap it after the fade.
        if not self.menu_open and not revealer.get_child_revealed():
            revealer.hide()

    def toggle_menu(self, *_):
        self.set_menu_open(not self.menu_open)

    def set_menu_open(self, opened):
        if opened == self.menu_open:
            return
        if opened:
            self.menu_restore_focus = self.window.get_focus()
        self.menu_open = opened
        self.content_root.set_sensitive(not opened)
        for revealer in (self.menu_overlay, self.menu_revealer):
            if opened:
                revealer.show()
            revealer.set_reveal_child(opened)
        if opened:
            self.window.set_focus_visible(True)
            controller_focus.grab(self.menu_buttons[self.stack.get_visible_child_name()])
        elif self.menu_restore_focus is not None and self.menu_restore_focus.get_mapped():
            controller_focus.grab(self.menu_restore_focus)
        self.feedback.play("confirm" if opened else "back")

    def dismiss_menu(self, _widget, event):
        if event.button == 1:
            self.set_menu_open(False)
        return True

    def menu_navigate(self, page):
        self.set_menu_open(False)
        self.navigate(page)
        self.focus_gamepad_page()

    def menu_saves(self, *_):
        self.set_menu_open(False)
        self.open_saves()

    def toggle_presentation(self, *_):
        enabled = not self.presentation_fullscreen
        values = {**self.preferences, "launcher_fullscreen": enabled}
        try:
            atomic_json(self.library.directory / PREFERENCES, values)
        except OSError as exc:
            self.error(tr("Não foi possível salvar as configurações"), str(exc))
            return
        self.preferences = values
        self.set_presentation(enabled)

    def set_presentation(self, enabled):
        self.presentation_fullscreen = enabled
        self.presentation_button.set_label(tr("Sair da tela cheia") if enabled else tr("Tela cheia"))
        self.header.set_visible(not enabled)
        self.home_page.set_child_packing(self.home_library, enabled, enabled, 0, Gtk.PackType.START)
        context = self.window.get_style_context()
        if enabled:
            context.add_class("picture-mode")
            self.window.fullscreen()
        else:
            context.remove_class("picture-mode")
            self.window.unfullscreen()

    def gamepad_window(self):
        # Only a focused launcher window or one of its own dialogs can receive input.
        for window in Gtk.Window.list_toplevels():
            if not window.is_active() or not window.get_visible():
                continue
            parent = window
            while parent and parent != self.window:
                parent = parent.get_transient_for()
            if parent == self.window:
                return window
        return None

    def gamepad_available(self):
        return not (self.busy or self.process or self.controller_process)

    def resolved_prompt_style(self):
        mode = self.preferences["button_prompts"]
        return self.last_input_style if mode == "auto" else mode

    def controller_hint_text(self):
        resolved = self.resolved_prompt_style()
        if resolved == "keyboard":
            return tr("Setas mover · Enter confirmar · Esc voltar · F1 menu · F11 tela cheia")
        xbox = resolved == "xbox"
        confirm, back, tabs, start = ("A", "B", "LB/RB", "Menu") if xbox else ("✕", "○", "L1/R1", "Options")
        return tr("Direcional/analógico mover · {confirm} confirmar · {back} voltar · {tabs} trocar aba · {start} jogar · Select menu").format(
            confirm=confirm, back=back, tabs=tabs, start=start)

    def poll_gamepad(self):
        target = self.gamepad_window() if self.gamepad_available() else None
        if target != getattr(self, "gamepad_target", None):
            self.gamepad.reset_navigation()
            self.gamepad_target = target
        actions = self.gamepad.poll(target is not None)
        if self.gamepad.activity_serial != getattr(self, "gamepad_activity_serial", 0):
            self.gamepad_activity_serial = self.gamepad.activity_serial
            self.last_input_style = self.gamepad.last_style
        hint = self.controller_hint_text() if self.gamepad.connected else tr("Setas mover · Enter confirmar · Esc voltar · F1 menu · F11 tela cheia")
        if self.gamepad_hint.get_text() != hint:
            self.gamepad_hint.set_text(hint)
        if self.gamepad.connected:
            diagnostic = tr("Controle no launcher: {devices}").format(devices=", ".join(self.gamepad.device_names))
        elif self.gamepad.unmapped_devices:
            diagnostic = tr("Controle sem mapeamento: {devices}. Use Configurar meu controle.").format(
                devices=", ".join(self.gamepad.unmapped_devices))
        elif self.gamepad.error:
            diagnostic = tr("Não foi possível iniciar a leitura do controle. Teclado e mouse continuam disponíveis.")
        else:
            diagnostic = tr("Nenhum controle detectado. Conecte um controle; a detecção é automática.")
        if self.controller_detection.get_text() != diagnostic:
            self.controller_detection.set_text(diagnostic)
            self.controller_detection.set_tooltip_text(self.gamepad.error)
        if actions:
            # Dispatch outside the timer so Gtk.Dialog.run() can keep polling
            # in its nested loop while the triggering action is still running.
            GLib.idle_add(self.dispatch_gamepad_action, target, actions[0])
        return True

    def dispatch_gamepad_action(self, target, action):
        if self.gamepad_available() and self.gamepad_window() == target:
            self.gamepad_action(target, action)
        return False

    def gamepad_action(self, target, action):
        if target is None:
            return
        target.set_focus_visible(True)
        is_main = target == self.window
        focus = controller_focus.focus_widget(target)
        if is_main and action == "menu":
            self.toggle_menu()
            return
        if is_main and self.menu_open:
            if action == "back":
                self.set_menu_open(False)
            elif action == "confirm" and focus is not None:
                self.confirm_gamepad_focus(target, focus)
            elif action in ("up", "down", "left", "right"):
                controller_focus.move(target, focus, action, scope=self.menu_panel)
            return
        if is_main and action in ("previous", "next"):
            pages = list(self.nav)
            index = pages.index(self.stack.get_visible_child_name())
            self.navigate(pages[(index + (1 if action == "next" else -1)) % len(pages)])
            self.focus_gamepad_page()
        elif action == "back":
            if is_main:
                if self.stack.get_visible_child_name() != "home":
                    self.feedback.play("back")
                    self.navigate("home")
                    self.focus_gamepad_page()
                elif self.presentation_fullscreen:
                    self.feedback.play("back")
                    self.toggle_presentation()
            elif isinstance(target, Gtk.Dialog):
                self.feedback.play("back")
                target.response(Gtk.ResponseType.CANCEL)
        elif action == "play":
            if is_main and self.play_button.is_sensitive():
                self.feedback.play("confirm")
                self.play_button.clicked()
            elif not is_main:
                default = target.get_default_widget()
                if default is not None and default.is_sensitive():
                    self.confirm_gamepad_focus(target, default)
        elif action == "parent":
            if isinstance(target, Gtk.FileChooserDialog):
                chooser = next((widget for widget in controller_focus.widgets(target)
                                if isinstance(widget, Gtk.FileChooserWidget)), None)
                if chooser is not None:
                    chooser.emit("up-folder")
                    self.feedback.play("back")
        elif focus is None or not focus.is_sensitive() or not focus.get_mapped():
            if is_main:
                self.focus_gamepad_page()
            else:
                first = next(controller_focus.focusables(target), None)
                if first is not None:
                    controller_focus.grab(first)
        elif action == "confirm":
            self.confirm_gamepad_focus(target, focus)
        elif action in ("previous", "next"):
            direction = Gtk.DirectionType.TAB_BACKWARD if action == "previous" else Gtk.DirectionType.TAB_FORWARD
            target.child_focus(direction)
        elif action in ("up", "down", "left", "right"):
            self.move_gamepad_focus(target, focus, action)

    def confirm_gamepad_focus(self, target, focus):
        if isinstance(focus, Gtk.ComboBox):
            self.feedback.play("confirm")
            self.choose_combo_option(target, focus)
        elif isinstance(focus, Gtk.Button):
            self.feedback.play("confirm")
            focus.clicked()
        elif isinstance(focus, Gtk.TreeView):
            path, column = focus.get_cursor()
            column = column or focus.get_column(0)
            if path is not None and column is not None:
                self.feedback.play("confirm")
                focus.row_activated(path, column)
        elif isinstance(focus, Gtk.IconView):
            _, path, _ = focus.get_cursor()
            if path is not None:
                self.feedback.play("confirm")
                focus.item_activated(path)
        elif isinstance(focus, Gtk.ListBoxRow):
            self.feedback.play("confirm")
            focus.activate()
        elif focus.activate():
            self.feedback.play("confirm")

    def move_gamepad_focus(self, target, focus, action):
        if isinstance(focus, Gtk.ComboBox) and action in ("left", "right"):
            current = focus.get_active()
            index = current + (1 if action == "right" else -1)
            if 0 <= index < len(focus.get_model()):
                focus.set_active(index)
                self.feedback.play("focus")
        elif isinstance(focus, Gtk.Range) and action in ("left", "right"):
            previous = focus.get_value()
            step = focus.get_adjustment().get_step_increment() * (1 if action == "right" else -1)
            focus.set_value(previous + step)
            if focus.get_value() != previous:
                self.feedback.play("focus")
        elif isinstance(focus, (Gtk.TreeView, Gtk.IconView)) and action in ("up", "down"):
            index = int(isinstance(focus, Gtk.IconView))
            previous = focus.get_cursor()[index]
            key = Gdk.KEY_Up if action == "up" else Gdk.KEY_Down
            Gtk.bindings_activate(focus, key, Gdk.ModifierType(0))
            if str(focus.get_cursor()[index]) != str(previous):
                self.feedback.play("focus")
        else:
            self.move_gamepad_widget(target, focus, action)

    def move_gamepad_widget(self, target, focus, action):
        viewport = focus.get_ancestor(Gtk.Viewport)
        vertical = action in ("up", "down")
        if vertical and viewport is not None:
            # Traverse the scrollable content before considering the persistent
            # rows above and below it, even when the next widget is off-screen.
            if controller_focus.move(target, focus, action, scope=viewport):
                return
            adjustment = viewport.get_vadjustment()
            previous = adjustment.get_value()
            delta = max(42, adjustment.get_step_increment())
            adjustment.set_value(previous + (delta if action == "down" else -delta))
            if adjustment.get_value() != previous:
                return
        controller_focus.move(target, focus, action)

    def choose_combo_option(self, target, combo):
        model = combo.get_model()
        if not model or not len(model):
            return
        dialog = style(Gtk.Dialog(title=tr("Escolha uma opção"), transient_for=target, modal=True), "controller-dialog")
        dialog.add_button(tr("Cancelar"), Gtk.ResponseType.CANCEL)
        dialog.set_default_size(460, min(480, 94 + len(model) * 46))
        scroll = Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER, vscrollbar_policy=Gtk.PolicyType.AUTOMATIC)
        options = box(spacing=6, margin=16)
        buttons = []
        for index, row in enumerate(model):
            button = style(Gtk.Button(label=row[combo.get_entry_text_column()]), "secondary")
            if index == combo.get_active():
                button.get_style_context().add_class("active")
            button.connect("clicked", lambda _, response=index: dialog.response(response))
            options.pack_start(button, False, False, 0)
            buttons.append(button)
        scroll.add(options)
        dialog.get_content_area().pack_start(scroll, True, True, 0)
        self.connect_dialog_feedback(dialog)
        dialog.show_all()
        current = buttons[max(0, combo.get_active())]
        current.grab_focus()
        GLib.idle_add(controller_focus.reveal, current)
        response = dialog.run()
        dialog.destroy()
        if response >= 0:
            combo.set_active(response)
        controller_focus.grab(combo)

    def focus_gamepad_page(self):
        page = self.stack.get_visible_child_name()
        if page == "home":
            preferred = self.play_button if self.play_button.is_sensitive() else self.choose_button
            if preferred.is_sensitive():
                controller_focus.grab(preferred)
                return
        preferred = next(controller_focus.focusables(self.stack.get_visible_child()), self.nav[page])
        controller_focus.grab(preferred)

    def configure_file_chooser(self, dialog):
        self.connect_dialog_feedback(dialog)
        resolved = self.resolved_prompt_style()
        if resolved == "keyboard":
            hint = label(tr("Setas mover · Enter confirmar · Esc voltar"), "small", wrap=True)
            dialog.set_extra_widget(hint)
            hint.show()
            return
        xbox = resolved == "xbox"
        back, parent, tabs, start = ("B", "Y", "LB/RB", "Menu") if xbox else ("○", "△", "L1/R1", "Options")
        hint = label(tr("{tabs} alternar regiões · {parent} pasta acima · {back} voltar · {start} aceitar").format(
            parent=parent, back=back, tabs=tabs, start=start), "small", wrap=True)
        dialog.set_extra_widget(hint)
        hint.show()

    def connect_dialog_feedback(self, dialog):
        self.connect_feedback(dialog)
        dialog.connect("set-focus", self.focus_feedback)

    def home(self):
        page = box(spacing=14)
        library = self.home_library = box(spacing=14)
        hero_outer = style(Gtk.Overlay(), "hero")
        artwork = Artwork()
        artwork.set_size_request(-1, 284)
        for edge in ("top", "bottom", "start", "end"):
            getattr(artwork, "set_margin_" + edge)(1)
        hero_outer.add(artwork)
        hero = box(spacing=10, margin=28)
        hero.set_valign(Gtk.Align.END)
        hero.pack_start(label("Shadow of the\nColossus", "hero-title", wrap=True), False, False, 0)
        self.status = label(tr("Escolha sua ROM para começar."), "status", wrap=True)
        hero.pack_start(self.status, False, False, 0)
        self.play_button = style(FocusButton(label=tr("▶   Iniciar jogo")), "play")
        style(self.play_button, "action-card")
        self.play_button.connect("clicked", self.play)
        actions = box(Gtk.Orientation.HORIZONTAL, 12)
        actions.pack_start(self.play_button, False, False, 0)
        self.compile_button = style(FocusButton(label=tr("Compilar / atualizar")), "secondary")
        style(self.compile_button, "action-card")
        self.compile_button.set_tooltip_text(tr("Prepara a compilação em segundo plano. Inicie o jogo quando quiser."))
        self.compile_button.connect("clicked", self.compile_game)
        actions.pack_start(self.compile_button, False, False, 0)
        hero.pack_start(actions, False, False, 0)
        hero_outer.add_overlay(hero)
        library.pack_start(hero_outer, True, True, 0)
        panel_outer = style(Gtk.Box(orientation=Gtk.Orientation.VERTICAL),"panel")
        panel = box(spacing=9, margin=15)
        panel_outer.pack_start(panel, True, True, 0)
        self.drop = style(Gtk.EventBox(),"drop-zone")
        inner = box(Gtk.Orientation.HORIZONTAL, 16, 13)
        info = box(spacing=4)
        self.file_title = label(tr("Selecione sua ROM"), "file-title")
        self.file_title.set_ellipsize(Pango.EllipsizeMode.MIDDLE)
        self.file_title.set_max_width_chars(45)
        info.pack_start(self.file_title,False,False,0)
        self.file_subtitle = label(tr("ISO, RAR, ZIP ou 7Z · edição americana"), "muted", wrap=True)
        info.pack_start(self.file_subtitle,False,False,0)
        inner.pack_start(info,True,True,0)
        self.choose_button = style(FocusButton(label=tr("Escolher arquivo…")),"secondary")
        self.choose_button.connect("clicked",self.choose)
        inner.pack_end(self.choose_button,False,False,0); self.drop.add(inner)
        panel.pack_start(self.drop,False,False,0)
        self.progress = Gtk.ProgressBar(); panel.pack_start(self.progress,False,False,0)
        library.pack_start(panel_outer, False, False, 0)
        page.pack_start(library, False, False, 0)
        page.pack_start(self.updates_panel(), False, False, 0)
        self.drop.set_tooltip_text(tr("Dica: você também pode arrastar o arquivo para a área acima."))
        if SAVE_STATES_AVAILABLE:
            page.pack_start(label(tr("Durante o jogo: F5 salva rápido · F8 carrega · F1 gerencia estados."), "small", wrap=True),False,False,0)
        return page

    def controls(self):
        page=box(spacing=18)
        page.pack_start(label(tr("Controles"), "section-title"),False,False,0)
        page.pack_start(label(tr("Teclado e mouse ou controle USB/Bluetooth.\nF1 abre o menu, libera o cursor e oferece Voltar ao launcher. Esc pausa o jogo."),"muted",wrap=True),False,False,0)
        edit=style(Gtk.Button(label=tr("Personalizar teclado e mouse…")),"secondary")
        edit.connect("clicked", self.configure_desktop_input)
        page.pack_start(edit,False,False,0)
        self.keyboard_summary = label("", "muted", wrap=True)
        page.pack_start(self.keyboard_summary,False,False,0)
        self.refresh_keyboard_summary()
        if SAVE_STATES_AVAILABLE:
            page.pack_start(label(tr("F5 · Salvar estado rápido    /    F8 · Carregar estado rápido\nF1 · Gerenciar estados, configurações e sair    /    F11 · Tela cheia"), "small", wrap=True),False,False,0)
        page.pack_start(label(tr("Controle USB / Bluetooth"), "panel-title"),False,False,8)
        self.controller_detection = label("", "status", wrap=True)
        page.pack_start(self.controller_detection, False, False, 0)
        self.controller_status = label(tr("Perfis automáticos para Xbox, PlayStation e genéricos.\nSe os botões não responderem ou estiverem trocados, configure seu controle."), "muted", wrap=True)
        page.pack_start(self.controller_status, False, False, 0)
        self.controller_button = style(Gtk.Button(label=tr("Configurar meu controle…")), "secondary")
        self.controller_button.connect("clicked", self.configure_controller)
        page.pack_start(self.controller_button, False, False, 0)
        page.pack_start(label(tr("Dica: os sigilos dos colossos brilham com a espada equipada."), "small", wrap=True),False,False,0)
        page.pack_start(Gtk.Box(),True,True,0)
        return page

    def refresh_keyboard_summary(self):
        from controls import Controls, binding_label
        values = Controls(self.library).load()
        def keys(*names):
            return " / ".join(binding_label(values[name]) for name in names)
        rows = (
            (("move_up", "move_left", "move_down", "move_right"), tr("Movimentar")),
            (("jump",), tr("Pular")), (("attack",), tr("Atacar")), (("grab",), tr("Agarrar")),
            (("action",), tr("Ação / luz da espada")), (("weapon_left", "weapon_right"), tr("Trocar arma")),
            (("horse",), "Agro"), (("look", "camera_reset"), "L1 / L2"), (("crouch",), "R2"),
            (("start", "select"), "Start / Select"),
            (("confirm", "cancel"), tr("Confirmar / voltar nos menus")),
        )
        summary = [f"{keys(*names)}  ·  {action}" for names, action in rows]
        self.keyboard_summary.set_text("\n".join((
            summary[0], "    |    ".join(summary[1:4]), "    |    ".join(summary[4:7]),
            "    |    ".join(summary[7:10]), summary[10],
            tr("Mouse  ·  Câmera") if values["mouse_camera"] else tr("Câmera pelo mouse desativada"),
        )))

    def configure_desktop_input(self, *_):
        if self.busy or self.process:
            self.error(tr("Feche o jogo"), tr("O perfil será aplicado na próxima abertura do jogo."))
            return
        from controls_dialog import ControlsDialog
        dialog = ControlsDialog(self.window, self.library, connect_feedback=self.connect_dialog_feedback)
        dialog.connect("destroy", lambda *_: self.refresh_keyboard_summary())
        dialog.show_all()

    def configure_controller(self, *_):
        if self.busy or self.process or self.controller_process:
            self.controller_status.set_text(tr("Feche o jogo antes de configurar o controle."))
            return
        from pipeline import compiler_tool
        tool = compiler_tool(self.library.program, "sotc-input-tool")
        if not tool.is_file():
            self.error(tr("Atualize o pacote"), tr("A ferramenta de configuração não está neste pacote."))
            return
        profile = self.library.directory / "controllers/gamecontrollerdb.txt"
        profile.parent.mkdir(parents=True, exist_ok=True)
        self.gamepad.suspend()
        try:
            with (self.library.directory / "last-controller.log").open("w") as log:
                self.controller_process = subprocess.Popen([str(tool), "--output", str(profile)],
                    env=self.library.game_environment(), stdout=log, stderr=subprocess.STDOUT)
        except OSError as exc:
            self.error(tr("Não foi possível configurar o controle"), str(exc))
            return
        self.controller_button.set_sensitive(False)
        self.refresh_prepare_controls()
        self.controller_status.set_text(tr("Siga as instruções na janela de configuração."))
        GLib.timeout_add(250, self.watch_controller)

    def watch_controller(self):
        code = self.controller_process.poll()
        if code is None:
            return True
        self.controller_process = None
        self.refresh_prepare_controls()
        self.controller_button.set_sensitive(True)
        self.controller_status.set_text(tr("Perfil salvo. Ele será usado na próxima abertura do jogo.") if code == 0
            else tr("Configuração não concluída. Os perfis anteriores foram preservados."))
        return False

    def graphics_page(self):
        page = box(spacing=20)
        page.pack_start(label(tr("Gráficos"), "section-title"),False,False,0)
        page.pack_start(label(tr("Escolha como jogar. As mudanças serão usadas na próxima abertura do jogo."),"muted",wrap=True),False,False,0)
        panel = box(spacing=16,classes=("panel","graphics-panel"))
        panel.pack_start(label(tr("Tela e proporção"), "panel-title"),False,False,0)
        fullscreen, mode = self.graphics.display()
        self.fullscreen = style(Gtk.ComboBoxText(),"graphics-select")
        for text in (tr("Janela redimensionável"), tr("Tela cheia · resolução do monitor")):
            self.fullscreen.append_text(text)
        self.fullscreen.set_active(int(fullscreen))
        panel.pack_start(self.fullscreen,False,False,0)
        self.aspect = style(Gtk.ComboBoxText(),"graphics-select")
        for text in (tr("Original 4:3 · preservar proporção"), tr("Esticar · preencher a tela"), tr("Pixels · escala inteira"),
                     tr("Widescreen 16:9 · câmera ampliada")):
            self.aspect.append_text(text)
        self.aspect.set_active(mode)
        panel.pack_start(self.aspect,False,False,0)
        window, fsr = self.graphics.upscaling()
        panel.pack_start(label(tr("Resolução da janela"), "muted"), False, False, 0)
        self.window_resolution = style(Gtk.ComboBoxText(), "graphics-select")
        self.window_resolution.append_text(tr("Automática"))
        for width, height in WINDOW_RESOLUTIONS:
            self.window_resolution.append_text(f"{width} × {height}")
        self.window_resolution.set_active(window)
        panel.pack_start(self.window_resolution, False, False, 0)
        self.fsr = Gtk.CheckButton(label=tr("Ativar upscaling AMD FSR 1"))
        self.fsr.set_active(fsr)
        panel.pack_start(self.fsr, False, False, 0)
        self.show_fps = Gtk.CheckButton(label=tr("Mostrar FPS"))
        self.show_fps.set_active(self.graphics.fps_enabled())
        self.show_fps.connect("toggled", self.save_fps)
        panel.pack_start(self.show_fps, False, False, 0)
        panel.pack_start(label(tr("Perfil gráfico"), "muted"), False, False, 0)
        self.quality_profile = style(Gtk.ComboBoxText(), "graphics-select")
        for profile_id, name in GRAPHICS_PROFILES + LEGACY_PROFILES:
            self.quality_profile.append(profile_id, tr(name))
        self.quality_profile.append("4", tr("Personalizado (F1)"))
        self.quality_profile.set_active_id(self.graphics.quality_profile())
        self.quality_profile.connect("changed", self.save_quality_profile)
        panel.pack_start(self.quality_profile, False, False, 0)
        panel.pack_start(label(tr("Equilibrado suaviza bordas. Aprimorado e Ultra aumentam a resolução e o detalhe da água. Ajuste cada opção no jogo em F1 → Qualidade."),
                               "muted", wrap=True), False, False, 0)
        self.display_panel = panel
        self.window_resolution.connect("changed", self.save_graphics)
        self.fsr.connect("toggled", self.save_graphics)
        self.fullscreen.connect("changed", self.save_graphics)
        self.aspect.connect("changed", self.save_graphics)
        panel.pack_start(label(tr("F11 alterna entre janela e tela cheia. Widescreen amplia a visão lateral e preserva o quadro 16:9 em outras proporções."),"muted",wrap=True),False,False,0)
        page.pack_start(panel,False,False,0)
        panel = box(spacing=15,classes=("panel","graphics-panel"))
        panel.pack_start(label(tr("Shaders e filtros"), "panel-title"),False,False,0)
        panel.pack_start(label(tr("Importe um fragment shader GLSL 330 (.frag, .fs ou .glsl).\nO filtro atua sobre a imagem final."),"muted",wrap=True),False,False,0)
        self.shader_status = label("", "status",wrap=True)
        panel.pack_start(self.shader_status,False,False,0)
        actions = box(Gtk.Orientation.HORIZONTAL,10)
        self.shader_import = style(Gtk.Button(label=tr("Importar shader…")),"secondary")
        self.shader_import.connect("clicked",self.choose_shader)
        actions.pack_start(self.shader_import,False,False,0)
        self.shader_hd = style(Gtk.Button(label=tr("Usar Nitidez HD")),"secondary")
        self.shader_hd.connect("clicked",lambda *_: self.import_shader(assets_directory()/"shaders/nitidez-hd.frag"))
        actions.pack_start(self.shader_hd,False,False,0)
        self.shader_clear = style(Gtk.Button(label=tr("Desativar")),"secondary")
        self.shader_clear.connect("clicked",self.clear_shader)
        actions.pack_start(self.shader_clear,False,False,0)
        panel.pack_start(actions,False,False,0)
        template = style(Gtk.Button(label=tr("Abrir modelo de shader ↗")),"link")
        template.set_halign(Gtk.Align.START); template.connect("clicked",self.open_shader_template)
        panel.pack_start(template,False,False,0)
        self.video_panel = panel
        self.refresh_shader()
        page.pack_start(panel,False,False,0)
        page.pack_start(label(tr("Shaders são copiados para a biblioteca e validados no seu driver.\nDepois da importação, você pode mover o arquivo original."),"muted",wrap=True),False,False,0)
        return page

    def save_graphics(self,*_):
        if self.busy or self.process or getattr(self, "syncing_display", False):
            return
        try:
            self.graphics.save_display(self.fullscreen.get_active() == 1, self.aspect.get_active(),
                                       window=self.window_resolution.get_active(), fsr=self.fsr.get_active())
        except OSError as exc:
            self.error(tr("Não foi possível salvar a imagem"),str(exc))

    def save_fps(self, checkbox):
        if self.busy or self.process or getattr(self, "syncing_display", False):
            return
        try:
            self.graphics.save_fps(checkbox.get_active())
        except OSError as exc:
            self.refresh_graphics()
            self.error(tr("Não foi possível salvar a imagem"), str(exc))

    def refresh_graphics(self):
        fullscreen, mode = self.graphics.display()
        window, fsr = self.graphics.upscaling()
        self.syncing_display = True
        try:
            self.fullscreen.set_active(int(fullscreen))
            self.aspect.set_active(mode)
            self.window_resolution.set_active(window)
            self.fsr.set_active(fsr)
            self.show_fps.set_active(self.graphics.fps_enabled())
            self.quality_profile.set_active_id(self.graphics.quality_profile())
        finally:
            self.syncing_display = False

    def save_quality_profile(self, combo):
        if self.busy or self.process or getattr(self, "syncing_display", False):
            return
        profile = combo.get_active_id()
        if profile == "4":
            active = self.graphics.quality_profile()
            if active != profile:
                self.syncing_display = True
                combo.set_active_id(active)
                self.syncing_display = False
            return
        try:
            self.graphics.save_quality_profile(profile)
        except OSError as exc:
            self.error(tr("Não foi possível salvar a imagem"), str(exc))

    def refresh_shader(self):
        shader = self.graphics.selected()
        self.shader_status.set_text("●  " + (tr("Ativo: {name}").format(name=shader.stem.rsplit("-",1)[0]) if shader else tr("Imagem original · sem shader")))
        self.shader_clear.set_sensitive(bool(shader))

    def clear_shader(self,*_):
        try:
            self.graphics.clear(); self.refresh_shader()
        except OSError as exc:
            self.error(tr("Não foi possível desativar o shader"),str(exc))

    def textures_page(self):
        page = box(spacing=18)
        page.pack_start(label(tr("Mods de textura"), "section-title"), False, False, 0)
        panel = self.texture_panel = box(spacing=14, classes=("panel", "graphics-panel"))
        panel.pack_start(label(tr("Pacote de texturas"), "panel-title"), False, False, 0)
        panel.pack_start(label(tr("Selecione a pasta que contém os PNGs nomeados pelo código do port. As mudanças valem na próxima abertura do jogo."),
                               "muted", wrap=True), False, False, 0)
        self.texture_status = label("", "status", wrap=True)
        panel.pack_start(self.texture_status, False, False, 0)
        actions = box(Gtk.Orientation.HORIZONTAL, 10)
        choose = style(FocusButton(label=tr("Selecionar pasta…")), "secondary")
        choose.connect("clicked", self.choose_texture_pack)
        actions.pack_start(choose, False, False, 0)
        self.texture_clear = style(FocusButton(label=tr("Desativar pacote")), "secondary")
        self.texture_clear.connect("clicked", self.clear_texture_pack)
        actions.pack_start(self.texture_clear, False, False, 0)
        panel.pack_start(actions, False, False, 0)
        panel.pack_start(Gtk.Separator(), False, False, 0)
        panel.pack_start(label(tr("Criar um mod"), "panel-title"), False, False, 0)
        panel.pack_start(label(tr("Exporte os PNGs originais, edite as imagens e mantenha os nomes e a proporção. Cada sessão exporta até 512 MiB."),
                               "muted", wrap=True), False, False, 0)
        self.texture_export = Gtk.CheckButton(label=tr("Exportar texturas durante o jogo"))
        self.texture_export.set_active(self.texture_mods.preferences()[1])
        self.texture_export.connect("toggled", self.toggle_texture_export)
        panel.pack_start(self.texture_export, False, False, 0)
        folder = style(FocusButton(label=tr("Abrir pasta de exportação ↗")), "link")
        folder.set_halign(Gtk.Align.START)
        folder.connect("clicked", self.open_texture_exports)
        panel.pack_start(folder, False, False, 0)
        page.pack_start(panel, False, False, 0)
        self.refresh_texture_pack()
        return page

    def choose_texture_pack(self, *_):
        dialog = Gtk.FileChooserDialog(title=tr("Selecionar pasta de texturas"), parent=self.window,
                                       action=Gtk.FileChooserAction.SELECT_FOLDER)
        dialog.add_buttons(tr("Cancelar"), Gtk.ResponseType.CANCEL, tr("Ativar pacote"), Gtk.ResponseType.OK)
        dialog.set_default_response(Gtk.ResponseType.OK)
        self.configure_file_chooser(dialog)
        folder = dialog.get_filename() if dialog.run() == Gtk.ResponseType.OK else None
        dialog.destroy()
        if folder:
            try:
                self.texture_mods.set_pack(folder)
                self.refresh_texture_pack()
            except (OSError, ValueError, RuntimeError) as exc:
                self.error(tr("O pacote de texturas não foi ativado"), str(exc))

    def refresh_texture_pack(self):
        folder, _ = self.texture_mods.preferences()
        if folder:
            text = tr("Ativo: {name}") if folder.is_dir() else tr("Pasta indisponível: {name}")
            self.texture_status.set_text(text.format(name=folder.name))
            self.texture_status.set_tooltip_text(str(folder))
        else:
            self.texture_status.set_text(tr("Texturas originais"))
            self.texture_status.set_tooltip_text(None)
        self.texture_clear.set_sensitive(folder is not None)

    def clear_texture_pack(self, *_):
        try:
            self.texture_mods.set_pack(None)
            self.refresh_texture_pack()
        except OSError as exc:
            self.error(tr("Não foi possível salvar as configurações"), str(exc))

    def toggle_texture_export(self, button):
        try:
            self.texture_mods.set_export(button.get_active())
        except OSError as exc:
            button.handler_block_by_func(self.toggle_texture_export)
            button.set_active(self.texture_mods.preferences()[1])
            button.handler_unblock_by_func(self.toggle_texture_export)
            self.error(tr("Não foi possível salvar as configurações"), str(exc))

    def open_texture_exports(self, *_):
        try:
            self.texture_mods.exports.mkdir(parents=True, exist_ok=True)
            self.open_desktop_path(self.texture_mods.exports)
        except OSError as exc:
            self.error(tr("Não foi possível abrir a pasta"), str(exc))

    def settings_page(self):
        page = box(spacing=18)
        page.pack_start(label(tr("Configurações do launcher"), "section-title"), False, False, 0)
        preferences_panel = box(spacing=12, classes=("panel", "graphics-panel"))
        preferences_panel.pack_start(label(tr("Idioma do launcher"), "panel-title"), False, False, 0)
        self.language_choice = style(Gtk.ComboBoxText(), "graphics-select")
        for code, name in LANGUAGES.items():
            self.language_choice.append(code, name)
        self.language_choice.set_active_id(self.preferences["language"])
        preferences_panel.pack_start(self.language_choice, False, False, 0)
        preferences_panel.pack_start(label(tr("Ícones dos botões no jogo"), "panel-title"), False, False, 0)
        self.prompts_choice = style(Gtk.ComboBoxText(), "graphics-select")
        for mode, text in (("auto", tr("Automático · último dispositivo usado")),
                           ("playstation", "PlayStation  ✕ ○ □ △"),
                           ("xbox", "Xbox  A B X Y"),
                           ("keyboard", tr("Teclado e mouse"))):
            self.prompts_choice.append(mode, text)
        self.prompts_choice.set_active_id(self.preferences["button_prompts"])
        preferences_panel.pack_start(self.prompts_choice, False, False, 0)
        preferences_panel.pack_start(label(tr("No modo Automático, os ícones acompanham o último dispositivo usado durante o jogo. Os atalhos de teclado respeitam seu perfil de controles. A escolha do modo vale na próxima abertura do jogo."), "muted", wrap=True), False, False, 0)
        preferences_panel.pack_start(label(tr("Assistências (cheats)"), "panel-title"), False, False, 0)
        self.health_choice = style(Gtk.CheckButton(label=tr("Vida infinita")), "sound-option")
        self.health_choice.set_active(self.preferences["infinite_health"])
        preferences_panel.pack_start(self.health_choice, False, False, 0)
        self.grip_choice = style(Gtk.CheckButton(label=tr("Resistência infinita")), "sound-option")
        self.grip_choice.set_active(self.preferences["infinite_grip"])
        preferences_panel.pack_start(self.grip_choice, False, False, 0)
        preferences_panel.pack_start(label(tr("Desligadas por padrão. As assistências são aplicadas na próxima abertura do jogo."), "muted", wrap=True), False, False, 0)
        self.sound_choice = style(Gtk.CheckButton(label=tr("Sons da interface")), "sound-option")
        self.sound_choice.set_active(self.preferences["ui_sounds"])
        preferences_panel.pack_start(self.sound_choice, False, False, 0)
        apply = style(Gtk.Button(label=tr("Aplicar")), "secondary")
        apply.set_halign(Gtk.Align.START)
        apply.connect("clicked", self.apply_preferences)
        preferences_panel.pack_start(apply, False, False, 0)
        self.preferences_status = label("", "status", wrap=True)
        preferences_panel.pack_start(self.preferences_status, False, False, 0)
        page.pack_start(preferences_panel, False, False, 0)
        return page

    def updates_panel(self):
        panel = box(spacing=8, classes=("panel", "update-panel"))
        row = box(Gtk.Orientation.HORIZONTAL, spacing=12)
        info = box(spacing=4)
        info.pack_start(label(tr("Atualizações"), "panel-title"), False, False, 0)
        self.update_status_label = label(tr("Versão instalada {version}").format(version=VERSION),
                                         "muted", wrap=True)
        self.update_status_label.set_max_width_chars(42)
        info.pack_start(self.update_status_label, False, False, 0)
        row.pack_start(info, True, True, 0)
        self.update_progress = Gtk.ProgressBar()
        self.update_progress.set_no_show_all(True)
        self.check_button = style(Gtk.Button(label=tr("Verificar agora")), "secondary")
        self.check_button.set_valign(Gtk.Align.CENTER)
        self.check_button.connect("clicked", self.check_updates)
        self.update_button = style(Gtk.Button(label=tr("Atualizar")), "play")
        self.update_button.set_valign(Gtk.Align.CENTER)
        self.update_button.set_no_show_all(True)
        self.update_button.connect("clicked", self.install_update)
        row.pack_end(self.update_button, False, False, 0)
        row.pack_end(self.check_button, False, False, 0)
        panel.pack_start(row, False, False, 0)
        panel.pack_start(self.update_progress, False, False, 0)
        return panel

    def apply_preferences(self, *_):
        if self.busy or self.process or self.controller_process:
            self.error(tr("Feche o jogo"), tr("Feche o jogo ou termine a preparação antes de aplicar."))
            return
        values = {**self.preferences, "language": self.language_choice.get_active_id(),
                  "button_prompts": self.prompts_choice.get_active_id(),
                  "infinite_health": self.health_choice.get_active(),
                  "infinite_grip": self.grip_choice.get_active(),
                  "ui_sounds": self.sound_choice.get_active()}
        try:
            atomic_json(self.library.directory / PREFERENCES, values)
        except OSError as exc:
            self.error(tr("Não foi possível salvar as configurações"), str(exc))
            return
        self.restart_language = values["language"] != self.preferences["language"]
        self.preferences = values
        self.feedback.set_enabled(values["ui_sounds"])
        if self.restart_language:
            Gtk.main_quit()
        else:
            self.preferences_status.set_text(tr("Configurações salvas."))

    def periodic_check(self):
        self.check_updates()
        return True

    def check_updates(self, *_):
        if self.checking or self.updating:
            return False
        self.checking = True
        self.check_button.set_sensitive(False)
        self.update_status_label.set_tooltip_text(None)
        self.update_status_label.get_style_context().remove_class("ready")
        self.update_status_label.set_text(tr("Consultando o GitHub…"))

        def worker():
            try:
                result = check_for_update(VERSION)
            except Exception as exc:
                GLib.idle_add(self.update_checked, None, exc)
            else:
                GLib.idle_add(self.update_checked, result, None)
        threading.Thread(target=worker, daemon=True).start()
        return False

    def update_checked(self, result, error):
        if self.disposed:
            return False
        self.checking = False
        self.check_button.set_sensitive(True)
        if error:
            if isinstance(error, RateLimitedError):
                message = tr("O GitHub limitou as consultas. Tente mais tarde.")
            else:
                message = tr("Não foi possível consultar o GitHub. Tente novamente.")
            self.update_status_label.set_text(message)
            self.update_status_label.set_tooltip_text(str(error))
            return False
        self.update = result
        if result:
            self.update_status_label.set_text(tr("Versão {version} disponível.").format(version=result.version))
            self.update_button.set_label(tr("Atualizar para {version}").format(version=result.version))
            self.update_button.show()
        else:
            self.update_status_label.set_text(tr("O launcher já está atualizado."))
            self.update_status_label.get_style_context().add_class("ready")
            self.update_button.hide()
        return False

    def install_update(self, *_):
        if self.update is None or self.updating or self.checking:
            return
        if self.busy or self.process or self.controller_process:
            self.error(tr("Feche o jogo"), tr("Feche o jogo ou termine a preparação antes de atualizar."))
            return
        update = self.update
        self.busy = self.updating = True
        self.refresh_prepare_controls()
        self.navigate("home")
        self.update_status_label.set_tooltip_text(None)
        for button in (self.update_button, self.check_button):
            button.set_sensitive(False)
        self.update_progress.set_fraction(0)
        self.update_progress.show()
        self.update_status_label.set_text(tr("Baixando a versão {version}…").format(version=update.version))

        def worker():
            try:
                stage = stage_update(self.library.program, self.library.directory, VERSION, update,
                                     lambda fraction: GLib.idle_add(self.update_downloaded, fraction))
                restart_with_update(stage, self.library.program, self.library.directory,
                                    VERSION, update.system, assets_directory())
            except Exception as exc:
                GLib.idle_add(self.update_finished, str(exc))
            else:
                GLib.idle_add(self.update_finished, None)
        threading.Thread(target=worker, daemon=False).start()

    def update_downloaded(self, fraction):
        if fraction is None:
            self.update_progress.set_fraction(1)
            self.update_status_label.set_text(tr("Verificando e extraindo o pacote…"))
        else:
            self.update_progress.set_fraction(fraction)
        return False

    def update_finished(self, error):
        self.busy = self.updating = False
        self.refresh_prepare_controls()
        for button in (self.update_button, self.check_button):
            button.set_sensitive(True)
        self.update_progress.hide()
        if error is None or self.closing:
            Gtk.main_quit()
        else:
            self.update_status_label.set_text(tr("A atualização falhou"))
            self.error(tr("A atualização falhou"), error)
        return False

    def choose_shader(self,*_):
        if self.busy or self.process: return
        dialog=Gtk.FileChooserDialog(title=tr("Importar shader GLSL 330"),parent=self.window,action=Gtk.FileChooserAction.OPEN)
        dialog.add_buttons(tr("Cancelar"),Gtk.ResponseType.CANCEL,tr("Validar e importar"),Gtk.ResponseType.OK)
        dialog.set_default_response(Gtk.ResponseType.OK)
        selected=Gtk.FileFilter(); selected.set_name("Fragment shaders GLSL 330")
        for extension in ("frag","fs","glsl"):
            selected.add_pattern("*."+extension); selected.add_pattern("*."+extension.upper())
        dialog.add_filter(selected)
        self.configure_file_chooser(dialog)
        path = dialog.get_filename() if dialog.run()==Gtk.ResponseType.OK else None
        dialog.destroy()
        if path: self.import_shader(path)

    def import_shader(self,path):
        if self.busy or self.process: return
        self.busy=True; self.video_panel.set_sensitive(False)
        self.refresh_prepare_controls()
        self.play_button.set_sensitive(False); self.choose_button.set_sensitive(False)
        self.shader_status.set_text(tr("Validando o shader no seu driver…"))
        def worker():
            error=None
            try: self.graphics.import_shader(path)
            except Exception as exc: error=str(exc)
            GLib.idle_add(self.shader_imported,error)
        threading.Thread(target=worker,daemon=False).start()

    def shader_imported(self,error):
        self.busy=False; self.video_panel.set_sensitive(True)
        self.refresh_prepare_controls()
        self.play_button.set_sensitive(bool(self.library.disc)); self.choose_button.set_sensitive(True)
        if self.closing: Gtk.main_quit(); return False
        if error:
            self.refresh_shader()
            self.error(tr("Esse shader não foi ativado"),error)
        else: self.refresh_shader()
        return False

    def open_shader_template(self,*_):
        try:
            import shutil
            path=self.library.directory/"shaders/modelo.frag"; path.parent.mkdir(parents=True,exist_ok=True)
            if not path.exists(): shutil.copy2(assets_directory()/"shaders/modelo.frag",path)
            self.open_desktop_path(path)
        except (OSError,GLib.Error) as exc:
            self.error(tr("Não foi possível abrir o modelo"),str(exc))

    def about(self):
        page=box(spacing=23)
        page.pack_start(label(tr("Sobre o port"),"section-title"),False,False,0)
        page.pack_start(label(tr("Escolha seu disco uma vez. Nas próximas sessões, é só clicar em jogar."),"hero-subtitle",wrap=True),False,False,0)
        page.pack_start(label(tr("SUA ROM"), "eyebrow"),False,False,0)
        page.pack_start(label(tr("Compatível com a edição americana SCUS-97472. ISO e RAR/ZIP/7Z são preparados na pasta do aplicativo. A primeira preparação recompila seu disco localmente; depois o cache é reutilizado. Sua ROM original não é alterada."),"muted",wrap=True),False,False,0)
        page.pack_start(label(tr("CRÉDITOS"), "eyebrow"),False,False,0)
        page.pack_start(label(tr("Interface: GenissonEmilio, baseada no launcher de ForbiddenMemoriesPC.\nBase nativa: HenryKun55 / Celosia (GPL-3.0).\nRecompilador/runtime: ran-j / PS2Recomp e colaboradores.\nReferência PS2: Tenkaichi3Decomp; launcher: ForbiddenMemoriesPC / CrashBandicootPC.\nShadow of the Colossus pertence a seus respectivos titulares."),"muted",wrap=True),False,False,0)
        install=style(Gtk.Button(label=tr("＋  Adicionar ao menu de aplicativos")),"secondary")
        install.set_halign(Gtk.Align.START); install.connect("clicked",self.install_menu)
        page.pack_start(install,False,False,0)
        self.install_status=label(tr("Abra o launcher pelo menu de aplicativos depois de criar o atalho."),"muted",wrap=True)
        page.pack_start(self.install_status,False,False,0)
        page.pack_start(Gtk.Box(),True,True,0)
        return page

    def navigate(self,name):
        changed = name != self.stack.get_visible_child_name()
        if name == "graphics":
            self.refresh_graphics()
        self.stack.set_visible_child_name(name)
        if changed:
            adjustment = self.page_scroller.get_vadjustment()
            adjustment.set_value(adjustment.get_lower())
        for key,button in self.nav.items():
            context=button.get_style_context()
            (context.add_class if key==name else context.remove_class)("active")

    def choose(self,*_):
        dialog=Gtk.FileChooserDialog(title=tr("Escolha sua ROM de Shadow of the Colossus"),parent=self.window,
                                     action=Gtk.FileChooserAction.OPEN)
        dialog.add_buttons(tr("Cancelar"),Gtk.ResponseType.CANCEL,tr("Selecionar ROM"),Gtk.ResponseType.OK)
        dialog.set_default_response(Gtk.ResponseType.OK)
        selected=Gtk.FileFilter(); selected.set_name("Shadow of the Colossus · ISO / RAR / ZIP / 7Z")
        for extension in ("iso","rar","zip","7z"):
            selected.add_pattern("*."+extension); selected.add_pattern("*."+extension.upper())
        dialog.add_filter(selected)
        self.configure_file_chooser(dialog)
        if dialog.run()==Gtk.ResponseType.OK:
            path=dialog.get_filename(); dialog.destroy(); self.prepare(path)
        else:
            dialog.destroy()

    def dropped(self,widget,context,x,y,selection,info,time):
        uris=selection.get_uris()
        accepted=False
        if not self.busy and not self.process and len(uris)==1:
            try:
                path,host=GLib.filename_from_uri(uris[0])
                if host and host!="localhost": raise ValueError(tr("Escolha um arquivo deste computador."))
                self.prepare(path); accepted=True
            except (GLib.Error,ValueError) as exc:
                self.error(tr("Confira o arquivo escolhido"),str(exc))
        Gtk.drag_finish(context,accepted,False,time)

    def update_status(self,text,error=False):
        self.status.set_text(text)
        context=self.status.get_style_context()
        context.remove_class("ready"); context.remove_class("error")
        if error: context.add_class("error")
        return False

    def set_ready(self):
        self.file_title.set_text(self.library.state.get("name","Shadow of the Colossus"))
        self.file_subtitle.set_text(tr("SCUS-97472  ·  Edição americana validada"))
        self.status.set_text(tr("●  Pronto para jogar"))
        self.status.get_style_context().add_class("ready")
        self.play_button.set_sensitive(True)
        self.choose_button.set_label(tr("Trocar ROM…"))
        self.refresh_prepare_controls()

    def refresh_prepare_controls(self):
        idle = not (self.busy or self.process or self.controller_process)
        self.compile_button.set_sensitive(bool(self.library.disc) and idle)
        self.texture_panel.set_sensitive(idle)

    def compile_game(self, *_):
        if self.library.disc:
            return self.prepare()
        return False

    def pulse(self):
        if not self.busy: return False
        self.progress.pulse(); return True

    def prepare(self,path=None):
        if self.busy or self.process or self.controller_process: return False
        self.prepare_only = path is None
        self.busy=True; self.failed=False
        self.refresh_prepare_controls()
        self.choose_button.set_sensitive(False); self.play_button.set_sensitive(False)
        self.status.set_tooltip_text(None)
        self.progress.show()
        self.update_status(tr("Compilando em segundo plano…") if self.prepare_only else tr("Preparando seu jogo…"))
        GLib.timeout_add(80,self.pulse)
        def worker():
            error=None
            try:
                progress = lambda text: GLib.idle_add(self.update_status,text)
                if self.prepare_only:
                    self.library.prepare_game(progress)
                else:
                    self.library.prepare(path,progress)
            except Exception as exc:
                error=exc
            GLib.idle_add(self.prepared,error)
        threading.Thread(target=worker,daemon=False).start()
        return False

    def prepared(self,error):
        self.busy=False; self.progress.hide(); self.choose_button.set_sensitive(True)
        self.refresh_prepare_controls()
        if self.closing: Gtk.main_quit(); return False
        if error:
            self.failed=True
            if self.prepare_only:
                self.update_status(tr("A compilação falhou. Tente novamente. Detalhes em last-prepare.log."), True)
                self.status.set_tooltip_text(str(error))
            elif isinstance(error, CompilationError):
                title = tr("Falha na compilação")
                self.update_status(title, True)
                self.error(title, str(error))
            else:
                self.update_status(tr("Não foi possível preparar essa ROM."),True)
                self.error(tr("Confira o arquivo escolhido"),str(error))
            self.play_button.set_sensitive(bool(self.library.disc))
        else:
            self.set_ready()
            if self.prepare_only:
                self.update_status(tr("Compilação pronta. Inicie o jogo quando quiser."))
                self.status.get_style_context().add_class("ready")
            elif self.args.test_game: GLib.idle_add(self.play)
            elif self.args.screenshot: GLib.timeout_add(500,self.capture_and_exit)
        return False

    def play(self,*_):
        if self.busy or self.process or self.controller_process: return False
        self.gamepad.suspend()
        self.busy=True; self.play_button.set_sensitive(False); self.choose_button.set_sensitive(False)
        self.refresh_prepare_controls()
        self.update_status(tr("Abrindo o jogo…"))
        self.progress.show(); GLib.timeout_add(80,self.pulse)
        self.display_panel.set_sensitive(False); self.video_panel.set_sensitive(False)
        def worker():
            try:
                process=self.library.launch(self.args.test_game,lambda text: GLib.idle_add(self.update_status,text))
                GLib.idle_add(self.game_started,process,None)
            except Exception as exc:
                GLib.idle_add(self.game_started,None,str(exc))
        threading.Thread(target=worker,daemon=False).start()
        return False

    def game_started(self,process,error):
        self.busy=False; self.progress.hide()
        if self.closing:
            Gtk.main_quit(); return False
        if error:
            self.failed=True; self.choose_button.set_sensitive(True); self.play_button.set_sensitive(True)
            self.display_panel.set_sensitive(True); self.video_panel.set_sensitive(True)
            self.update_status(tr("Não foi possível abrir o jogo."),True); self.error(tr("O jogo não abriu"),error)
        else:
            self.process=process; self.play_button.set_label(tr("Em jogo…"))
            self.feedback.close()
            self.update_status(tr("●  Jogo em execução"))
            GLib.timeout_add(300,self.watch_game)
        self.refresh_prepare_controls()
        return False

    def watch_game(self):
        code=self.process.poll()
        if code is None: return True
        self.process=None; self.choose_button.set_sensitive(True)
        self.display_panel.set_sensitive(True); self.video_panel.set_sensitive(True)
        self.play_button.set_label(tr("▶   Iniciar jogo")); self.set_ready()
        self.refresh_graphics()
        if code:
            self.failed=True
            self.update_status(tr("O jogo encerrou com um erro."),True)
            self.error(tr("O jogo foi encerrado"), tr("Código de saída {code}.\n\nDetalhes em {path}").format(code=code, path=self.library.directory / "last-game.log"))
        elif self.args.test_game:
            directory = self.library.directory / "game-test"
            captures = sorted(directory.glob("frame_*.png"))
            log = (self.library.directory / "last-game.log").read_text(errors="replace")
            result = {"scope":"boot and native renderer; campaign not covered",
                      "native_opengl":"[native] OpenGL renderer up" in log,
                      "gamecore":"GAMECORE entered" in log, "captures":len(captures)}
            result["passed"] = result["native_opengl"] and result["gamecore"] and bool(captures)
            (directory / "report.json").write_text(json.dumps(result,indent=2)+"\n")
            self.failed |= not result["passed"]
            GLib.timeout_add(500,self.capture_and_exit)
        return False

    def error(self,title,text):
        if self.args.test_game or self.args.screenshot:
            print(title+": "+text,file=sys.stderr)
            if self.args.rom or self.args.test_game:
                GLib.timeout_add(100,self.capture_and_exit)
            return
        self.feedback.play("error")
        dialog=Gtk.MessageDialog(transient_for=self.window,flags=0,message_type=Gtk.MessageType.ERROR,
                                 buttons=Gtk.ButtonsType.NONE,text=title)
        dialog.add_button(tr("Fechar"), Gtk.ResponseType.CLOSE)
        dialog.set_default_response(Gtk.ResponseType.CLOSE)
        self.connect_dialog_feedback(dialog)
        dialog.format_secondary_text(text); dialog.run(); dialog.destroy()

    def open_saves(self,*_):
        from saves import SaveCards
        cards = SaveCards(self.library)
        dialog = Gtk.Dialog(title=tr("Meus saves"), transient_for=self.window, modal=True)
        dialog.set_default_size(620, 260)
        dialog.add_button(tr("Fechar"), Gtk.ResponseType.CLOSE)
        dialog.set_default_response(Gtk.ResponseType.CLOSE)
        panel = box(spacing=16, margin=24)
        panel.pack_start(label(tr("Leve sua aventura para outro PC"), "section-title"), False, False, 0)
        panel.pack_start(label(tr("Importe seu cartão ou exporte uma cópia para usar em outro computador.\n"
                               "Depois, escolha Carregar jogo no menu do próprio jogo."), "muted", wrap=True), False, False, 0)
        location = label(str(cards.directory), "muted", wrap=True)
        location.set_selectable(True)
        panel.pack_start(location, False, False, 0)
        if SAVE_STATES_AVAILABLE:
            panel.pack_start(label(tr("Estados rápidos: F5 salva e F8 carrega durante o jogo.\n"
                                   "No menu F1 → Estados, escolha o slot rápido ou um dos cinco slots manuais.\n"
                                   "Estados exigem a mesma versão do runtime e do disco. Continue salvando também nos altares."), "muted", wrap=True), False, False, 0)
            states_folder = Gtk.Button(label=tr("Abrir pasta dos estados ↗"))
            def open_states_folder(*_):
                try:
                    destination = self.library.player_directory / "savestates"
                    destination.mkdir(parents=True, exist_ok=True)
                    self.open_desktop_path(destination)
                except OSError as exc:
                    self.error(tr("Não foi possível abrir a pasta"), str(exc))
            states_folder.connect("clicked", open_states_folder)
            panel.pack_start(states_folder, False, False, 0)
        status = label(tr("Cartão 1 disponível.") if cards.card.is_file() else tr("Nenhum cartão salvo neste computador."), wrap=True)
        panel.pack_start(status, False, False, 0)
        row = box(Gtk.Orientation.HORIZONTAL, spacing=12)
        folder = Gtk.Button(label=tr("Abrir pasta ↗"))
        def open_folder(*_):
            try:
                cards.directory.mkdir(parents=True, exist_ok=True)
                self.open_desktop_path(cards.directory)
            except OSError as exc:
                self.error(tr("Não foi possível abrir a pasta"), str(exc))
        folder.connect("clicked", open_folder)
        row.pack_start(folder, False, False, 0)
        for importing, caption in ((True, tr("Importar cartão…")), (False, tr("Exportar cartão…"))):
            button = Gtk.Button(label=caption)
            button.set_sensitive(not self.busy and self.process is None)
            def transfer(_, importing=importing):
                if self.busy or self.process is not None:
                    status.set_text(tr("Feche o jogo antes de importar ou exportar o cartão."))
                    return
                chooser = Gtk.FileChooserDialog(title=tr("Importar cartão PS2") if importing else tr("Exportar cartão PS2"),
                    transient_for=dialog, action=Gtk.FileChooserAction.OPEN if importing else Gtk.FileChooserAction.SAVE)
                chooser.add_buttons(tr("Cancelar"), Gtk.ResponseType.CANCEL, tr("Importar") if importing else tr("Exportar"), Gtk.ResponseType.OK)
                chooser.set_default_response(Gtk.ResponseType.OK)
                if importing:
                    pattern = Gtk.FileFilter(); pattern.set_name(tr("Cartão PS2 (.ps2)"))
                    pattern.add_pattern("*.ps2"); pattern.add_pattern("*.PS2"); chooser.add_filter(pattern)
                else:
                    chooser.set_current_name("Mcd001.ps2"); chooser.set_do_overwrite_confirmation(True)
                self.configure_file_chooser(chooser)
                response = chooser.run(); selected = chooser.get_filename(); chooser.destroy()
                if response != Gtk.ResponseType.OK or not selected:
                    return
                try:
                    if importing:
                        backup = cards.import_card(selected)
                        status.set_text(tr("Cartão importado. Abra o jogo e escolha Carregar jogo.") +
                                        ("\n" + tr("Cartão anterior preservado em {path}").format(path=backup) if backup else ""))
                    else:
                        cards.export_card(selected)
                        status.set_text(tr("Cópia exportada. No outro PC, use Meus saves → Importar cartão."))
                except (OSError, ValueError) as exc:
                    self.error(tr("Não foi possível transferir o cartão"), str(exc))
            button.connect("clicked", transfer)
            row.pack_start(button, False, False, 0)
        panel.pack_start(row, False, False, 0)
        if self.busy or self.process is not None:
            panel.pack_start(label(tr("Feche o jogo para importar ou exportar com segurança."), "muted", wrap=True), False, False, 0)
        dialog.get_content_area().pack_start(panel, True, True, 0)
        self.connect_dialog_feedback(dialog)
        dialog.show_all(); dialog.run(); dialog.destroy()

    def open_desktop_path(self, path):
        # Running xdg-open/GIO with the frozen GTK libraries can break host
        # file managers on other distributions. Keep this off the GTK thread.
        from desktop_services import open_path
        def worker():
            try:
                open_path(path)
            except OSError as exc:
                GLib.idle_add(self.error, tr("Não foi possível abrir o aplicativo padrão"), str(exc))
        threading.Thread(target=worker, daemon=True).start()

    def install_menu(self,*_):
        try:
            self.library.install_desktop_entry()
            self.install_status.set_text(tr("✓  Atalho criado! Procure “Shadow of the Colossus PC” no menu de aplicativos."))
        except OSError as exc:
            self.error(tr("Não foi possível criar o atalho"),str(exc))

    def capture_and_exit(self):
        if self.args.screenshot:
            # Draw the actual widget tree. GDK's X11 window readback can abort
            # inside Cairo 1.16 under Xvfb; widget rendering works on Wayland too.
            surface=cairo.ImageSurface(cairo.FORMAT_ARGB32,self.window.get_allocated_width(),self.window.get_allocated_height())
            self.window.draw(cairo.Context(surface))
            output=Path(self.args.screenshot); output.parent.mkdir(parents=True,exist_ok=True)
            surface.write_to_png(str(output))
        if self.args.test_game:
            result={"passed":not self.failed,"disc":str(self.library.disc),
                    "frame_created":(self.library.directory / "game-test" / "report.json").is_file()}
            (self.library.directory / "launcher-test.json").write_text(json.dumps(result,indent=2)+"\n")
        Gtk.main_quit(); return False

    def close(self,*_):
        if self.busy:
            self.closing=True; self.window.hide(); return True
        Gtk.main_quit(); return False


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom",type=Path,help=argparse.SUPPRESS)
    parser.add_argument("--data-dir",type=Path,default=os.environ.get("SOTC_DATA_DIR"),help=argparse.SUPPRESS)
    parser.add_argument("--screenshot",type=Path,help=argparse.SUPPRESS)
    parser.add_argument("--test-game",action="store_true",help=argparse.SUPPRESS)
    parser.add_argument("--install-shortcut",action="store_true",help=argparse.SUPPRESS)
    parser.add_argument("--no-update-check",action="store_true",help=argparse.SUPPRESS)
    parser.add_argument("--page",choices=("home","graphics","textures","controls","settings","about"),default="home",help=argparse.SUPPRESS)
    args=parser.parse_args()
    if args.install_shortcut:
        print(Library(args.data_dir).install_desktop_entry()); return 0
    if args.test_game and not args.rom and not Library(args.data_dir).disc:
        parser.error("--test-game requires an imported disc or --rom")
    while True:
        launcher=Launcher(args)
        confirm_startup(launcher.library.program)
        try:
            Gtk.main()
        finally:
            launcher.disposed = True
            GLib.source_remove(launcher.gamepad_timer)
            if launcher.update_timer is not None:
                GLib.source_remove(launcher.update_timer)
            launcher.gamepad.suspend()
            launcher.window.destroy()
        if not launcher.restart_language:
            return int(launcher.failed)
        args.page = "settings"


if __name__=="__main__":
    raise SystemExit(main())
