"""Tiny filesystem fixture; no compiler, shared kit or game execution."""
from pathlib import Path
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools/linux'))
from stage_source import stage_source
import stage_source as candidate
import compiler_key as keys


class StageSourceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='.fixture-', dir=HERE)
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.root, self.build, self.out = (self.base/n for n in ('repo', 'build', 'kit'))
        self.out.mkdir()
        for n in ('PS2Recomp', 'game', 'tools', 'tests', 'docs'):
            (self.root/n).mkdir(parents=True)
            (self.root/n/'current.txt').write_text(n)
        for n in ('LICENSE', 'README.md', 'celosia'):
            (self.root/n).write_text(n)
        for n in ('raylib.h', 'raymath.h', 'rlgl.h'):
            p = self.build/'_deps/raylib-src/src'/n
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(n)
        (self.out/'lib').mkdir()
        (self.out/'lib/keep.a').write_bytes(b'unchanged dependency')

    def private_paths(self):
        return list(self.out.glob('.source-*'))

    def test_removed_source_disappears_and_key_matches_fresh_stage(self):
        removed = self.root/'game/removed.cpp'
        removed.write_text('old source A')
        stage_source(self.root, self.build, self.out)
        old_key = keys.compiler_key(self.out)
        removed.unlink()
        stage_source(self.root, self.build, self.out)
        self.assertFalse((self.out/'source/game/removed.cpp').exists())
        fresh = self.base/'fresh'
        fresh.mkdir()
        (fresh/'lib').mkdir()
        (fresh/'lib/keep.a').write_bytes(b'unchanged dependency')
        stage_source(self.root, self.build, fresh)
        self.assertEqual(keys.compiler_key(self.out), keys.compiler_key(fresh))
        self.assertNotEqual(old_key, keys.compiler_key(self.out))
        self.assertEqual((self.out/'source/docs/current.txt').read_text(), 'docs')
        self.assertEqual((self.out/'lib/keep.a').read_bytes(), b'unchanged dependency')
        self.assertEqual(self.private_paths(), [])

    def test_copy_failure_keeps_previous_tree_and_dependencies(self):
        stage_source(self.root, self.build, self.out)
        before = keys.compiler_key(self.out)
        with patch.object(candidate.shutil, 'copy2', side_effect=OSError('injected copy failure')):
            with self.assertRaises(OSError):
                stage_source(self.root, self.build, self.out)
        self.assertEqual(keys.compiler_key(self.out), before)
        self.assertEqual(self.private_paths(), [])

    def test_publication_failure_rolls_back(self):
        stage_source(self.root, self.build, self.out)
        before = keys.compiler_key(self.out)
        original = os.replace
        def rename(source, destination):
            if Path(source).name.startswith('.source-stage-'):
                raise OSError('injected publication failure')
            return original(source, destination)
        with patch.object(candidate.os, 'replace', side_effect=rename):
            with self.assertRaises(OSError):
                stage_source(self.root, self.build, self.out)
        self.assertEqual(keys.compiler_key(self.out), before)
        self.assertEqual(self.private_paths(), [])

    def test_symlink_destination_rejected_without_touching_external_tree(self):
        external = self.base/'external'
        external.mkdir()
        (external/'important').write_text('preserve')
        (self.out/'source').symlink_to(external, target_is_directory=True)
        with self.assertRaises(ValueError):
            stage_source(self.root, self.build, self.out)
        self.assertEqual((external/'important').read_text(), 'preserve')
        self.assertTrue((self.out/'source').is_symlink())
        self.assertEqual(self.private_paths(), [])

    def test_symlink_output_rejected(self):
        alias = self.base/'alias'
        alias.symlink_to(self.out, target_is_directory=True)
        with self.assertRaises(ValueError):
            stage_source(self.root, self.build, alias)


if __name__ == '__main__':
    unittest.main(verbosity=2)
