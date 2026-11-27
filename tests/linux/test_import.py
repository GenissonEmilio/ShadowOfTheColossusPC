import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import MagicMock, patch
import zipfile
import zlib
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[2] / 'tools/desktop'))
import campaign_locale
from import_disc import (Budget, DiscProfile, ImportError_, candidates, extract_zip,
                         import_disc, inspect_disc, profile_from_config, safe_name, validate_disc)
from model import Library, atomic_json
from pipeline import MODULES, STAGE_COUNT, Pipeline, PreparationProgress, executable_name

class ImportTests(unittest.TestCase):
    def test_portuguese_patch_requires_verified_local_asset(self):
        payload = b'verified translation patch'
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(campaign_locale, 'PATCH_SIZE', len(payload)), \
                patch.object(campaign_locale, 'PATCH_SHA256', hashlib.sha256(payload).hexdigest()):
            asset = Path(temporary) / 'pt-BR.xdelta'
            with self.assertRaises(ImportError_):
                campaign_locale._load_patch(asset)
            asset.write_bytes(payload)
            self.assertEqual(campaign_locale._load_patch(asset), payload)
            for invalid in (payload.upper(), payload + b'extra', payload[:-1]):
                asset.write_bytes(invalid)
                with self.subTest(invalid=invalid), self.assertRaises(ImportError_):
                    campaign_locale._load_patch(asset)

    def test_portuguese_prepares_from_imported_files_and_reuses_cache(self):
        def window(position, literal, checksum_output):
            # A source COPY of seven bytes, then an ADD of a one-byte literal.
            instructions = b'\x13\x07\x02'
            checksum = zlib.adler32(checksum_output).to_bytes(4, 'big')
            body = b'\x08\x00\x01\x03\x01' + checksum + literal + instructions + b'\0'
            return bytes((5, 8, position, len(body))) + body

        patch_data = (b'\xd6\xc3\xc4\x00\x04\x00' + window(0, b'E', b'metacorE')
                      + window(8, b'O', b'\0\0\0\0nicO') + window(16, b'?', b'ignored!'))
        extents = {'GAMECORE.XFF': (1, 4), 'NICO.DAT': (3, 4), 'SYSTEM.CNF': (4, 2)}
        replacements = {name: hashlib.sha256(data).hexdigest() for name, data in
                        {'GAMECORE.XFF': b'corE', 'NICO.DAT': b'nicO'}.items()}
        with tempfile.TemporaryDirectory() as temporary, \
                patch.multiple(campaign_locale, SECTOR_SIZE=4, ISO_SIZE=24, ISO_EXTENTS=extents,
                               PATCH_SIZE=len(patch_data), PATCH_SHA256=hashlib.sha256(patch_data).hexdigest(),
                               FIRST_WINDOW_CHECKSUM=zlib.adler32(b'\0\0\0\0corE'),
                               REPLACEMENTS=replacements, REQUIRED_SIZES={'GAMECORE.XFF': 4, 'NICO.DAT': 4}), \
                patch.object(campaign_locale, 'validate_disc', return_value=campaign_locale.USA), \
                patch.object(campaign_locale, '_clone_nico', return_value=False):
            root = Path(temporary)
            original = root / 'imported'
            original.mkdir()
            asset = root / 'pt-BR.xdelta'
            asset.write_bytes(patch_data)
            contents = {'GAMECORE.XFF': b'core', 'NICO.DAT': b'nico', 'SYSTEM.CNF': b'cn'}
            for name, data in contents.items():
                (original / name).write_bytes(data)
            target = root / 'campaign' / 'pt-BR'

            self.assertEqual(campaign_locale.prepare_portuguese(original, target, asset), target / campaign_locale.BOOT)
            self.assertEqual((target / 'GAMECORE.XFF').read_bytes(), b'corE')
            self.assertEqual((target / 'NICO.DAT').read_bytes(), b'nicO')
            for name, data in contents.items():
                self.assertEqual((original / name).read_bytes(), data)

            corrupt_target = target.with_name('broken')
            with patch.object(campaign_locale, 'REPLACEMENTS', {**replacements, 'NICO.DAT': 'invalid digest'}):
                with self.assertRaises(ImportError_):
                    campaign_locale.prepare_portuguese(original, corrupt_target, asset)
            self.assertFalse(corrupt_target.exists())
            self.assertFalse(list(target.parent.glob('.campaign-ptbr-*')))

            (original / 'NICO.DAT').write_bytes(b'Xico')
            with self.assertRaises(ImportError_):
                campaign_locale.prepare_portuguese(original, corrupt_target, asset)
            self.assertFalse(corrupt_target.exists())

            asset.unlink()
            self.assertEqual(campaign_locale.prepare_portuguese(original, target, asset), target / campaign_locale.BOOT)

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

    def test_boot_setting_identifies_the_disc(self):
        self.assertEqual(profile_from_config(b'BOOT2 = cdrom0:\\SCUS_974.72;1\r\n').serial, 'SCUS-97472')
        self.assertEqual(profile_from_config(b'boot2 = CDROM0:\\sces_533.26;1\r\n').serial, 'SCES-53326')
        invalid = (
            b'COMMENT = cdrom0:\\SCES_533.26;1\r\n',
            b'BOOT2 = cdrom0:\\SCES_533.26;1\r\nBOOT2 = cdrom0:\\SCUS_974.72;1\r\n',
            b'BOOT2 = cdrom0:\\SLUS_123.45;1\r\n',
        )
        for config in invalid:
            with self.subTest(config=config), self.assertRaises(ImportError_):
                profile_from_config(config)

    def test_pal_iso_checks_its_own_binary_and_all_asset_parts(self):
        config = b'BOOT2 = cdrom0:\\SCES_533.26;1\r\n'
        boot = b'pal boot fixture'
        profile = DiscProfile('SCES-53326', 'SCES_533.26', {
            'SCES_533.26': (len(boot), hashlib.sha256(boot).hexdigest()),
        }, {'NICO.DAT': 6144, 'XAE.': 2100})
        files = {'SYSTEM.CNF': (20, len(config)), 'SCES_533.26': (21, len(boot)),
                 'NICO.DAT': (22, 6144), 'XAE.': (25, 2100)}
        image = MagicMock()
        image.sector, image.skip = 2048, 0
        image.find.side_effect = files.get
        image.read.side_effect = lambda sector, size: {20: config, 21: boot}[sector]
        with patch('import_disc.DiscImage') as disc, patch.dict('import_disc.PROFILES', {'SCES-53326': profile}):
            disc.return_value.__enter__.return_value = image
            self.assertEqual(inspect_disc('/synthetic-pal.iso'), {'serial': profile.serial, 'boot': profile.boot})
            self.assertEqual([call.args[0] for call in image.read_sector.call_args_list], [24, 26])
            files['XAE.'] = (26, 2100)
            with self.assertRaisesRegex(ImportError_, r'XAE\.'):
                inspect_disc('/synthetic-pal.iso')
            files.pop('XAE.')
            with self.assertRaisesRegex(ImportError_, r'XAE\.'):
                inspect_disc('/synthetic-pal.iso')

    def test_pal_cached_files_reject_modified_binary_and_incomplete_payload(self):
        boot = b'pal boot fixture'
        profile = DiscProfile('SCES-53326', 'SCES_533.26', {
            'SCES_533.26': (len(boot), hashlib.sha256(boot).hexdigest()),
        }, {'NICO.DAT': 1, 'XAE.': 1})
        with tempfile.TemporaryDirectory() as temporary, patch.dict('import_disc.PROFILES', {profile.serial: profile}):
            path = Path(temporary)
            (path / 'SYSTEM.CNF').write_bytes(b'BOOT2 = cdrom0:\\SCES_533.26;1\r\n')
            (path / profile.boot).write_bytes(boot)
            (path / 'NICO.DAT').write_bytes(b'n')
            with self.assertRaisesRegex(ImportError_, r'NICO\.DAT'):
                validate_disc(path)
            (path / 'NICO.DAT').write_bytes(b'nx')
            self.assertEqual(validate_disc(path), profile)
            (path / profile.boot).write_bytes(boot.upper())
            with self.assertRaisesRegex(ImportError_, r'SCES_533\.26'):
                validate_disc(path)

    def test_pal_import_writes_a_single_contiguous_archive(self):
        config = b'BOOT2 = cdrom0:\\SCES_533.26;1\r\n'
        boot = b'pal boot fixture'
        first_part, second_part = b'n' * 2048, b'x' * 65
        profile = DiscProfile('SCES-53326', 'SCES_533.26', {
            'SCES_533.26': (len(boot), hashlib.sha256(boot).hexdigest()),
        }, {'NICO.DAT': len(first_part), 'XAE.': len(second_part)})
        files = {'SYSTEM.CNF': (20, len(config)), profile.boot: (21, len(boot)),
                 'NICO.DAT': (22, len(first_part)), 'XAE.': (23, len(second_part))}
        sectors = {20: config, 21: boot, 22: first_part, 23: second_part}
        image = MagicMock()
        image.sector, image.skip = 2048, 0
        image.find.side_effect = files.get
        image.read.side_effect = lambda sector, size: sectors[sector]
        image.read_sector.return_value = bytes(2048)
        image._directory.return_value = [(name, sector, size, False) for name, (sector, size) in files.items()]
        image.file = io.BytesIO(bytes(20 * 2048) + b''.join(data.ljust(2048, b'\0') for data in sectors.values()))
        with tempfile.TemporaryDirectory() as temporary, patch('import_disc.DiscImage') as disc, \
                patch.dict('import_disc.PROFILES', {profile.serial: profile}):
            root = Path(temporary)
            destination = root / 'disc'
            disc.return_value.__enter__.return_value = image
            self.assertEqual(import_disc(root / 'disc.iso', destination), destination / profile.boot)
            self.assertEqual((destination / 'NICO.DAT').read_bytes(), first_part + second_part)
            self.assertFalse((destination / 'XAE.').exists())
            self.assertFalse(list(root.glob('.import-*')))

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

    def test_preparation_progress_counts_all_stages_and_reuses_batches(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            kit = root / 'compiler'
            (kit / 'source/game').mkdir(parents=True)
            (kit / 'source/game/scus.toml').write_text('input = "old"\n')
            (kit / 'manifest.json').write_text(json.dumps({'build_id': 'test', 'link_inputs': ['libbase.a']}))
            statuses = []
            pipeline = Pipeline(root, root / 'data', {'SOTC_BUILD_BATCH_SIZE': '1', 'SOTC_BUILD_JOBS': '1'}, statuses.append)
            compiles = []

            def run(args, cwd=None, env=None):
                if args[0] == pipeline.tool('ps2_recomp'):
                    output = cwd / 'output'
                    output.mkdir(exist_ok=True)
                    for index in range(3):
                        (output / f'{index}.cpp').write_text(f'// block {index}\n')
                elif '-c' in args:
                    compiles.append(args[-1])
                    args[-1].touch()
                else:
                    objects = [arg.name for arg in args if isinstance(arg, Path) and arg.suffix == '.o']
                    response = next((Path(arg[1:]) for arg in args if isinstance(arg, str) and arg.startswith('@')), None)
                    if response:
                        objects = [Path(line.strip('"')).name for line in response.read_text().splitlines()
                                   if line.endswith('.o"')]
                    self.assertEqual(objects[:3], ['batch-0000.o', 'batch-0001.o', 'batch-0002.o'])
                    args[-1].write_bytes(b'fixture executable')

            with patch.object(pipeline, 'run', side_effect=run), patch.object(pipeline, 'helper'), \
                 patch.object(pipeline, 'capture', return_value=pipeline.work / 'capture.bin'):
                result = pipeline.prepare(root / 'disc')
                self.assertEqual(result.name, executable_name('game'))
                self.assertEqual(len(compiles), STAGE_COUNT * 3)
                stages = [event for event in statuses if isinstance(event, PreparationProgress)]
                self.assertEqual(len({event.title for event in stages}), STAGE_COUNT)
                for index, module in enumerate(MODULES, 2):
                    self.assertTrue(any(f'{index}/{STAGE_COUNT} · {module}' in event.title for event in stages))
                self.assertEqual([event.fraction for event in stages], sorted(event.fraction for event in stages))
                self.assertTrue(any('1/3' in event.detail and ' s' in event.detail for event in stages))
                self.assertEqual(stages[-1].fraction, 1)

                statuses.clear()
                self.assertEqual(pipeline.prepare(root / 'disc'), result)
                self.assertEqual(len(statuses), 1)
                self.assertIs(type(statuses[0]), str)

                (pipeline.work / 'complete.json').unlink()
                statuses.clear()
                self.assertEqual(pipeline.prepare(root / 'disc'), result)
                self.assertEqual(len(compiles), STAGE_COUNT * 3)

    def test_frozen_gui_environment_does_not_leak_into_game(self):
        with patch.dict(os.environ,{'LD_LIBRARY_PATH':'/tmp/frozen','GI_TYPELIB_PATH':'/tmp/frozen','LP_NUM_THREADS':'2'}):
            env=Library('/tmp/sotc-test','/tmp/program').game_environment()
            self.assertEqual(env['LD_LIBRARY_PATH'],'/tmp/program/compiler/lib')
            self.assertNotIn('GI_TYPELIB_PATH',env)
            self.assertEqual(env['SOTC_NATIVE'],'1')
            self.assertTrue(env['PS2X_MEMCARD_DIR'].endswith('/player/memcards'))

if __name__=='__main__':unittest.main()
