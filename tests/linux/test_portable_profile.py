"""Exercise portable profile selection and relocation without compiling a game."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/desktop"))
from graphics import Graphics
from import_disc import identify_disc
from language import PREFERENCES, preferences
from model import Library, PORTABLE_DATA_NAME, PORTABLE_MARKER_NAME, atomic_json
from pipeline import BOOT, Pipeline, executable_name
from recomp_cli import run


class PortableProfileTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory(prefix="sotc-portable-profile-")
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name).resolve()
        self.program = self.root / "Launcher extraído"
        self.program.mkdir()
        self.standard = self.root / "standard"
        standard_profile = patch("model.data_directory", return_value=self.standard)
        standard_profile.start()
        self.addCleanup(standard_profile.stop)

    def test_activation_and_deactivation_keep_both_profiles(self):
        standard = Library(program=self.program)
        self.assertEqual(standard.directory, self.standard)
        atomic_json(standard.directory / PREFERENCES, {"language": "es"})
        standard.set_portable(True)
        portable = Library(program=self.program)
        self.assertTrue(portable.portable)
        self.assertTrue((self.program / PORTABLE_MARKER_NAME).is_file())
        self.assertFalse((portable.directory / PREFERENCES).exists())
        atomic_json(portable.directory / PREFERENCES, {"language": "ja"})
        portable.set_portable(False)
        self.assertEqual(preferences(Library(program=self.program).directory)["language"], "es")
        self.assertEqual(preferences(portable.directory)["language"], "ja")
        self.assertFalse(portable.portable_marker.exists())
        standard.set_portable(True)
        self.assertEqual(preferences(Library(program=self.program).directory)["language"], "ja")

    def test_activation_fails_without_selecting_unusable_data_path(self):
        library = Library(program=self.program)
        library.portable_directory.touch()
        with self.assertRaises(OSError):
            library.set_portable(True)
        self.assertFalse(library.portable_marker.exists())
        self.assertEqual(Library(program=self.program).directory, self.standard)

    def test_portable_root_entries_cannot_be_links(self):
        library = Library(program=self.program)
        outside = self.root / "outside"
        outside.mkdir()
        library.portable_directory.symlink_to(outside, target_is_directory=True)
        with self.assertRaises(OSError):
            library.set_portable(True)
        self.assertFalse(library.portable_marker.exists())
        library.portable_marker.touch()
        with self.assertRaises(OSError):
            Library(program=self.program)
        self.assertEqual(Library(outside, self.program).directory, outside)

    def test_headless_play_honors_portable_env_and_cli_precedence(self):
        library = Library(program=self.program)
        library.set_portable(True)
        external = self.root / "RecompLauncher"
        argument = self.root / "explicit override"
        selected = []

        def launch(instance, *, attached):
            self.assertTrue(attached)
            selected.append(instance.directory)
            return Mock(wait=Mock(return_value=0))

        with patch("model.program_directory", return_value=self.program), patch.object(Library, "launch", launch):
            with patch.dict(os.environ, clear=True):
                self.assertEqual(run(["--play"]), 0)
            with patch.dict(os.environ, {"SOTC_DATA_DIR": str(external)}):
                self.assertEqual(run(["--play"]), 0)
                self.assertEqual(run(["--play", "--data-dir", str(argument)]), 0)
        self.assertEqual(selected, [self.program / PORTABLE_DATA_NAME, external, argument])
        override = Library(external, self.program)
        self.assertFalse(override.portable)
        with self.assertRaises(ValueError):
            override.set_portable(False)
        self.assertTrue(library.portable_marker.is_file())

    def test_moved_launcher_keeps_disc_saves_preferences_and_valid_cache(self):
        Library(program=self.program).set_portable(True)
        before = Library(program=self.program)
        disc = before.directory / "discs" / "fixture-disc"
        disc.mkdir(parents=True)
        (disc / BOOT).touch()
        (disc / "SYSTEM.CNF").write_text(f"BOOT2 = cdrom0:\\{BOOT};1\n")
        # Older profiles can contain an absolute path to the extracted disc.
        atomic_json(before.config, {"disc": str(disc / BOOT)})
        atomic_json(before.directory / PREFERENCES, {"language": "ja"})
        memcard = before.player_directory / "memcards" / "Mcd001.ps2"
        memcard.parent.mkdir(parents=True)
        memcard.write_bytes(b"save fixture")
        Graphics(before).config.write_text("render_scale=2.5\n")
        atomic_json(self.program / "compiler/manifest.json", {"build_id": "fixture", "link_inputs": []})
        clang = self.program / "compiler/toolchain/bin" / executable_name("clang++")
        clang.parent.mkdir(parents=True)
        clang.touch()
        work = before.directory / "builds" / "fixture"
        game = work / "gamecore" / executable_name("game")
        game.parent.mkdir(parents=True)
        game.write_bytes(b"game fixture")
        atomic_json(work / "complete.json", {"sha256": hashlib.sha256(game.read_bytes()).hexdigest()})

        moved_path = self.program.rename(self.root / "Em outro diretório")
        after = Library(program=moved_path)
        self.assertEqual(after.disc, after.directory / "discs" / "fixture-disc" / BOOT)
        after.state["disc"] = f"discs\\fixture-disc\\{BOOT}"
        self.assertEqual(after.disc, after.directory / "discs" / "fixture-disc" / BOOT)
        self.assertEqual(preferences(after.directory)["language"], "ja")
        self.assertEqual(Graphics(after).config.read_text(), "render_scale=2.5\n")
        environment = after.game_environment()
        self.assertEqual(Path(environment["PS2X_MEMCARD_DIR"], memcard.name).read_bytes(), b"save fixture")
        self.assertEqual(Path(environment["PS2X_STATE_DIR"]), after.player_directory / "savestates")
        with (patch.dict(os.environ, {"SOTC_BUILD_CLANG": "1"}),
              patch.object(after, "validated_disc", side_effect=identify_disc),
              patch.object(Pipeline, "prepare_stages", side_effect=AssertionError("valid cache must be reused"))):
            self.assertEqual(after.prepare_game(), after.directory / game.relative_to(before.directory))

    def test_portable_disc_cannot_reference_external_game_path(self):
        Library(program=self.program).set_portable(True)
        portable = Library(program=self.program)
        outside = self.root / BOOT
        outside.touch()
        for reference in (str(outside), f"../../{BOOT}"):
            with self.subTest(reference=reference):
                portable.state["disc"] = reference
                self.assertIsNone(portable.disc)


if __name__ == "__main__":
    unittest.main()
