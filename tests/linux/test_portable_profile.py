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
from model import Library, PORTABLE_DATA_NAME, PORTABLE_DISABLED, PORTABLE_MARKER_NAME, atomic_json, publish_profile
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

    def make_package(self):
        atomic_json(self.program / "release.json", {"version": "1.2.14", "platform": "linux-x86_64"})

    def write_legacy(self, name, value):
        destination = self.standard / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(value)
        return destination

    def test_standard_installation_migrates_on_launch_only_once(self):
        self.make_package()
        boot = self.write_legacy(f"discs/fixture/{BOOT}", b"disc")
        atomic_json(self.standard / "launcher.json", {"disc": str(boot)})
        atomic_json(self.standard / PREFERENCES, {"language": "es"})
        migrated_paths = (
            "controllers/keyboard-mouse.cfg", "player/celosia_settings.cfg",
            "player/memcards/Mcd001.ps2", "player/memcards/backups/backup.ps2",
            "player/savestates/save", "player/mc0/slot", "shaders/fixture.frag",
            "textures/exported/fixture.png", "campaign-languages/fixture/pt-BR/NICO.DAT",
        )
        for entry in migrated_paths:
            self.write_legacy(entry, entry.encode())
        excluded_paths = ("last-game.log", "game-test/frame_0.png", "android/key.jks",
                          "builds/obsolete/gamecore/game", "builds/current/boot/object.o", "player/logs/log")
        for entry in excluded_paths:
            self.write_legacy(entry, b"excluded")
        atomic_json(self.program / "compiler/manifest.json", {"build_id": "current"})
        filename = executable_name("game")
        game = self.write_legacy(f"builds/current/gamecore/{filename}", b"verified game")
        stamp_path = Path("builds/current/complete.json")
        stamp = {"sha256": hashlib.sha256(game.read_bytes()).hexdigest()}
        atomic_json(self.standard / stamp_path, stamp)
        self.write_legacy(f"builds/current/gcc/gamecore/{filename}", b"modified game")
        atomic_json(self.standard / "builds/current/gcc/complete.json", {**stamp, "compiler": "gcc"})

        selected = Library(program=self.program)
        self.assertTrue(selected.portable)
        self.assertFalse(selected.directory.exists())
        migrated = Library.for_launch(program=self.program)
        self.assertTrue(migrated.portable)
        self.assertIsNone(migrated.portable_error)
        self.assertEqual(migrated.state["disc"], f"discs/fixture/{BOOT}")
        self.assertEqual(migrated.disc.read_bytes(), b"disc")
        self.assertEqual(preferences(migrated.directory)["language"], "es")
        for entry in migrated_paths:
            self.assertEqual((migrated.directory / entry).read_bytes(), entry.encode())
        for entry in (*excluded_paths, f"builds/current/gcc/gamecore/{filename}"):
            self.assertFalse((migrated.directory / entry).exists())
            self.assertTrue((self.standard / entry).exists())
        self.assertEqual((migrated.directory / game.relative_to(self.standard)).read_bytes(), b"verified game")
        self.assertTrue((migrated.directory / stamp_path).is_file())
        self.assertEqual(Library(self.standard, self.program).disc, boot)
        atomic_json(self.standard / PREFERENCES, {"language": "ja"})
        self.assertEqual(preferences(Library.for_launch(program=self.program).directory)["language"], "es")

    def test_explicit_standard_override_and_update_acknowledgment_skip_migration(self):
        self.make_package()
        atomic_json(self.standard / PREFERENCES, {"language": "es"})
        with patch.dict(os.environ, {"SOTC_UPDATE_READY": "ready"}):
            self.assertEqual(Library.for_launch(self.standard, self.program).directory, self.standard)
            self.assertEqual(Library.for_launch(program=self.program).directory, self.standard)
        self.assertFalse((self.program / PORTABLE_DATA_NAME).exists())
        self.assertTrue(Library.for_launch(program=self.program).portable)

    def test_failed_copy_leaves_old_profile_active_and_can_retry(self):
        self.make_package()
        saved = self.write_legacy("player/memcards/Mcd001.ps2", b"save")
        with patch("model.shutil.copy2", side_effect=OSError("no space")):
            fallback = Library.for_launch(program=self.program)
        self.assertEqual(fallback.directory, self.standard)
        self.assertFalse(fallback.portable)
        self.assertFalse(fallback.has_data_override)
        self.assertIn("no space", str(fallback.portable_error))
        self.assertFalse(fallback.portable_directory.exists())
        self.assertEqual(list(self.program.glob(".sotc-profile-*")), [])
        self.assertEqual(saved.read_bytes(), b"save")
        fallback.set_portable(True)
        portable = Library.for_launch(program=self.program)
        self.assertTrue(portable.portable)
        self.assertEqual((portable.player_directory / "memcards/Mcd001.ps2").read_bytes(), b"save")

    def test_concurrent_existing_portable_directory_wins(self):
        self.make_package()
        atomic_json(self.standard / PREFERENCES, {"language": "es"})

        def publish_with_race(source, target):
            target.mkdir()
            publish_profile(source, target)

        with patch("model.publish_profile", side_effect=publish_with_race):
            portable = Library.for_launch(program=self.program)
        self.assertTrue(portable.portable)
        self.assertIsNone(portable.portable_error)
        self.assertFalse((portable.directory / PREFERENCES).exists())
        self.assertEqual(preferences(self.standard)["language"], "es")

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
        self.assertEqual(portable.portable_marker.read_text().strip(), PORTABLE_DISABLED)
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
