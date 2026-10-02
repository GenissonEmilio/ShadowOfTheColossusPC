"""Drop stub bindings whose runtime handler is only a TODO placeholder,
so the original SDK code gets recompiled instead."""
import re, sys, pathlib
runtime = pathlib.Path(sys.argv[1])
cfg = pathlib.Path(sys.argv[2])
todo = set()
for f in runtime.glob("src/lib/**/*.cpp"):
    todo |= set(re.findall(r'TODO_NAMED\("([A-Za-z0-9_]+)"', f.read_text(errors="ignore")))
text = cfg.read_text()
m = re.search(r'^stubs = \[(.*?)^\]', text, re.S | re.M)
entries = re.findall(r'"([^"]+)"', m.group(1))
keep, drop = [], []
for e in entries:
    name = e.split("@")[0]
    (drop if name in todo or name.lstrip("_") in todo else keep).append(e)
new = "stubs = [\n" + "".join(f'  "{e}",\n' for e in keep) + "]"
cfg.write_text(text[:m.start()] + new + text[m.end():])
print(f"kept {len(keep)}, dropped {len(drop)}: {[d.split('@')[0] for d in drop]}")
