import hashlib
from http.client import HTTPMessage
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
from urllib.error import HTTPError
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


class MetadataResponse(io.BytesIO):
    def __init__(self, data, url="https://release-assets.githubusercontent.com/manifest"):
        super().__init__(json.dumps(data).encode())
        self.url = url

    def geturl(self):
        return self.url


def http_error(url, code, headers=None, content=b""):
    message = HTTPMessage()
    for name, value in (headers or {}).items():
        message[name] = value
    return HTTPError(url, code, "test response", message, io.BytesIO(content))


class UpdateTests(unittest.TestCase):
    def test_linux_context_uses_system_bundle_and_respects_explicit_trust(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle = Path(directory) / "ca-bundle.pem"
            bundle.touch()
            with (patch("updates.sys.platform", "linux"),
                  patch("updates.LINUX_CA_BUNDLES", (bundle.with_name("missing.pem"), bundle)),
                  patch("updates.ssl.create_default_context") as create,
                  patch.dict(os.environ, {}, clear=True)):
                self.assertIs(updates.https_context(), create.return_value)
                create.return_value.load_verify_locations.assert_called_once_with(cafile=str(bundle))
                for override in ("SSL_CERT_FILE", "SSL_CERT_DIR"):
                    create.return_value.reset_mock()
                    with patch.dict(os.environ, {override: str(bundle)}):
                        updates.https_context()
                    create.return_value.load_verify_locations.assert_not_called()

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

    @patch("updates.platform_id", return_value=LINUX)
    @patch("updates.https_context")
    def test_manifest_is_sufficient_for_update_and_current_version(self, context, _platform):
        latest = release("1.0.3", LINUX, draft=False, prerelease=False)
        for current, expected in (("1.0.2", "1.0.3"), ("1.0.3", None), ("1.0.4", None)):
            with (self.subTest(current=current),
                  patch("updates.urlopen", return_value=MetadataResponse(latest)) as open_url):
                result = updates.check_for_update(current)
                self.assertEqual(result.version if result else None, expected)
                if result:
                    self.assertEqual((result.system, result.size, result.sha256), (LINUX, 1200, "a" * 64))
                open_url.assert_called_once()
                self.assertEqual(open_url.call_args.args[0].full_url, updates.MANIFEST)
                self.assertIs(open_url.call_args.kwargs["context"], context.return_value)

    @patch("updates.platform_id", return_value=LINUX)
    @patch("updates.https_context")
    def test_api_fallback_for_missing_manifest_or_platform(self, _context, _platform):
        latest = release("1.0.4", WINDOWS, draft=False, prerelease=False)
        for primary in (http_error(updates.MANIFEST, 404), MetadataResponse(latest)):
            with patch("updates.urlopen", side_effect=[primary, MetadataResponse(
                    [release("1.0.3", LINUX)], updates.RELEASES)]) as open_url:
                self.assertEqual(updates.check_for_update("1.0.2").version, "1.0.3")
                self.assertEqual([call.args[0].full_url for call in open_url.call_args_list],
                                 [updates.MANIFEST, updates.RELEASES])

    @patch("updates.platform_id", return_value=LINUX)
    @patch("updates.https_context")
    def test_manifest_refuses_untrusted_redirect_or_invalid_package(self, _context, _platform):
        latest = release("1.0.3", LINUX, draft=False, prerelease=False)
        for url in ("http://github.com/manifest", "https://github.com.example.org/manifest"):
            with (patch("updates.urlopen", return_value=MetadataResponse(latest, url)) as open_url,
                  self.assertRaisesRegex(ValueError, "must come from GitHub")):
                updates.check_for_update("1.0.2")
            open_url.assert_called_once()
        for field, invalid in (("digest", None), ("size", True), ("state", "new"),
                               ("browser_download_url", "https://github.com/example/package")):
            invalid_manifest = release("1.0.3", LINUX, draft=False, prerelease=False)
            invalid_manifest["assets"][0][field] = invalid
            with (self.subTest(field=field),
                  patch("updates.urlopen", return_value=MetadataResponse(invalid_manifest)) as open_url,
                  self.assertRaisesRegex(ValueError, "invalid package metadata")):
                updates.check_for_update("1.0.2")
            open_url.assert_called_once()

    @patch("updates.platform_id", return_value=LINUX)
    @patch("updates.https_context")
    def test_distinguishes_rate_limited_fallback_from_forbidden(self, _context, _platform):
        for code, headers, content, expected in (
                (403, {"X-RateLimit-Remaining": "0"}, b"", updates.RateLimitedError),
                (403, {"Retry-After": "60"}, b"", updates.RateLimitedError),
                (403, {}, b'{"message": "You have exceeded a secondary rate limit."}', updates.RateLimitedError),
                (429, {}, b"", updates.RateLimitedError),
                (403, {}, b'{"message": "Forbidden"}', HTTPError)):
            responses = [http_error(updates.MANIFEST, 404), http_error(updates.RELEASES, code, headers, content)]
            with (self.subTest(code=code, headers=headers, content=content),
                  patch("updates.urlopen", side_effect=responses), self.assertRaises(expected)):
                updates.check_for_update("1.0.2")

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
    def test_launcher_presentation_and_sound_preferences(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            defaults = language.preferences(folder)
            self.assertFalse(defaults["launcher_fullscreen"])
            self.assertTrue(defaults["ui_sounds"])
            self.assertEqual(defaults["button_prompts"], "auto")
            self.assertFalse(defaults["infinite_health"])
            self.assertFalse(defaults["infinite_grip"])
            atomic_json(folder / language.PREFERENCES, {"language": "es", "button_prompts": "xbox",
                                                         "launcher_fullscreen": True, "ui_sounds": False})
            selected = language.preferences(folder)
        self.assertEqual(selected, {"language": "es", "campaign_languages": {}, "button_prompts": "xbox",
                                        "launcher_fullscreen": True, "ui_sounds": False,
                                        "infinite_health": False, "infinite_grip": False})

    def test_prompt_style_reaches_the_game_without_changing_disc_state(self):
        with tempfile.TemporaryDirectory() as directory:
            library = Library(directory)
            self.assertEqual(library.game_environment()["SOTC_BUTTON_PROMPTS"], "auto")
            atomic_json(library.directory / language.PREFERENCES, {"language": "ja", "button_prompts": "xbox"})
            self.assertEqual(library.game_environment()["SOTC_BUTTON_PROMPTS"], "xbox")
            self.assertEqual(language.preferences(library.directory)["language"], "ja")
            self.assertFalse(library.config.exists())

    def test_modes_and_assists_are_strict_and_isolated_from_game_progress(self):
        with tempfile.TemporaryDirectory() as directory:
            library = Library(directory)
            for mode in language.PROMPTS:
                atomic_json(library.directory / language.PREFERENCES,
                            {"button_prompts": mode, "infinite_health": True, "infinite_grip": False})
                env = library.game_environment()
                self.assertEqual(env["SOTC_BUTTON_PROMPTS"], mode)
                self.assertEqual(env["SOTC_INFINITE_HEALTH"], "1")
                self.assertEqual(env["SOTC_INFINITE_GRIP"], "0")
            atomic_json(library.directory / language.PREFERENCES,
                        {"button_prompts": "bad", "infinite_health": "true", "infinite_grip": 1})
            self.assertEqual(language.preferences(library.directory)["button_prompts"], "auto")
            with patch.dict(os.environ, {"SOTC_INFINITE_HEALTH": "1", "SOTC_INFINITE_GRIP": "1"}):
                env = library.game_environment()
            self.assertEqual((env["SOTC_INFINITE_HEALTH"], env["SOTC_INFINITE_GRIP"]), ("0", "0"))
            self.assertFalse(library.config.exists())
            self.assertFalse(library.player_directory.exists())

    def test_catalog_placeholders_are_preserved(self):
        fields = lambda message: {field for _, field, _, _ in Formatter().parse(message) if field}
        for source, translations in language.catalog().items():
            self.assertEqual(set(translations), {"en", "es", "ja"})
            for translation in translations.values():
                self.assertTrue(translation.strip())
                self.assertEqual(fields(source), fields(translation), source)
