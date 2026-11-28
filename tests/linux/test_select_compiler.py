import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/desktop/release"))
import select_compiler as selector

LINUX, WINDOWS = selector.SYSTEMS
TAG = "v1.2.13"
BUILD_ID = "a" * 24


class Download(io.BytesIO):
    status = 200
    headers = {}

    def geturl(self):
        return "https://release-assets.githubusercontent.com/example"


def package_bytes(system):
    files = {"release.json": {"version": TAG[1:], "platform": system},
             "compiler/manifest.json": {"build_id": BUILD_ID}}
    result = io.BytesIO()
    if system == WINDOWS:
        with zipfile.ZipFile(result, "w") as package:
            for name, value in files.items():
                package.writestr(f"{selector.NAME}/{name}", json.dumps(value))
    else:
        with tarfile.open(fileobj=result, mode="w:gz") as package:
            for name, value in files.items():
                content = json.dumps(value).encode()
                member = tarfile.TarInfo(f"{selector.NAME}/{name}")
                member.size = len(content)
                package.addfile(member, io.BytesIO(content))
    return result.getvalue()


def release(system, payload):
    ext = "zip" if system == WINDOWS else "tar.gz"
    name = f"{selector.NAME}-{TAG[1:]}-{system}.{ext}"
    return {"tag_name": TAG, "prerelease": False, "draft": False, "assets": [
        {"name": name, "size": len(payload), "digest": "sha256:" + hashlib.sha256(payload).hexdigest(),
         "state": "uploaded",
         "browser_download_url": f"https://github.com/{selector.REPOSITORY}/releases/download/{TAG}/{name}"}
    ]}


class StableCompilerTests(unittest.TestCase):
    def test_automatically_selects_official_release_on_each_platform(self):
        for platform in (LINUX, WINDOWS):
            payload = package_bytes(platform)
            metadata = release(platform, payload)
            with (self.subTest(platform=platform), tempfile.TemporaryDirectory() as folder,
                  patch.object(selector, "official_release", return_value=metadata) as get_release,
                  patch.object(selector, "git") as git, patch.object(selector, "changed_inputs", return_value=[]),
                  patch("updates.urlopen", return_value=Download(payload))):
                archive = Path(folder) / "previous-package"
                self.assertEqual(selector.select(platform, archive), {
                    "COMPILER_VERSION": TAG, "COMPILER_SHA256": hashlib.sha256(payload).hexdigest(),
                    "COMPILER_BUILD_ID": BUILD_ID,
                })
                self.assertEqual(archive.read_bytes(), payload)
                get_release.assert_called_once_with(None)
                self.assertEqual(git.call_args.args[-1], f"refs/tags/{TAG}")

    def test_skips_download_on_native_change_and_rejects_incompatible_manual_request(self):
        payload = package_bytes(LINUX)
        with (tempfile.TemporaryDirectory() as folder,
              patch.object(selector, "official_release", return_value=release(LINUX, payload)),
              patch.object(selector, "git"), patch.object(selector, "changed_inputs", return_value=["game/new.cpp"]),
              patch.object(selector, "download") as download):
            archive = Path(folder) / "previous-package"
            self.assertIsNone(selector.select(LINUX, archive))
            with self.assertRaisesRegex(ValueError, "Compiler inputs changed"):
                selector.select(LINUX, archive, tag=TAG, digest=hashlib.sha256(payload).hexdigest(),
                                build_id=BUILD_ID)
            download.assert_not_called()

    def test_rejects_corrupted_download_before_staging_it(self):
        payload = package_bytes(WINDOWS)
        with (tempfile.TemporaryDirectory() as folder,
              patch.object(selector, "official_release", return_value=release(WINDOWS, payload)),
              patch.object(selector, "git"), patch.object(selector, "changed_inputs", return_value=[]),
              patch("updates.urlopen", return_value=Download(b"x" + payload[1:]))):
            archive = Path(folder) / "previous-package"
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                selector.select(WINDOWS, archive)
            self.assertEqual(list(Path(folder).iterdir()), [])

    def test_dispatch_requires_all_inputs_and_verifies_digest_and_build_id(self):
        payload = package_bytes(WINDOWS)
        with (tempfile.TemporaryDirectory() as folder,
              patch.object(selector, "official_release", return_value=release(WINDOWS, payload)) as get_release,
              patch.object(selector, "git"), patch.object(selector, "changed_inputs", return_value=[]),
              patch("updates.urlopen", return_value=Download(payload))):
            archive = Path(folder) / "previous-package"
            self.assertIsNone(selector.select(WINDOWS, archive, force_build=True))
            with self.assertRaisesRegex(ValueError, "all three"):
                selector.select(WINDOWS, archive, tag=TAG)
            get_release.assert_not_called()
            with self.assertRaisesRegex(ValueError, "official package digest"):
                selector.select(WINDOWS, archive, tag=TAG, digest="0" * 64, build_id=BUILD_ID)
            with self.assertRaisesRegex(ValueError, "verified package manifest"):
                selector.select(WINDOWS, archive, tag=TAG, digest=hashlib.sha256(payload).hexdigest(),
                                build_id="0" * 24)
            get_release.assert_called_with(TAG)
            self.assertFalse(archive.exists())

    def test_platform_specific_changes_and_unknown_inputs(self):
        changes = "tools/desktop/main.py\0tools/linux/prepare_compiler.py\0tools/windows/package.py\0new/tool.cpp\0"
        with patch.object(selector, "git", return_value=changes):
            self.assertEqual(selector.changed_inputs("stable", LINUX), ["tools/linux/prepare_compiler.py", "new/tool.cpp"])
            self.assertEqual(selector.changed_inputs("stable", WINDOWS), ["tools/windows/package.py", "new/tool.cpp"])

    def test_windows_configuration_changes_invalidate_reuse(self):
        workflow = (Path(__file__).resolve().parents[2] / selector.WINDOWS_WORKFLOW).read_text()
        changes = selector.WINDOWS_WORKFLOW + "\0"
        modified = workflow.replace("-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_TEST=ON")
        with patch.object(selector, "git", side_effect=[changes, workflow, modified]):
            self.assertEqual(selector.changed_inputs("stable", WINDOWS), [selector.WINDOWS_WORKFLOW])
        with patch.object(selector, "git", side_effect=[changes, workflow, workflow]):
            self.assertEqual(selector.changed_inputs("stable", WINDOWS), [])
