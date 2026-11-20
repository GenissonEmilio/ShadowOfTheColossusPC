"""Shared display preferences and local, GPU-validated GLSL imports."""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from language import translate as tr

WINDOW_RESOLUTIONS = (
    (640, 480), (800, 600), (1024, 720), (1280, 720),
    (1600, 900), (1920, 1080), (2560, 1440), (3840, 2160),
)
SCREEN_MODE_COUNT = 4  # matches hostmenu::ScreenMode

# IDs are persisted by native_renderer::Tuning. Keep the older IDs readable.
GRAPHICS_PROFILES = (
    ("0", "Baixo"), ("1", "Original"), ("5", "Equilibrado"), ("6", "Aprimorado"), ("7", "Ultra"),
)
LEGACY_PROFILES = (("2", "Alto (legado)"), ("3", "Ultra (legado)"))
QUALITY_KEYS = {
    "quality_preset", "render_scale", "msaa", "fxaa", "texture_filter", "model_detail", "water_detail", "effect_detail",
}
RECOMP_VIDEO_KEYS = ("fullscreen", "screen", "window_resolution", "vsync", "upscale_fsr", "show_fps", "volume")


class Graphics:
    def __init__(self, library):
        self.library = library
        self.config = library.player_directory / "celosia_settings.cfg"
        self.shader_config = library.directory / "shader.json"

    def settings(self):
        values = {}
        try:
            for line in self.config.read_text(encoding="utf-8").splitlines():
                key, separator, value = line.partition("=")
                if separator:
                    values[key.strip()] = value.strip()
        except OSError:
            pass
        return values

    def display(self):
        values = self.settings()
        mode = values.get("screen", "0")
        modes = tuple(str(index) for index in range(SCREEN_MODE_COUNT))
        return values.get("fullscreen", "1") == "1", int(mode) if mode in modes else 0

    def upscaling(self):
        values = self.settings()
        modes = tuple(str(index) for index in range(len(WINDOW_RESOLUTIONS) + 1))
        window = values.get("window_resolution", "0")
        return int(window) if window in modes else 0, values.get("upscale_fsr") == "1"

    def quality_profile(self):
        values = self.settings()
        profile = values.get("quality_preset")
        if profile in dict(GRAPHICS_PROFILES + LEGACY_PROFILES):
            return profile
        return "4" if QUALITY_KEYS.intersection(values) else "1"

    def save_quality_profile(self, profile):
        if profile in dict(GRAPHICS_PROFILES + LEGACY_PROFILES):
            # The runtime applies the selected preset, then does individual overrides.
            self._save_settings({"quality_preset": profile}, QUALITY_KEYS)

    def save_display(self, fullscreen, mode, *, window=None, fsr=None):
        values = {"fullscreen": int(bool(fullscreen)), "screen": max(0, min(SCREEN_MODE_COUNT - 1, int(mode)))}
        if window is not None:
            values["window_resolution"] = max(0, min(len(WINDOW_RESOLUTIONS), int(window)))
        if fsr is not None:
            values["upscale_fsr"] = int(bool(fsr))
        self._save_settings(values, values)

    def normalize_external_settings(self):
        # An external INI writer may use `key = value`. The runtime expects
        # `key=value`. Preserve the native renderer's other settings and order.
        values = self.settings()
        external = {key: values[key] for key in RECOMP_VIDEO_KEYS if key in values}
        if external:
            self._save_settings(external, external)

    def _save_settings(self, values, replace):
        self.config.parent.mkdir(parents=True, exist_ok=True)
        try:
            lines = [line for line in self.config.read_text(encoding="utf-8").splitlines()
                     if line.partition("=")[0].strip() not in replace]
        except OSError:
            lines = []
        lines += [f"{key}={value}" for key, value in values.items()]
        fd, temporary = tempfile.mkstemp(prefix=".video-", dir=self.config.parent)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as stream:
                stream.write("\n".join(lines) + "\n")
            os.replace(temporary, self.config)
        finally:
            Path(temporary).unlink(missing_ok=True)

    def selected(self):
        try:
            state = json.loads(self.shader_config.read_text())
            if not isinstance(state, dict):
                return None
            name = state.get("file", "")
            if isinstance(name, str) and name and Path(name).name == name and (self.library.directory / "shaders" / name).is_file():
                return self.library.directory / "shaders" / name
        except (OSError, ValueError, AttributeError):
            pass
        return None

    def clear(self):
        from model import atomic_json
        atomic_json(self.shader_config, {})

    def import_shader(self, source):
        from model import PROCESS_FLAGS, atomic_json, compiler_tool
        source = Path(source)
        if source.suffix.lower() not in (".frag", ".fs", ".glsl"):
            raise ValueError(tr("Escolha um fragment shader GLSL 330: .frag, .fs ou .glsl."))
        with source.open("rb") as stream:
            data = stream.read(262145)
        if not data or len(data) > 262144 or b"\0" in data:
            raise ValueError(tr("O shader deve ser um texto de até 256 KiB."))
        try:
            text = data.decode("utf-8-sig")
        except UnicodeDecodeError as exc:
            raise ValueError(tr("Salve o shader como texto UTF-8.")) from exc
        if not re.search(r"^\s*#version\s+330(?:\s+core)?\s*$", text, re.M):
            raise ValueError(tr("Este port usa GLSL 330. Veja o modelo de shader na aba Gráficos."))
        data = text.encode("utf-8")
        folder = self.library.directory / "shaders"
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".import-", dir=folder) as temporary:
            candidate, report = Path(temporary) / "shader.frag", Path(temporary) / "report.json"
            candidate.write_bytes(data)
            log = self.library.directory / "last-shader.log"
            try:
                with log.open("w") as output:
                    result = subprocess.run([str(compiler_tool(self.library.program, "sotc-video-tool")),
                        "--validate", str(candidate), "--report", str(report)],
                        env=self.library.game_environment(), stdout=output, stderr=subprocess.STDOUT,
                        creationflags=PROCESS_FLAGS, timeout=30)
                details = json.loads(report.read_text()) if report.is_file() else {}
            except subprocess.TimeoutExpired as exc:
                raise ValueError(tr("O driver demorou para validar o shader. Consulte last-shader.log.")) from exc
            if result.returncode or not details.get("ok"):
                raise ValueError(details.get("error") or tr("Não foi possível validar o shader. Detalhes em {path}").format(path=log))
            stem = re.sub(r"[^\w.-]", "-", source.stem, flags=re.UNICODE)[:64] or "shader"
            destination = folder / (stem + "-" + hashlib.sha256(data).hexdigest()[:12] + ".frag")
            os.replace(candidate, destination)
        atomic_json(self.shader_config, {"file": destination.name, "name": source.stem})
        return destination
