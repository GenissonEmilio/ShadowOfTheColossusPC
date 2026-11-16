"""Wrap the existing square launcher artwork in a Windows icon resource."""
from pathlib import Path
import struct
import sys

import gi

gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf

art = GdkPixbuf.Pixbuf.new_from_file_at_scale(sys.argv[1], 256, 256, True)
ok, png = art.save_to_bufferv("png", [], [])
if not ok:
    raise ValueError("Could not resize the launcher icon")
width, height = art.get_width(), art.get_height()
header = struct.pack("<HHHBBBBHHII", 0, 1, 1, width % 256, height % 256,
                     0, 0, 1, 32, len(png), 22)
Path(sys.argv[2]).write_bytes(header + png)
