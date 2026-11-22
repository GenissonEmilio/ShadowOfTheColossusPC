"""Support and maintenance actions for the GTK launcher."""
import threading

from gi.repository import GLib, Gtk

from language import PREFERENCES, translate as tr
from model import atomic_json
from motion import FocusButton


class MoreOptions:
    def __init__(self, launcher, *, box, label, style):
        self.launcher = launcher
        self.verifying = False
        self.verification_disc = None
        self.page = box(spacing=18, classes=("more-options-page",))
        self.page.pack_start(label(tr("Mais opções"), "section-title"), False, False, 0)
        panel = box(classes=("panel", "more-options-panel"))
        self.page.pack_start(panel, False, False, 0)
        actions = Gtk.SizeGroup(mode=Gtk.SizeGroupMode.HORIZONTAL)

        def add_row(icon, title, description, button):
            row = box(Gtk.Orientation.HORIZONTAL, spacing=16, classes=("more-options-row",))
            image = label(icon, "more-options-icon")
            image.set_width_chars(2)
            image.set_xalign(.5)
            image.set_valign(Gtk.Align.CENTER)
            row.pack_start(image, False, False, 0)
            info = box(spacing=4)
            info.set_valign(Gtk.Align.CENTER)
            info.pack_start(label(tr(title), "panel-title", wrap=True), False, False, 0)
            info.pack_start(label(tr(description), "muted", wrap=True), False, False, 0)
            row.pack_start(info, True, True, 0)
            style(button, "secondary", "more-options-action")
            button.set_valign(Gtk.Align.CENTER)
            actions.add_widget(button)
            row.pack_end(button, False, False, 0)
            panel.pack_start(row, False, False, 0)
            return info

        def action(caption, callback):
            button = FocusButton(label=tr(caption))
            button.connect("clicked", callback)
            return button

        add_row("▦", "Compilação local",
                "Prepara a compilação em segundo plano. Inicie o jogo quando quiser.",
                launcher.compile_button)

        self.verify_button = action("Verificar arquivos", self.verify)
        verify_info = add_row("✓", "Arquivos do disco",
                              "Confere os arquivos já importados do disco selecionado.",
                              self.verify_button)
        self.verify_status = label("", "status", wrap=True)
        self.verify_status.set_no_show_all(True)
        verify_info.pack_start(self.verify_status, False, False, 0)

        self.logs = []
        for filename, title, caption, description in (
            ("last-prepare.log", "Preparação do jogo", "Abrir log da preparação",
             "Veja os detalhes da última compilação local."),
            ("last-game.log", "Execução do jogo", "Abrir log do jogo",
             "Veja as mensagens da última execução do jogo."),
        ):
            path = launcher.library.directory / filename
            button = action(caption, lambda _, path=path: launcher.open_desktop_path(path))
            button.set_tooltip_text(filename)
            add_row("≡", title, description, button)
            self.logs.append((path, button))

        add_row("▣", "Cartão de memória",
                "Importe seu cartão de PS2 ou exporte uma cópia.",
                action("Meus saves", launcher.open_saves))
        self.reset_button = action("Restaurar padrão", self.reset_preferences)
        reset_info = add_row("↻", "Preferências da interface",
                             "Restaura os sons e o modo de exibição do launcher.",
                             self.reset_button)
        self.reset_status = label("", "status", "ready", wrap=True)
        self.reset_status.set_no_show_all(True)
        reset_info.pack_start(self.reset_status, False, False, 0)
        self.refresh()

    def refresh(self):
        launcher = self.launcher
        disc = launcher.library.disc
        idle = launcher.gamepad_available()
        self.verify_button.set_sensitive(disc is not None and idle and not self.verifying)
        if disc != self.verification_disc:
            self.verify_status.hide()
        self.reset_button.set_sensitive(idle and not launcher.updating)
        for path, button in self.logs:
            button.set_sensitive(path.is_file())

    def verify_message(self, message, state=None):
        self.verify_status.set_text(message)
        context = self.verify_status.get_style_context()
        context.remove_class("ready")
        context.remove_class("error")
        if state:
            context.add_class(state)
        self.verify_status.show()

    def verify(self, *_):
        if not self.verify_button.is_sensitive():
            return
        self.verification_disc = self.launcher.library.disc
        self.verifying = True
        self.verify_message(tr("Verificando os arquivos do disco…"))
        self.refresh()

        def worker():
            try:
                self.launcher.library.verify()
            except (OSError, ValueError) as exc:
                GLib.idle_add(self.verified, str(exc))
            else:
                GLib.idle_add(self.verified, None)

        threading.Thread(target=worker, daemon=True).start()

    def verified(self, error):
        self.verifying = False
        if self.launcher.disposed:
            return False
        if self.launcher.library.disc == self.verification_disc:
            if error:
                self.verify_message(error, "error")
            else:
                self.verify_message(tr("Arquivos do disco verificados."), "ready")
        self.refresh()
        return False

    def reset_preferences(self, *_):
        launcher = self.launcher
        dialog = Gtk.MessageDialog(transient_for=launcher.window, modal=True,
                                   message_type=Gtk.MessageType.QUESTION, buttons=Gtk.ButtonsType.NONE,
                                   text=tr("Restaurar preferências da interface?"))
        dialog.format_secondary_text(tr("Os sons serão ativados e o launcher sairá da tela cheia."))
        dialog.add_buttons(tr("Cancelar"), Gtk.ResponseType.CANCEL,
                           tr("Restaurar padrão"), Gtk.ResponseType.OK)
        dialog.set_default_response(Gtk.ResponseType.CANCEL)
        launcher.connect_dialog_feedback(dialog)
        response = dialog.run()
        dialog.destroy()
        if response != Gtk.ResponseType.OK:
            return

        values = dict(launcher.preferences, ui_sounds=True, launcher_fullscreen=False)
        try:
            atomic_json(launcher.library.directory / PREFERENCES, values)
        except OSError as exc:
            launcher.error(tr("Não foi possível salvar as configurações"), str(exc))
            return
        launcher.preferences = values
        launcher.feedback.set_enabled(values["ui_sounds"])
        launcher.sound_choice.set_active(values["ui_sounds"])
        launcher.set_presentation(values["launcher_fullscreen"])
        self.reset_status.set_text(tr("Preferências da interface restauradas."))
        self.reset_status.show()
