import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/desktop"))
from graphics import RECOMP_VIDEO_KEYS, Graphics
from model import Library, atomic_json
from pipeline import BOOT, Pipeline, executable_name
from recomp_cli import run


class RecompLauncherTests(unittest.TestCase):
    def test_profile_uses_real_settings_and_shared_data_path(self):
        profile = json.loads((ROOT / "recomplauncher.json").read_text())
        self.assertEqual(profile["game"]["check"]["iso9660"], BOOT)
        self.assertEqual(profile["start"]["program"], profile["prepare"]["program"])
        profile_path = "{local}/ShadowOfTheColossusPC"
        self.assertEqual(profile["start"]["args"], ["--play", "--data-dir", profile_path])
        self.assertEqual(profile["prepare"]["args"], ["--prepare", "--rom", "{game}", "--data-dir", profile_path])
        self.assertEqual(profile["config"][0]["file"], [{"path": profile_path + "/player/celosia_settings.cfg"}])
        defaults = profile["config"][0]["new"]
        self.assertEqual(set(defaults), set(RECOMP_VIDEO_KEYS))
        for option in profile["settings"]:
            for key, value in option["defaults"].items():
                self.assertEqual(defaults[key], value)
            for choice in option["choices"]:
                self.assertLessEqual(choice["set"].keys(), defaults.keys())

    def test_setup_and_play_reuse_completed_game_and_external_settings(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            library = Library(root / "data with spaces", root / "program")
            rom = root / "Sony PS2.iso"
            rom.touch()
            source_id = hashlib.sha256(f"{rom}\0{rom.stat().st_size}\0{rom.stat().st_mtime_ns}".encode()).hexdigest()[:20]
            boot = library.directory / "discs" / source_id / BOOT
            boot.parent.mkdir(parents=True)
            boot.touch()
            build_id = "v098-test-cache"
            atomic_json(library.program / "compiler/manifest.json", {"build_id": build_id, "link_inputs": []})
            work = library.directory / "builds" / build_id
            game = work / "gamecore" / executable_name("game")
            game.parent.mkdir(parents=True)
            game.write_bytes(b"previously compiled fixture")
            atomic_json(work / "complete.json", {"sha256": hashlib.sha256(game.read_bytes()).hexdigest()})
            graphics = Graphics(library)
            graphics.config.parent.mkdir()
            graphics.config.write_text("render_scale=2.500\nfullscreen = 0\nscreen = 3\nvsync = 0\nvolume = 7\n")
            output = io.StringIO()
            with (patch("recomp_cli.Library", return_value=library),
                  patch.object(library, "valid_extracted", return_value=True),
                  patch.object(Pipeline, "helper", side_effect=AssertionError("cached game must not compile")),
                  patch("model.import_disc", side_effect=AssertionError("existing disc must not import twice")),
                  patch("model.subprocess.Popen", return_value=Mock(wait=Mock(return_value=0))) as play,
                  contextlib.redirect_stdout(output)):
                data_args = ["--data-dir", str(library.directory)]
                self.assertEqual(run(["--prepare", "--rom", str(rom), *data_args]), 0)
                play.assert_not_called()
                self.assertEqual(run(["--play", *data_args]), 0)
            self.assertIn("progress 0.05 ", output.getvalue())
            self.assertIn("progress 1 Ready to play", output.getvalue())
            self.assertEqual(play.call_args.args[0], [str(game), str(boot)])
            self.assertFalse(play.call_args.kwargs["start_new_session"])
            contents = graphics.config.read_text()
            self.assertIn("render_scale=2.500\n", contents)
            for line in ("fullscreen=0", "screen=3", "vsync=0", "volume=7"):
                self.assertIn(line + "\n", contents)
            self.assertEqual(game.read_bytes(), b"previously compiled fixture")
