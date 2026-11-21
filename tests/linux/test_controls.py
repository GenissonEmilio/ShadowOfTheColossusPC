import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/desktop'))
from controls import Controls, DEFAULTS
from model import Library


class DesktopControlsTests(unittest.TestCase):
    def test_fresh_profile_and_custom_profile_survive_launcher_restart(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Library(Path(tmp), Path(tmp)/'program')
            profile = Controls(library)
            self.assertEqual(profile.load(), DEFAULTS)
            values = profile.load()
            values.update(attack=66, mouse_camera=0, invert_y=1, sensitivity=2.3)
            profile.save(values)
            restarted = Library(Path(tmp), Path(tmp)/'program')
            self.assertEqual(Controls(restarted).load(), values)
            self.assertEqual(restarted.game_environment()['PS2X_DESKTOP_INPUT'], str(profile.path))

    def test_failed_or_invalid_edit_keeps_previous_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Controls(Library(Path(tmp), Path(tmp)/'program'))
            profile.save(DEFAULTS)
            before = profile.path.read_bytes()
            for override in ({'jump': 87}, {'attack': 290}, {'attack': 300},
                             {'attack': -8}, {'attack': None}, {'sensitivity': float('nan')}):
                with self.assertRaises(ValueError):
                    profile.save(dict(DEFAULTS, **override))
                self.assertEqual(profile.path.read_bytes(), before)
            with patch('controls.os.replace', side_effect=OSError('disk failure')):
                with self.assertRaises(OSError):
                    profile.save(dict(DEFAULTS, attack=66))
            self.assertEqual(profile.path.read_bytes(), before)
            self.assertEqual(list(profile.path.parent.glob('.controls-*')), [])

    def test_corrupt_values_do_not_disable_default_controls(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile = Controls(Library(Path(tmp), Path(tmp)/'program'))
            profile.path.parent.mkdir(parents=True)
            profile.path.write_text('attack=nan\njump=290\nstart=300\nmove_up=9999999999999\n'
                                    'sensitivity=inf\nselect=garbage\ngrab=-4\n')
            expected = dict(DEFAULTS, grab=-4)
            self.assertEqual(profile.load(), expected)
