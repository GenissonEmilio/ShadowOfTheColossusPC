import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/desktop'))
sys.path.insert(0, str(ROOT / 'tools/linux'))
from model import Library
from compiler_key import compiler_key


class ControllerTests(unittest.TestCase):
    def test_saved_controller_profile_is_passed_to_the_game(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Library(Path(tmp) / 'player', Path(tmp) / 'program')
            profile = library.directory / 'controllers/gamecontrollerdb.txt'
            profile.parent.mkdir(parents=True)
            profile.write_text('synthetic profile fixture\n')
            env = library.game_environment()
            self.assertEqual(env['PS2X_GAMEPAD_MAPPINGS'], str(profile))
            self.assertEqual(profile.read_text(), 'synthetic profile fixture\n')

    def test_environment_mapping_survives_when_no_saved_profile_exists(self):
        with tempfile.TemporaryDirectory() as tmp:
            with patch.dict(os.environ, {'PS2X_GAMEPAD_MAPPINGS': '/tmp/custom-controller.txt'}):
                env = Library(Path(tmp) / 'data', Path(tmp) / 'program').game_environment()
            self.assertEqual(env['PS2X_GAMEPAD_MAPPINGS'], '/tmp/custom-controller.txt')

    def test_setup_tool_update_keeps_native_game_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            kit = Path(tmp)
            (kit / 'bin').mkdir()
            (kit / 'lib').mkdir()
            runtime = kit / 'lib/runtime.a'
            runtime.write_bytes(b'fixture runtime')
            before = compiler_key(kit)
            (kit / 'bin/sotc-input-tool').write_bytes(b'fixture setup program')
            self.assertEqual(before, compiler_key(kit))
            runtime.write_bytes(b'changed fixture runtime')
            self.assertNotEqual(before, compiler_key(kit))
