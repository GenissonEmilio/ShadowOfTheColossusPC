import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/"tools/desktop"))
from graphics import Graphics
from gpu import amd_environment, amd_is_integrated, nvidia_environment, prefer_discrete_gpu, prefer_windows_gpu, switcheroo_environment
from model import Library
from texture_mods import TextureMods
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/"tools/linux"))
from compiler_key import compiler_key


class GraphicsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name)
        self.library = Library(self.folder/"data",self.folder/"program")
        self.graphics = Graphics(self.library)

    def test_display_preserves_sound_and_renderer_settings(self):
        self.graphics.config.parent.mkdir(parents=True)
        self.graphics.config.write_text("volume=7\nexact_alpha=1\nrender_scale=2\nscreen=0\n")
        self.graphics.save_display(True,1)
        self.assertEqual(self.graphics.display(),(True,1))
        text = self.graphics.config.read_text()
        for expected in ("volume=7","exact_alpha=1","render_scale=2"):
            self.assertIn(expected,text)
        self.graphics.save_display(False,0)
        self.assertEqual(Graphics(self.library).display(),(False,0))

    def test_widescreen_round_trips_with_fullscreen_and_window_settings(self):
        self.assertEqual(self.graphics.display(), (True, 0))
        self.graphics.save_display(False, 3, window=4, fsr=True)
        graphics = Graphics(self.library)
        self.assertEqual(graphics.display(), (False, 3))
        self.assertEqual(graphics.upscaling(), (4, True))
        graphics.save_display(True, 3)
        self.assertEqual(graphics.display(), (True, 3))
        self.assertEqual(graphics.upscaling(), (4, True))

    def test_menu_language_round_trips_without_overwriting_video_settings(self):
        self.assertEqual(self.graphics.menu_language(), "0")
        self.graphics.save_display(False, 3)
        self.graphics.save_menu_language("2")
        reloaded = Graphics(self.library)
        self.assertEqual(reloaded.menu_language(), "2")
        self.assertEqual(reloaded.display(), (False, 3))
        reloaded.save_menu_language("unsupported")
        self.assertEqual(reloaded.menu_language(), "2")

    def test_quality_profile_replaces_overrides_but_preserves_other_settings(self):
        self.assertEqual(self.graphics.quality_profile(), "1")
        self.graphics.config.parent.mkdir(parents=True)
        self.graphics.config.write_text(
            "quality_preset=4\nrender_scale=2\nfxaa=0\nwater_detail=0\neffect_detail=2\n"
            "volume=7\nfullscreen=0\nscreen=2\nupscale_fsr=1\n")
        self.assertEqual(self.graphics.quality_profile(), "4")
        self.graphics.save_quality_profile("6")
        self.assertEqual(self.graphics.quality_profile(), "6")
        self.assertEqual(self.graphics.settings(), {
            "volume": "7", "fullscreen": "0", "screen": "2", "upscale_fsr": "1", "quality_preset": "6",
        })
        self.graphics.save_display(True, 1)
        self.assertEqual(self.graphics.quality_profile(), "6")

    def test_invalid_shader_preserves_current_selection(self):
        self.library.directory.mkdir()
        self.graphics.shader_config.write_text('{"file":"previous.frag"}')
        previous = self.graphics.shader_config.read_bytes()
        for filename,data in (("bad.fx",b"test"),("large.frag",b"x"*262145),
                              ("binary.frag",b"\0"),("version.frag",b"#version 450\nvoid main(){}")):
            source=self.folder/filename; source.write_bytes(data)
            with self.subTest(filename=filename), self.assertRaises(ValueError):
                self.graphics.import_shader(source)
            self.assertEqual(self.graphics.shader_config.read_bytes(),previous)

    def test_gl_compiler_rejection_does_not_activate_shader(self):
        source=self.folder/"broken.frag";source.write_text("#version 330 core\ninvalid syntax")
        def rejected(args,**kwargs):
            Path(args[-1]).write_text('{"ok":false,"error":"compile failed"}')
            return type("Result",(),{"returncode":2})()
        with patch("graphics.subprocess.run",side_effect=rejected):
            with self.assertRaisesRegex(ValueError,"compile failed"):self.graphics.import_shader(source)
        self.assertIsNone(self.graphics.selected())
        self.assertFalse(self.graphics.shader_config.exists())

    def test_managed_shader_survives_original_removal_and_is_disableable(self):
        source=self.folder/"My shader.frag";source.write_text("#version 330 core\nvoid main(){}")
        def accepted(args,**kwargs):
            Path(args[-1]).write_text('{"ok":true}')
            return type("Result",(),{"returncode":0})()
        with patch("graphics.subprocess.run",side_effect=accepted):
            managed=self.graphics.import_shader(source)
        source.unlink()
        self.assertEqual(self.graphics.selected(),managed)
        self.assertEqual(self.library.game_environment()["SOTC_SHADER"],str(managed))
        self.graphics.clear();self.assertIsNone(self.graphics.selected())
        self.assertEqual(self.library.game_environment()["SOTC_SHADER"],"")

    def test_shader_state_cannot_escape_library(self):
        self.library.directory.mkdir()
        self.graphics.shader_config.write_text(json.dumps({"file":"../../outside.frag"}))
        self.assertIsNone(self.graphics.selected())

    def test_launch_prefers_discrete_gpu_with_switcheroo(self):
        gpus = [
            {"Default": True, "Discrete": False, "Environment": ["DRI_PRIME", "0"]},
            {"Default": False, "Discrete": True, "Environment": [
                "__NV_PRIME_RENDER_OFFLOAD", "1", "__GLX_VENDOR_LIBRARY_NAME", "nvidia",
                "VK_LOADER_DRIVERS_SELECT", "*nvidia*",
            ]},
        ]
        disc = self.folder / "SCUS_974.72"
        disc.touch()
        self.library.state["disc"] = str(disc)
        with patch.dict(os.environ, {}, clear=True), patch("gpu.switcheroo_gpus", return_value=gpus) as probe:
            self.assertNotIn("__NV_PRIME_RENDER_OFFLOAD", self.library.game_environment())
            probe.assert_not_called()
            with patch.object(self.library, "prepare_game", return_value=self.folder / "game"), patch("model.subprocess.Popen") as launch:
                self.library.launch()
        environment = launch.call_args.kwargs["env"]
        self.assertEqual(environment["__NV_PRIME_RENDER_OFFLOAD"], "1")
        self.assertEqual(environment["__GLX_VENDOR_LIBRARY_NAME"], "nvidia")
        self.assertNotIn("VK_LOADER_DRIVERS_SELECT", environment)
        probe.assert_called_once()

    def test_mesa_discrete_and_legacy_switcheroo_selection(self):
        environment = ["DRI_PRIME", "pci-0000_03_00_0"]
        for flags in ({"Discrete": True}, {}):
            with self.subTest(flags=flags):
                self.assertEqual(switcheroo_environment([
                    {"Default": True, "Environment": ["DRI_PRIME", "0"]},
                    {"Default": False, "Environment": environment, **flags},
                ]), {"DRI_PRIME": "pci-0000_03_00_0"})

    def test_keeps_default_discrete_or_integrated_only(self):
        dedicated = {"Default": True, "Discrete": True, "Environment": ["DRI_PRIME", "pci-0000_03_00_0"]}
        integrated = {"Default": False, "Discrete": False, "Environment": ["DRI_PRIME", "0"]}
        self.assertEqual(switcheroo_environment([dedicated, integrated]), {})
        self.assertEqual(switcheroo_environment([integrated]), {})

    def test_explicit_gpu_choice_is_kept(self):
        for environment in ({"DRI_PRIME": "0"}, {"__NV_PRIME_RENDER_OFFLOAD": "0"}, {"LIBGL_ALWAYS_SOFTWARE": "1"}):
            with self.subTest(environment=environment), patch("gpu.switcheroo_gpus") as probe:
                original = environment.copy()
                prefer_discrete_gpu(environment)
                self.assertEqual(environment, original)
                probe.assert_not_called()

    def test_windows_gpu_preference_targets_game_and_preserves_existing_choice(self):
        from unittest.mock import MagicMock

        registry = MagicMock()
        handle = registry.CreateKeyEx.return_value.__enter__.return_value
        game = self.folder / "builds" / "gamecore" / "game.exe"
        with patch.dict(sys.modules, {"winreg": registry}):
            registry.QueryValueEx.return_value = ("GpuPreference=1;", registry.REG_SZ)
            self.assertIn("kept existing", prefer_windows_gpu(game))
            registry.SetValueEx.assert_not_called()
            registry.QueryValueEx.side_effect = FileNotFoundError
            self.assertIn("high performance", prefer_windows_gpu(game))
        registry.SetValueEx.assert_called_once_with(handle, str(game.resolve()), 0,
                                                     registry.REG_SZ, "GpuPreference=2;")

    def test_windows_gpu_preference_failure_allows_game_start(self):
        from unittest.mock import MagicMock

        registry = MagicMock()
        registry.CreateKeyEx.side_effect = PermissionError(13, "Access denied")
        with patch.dict(sys.modules, {"winreg": registry}):
            self.assertIn("unavailable", prefer_windows_gpu(self.folder / "game.exe"))

    def test_nvidia_fallback_requires_secondary_render_device(self):
        drm = self.folder / "drm"
        sys_device = self.folder / "0000:01:00.0"
        sys_device.mkdir()
        (drm / "renderD129").mkdir(parents=True)
        (drm / "renderD129/device").symlink_to(sys_device)
        (sys_device / "vendor").write_text("0x10de\n")
        (sys_device / "boot_vga").write_text("0\n")
        driver = self.folder / "nvidia"
        driver.mkdir()
        (sys_device / "driver").symlink_to(driver)
        self.assertEqual(nvidia_environment(drm), {})
        (drm / "renderD128").mkdir()
        self.assertEqual(nvidia_environment(drm), {
            "__NV_PRIME_RENDER_OFFLOAD": "1", "__GLX_VENDOR_LIBRARY_NAME": "nvidia",
        })
        (sys_device / "boot_vga").write_text("1\n")
        self.assertEqual(nvidia_environment(drm), {})

    def test_amd_hybrid_fallback_uses_driver_reported_discrete_adapter(self):
        drm = self.folder / "drm"
        nodes = self.folder / "dev"
        driver = self.folder / "amdgpu"
        driver.mkdir()

        def adapter(render, slot, boot):
            target = self.folder / slot
            target.mkdir()
            (drm / render).mkdir(parents=True)
            (drm / render / "device").symlink_to(target)
            (target / "vendor").write_text("0x1002\n")
            (target / "boot_vga").write_text(f"{boot}\n")
            (target / "driver").symlink_to(driver)

        adapter("renderD128", "0000:04:00.0", 1)
        adapter("renderD129", "0000:01:00.0", 0)
        for results, expected in (
            ([True, False], {"DRI_PRIME": "pci-0000_01_00_0"}),
            ([True, True], {}),
            ([True, None], {}),
            ([None], {}),
            ([False], {}),
        ):
            with self.subTest(results=results), patch("gpu.amd_is_integrated", side_effect=results):
                self.assertEqual(amd_environment(drm, nodes), expected)

    def test_amd_kernel_query_distinguishes_fusion_flag_from_other_flags(self):
        import ctypes
        import struct

        node = self.folder / "renderD129"
        node.touch()
        for flags, integrated in ((0x03, True), (0x02, False)):
            def reply(device, command, request):
                self.assertEqual(command, 0x40206445)
                pointer, size, query = struct.unpack("=QII16x", request)
                self.assertEqual((size, query), (144, 0x16))
                result = (ctypes.c_char * size).from_address(pointer)
                struct.pack_into("=I", result, 0, 0x67FF)
                struct.pack_into("=Q", result, 136, flags)

            with self.subTest(flags=flags), patch("fcntl.ioctl", side_effect=reply):
                self.assertIs(amd_is_integrated(node), integrated)
        with patch("fcntl.ioctl", side_effect=PermissionError):
            self.assertIsNone(amd_is_integrated(node))

    def test_amd_fallback_respects_switcheroo_and_nvidia(self):
        with patch("gpu.switcheroo_gpus") as switcheroo, patch("gpu.nvidia_environment") as nvidia, \
                patch("gpu.amd_environment", return_value={"DRI_PRIME": "pci-0000_01_00_0"}) as amd:
            switcheroo.return_value = []
            environment = {}
            prefer_discrete_gpu(environment)
            self.assertEqual(environment, {})
            nvidia.assert_not_called()
            amd.assert_not_called()

            switcheroo.return_value = None
            nvidia.return_value = {"__NV_PRIME_RENDER_OFFLOAD": "1"}
            prefer_discrete_gpu(environment)
            self.assertEqual(environment, nvidia.return_value)
            amd.assert_not_called()

            environment = {}
            nvidia.return_value = {}
            prefer_discrete_gpu(environment)
            self.assertEqual(environment, amd.return_value)

    def test_texture_mod_selection_is_atomic_and_deactivates_without_deleting_files(self):
        mods = TextureMods(self.library)
        pack = self.folder / "HD textures"
        pack.mkdir()
        image = pack / "0400123456789abc.png"
        # Header metadata is checked during selection and the native decoder checks the PNG.
        image.write_bytes(b"\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR" + b"\x00\x00\x00\x02" * 2 + bytes(16))
        mods.set_pack(pack)
        mods.set_export(True)
        self.assertEqual(mods.environment(), {
            "SOTC_TEXTURE_MOD_DIR": str(pack), "SOTC_TEXTURE_EXPORT_DIR": str(mods.exports),
        })
        self.assertTrue(mods.exports.is_dir())
        with self.assertRaises(ValueError):
            mods.set_pack(self.folder)
        self.assertEqual(mods.preferences(), (pack, True))
        mods.set_pack(None)
        self.assertEqual(mods.preferences(), (None, True))
        self.assertTrue(image.is_file())
        mods.set_export(False)
        self.assertEqual(mods.environment()["SOTC_TEXTURE_EXPORT_DIR"], "")

    def test_gui_update_keeps_cache_but_runtime_and_pipeline_changes_invalidate_it(self):
        root=self.folder/"compiler"
        gui=root/"source/tools/desktop/main.py"; gui.parent.mkdir(parents=True)
        gui.write_text("interface v1")
        runtime=root/"lib/libps2_runtime.a";runtime.parent.mkdir();runtime.write_bytes(b"runtime v1")
        previous=compiler_key(root)
        gui.write_text("interface v2")
        docs=root/"source/docs/SHADERS.md";docs.parent.mkdir(parents=True)
        docs.write_text("Shader documentation")
        self.assertEqual(compiler_key(root),previous)
        runtime.write_bytes(b"runtime v2")
        self.assertNotEqual(compiler_key(root),previous)
        previous=compiler_key(root)
        (gui.parent/"pipeline.py").write_text("updated ABI and compiler flags")
        self.assertNotEqual(compiler_key(root),previous)

if __name__=="__main__":unittest.main()
