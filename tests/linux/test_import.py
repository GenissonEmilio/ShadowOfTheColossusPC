import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[2] / 'tools/desktop'))
from import_disc import Budget, ImportError_, candidates, extract_zip, inspect_disc, safe_name
from model import Library, atomic_json
from pipeline import Pipeline

class ImportTests(unittest.TestCase):
    def test_traversal_and_absolute_paths(self):
        for path in ('../escape.iso','/tmp/escape.iso','x/../../bad','C:/escape.iso','x\\..\\bad'):
            with self.subTest(path=path),self.assertRaises(ImportError_):safe_name(path)

    def test_nested_archives_share_a_budget(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);inner=p/'nested.zip'
            with zipfile.ZipFile(inner,'w') as z:z.writestr('game.iso',b'x'*200)
            outer=p/'outer.zip'
            with zipfile.ZipFile(outer,'w') as z:z.write(inner,'nested.zip')
            with self.assertRaises(ImportError_):list(candidates(outer,p,Budget(300)))

    def test_zip_links_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);zpath=p/'link.zip'
            info=zipfile.ZipInfo('game.iso');info.create_system=3;info.external_attr=0o120777<<16
            with zipfile.ZipFile(zpath,'w') as z:z.writestr(info,'/etc/passwd')
            with self.assertRaises(ImportError_):extract_zip(zpath,p,Budget())

    def test_no_program_execution_and_one_candidate(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);archive=p/'game.zip'
            with zipfile.ZipFile(archive,'w') as z:
                z.writestr('instructions.sh','touch /tmp/should-not-run')
                z.writestr('disc.iso',b'own fixture')
            items=list(candidates(archive,p,Budget()))
            self.assertEqual([x.name for x in items],['disc.iso'])

    def test_duplicate_casefold_paths_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);archive=p/'duplicate.zip'
            with zipfile.ZipFile(archive,'w') as z:
                z.writestr('game.iso','one');z.writestr('GAME.ISO','two')
            with self.assertRaises(ImportError_):extract_zip(archive,p,Budget())

    def test_truncated_image_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t)/'truncated.iso';p.write_bytes(b'CD001')
            with self.assertRaises(ValueError):inspect_disc(p)

    def test_config_failure_preserves_previous_selection(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);library=Library(p/'data',p/'program');atomic_json(library.config,{'name':'previous'})
            original=library.config.read_bytes()
            invalid=p/'invalid.iso';invalid.write_bytes(b'not an ISO')
            with self.assertRaises(ValueError):library.prepare(invalid)
            self.assertEqual(library.config.read_bytes(),original)

    def test_cached_executable_is_checked_without_rebuild(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);kit=p/'compiler';kit.mkdir();(kit/'manifest.json').write_text(json.dumps({'build_id':'test','link_inputs':[]}))
            data=p/'data';work=data/'builds/test';(work/'gamecore').mkdir(parents=True)
            exe=work/'gamecore/game';exe.write_bytes(b'local executable fixture')
            (work/'complete.json').write_text(json.dumps({'sha256':hashlib.sha256(exe.read_bytes()).hexdigest()}))
            pipeline=Pipeline(p,data,{})
            with patch.object(pipeline,'stage',side_effect=AssertionError('must not rebuild')):
                self.assertEqual(pipeline.prepare(p/'disc'),exe)
            exe.write_bytes(b'corrupt')
            with patch.object(pipeline,'helper',side_effect=RuntimeError('rebuild required')):
                with self.assertRaisesRegex(RuntimeError,'rebuild required'):pipeline.prepare(p/'disc')

    def test_frozen_gui_environment_does_not_leak_into_game(self):
        with patch.dict(os.environ,{'LD_LIBRARY_PATH':'/tmp/frozen','GI_TYPELIB_PATH':'/tmp/frozen','LP_NUM_THREADS':'2'}):
            env=Library('/tmp/sotc-test','/tmp/program').game_environment()
            self.assertEqual(env['LD_LIBRARY_PATH'],'/tmp/program/compiler/lib')
            self.assertNotIn('GI_TYPELIB_PATH',env)
            self.assertEqual(env['SOTC_NATIVE'],'1')
            self.assertTrue(env['PS2X_MEMCARD_DIR'].endswith('/player/memcards'))

if __name__=='__main__':unittest.main()
