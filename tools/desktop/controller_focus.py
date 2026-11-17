"""Direction-based focus and scrolling for GTK controller navigation."""
from gi.repository import Gtk


def widgets(root):
    yield root
    if isinstance(root, Gtk.Container):
        for child in root.get_children():
            yield from widgets(child)


def focusables(root):
    if not (root.get_visible() and root.get_child_visible() and root.is_sensitive()):
        return
    if isinstance(root, Gtk.ComboBox) and not root.get_has_entry():
        # A non-editable combo delegates focus to its internal button.
        yield root
    elif isinstance(root, (Gtk.Button, Gtk.Range, Gtk.Entry, Gtk.TreeView, Gtk.IconView, Gtk.ListBoxRow)):
        if root.get_can_focus():
            yield root
    elif isinstance(root, Gtk.Container):
        for child in root.get_children():
            yield from focusables(child)


def focus_widget(window):
    focus = window.get_focus()
    if focus is None:
        return None
    combo = focus if isinstance(focus, Gtk.ComboBox) else focus.get_ancestor(Gtk.ComboBox)
    return combo if combo is not None and not combo.get_has_entry() else focus


def reveal(widget):
    ancestor = widget.get_parent()
    while ancestor is not None:
        if isinstance(ancestor, Gtk.Viewport):
            point = widget.translate_coordinates(ancestor.get_child(), 0, 0)
            if point is not None:
                ancestor.get_hadjustment().clamp_page(point[0], point[0] + widget.get_allocated_width())
                ancestor.get_vadjustment().clamp_page(point[1], point[1] + widget.get_allocated_height())
        ancestor = ancestor.get_parent()


def grab(widget):
    widget.grab_focus()
    reveal(widget)


def bounds(widget, window):
    point = widget.translate_coordinates(window, 0, 0)
    if point is None:
        return None
    # Keep off-screen controls as candidates so focus can scroll them into view.
    return ((point[0], point[0] + widget.get_allocated_width()),
            (point[1], point[1] + widget.get_allocated_height()))


def move(window, focus, action, scope=None):
    here = bounds(focus, window)
    if here is None:
        return False
    axis = int(action in ("up", "down"))
    sign = 1 if action in ("right", "down") else -1
    candidate = None
    best = None
    for widget in focusables(scope or window):
        if widget == focus:
            continue
        there = bounds(widget, window)
        if there is None:
            continue
        distance = sign * (sum(there[axis]) - sum(here[axis])) / 2
        if distance <= 1:
            continue
        start, end = here[1 - axis]
        other_start, other_end = there[1 - axis]
        gap = max(other_start - end, start - other_end, 0)
        skew = abs(other_start + other_end - start - end) / 2
        score = (distance + gap * 2, skew)
        if best is None or score < best:
            candidate, best = widget, score
    if candidate is None:
        return False
    grab(candidate)
    return True
