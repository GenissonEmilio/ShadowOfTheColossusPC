#!/usr/bin/env python3
"""Check pixels and native-monitor dimensions from the real GL presentation path."""
import json
from pathlib import Path
import sys

report = json.loads((Path(sys.argv[1]) / "report.json").read_text())
cases = {item["name"]: item for item in report["cases"]}
assert report["valid_shader"] and report["invalid_rejected"] and report["active_preserved"]
for name, item in cases.items():
    if name.startswith("fullscreen"):
        assert item["fullscreen"], item
        assert (item["width"], item["height"]) == (report["monitor_width"], report["monitor_height"]), item
    if "fit" in name or name == "window-restored":
        assert abs(item["picture_width"] / item["picture_height"] - 4/3) < 0.001, item
        assert item["x"] >= 0 and item["y"] >= 0, item
        if item["x"] > 1:
            assert item["edge"] == [0, 0, 0], item
    else:
        assert item["x"] == 0 and item["y"] == 0, item
        assert item["picture_width"] == item["width"] and item["picture_height"] == item["height"], item
        assert item["edge"] == item["body"], item
    expected = [25, 214, 200] if name in ("fullscreen-shader", "invalid-keeps-shader") else [230, 41, 55]
    assert all(abs(a-b) <= 2 for a,b in zip(item["body"], expected)), item
assert not cases["window-restored"]["fullscreen"]
assert (cases["window-restored"]["width"],cases["window-restored"]["height"]) == (960,540)
print("PASS: native fullscreen, fit/stretch pixels, valid/invalid shader, disable and window restoration")
