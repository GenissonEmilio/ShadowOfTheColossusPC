import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/desktop"))
from language import PREFERENCES
from model import Library, atomic_json


class CampaignLanguageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.library = Library(self.root / "profile", self.root / "program")
        self.asset_root = self.root / "assets"
        self.asset_patch = self.asset_root / "campaign/pt-BR.xdelta"
        asset_override = patch("model.assets_directory", return_value=self.asset_root)
        asset_override.start()
        self.addCleanup(asset_override.stop)

    def select_disc(self, name):
        boot = self.library.directory / "discs" / name / name
        boot.parent.mkdir(parents=True, exist_ok=True)
        boot.touch()
        self.library.state = {"disc": str(boot)}
        return boot

    def save_languages(self, languages, interface="en"):
        atomic_json(self.library.directory / PREFERENCES,
                    {"language": interface, "campaign_languages": languages})

    def test_options_and_choice_are_specific_to_the_imported_edition(self):
        self.assertEqual(self.library.campaign_language_options(), {})
        self.save_languages({"SCUS-97472": "auto", "SCES-53326": "fr"}, interface="pt")
        self.select_disc("SCUS_974.72")
        self.assertEqual(set(self.library.campaign_language_options()), {"auto", "en"})
        self.assertEqual(self.library.campaign_language(), "en")
        self.asset_patch.parent.mkdir(parents=True)
        self.asset_patch.touch()
        self.assertEqual(set(self.library.campaign_language_options()), {"auto", "en", "pt-BR"})
        self.assertEqual(self.library.campaign_language(), "pt-BR")
        self.select_disc("SCES_533.26")
        self.assertEqual(set(self.library.campaign_language_options()),
                         {"original", "auto", "en", "fr", "es", "de", "it"})
        self.assertEqual(self.library.campaign_language(), "fr")
        self.save_languages({"SCES-53326": ["malformed"]})
        self.assertEqual(self.library.campaign_language(), "original")

    def test_ptbr_play_uses_integrated_patch_and_original_compilation(self):
        original = self.select_disc("SCUS_974.72")
        self.asset_patch.parent.mkdir(parents=True)
        self.asset_patch.touch()
        self.save_languages({"SCUS-97472": "pt-BR"})
        localized = self.library.portuguese_directory / original.name
        executable = self.root / "game"
        with (patch.object(self.library, "prepare_game", return_value=executable) as prepare,
              patch("campaign_locale.prepare_portuguese", return_value=localized) as localize,
              patch("gpu.prefer_discrete_gpu"),
              patch("model.subprocess.Popen") as start):
            self.library.launch()
        prepare.assert_called_once()
        self.assertEqual(localize.call_args.args[:3],
                         (original.parent, localized.parent, self.asset_patch))
        self.assertEqual(start.call_args.args[0], [str(executable), str(localized)])
        environment = start.call_args.kwargs["env"]
        self.assertNotIn("SOTC_CAMPAIGN_PT_BR_BOOT", environment)
        self.assertEqual(Path(environment["PS2X_STATE_DIR"]).name, "pt-BR")
        self.assertEqual(Path(environment["PS2X_MEMCARD_DIR"]),
                         self.library.player_directory / "memcards")

    def test_pal_language_is_only_forwarded_for_play(self):
        original = self.select_disc("SCES_533.26")
        self.save_languages({"SCES-53326": "de"})
        with patch.dict(os.environ, {"SOTC_CAMPAIGN_PAL_LANGUAGE": "it"}):
            self.assertNotIn("SOTC_CAMPAIGN_PAL_LANGUAGE", self.library.game_environment())
        with (patch.object(self.library, "prepare_game", return_value=self.root / "game"),
              patch("campaign_locale.prepare_portuguese") as localize,
              patch("gpu.prefer_discrete_gpu"),
              patch("model.subprocess.Popen") as start):
            self.library.launch()
        localize.assert_not_called()
        self.assertEqual(start.call_args.args[0][-1], str(original))
        environment = start.call_args.kwargs["env"]
        self.assertEqual(environment["SOTC_CAMPAIGN_PAL_LANGUAGE"], "de")
        self.assertEqual(Path(environment["PS2X_STATE_DIR"]).parts[-2:], ("SCES-53326", "de"))
