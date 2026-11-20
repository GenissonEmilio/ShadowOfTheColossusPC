"""Persist the selected native texture pack and optional PNG export."""
from pathlib import Path
import json
import re
import struct

from language import translate as tr

TEXTURE_NAME = re.compile(r"[0-9a-f]{16}\.png")
PNG_HEADER = b"\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR"


class TextureMods:
    def __init__(self, library):
        self.config = library.directory / "texture-mods.json"
        self.exports = library.directory / "textures" / "exported"

    def preferences(self):
        try:
            state = json.loads(self.config.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            state = {}
        if not isinstance(state, dict):
            state = {}
        pack = state.get("pack")
        path = Path(pack) if isinstance(pack, str) and pack and "\0" not in pack else None
        return (path if path and path.is_absolute() else None), state.get("export") is True

    @staticmethod
    def is_texture(path):
        if not TEXTURE_NAME.fullmatch(path.name) or not path.is_file():
            return False
        try:
            if not 33 <= path.stat().st_size <= 64 * 1024 * 1024:
                return False
            with path.open("rb") as stream:
                header = stream.read(24)
        except OSError:
            return False
        if len(header) != 24 or not header.startswith(PNG_HEADER):
            return False
        width, height = struct.unpack(">II", header[16:])
        return 0 < width <= 4096 and 0 < height <= 4096

    def set_pack(self, folder):
        if folder is not None:
            folder = Path(folder).resolve(strict=True)
            if not folder.is_dir() or not any(self.is_texture(path) for path in folder.iterdir()):
                raise ValueError(tr("Escolha uma pasta com texturas PNG nomeadas pelo código do port."))
        _, export = self.preferences()
        self.save(folder, export)

    def set_export(self, enabled):
        pack, _ = self.preferences()
        if enabled:
            self.exports.mkdir(parents=True, exist_ok=True)
        self.save(pack, bool(enabled))

    def save(self, pack, export):
        from model import atomic_json
        atomic_json(self.config, {"pack": str(pack) if pack else None, "export": export})

    def environment(self):
        pack, export = self.preferences()
        if export:
            self.exports.mkdir(parents=True, exist_ok=True)
        return {
            "SOTC_TEXTURE_MOD_DIR": str(pack) if pack and pack.is_dir() else "",
            "SOTC_TEXTURE_EXPORT_DIR": str(self.exports) if export else "",
        }
