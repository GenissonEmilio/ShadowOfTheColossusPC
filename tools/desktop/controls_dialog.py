"""GTK editor for the persistent desktop input profile."""
from gi.repository import Gdk, Gtk
from controls import ACTIONS, DEFAULTS, Controls, binding_label, valid_binding


def key_code(event):
    name = Gdk.keyval_name(event.keyval) or ""
    special = {"space": 32, "Escape": 256, "Return": 257, "Tab": 258,
               "ISO_Left_Tab": 258, "BackSpace": 259, "Insert": 260,
               "Right": 262, "Left": 263, "Down": 264, "Up": 265,
               "Page_Up": 266, "Page_Down": 267, "Home": 268, "End": 269,
               "Shift_L": 340, "Control_L": 341, "Alt_L": 342, "Super_L": 343,
               "Shift_R": 344, "Control_R": 345, "Alt_R": 346, "Super_R": 347,
               "Menu": 348, "Caps_Lock": 280, "Scroll_Lock": 281, "Num_Lock": 282,
               "Print": 283, "Pause": 284, "KP_Enter": 335, "KP_Add": 334,
               "KP_Subtract": 333, "KP_Multiply": 332, "KP_Divide": 331,
               "KP_Decimal": 330, "KP_Equal": 336}
    if name in special:
        return special[name]
    if name.startswith("F") and name[1:].isdigit():
        return 289 + int(name[1:])
    if name.startswith("KP_") and name[3:].isdigit():
        return 320 + int(name[3:])
    character = Gdk.keyval_to_unicode(Gdk.keyval_to_upper(event.keyval))
    return character if 32 <= character <= 96 else None


class ControlsDialog(Gtk.Dialog):
    def __init__(self, parent, library):
        super().__init__(title="Teclado e mouse", transient_for=parent, modal=True)
        self.profile = Controls(library)
        self.values = self.profile.load()
        self.set_default_size(620, 660)
        for title, response in (("Restaurar padrão", 10), ("Cancelar", Gtk.ResponseType.CANCEL),
                                ("Salvar perfil", Gtk.ResponseType.OK)):
            self.add_button(title, response).get_style_context().add_class("secondary")
        self.connect("response", self.respond)
        content = self.get_content_area()
        content.set_spacing(12)
        content.set_border_width(18)
        intro = Gtk.Label(label="Clique numa atribuição para trocar a tecla ou o botão do mouse.\n"
                          "F5 salva um estado rápido; F8 carrega. F1 gerencia estados e libera o cursor. F11 alterna a tela cheia.", xalign=0)
        intro.set_line_wrap(True)
        content.pack_start(intro, False, False, 0)
        self.mouse = Gtk.CheckButton(label="Mover a câmera com o mouse")
        self.invert_x = Gtk.CheckButton(label="Inverter câmera horizontal")
        self.invert_y = Gtk.CheckButton(label="Inverter câmera vertical")
        for widget in (self.mouse, self.invert_x, self.invert_y):
            content.pack_start(widget, False, False, 0)
        row = Gtk.Box(spacing=12)
        row.pack_start(Gtk.Label(label="Sensibilidade do mouse", xalign=0), True, True, 0)
        self.sensitivity = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, .1, 5., .1)
        self.sensitivity.set_digits(1)
        self.sensitivity.set_draw_value(True)
        self.sensitivity.set_size_request(210, -1)
        row.pack_end(self.sensitivity, False, False, 0)
        content.pack_start(row, False, False, 0)
        scroller = Gtk.ScrolledWindow()
        scroller.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        grid = Gtk.Grid(column_spacing=22, row_spacing=7, margin=8)
        self.buttons = {}
        for index, (key, title, _) in enumerate(ACTIONS):
            grid.attach(Gtk.Label(label=title, xalign=0), 0, index, 1, 1)
            button = Gtk.Button()
            button.get_style_context().add_class("key")
            button.set_size_request(185, -1)
            button.connect("clicked", self.capture, key, title)
            grid.attach(button, 1, index, 1, 1)
            self.buttons[key] = button
        scroller.add(grid)
        content.pack_start(scroller, True, True, 0)
        self.status = Gtk.Label(xalign=0)
        self.status.set_line_wrap(True)
        content.pack_start(self.status, False, False, 0)
        self.refresh()

    def refresh(self):
        for key, button in self.buttons.items():
            button.set_label(binding_label(self.values[key]))
        self.mouse.set_active(bool(self.values["mouse_camera"]))
        self.invert_x.set_active(bool(self.values["invert_x"]))
        self.invert_y.set_active(bool(self.values["invert_y"]))
        self.sensitivity.set_value(self.values["sensitivity"])

    def capture(self, _, key, title):
        popup = Gtk.Dialog(title=title, transient_for=self, modal=True)
        popup.add_button("Cancelar", Gtk.ResponseType.CANCEL)
        area = Gtk.EventBox()
        area.add_events(Gdk.EventMask.BUTTON_PRESS_MASK)
        text = Gtk.Label(label="Pressione uma tecla, ou clique nesta área com o botão desejado.\n"
                        "Esc também pode ser atribuído. Delete remove a atribuição.")
        text.set_line_wrap(True)
        text.set_margin_top(24); text.set_margin_bottom(24)
        text.set_margin_start(24); text.set_margin_end(24)
        text.set_max_width_chars(48)
        area.add(text)
        popup.get_content_area().add(area)

        def assign(code):
            if not valid_binding(code):
                text.set_text("Tecla não suportada ou reservada (F1 / F5 / F8 / F9 / F11). Escolha outra ou cancele.")
                return True
            previous = next((title for name, title, _ in ACTIONS
                             if name != key and code and self.values[name] == code), None)
            if previous:
                text.set_text(f"{binding_label(code)} já está em ‘{previous}’.\n"
                              "Remova a atribuição anterior ou escolha outro botão.")
                return True
            self.values[key] = code
            self.buttons[key].set_label(binding_label(code))
            popup.response(Gtk.ResponseType.OK)
            return True

        def keyboard(_, event):
            return assign(0 if event.keyval == Gdk.KEY_Delete else key_code(event))

        def mouse(_, event):
            code = {1: -1, 3: -2, 2: -3, 8: -4, 9: -5}.get(event.button)
            return assign(code)

        popup.connect("key-press-event", keyboard)
        area.connect("button-press-event", mouse)
        popup.show_all()
        popup.run()
        popup.destroy()

    def respond(self, _, response):
        if response == 10:
            self.values = dict(DEFAULTS)
            self.refresh()
            self.status.set_text("Padrão restaurado. Clique em Salvar perfil para aplicar.")
        elif response == Gtk.ResponseType.OK:
            self.values.update(mouse_camera=int(self.mouse.get_active()),
                               invert_x=int(self.invert_x.get_active()),
                               invert_y=int(self.invert_y.get_active()),
                               sensitivity=self.sensitivity.get_value())
            try:
                self.profile.save(self.values)
            except (ValueError, OSError) as exc:
                self.status.set_text(str(exc))
                return
            self.destroy()
        else:
            self.destroy()
