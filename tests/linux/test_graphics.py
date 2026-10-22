import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/"tools/desktop"))
from graphics import Graphics
from model import Library
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
