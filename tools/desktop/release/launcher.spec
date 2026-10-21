from pathlib import Path

root = Path(SPECPATH).resolve().parents[2]
a = Analysis(
    [str(root / "tools/desktop/main.py")],
    pathex=[str(root / "tools/desktop")],
    # GI loads GDK dynamically; explicitly retain Wayland dependencies that
    # PyInstaller otherwise excludes as optional desktop libraries.
    binaries=[("/opt/libarchive/lib/libarchive.so.13", "."),
              ("/usr/lib/x86_64-linux-gnu/libwayland-client.so.0", "."),
              ("/usr/lib/x86_64-linux-gnu/libwayland-cursor.so.0", "."),
              ("/usr/lib/x86_64-linux-gnu/libwayland-egl.so.1", ".")],
    datas=[(str(root / "tools/desktop/assets"), "assets"),
           ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "assets/fonts"),
           ("/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf", "assets/fonts")],
    hiddenimports=["gi._gi_cairo", "cairo"],
    excludes=["tkinter", "PIL", "pytest", "numpy"],
    hooksconfig={"gi": {"icons": ["Adwaita"], "themes": ["Adwaita"],
                         "languages": ["pt_BR", "en"], "module-versions": {"Gtk": "3.0", "Gdk": "3.0"}}},
    noarchive=False,
)
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, [], exclude_binaries=True, name="ShadowOfTheColossusPC",
          debug=False, bootloader_ignore_signals=False, strip=False, upx=False, console=False)
coll = COLLECT(exe, a.binaries, a.datas, strip=False, upx=False, name="ShadowOfTheColossusPC")
