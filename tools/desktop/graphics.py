"""Shared display preferences and local, GPU-validated GLSL imports."""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


class Graphics:
    def __init__(self, library):
        self.library = library
        self.config = library.player_directory / "celosia_settings.cfg"
        self.shader_config = library.directory / "shader.json"

    def display(self):
        values = {}
        try:
            for line in self.config.read_text().splitlines():
                key, _, value = line.partition("=")
                values[key] = value
        except OSError:
            pass
        return values.get("fullscreen", "1") == "1", int(values.get("screen", "0")) if values.get("screen") in ("0", "1", "2") else 0

    def save_display(self, fullscreen, mode):
        self.config.parent.mkdir(parents=True, exist_ok=True)
        try:
            lines = [line for line in self.config.read_text().splitlines()
                     if line.partition("=")[0] not in ("fullscreen", "screen")]
        except OSError:
            lines = []
        lines += [f"fullscreen={int(bool(fullscreen))}", f"screen={max(0, min(2, int(mode)))}"]
        fd, temporary = tempfile.mkstemp(prefix=".video-", dir=self.config.parent)
        try:
            with os.fdopen(fd, "w") as stream:
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
        from model import atomic_json
        source = Path(source)
        if source.suffix.lower() not in (".frag", ".fs", ".glsl"):
            raise ValueError("Escolha um fragment shader GLSL 330: .frag, .fs ou .glsl.")
        with source.open("rb") as stream:
            data = stream.read(262145)
        if not data or len(data) > 262144 or b"\0" in data:
            raise ValueError("O shader deve ser um texto de até 256 KiB.")
        try:
            text = data.decode("utf-8-sig")
        except UnicodeDecodeError as exc:
            raise ValueError("Salve o shader como texto UTF-8.") from exc
        if not re.search(r"^\s*#version\s+330(?:\s+core)?\s*$", text, re.M):
            raise ValueError("Este port usa GLSL 330. Veja o modelo de shader na aba Gráficos.")
        data = text.encode("utf-8")
        folder = self.library.directory / "shaders"
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".import-", dir=folder) as temporary:
            candidate, report = Path(temporary) / "shader.frag", Path(temporary) / "report.json"
            candidate.write_bytes(data)
            log = self.library.directory / "last-shader.log"
            try:
                with log.open("w") as output:
                    result = subprocess.run([str(self.library.program / "compiler/bin/sotc-video-tool"),
                        "--validate", str(candidate), "--report", str(report)],
                        env=self.library.game_environment(), stdout=output, stderr=subprocess.STDOUT, timeout=30)
                details = json.loads(report.read_text()) if report.is_file() else {}
            except subprocess.TimeoutExpired as exc:
                raise ValueError("O driver demorou para validar o shader. Consulte last-shader.log.") from exc
            if result.returncode or not details.get("ok"):
                raise ValueError(details.get("error") or f"Não foi possível validar o shader. Detalhes: {log}")
            stem = re.sub(r"[^\w.-]", "-", source.stem, flags=re.UNICODE)[:64] or "shader"
            destination = folder / (stem + "-" + hashlib.sha256(data).hexdigest()[:12] + ".frag")
            os.replace(candidate, destination)
        atomic_json(self.shader_config, {"file": destination.name, "name": source.stem})
        return destination
