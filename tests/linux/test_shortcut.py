"""Real .desktop generation in owned temporary directories; no GTK/game/build."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

DESKTOP = Path(__file__).resolve().parents[2] / 'tools/desktop'
sys.path.insert(0, str(DESKTOP))
source = Path(os.environ.get('SOTC_SHORTCUT_MODEL_SOURCE', str(DESKTOP / 'model.py')))
spec = importlib.util.spec_from_file_location('shortcut_model_under_test', source)
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)


class ShortcutTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='sotc-shortcut-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.home = self.root / 'home'
        self.canonical = self.home / '.local/opt/ShadowOfTheColossusPC'
        self.canonical.parent.mkdir(parents=True)
        self.program = self.canonical.parent / 'ShadowOfTheColossusPC-0.2.7'
        self.executable, self.assets = self.make_program(self.program)

    def make_program(self, directory):
        directory.mkdir(parents=True)
        executable = directory / 'ShadowOfTheColossusPC'
        executable.write_text('test launcher; never execute\n')
        assets = directory / '_internal/assets'
        assets.mkdir(parents=True)
        (assets / 'icon.png').write_text(str(directory))
        return executable, assets

    def entry(self, executable=None, assets=None, frozen=True):
        executable, assets = executable or self.executable, assets or self.assets
        with patch.object(model.sys, 'frozen', frozen, create=True), \
             patch.object(model.sys, 'executable', str(executable)), \
             patch.object(model.Path, 'home', return_value=self.home), \
             patch.object(model, 'assets_directory', return_value=assets):
            library = model.Library(self.root / 'data', executable.parent)
            path = library.install_desktop_entry(self.root / 'applications')
        return path, dict(line.split('=', 1) for line in path.read_text().splitlines() if '=' in line)

    def test_installed_entry_follows_later_canonical_switch_without_rewrite(self):
        self.canonical.symlink_to(self.program.name, target_is_directory=True)
        path, entry = self.entry()
        self.assertEqual(entry['Exec'], f'"{self.canonical / self.executable.name}"')
        self.assertEqual(entry['Icon'], str(self.canonical / '_internal/assets/icon.png'))
        self.assertEqual(path.stat().st_mode & 0o777, 0o755)
        before = path.read_bytes()
        next_program = self.program.with_name('ShadowOfTheColossusPC-0.2.8')
        next_executable, next_assets = self.make_program(next_program)
        self.canonical.unlink()  # only this test's temporary link
        self.canonical.symlink_to(next_program.name, target_is_directory=True)
        self.assertEqual(Path(entry['Exec'][1:-1]).resolve(), next_executable)
        self.assertEqual(Path(entry['Icon']).resolve(), next_assets / 'icon.png')
        self.assertEqual(path.read_bytes(), before)

    def test_portable_copy_does_not_adopt_another_installation(self):
        self.canonical.symlink_to(self.program.name, target_is_directory=True)
        portable, assets = self.make_program(self.root / 'portable')
        _, entry = self.entry(portable, assets)
        self.assertEqual(entry['Exec'], f'"{portable}"')
        self.assertEqual(entry['Icon'], str(assets / 'icon.png'))

    def test_older_running_version_is_not_redirected_to_different_target(self):
        next_program = self.program.with_name('ShadowOfTheColossusPC-0.2.8')
        self.make_program(next_program)
        self.canonical.symlink_to(next_program.name, target_is_directory=True)
        _, entry = self.entry()
        self.assertEqual(entry['Exec'], f'"{self.executable}"')
        self.assertEqual(entry['Icon'], str(self.assets / 'icon.png'))

    def test_missing_broken_or_cyclic_link_preserves_own_path(self):
        for target in (None, 'missing', self.canonical.name):
            with self.subTest(target=target):
                if self.canonical.is_symlink(): self.canonical.unlink()
                if target is not None: self.canonical.symlink_to(target, target_is_directory=True)
                _, entry = self.entry()
                self.assertEqual(entry['Exec'], f'"{self.executable}"')

    def test_external_assets_remain_external(self):
        self.canonical.symlink_to(self.program.name, target_is_directory=True)
        external = self.root / 'onefile-assets'
        external.mkdir()
        (external / 'icon.png').write_text('external')
        _, entry = self.entry(assets=external)
        self.assertEqual(entry['Exec'], f'"{self.canonical / self.executable.name}"')
        self.assertEqual(entry['Icon'], str(external / 'icon.png'))

    def test_source_mode_remains_python_plus_script(self):
        self.canonical.symlink_to(self.program.name, target_is_directory=True)
        _, entry = self.entry(frozen=False)
        self.assertEqual(entry['Exec'], f'"{self.executable}" "{model.SOURCE_ROOT / "tools/desktop/main.py"}"')

    def test_canonical_exec_preserves_desktop_escaping(self):
        self.home = self.root / 'home space %$`"\\'
        self.canonical = self.home / '.local/opt/ShadowOfTheColossusPC'
        self.canonical.parent.mkdir(parents=True)
        self.program = self.canonical.parent / 'ShadowOfTheColossusPC-0.2.7'
        self.executable, self.assets = self.make_program(self.program)
        self.canonical.symlink_to(self.program.name, target_is_directory=True)
        _, entry = self.entry()
        self.assertIn('home space %%\\$\\`\\"\\\\/.local/opt/ShadowOfTheColossusPC/', entry['Exec'])
        self.assertTrue(entry['Exec'].startswith('"') and entry['Exec'].endswith('"'))
        self.assertNotIn('0.2.7', entry['Exec'])


if __name__ == '__main__': unittest.main()
