"""Persistent keyboard/mouse profile shared with the native input backend."""
from __future__ import annotations
import math
import os
from pathlib import Path
import tempfile

# raylib/GLFW key codes; mouse buttons use -(index+1). Zero unbinds an action.
ACTIONS = (
    ("move_up", "Mover para frente", 87), ("move_down", "Mover para trás", 83),
    ("move_left", "Mover para a esquerda", 65), ("move_right", "Mover para a direita", 68),
    ("jump", "Pular · ✕", 32), ("attack", "Atacar · □", -1),
    ("grab", "Agarrar · R1", -2), ("action", "Ação / luz da espada · ○", -3),
    ("horse", "Chamar Agro / montar · △", 67),
    ("weapon_left", "Arma anterior · ←", 81), ("weapon_right", "Próxima arma · →", 69),
    ("look", "L1", 70), ("camera_reset", "L2", 82), ("crouch", "R2", 340),
    ("start", "Pausar · Start", 256), ("select", "Mapa · Select", 73),
    ("dpad_up", "Direcional ↑", 265), ("dpad_down", "Direcional ↓", 264),
    ("l3", "L3", 341), ("r3", "R3", 86),
    ("camera_left", "Câmera para a esquerda", 263), ("camera_right", "Câmera para a direita", 262),
    ("camera_up", "Câmera para cima", 266), ("camera_down", "Câmera para baixo", 267),
)
DEFAULTS = {key: default for key, _, default in ACTIONS}
DEFAULTS.update(mouse_camera=1, invert_x=0, invert_y=0, sensitivity=1.0)
SPECIAL_KEYS = {32: "Espaço", 256: "Esc", 257: "Enter", 258: "Tab", 259: "Backspace",
                260: "Insert", 261: "Delete", 262: "→", 263: "←", 264: "↓", 265: "↑",
                266: "Page Up", 267: "Page Down", 268: "Home", 269: "End",
                340: "Shift esquerdo", 341: "Ctrl esquerdo", 342: "Alt esquerdo",
                344: "Shift direito", 345: "Ctrl direito", 346: "Alt direito"}


def valid_binding(code):
    return type(code) is int and (-7 <= code <= 0 or
                                 32 <= code <= 348 and code not in (290, 300))


def binding_label(code):
    if code == 0:
        return "Não atribuído"
    if code < 0:
        return {-1: "Mouse esquerdo", -2: "Mouse direito", -3: "Mouse central",
                -4: "Mouse lateral 1", -5: "Mouse lateral 2"}.get(code, f"Mouse {-code}")
    if code in SPECIAL_KEYS:
        return SPECIAL_KEYS[code]
    if 290 <= code <= 301:
        return f"F{code - 289}"
    return chr(code) if 33 <= code <= 96 else f"Tecla {code}"


class Controls:
    def __init__(self, library):
        self.path = library.directory / "controllers/keyboard-mouse.cfg"

    def load(self):
        values = dict(DEFAULTS)
        try:
            for line in self.path.read_text().splitlines():
                key, _, raw = line.partition("=")
                if key not in values:
                    continue
                try:
                    number = float(raw)
                    if not math.isfinite(number):
                        continue
                    if key == "sensitivity":
                        values[key] = max(.1, min(5., number))
                    elif number.is_integer():
                        code = int(number)
                        if key in ("mouse_camera", "invert_x", "invert_y") and code in (0, 1):
                            values[key] = code
                        elif key in dict((a[0], a[2]) for a in ACTIONS) and valid_binding(code):
                            values[key] = code
                except ValueError:
                    continue
        except OSError:
            pass
        return values

    def save(self, values):
        result = {key: values.get(key, default) for key, default in DEFAULTS.items()}
        for key, _, _ in ACTIONS:
            if not valid_binding(result[key]):
                raise ValueError("Tecla inválida. F1 e F11 são atalhos reservados.")
        used = [result[key] for key, _, _ in ACTIONS if result[key]]
        if len(used) != len(set(used)):
            raise ValueError("Cada tecla ou botão deve ter apenas uma ação. Remova a atribuição anterior.")
        for key in ("mouse_camera", "invert_x", "invert_y"):
            if result[key] not in (0, 1):
                raise ValueError("Opção de mouse inválida.")
        number = result["sensitivity"]
        if not isinstance(number, (int, float)) or not math.isfinite(number) or not .1 <= number <= 5.:
            raise ValueError("Use uma sensibilidade entre 0,1 e 5,0.")
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary = tempfile.mkstemp(prefix=".controls-", dir=self.path.parent)
        try:
            with os.fdopen(fd, "w") as stream:
                stream.write("".join(f"{key}={value}\n" for key, value in result.items()))
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, self.path)
        finally:
            Path(temporary).unlink(missing_ok=True)
