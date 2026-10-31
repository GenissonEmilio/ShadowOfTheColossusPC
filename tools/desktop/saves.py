"""Portable PS2 memory-card import/export with atomic replacement and backups."""
from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path
import os
import tempfile

CARD_BYTES = 16384 * 528
MAGIC = b"Sony PS2 Memory Card Format "


def atomic_bytes(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".card-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


class SaveCards:
    def __init__(self, library):
        self.directory = library.player_directory / "memcards"
        self.card = self.directory / "Mcd001.ps2"

    def import_card(self, source):
        source = Path(source).expanduser().resolve(strict=True)
        if source == self.card.resolve():
            raise ValueError("Este cartão já é o cartão usado pelo jogo.")
        with source.open("rb") as stream:
            data = stream.read(CARD_BYTES + 1)
        if len(data) != CARD_BYTES or not data.startswith(MAGIC):
            raise ValueError("Escolha um cartão PS2 formatado de 8 MB no formato PCSX2 (.ps2, "
                             "8.650.752 bytes). Um save individual .psu/.max não é um cartão.")
        # Keep the previous card even if the subsequent replacement fails.
        backup = None
        if self.card.exists():
            self.directory.mkdir(parents=True, exist_ok=True)
            backups = self.directory / "backups"
            backups.mkdir(exist_ok=True)
            stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
            backup = backups / f"Mcd001-{stamp}.ps2"
            atomic_bytes(backup, self.card.read_bytes())
        atomic_bytes(self.card, data)
        return backup

    def export_card(self, destination):
        if not self.card.is_file():
            raise ValueError("Ainda não existe um cartão. Salve no jogo ou importe seu cartão primeiro.")
        destination = Path(destination).expanduser().resolve()
        if destination == self.card.resolve():
            raise ValueError("Escolha outra pasta para exportar uma cópia.")
        atomic_bytes(destination, self.card.read_bytes())
