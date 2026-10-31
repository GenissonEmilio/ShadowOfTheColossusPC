import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/desktop"))
from desktop_services import host_environment, open_path
from model import Library
from saves import CARD_BYTES, MAGIC, SaveCards


class SavesTests(unittest.TestCase):
    def test_card_transfer_to_another_home_keeps_backup_and_source(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t)
            old = MAGIC + bytes(CARD_BYTES - len(MAGIC))
            new = MAGIC + b"1" * (CARD_BYTES - len(MAGIC))
            first = SaveCards(Library(p / "first-home"))
            first.directory.mkdir(parents=True); first.card.write_bytes(new)
            exported = p / "Minha aventura.ps2"
            first.export_card(exported)
            second = SaveCards(Library(p / "second-home"))
            second.directory.mkdir(parents=True); second.card.write_bytes(old)
            backup = second.import_card(exported)
            self.assertEqual(second.card.read_bytes(), new)
            self.assertEqual(backup.read_bytes(), old)
            self.assertEqual(exported.read_bytes(), new)
            self.assertEqual(first.card.read_bytes(), new)

    def test_invalid_card_or_failed_replace_preserves_existing_save(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t); cards = SaveCards(Library(p / "data"))
            cards.directory.mkdir(parents=True); cards.card.write_bytes(b"previous card")
            bad = p / "bad.ps2"; bad.write_bytes(b"bad")
            with self.assertRaises(ValueError): cards.import_card(bad)
            self.assertEqual(cards.card.read_bytes(), b"previous card")
            good = p / "good.ps2"; good.write_bytes(MAGIC + bytes(CARD_BYTES - len(MAGIC)))
            original_replace = os.replace
            def replace(src, dst):
                if Path(dst) == cards.card:
                    raise OSError("disk error")
                original_replace(src, dst)
            with patch("saves.os.replace", side_effect=replace), self.assertRaises(OSError):
                cards.import_card(good)
            self.assertEqual(cards.card.read_bytes(), b"previous card")
            self.assertEqual(next((cards.directory / "backups").glob("*.ps2")).read_bytes(), b"previous card")
            self.assertFalse(list(cards.directory.glob(".card-*")))

    def test_host_opener_strips_frozen_libraries_and_retains_desktop_session(self):
        env = {"LD_LIBRARY_PATH": "/bundle", "LD_LIBRARY_PATH_ORIG": "/host/lib",
               "GI_TYPELIB_PATH": "/bundle", "FONTCONFIG_FILE": "/bundle/fonts.conf",
               "DBUS_SESSION_BUS_ADDRESS": "unix:path=/session", "WAYLAND_DISPLAY": "wayland-0",
               "PATH": "/bundle/bin:/usr/bin:/bin"}
        with patch.dict(os.environ, env, clear=True), patch.object(sys, "frozen", True, create=True), \
                patch.object(sys, "_MEIPASS", "/bundle", create=True):
            clean = host_environment()
            self.assertEqual(clean["LD_LIBRARY_PATH"], "/host/lib")
            self.assertNotIn("GI_TYPELIB_PATH", clean)
            self.assertNotIn("FONTCONFIG_FILE", clean)
            self.assertEqual(clean["PATH"], "/usr/bin:/bin")
            self.assertEqual(clean["DBUS_SESSION_BUS_ADDRESS"], env["DBUS_SESSION_BUS_ADDRESS"])
            self.assertEqual(clean["WAYLAND_DISPLAY"], "wayland-0")
            self.assertEqual(os.environ["LD_LIBRARY_PATH"], "/bundle")

    def test_host_opener_fallback_and_error_are_observable(self):
        with tempfile.TemporaryDirectory() as t:
            path = Path(t) / "save com espaços"; path.mkdir()
            def which(name, **kw): return "/usr/bin/" + name
            fail = subprocess.CompletedProcess([], 1, stderr="no handler")
            ok = subprocess.CompletedProcess([], 0, stderr="")
            with patch("desktop_services.shutil.which", side_effect=which), \
                    patch("desktop_services.subprocess.run", side_effect=[fail, ok]) as run:
                open_path(path)
                self.assertEqual(run.call_args_list[0].args[0], ["/usr/bin/xdg-open", str(path)])
                self.assertEqual(run.call_args_list[1].args[0], ["/usr/bin/gio", "open", path.as_uri()])
            with patch("desktop_services.shutil.which", return_value=None), self.assertRaisesRegex(OSError, str(path)):
                open_path(path)


if __name__ == "__main__":
    unittest.main()
