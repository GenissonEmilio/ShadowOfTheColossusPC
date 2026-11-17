"""Small frame-clock animations for GTK launcher actions."""
from gi.repository import Gdk, GLib, Gtk


class FocusButton(Gtk.Button):
    """Enlarge inside a fixed hit target so pointer and pad keep stable geometry."""

    def __init__(self, label=None):
        super().__init__(label=label)
        self.progress = 0.0
        self.active = False
        self.hovered = False
        self._tick = 0
        self.connect("focus-in-event", self._change)
        self.connect("focus-out-event", self._change)
        self.connect("enter-notify-event", self._enter)
        self.connect("leave-notify-event", self._leave)
        self.connect("state-flags-changed", self._change)
        self.connect("destroy", self._destroy)

    def _enter(self, _widget, event):
        if event.detail != Gdk.NotifyType.INFERIOR:
            self.hovered = True
            self._change()
        return False

    def _leave(self, _widget, event):
        if event.detail != Gdk.NotifyType.INFERIOR:
            self.hovered = False
            self._change()
        return False

    def _change(self, *_):
        active = self.is_sensitive() and (self.hovered or self.has_focus())
        animate = self.get_settings().get_property("gtk-enable-animations")
        if active == self.active and (animate or not self._tick):
            return False
        self.active = active
        self._origin = self.progress
        self._start = GLib.get_monotonic_time()
        if not animate:
            if self._tick:
                self.remove_tick_callback(self._tick)
                self._tick = 0
            self.progress = float(active)
            self.queue_draw()
        elif not self._tick:
            self._tick = self.add_tick_callback(self._animate)
        return False

    def _animate(self, _widget, clock):
        if not self.get_settings().get_property("gtk-enable-animations"):
            self.progress = float(self.active)
            self._tick = 0
            self.queue_draw()
            return False
        elapsed = max(0.0, (clock.get_frame_time() - self._start) / 180000.0)
        fraction = min(1.0, elapsed)
        eased = 1.0 - (1.0 - fraction) ** 3
        self.progress = self._origin + (float(self.active) - self._origin) * eased
        self.queue_draw()
        if fraction >= 1.0:
            self._tick = 0
            return False
        return True

    def do_draw(self, context):
        scale = .965 + .035 * self.progress
        width, height = self.get_allocated_width(), self.get_allocated_height()
        context.save()
        context.translate(width * (1 - scale) / 2, height * (1 - scale) / 2)
        context.scale(scale, scale)
        result = Gtk.Button.do_draw(self, context)
        context.restore()
        return result

    def _destroy(self, *_):
        if self._tick:
            self.remove_tick_callback(self._tick)
            self._tick = 0
