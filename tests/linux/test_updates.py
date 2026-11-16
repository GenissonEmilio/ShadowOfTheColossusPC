import hashlib
import io
import json
import os
from pathlib import Path
from string import Formatter
import sys
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/desktop"))
import language
from model import Library, atomic_json
import updates


LINUX = "linux-x86_64"
WINDOWS = "windows-x86_64"


def release(version, system, **flags):
    suffix = "zip" if system == WINDOWS else "tar.gz"
    name = f"{updates.NAME}-{version}-{system}.{suffix}"
    tag = f"v{version}"
    url = f"https://github.com/{updates.REPOSITORY}/releases/download/{tag}/{name}"
    return {"tag_name": tag, "assets": [{"name": name, "state": "uploaded",
            "browser_download_url": url, "size": 1200, "digest": "sha256:" + "a" * 64}], **flags}


class UpdateTests(unittest.TestCase):
    def test_version_platform_origin_and_digest(self):
        candidates = [release("1.0.2", WINDOWS), release("1.0.9", LINUX),
                      release("1.0.10", LINUX), release("1.1.0", LINUX, prerelease=True),
                      release("1.2.0", LINUX, draft=True)]
        external = release("2.0.0", LINUX)
        external["assets"][0]["browser_download_url"] = "https://example.invalid/archive"
        missing_digest = release("2.1.0", LINUX)
        missing_digest["assets"][0]["digest"] = None
        candidates.extend((external, missing_digest))
        self.assertEqual(updates.select_update(candidates, "1.0.8", LINUX).version, "1.0.10")
        self.assertEqual(updates.select_update(candidates, "1.0.1", WINDOWS).version, "1.0.2")
        self.assertIsNone(updates.select_update(candidates, "1.0.10", LINUX))

    def test_stage_and_exchange_keeps_stable_path_and_user_data(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            program, data = root / "installed", root / "player"
            program.mkdir()
            data.mkdir()
            card = data / "memory-card"
            card.write_bytes(b"save")
            (program / "release.json").write_text(json.dumps({"version": "1.0.0", "platform": LINUX}))
            executable = program / updates.launcher_filename(LINUX)
            executable.write_bytes(b"old")
            shortcut = root / "shortcut"
            shortcut.symlink_to(program, target_is_directory=True)
            archive = root / "fixture.tar.gz"
            new_launcher = b'#!/bin/sh\nprintf ready > "$SOTC_UPDATE_READY"\n'
            with tarfile.open(archive, "w:gz") as bundle:
                for name, payload in ((executable.name, new_launcher), ("release.json", json.dumps(
                        {"version": "1.0.1", "platform": LINUX}).encode())):
                    item = tarfile.TarInfo(f"{updates.NAME}/{name}")
                    item.size, item.mode = len(payload), 0o755 if name == executable.name else 0o644
                    bundle.addfile(item, io.BytesIO(payload))
            payload = archive.read_bytes()
            update = updates.Update("1.0.1", LINUX, archive.name, "https://github.com/fixture",
                                    len(payload), hashlib.sha256(payload).hexdigest())

            class Response(io.BytesIO):
                def geturl(self):
                    return update.url

            with patch("updates.urlopen", return_value=Response(payload[:-1] + bytes([payload[-1] ^ 1]))):
                with self.assertRaisesRegex(ValueError, "SHA-256"):
                    updates.stage_update(program, data, "1.0.0", update, lambda *_: None)
            self.assertEqual(list(root.glob(".sotc-update-*")), [])

            with patch("updates.urlopen", return_value=Response(payload)):
                staged = updates.stage_update(program, data, "1.0.0", update, lambda *_: None)
            with patch("updates.subprocess.Popen", side_effect=OSError("invalid executable")):
                with self.assertRaisesRegex(OSError, "invalid executable"):
                    updates.restart_with_update(staged, program, data, "1.0.0", LINUX, root)
            self.assertEqual(executable.read_bytes(), b"old")
            with patch("updates.urlopen", return_value=Response(payload)):
                staged = updates.stage_update(program, data, "1.0.0", update, lambda *_: None)
            (staged / executable.name).write_bytes(b"#!/bin/sh\nexit 33\n")
            with self.assertRaisesRegex(RuntimeError, "exited during startup"):
                updates.restart_with_update(staged, program, data, "1.0.0", LINUX, root)
            self.assertEqual(executable.read_bytes(), b"old")
            with patch("updates.urlopen", side_effect=lambda *_args, **_kw: Response(payload)):
                staged = updates.stage_update(program, data, "1.0.0", update, lambda *_: None)
                stale = updates.stage_update(program, data, "1.0.0", update, lambda *_: None)
            # Keep a reference to the short-lived test process so the test
            # can reap it. The production launcher is free to stay open.
            original_popen = subprocess.Popen
            launched = []
            def launch(*args, **kwargs):
                process = original_popen(*args, **kwargs)
                launched.append(process)
                return process
            with patch("updates.subprocess.Popen", side_effect=launch):
                updates.restart_with_update(staged, program, data, "1.0.0", LINUX, root)
            self.assertEqual(launched[0].wait(timeout=2), 0)
            self.assertEqual((shortcut / executable.name).read_bytes(), new_launcher)
            with self.assertRaisesRegex(ValueError, "official portable package"):
                updates.restart_with_update(stale, program, data, "1.0.0", LINUX, root)
            self.assertTrue(os.access(executable, os.X_OK))
            self.assertEqual(card.read_bytes(), b"save")
            backup, = root.glob(".installed.previous-*")
            self.assertEqual((backup / executable.name).read_bytes(), b"old")
            self.assertEqual(list(root.glob(".sotc-update-*")), [])

    def test_rejects_archive_traversal_links_and_windows_aliases(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "unsafe.tar.gz"
            with tarfile.open(archive, "w:gz") as bundle:
                entry = tarfile.TarInfo(updates.NAME + "/linked")
                entry.type = tarfile.SYMTYPE
                entry.linkname = "../../escape"
                bundle.addfile(entry)
            with self.assertRaisesRegex(ValueError, "link"):
                updates.unpack(archive, root, LINUX)
            archive = root / "unsafe.zip"
            for path in ("../escape", updates.NAME + "/file:stream", updates.NAME + "/AUX.txt"):
                with zipfile.ZipFile(archive, "w") as bundle:
                    bundle.writestr(path, "bad")
                with self.assertRaisesRegex(ValueError, "Invalid path"):
                    updates.unpack(archive, root, WINDOWS)


class PreferencesTests(unittest.TestCase):
    def test_prompt_style_reaches_the_game_without_changing_disc_state(self):
        with tempfile.TemporaryDirectory() as directory:
            library = Library(directory)
            self.assertEqual(library.game_environment()["SOTC_BUTTON_PROMPTS"], "playstation")
            atomic_json(library.directory / language.PREFERENCES, {"language": "ja", "button_prompts": "xbox"})
            self.assertEqual(library.game_environment()["SOTC_BUTTON_PROMPTS"], "xbox")
            self.assertEqual(language.preferences(library.directory)["language"], "ja")
            self.assertFalse(library.config.exists())

    def test_catalog_placeholders_are_preserved(self):
        fields = lambda message: {field for _, field, _, _ in Formatter().parse(message) if field}
        for source, translations in language.catalog().items():
            self.assertEqual(set(translations), {"en", "es", "ja"})
            for translation in translations.values():
                self.assertTrue(translation.strip())
                self.assertEqual(fields(source), fields(translation), source)
