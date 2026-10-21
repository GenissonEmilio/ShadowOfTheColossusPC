"""Original vector icon, rendered with Cairo (no game assets)."""
import cairo
from pathlib import Path
s=cairo.ImageSurface(cairo.FORMAT_ARGB32,256,256)
c=cairo.Context(s);c.set_source_rgb(.07,.10,.08);c.paint()
c.set_source_rgb(.28,.34,.28);c.move_to(48,228);c.line_to(61,90);c.line_to(86,58);c.line_to(91,22);c.line_to(163,22);c.line_to(178,58);c.line_to(200,90);c.line_to(214,228);c.close_path();c.fill()
c.set_source_rgb(.46,.49,.40)
for x,y,w,h in [(95,33,62,32),(77,80,100,36),(69,130,117,31),(65,177,128,31)]:c.rectangle(x,y,w,h);c.fill()
c.set_source_rgb(.5,.95,.8)
for x in (109,144):c.rectangle(x,48,13,5);c.fill()
c.set_line_width(3);c.move_to(127,88);c.line_to(111,106);c.line_to(127,128);c.line_to(143,106);c.close_path();c.stroke()
s.write_to_png(str(Path(__file__).with_name('icon.png')))
