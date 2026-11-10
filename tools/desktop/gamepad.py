"""Optional SDL2 controller input, scoped to the focused launcher.

No input injection or device grabs. SDL's standard controller mappings (including
Steam virtual controllers and the user's native-game profile) supply layout names.
Unmapped joysticks remain available to the existing controller calibration tool.
"""
from __future__ import annotations

import ctypes
from pathlib import Path
import time


class Navigation:
    """Neutral-arm, edge buttons and bounded directional repeat, independent of GTK."""
    def __init__(self):
        self.reset()

    def reset(self):
        self.armed = False
        self.previous = set()
        self.direction = None
        self.next_repeat = 0.

    def update(self, held, now):
        held = set(held)
        if not self.armed:
            self.armed = not held
            self.previous = held
            return []
        actions = [name for name in ("confirm", "back", "play", "previous", "next")
                   if name in held and name not in self.previous]
        direction = next((name for name in ("up", "down", "left", "right") if name in held), None)
        if direction != self.direction:
            if direction:
                actions.append(direction)
            self.next_repeat = now + .4
        elif direction and now >= self.next_repeat:
            actions.append(direction)
            self.next_repeat = now + .12
        self.direction = direction
        self.previous = held
        return actions


class Gamepad:
    FLAGS = 0x2000  # SDL_INIT_GAMECONTROLLER (also initializes joystick/events)
    BUTTONS = {0: "confirm", 1: "back", 6: "play", 9: "previous", 10: "next",
               11: "up", 12: "down", 13: "left", 14: "right"}

    def __init__(self, program, profile, clock=time.monotonic):
        self.clock = clock
        self.navigation = Navigation()
        self.sdl = None
        self.controller = None
        self.next_scan = 0.
        self.running = False
        self.mapping_paths = (Path(__file__).resolve().parent / "assets/gamecontrollerdb.txt",
                              Path(program) / "PS2Recomp/ps2xRuntime/third_party/gamecontrollerdb/gamecontrollerdb.txt",
                              Path(profile))
        # A release may bundle SDL2 alongside the launcher. Source launches can
        # use the system runtime; absence never prevents keyboard/mouse startup.
        candidates = (Path(program) / "_internal/libSDL2-2.0.so.0",
                      Path(program) / "compiler/lib/libSDL2-2.0.so.0", "libSDL2-2.0.so.0")
        for candidate in candidates:
            try:
                self.sdl = ctypes.CDLL(str(candidate))
                break
            except OSError:
                pass
        if self.sdl is None:
            return
        signatures = {
            "SDL_SetHint": ([ctypes.c_char_p, ctypes.c_char_p], ctypes.c_int),
            "SDL_InitSubSystem": ([ctypes.c_uint32], ctypes.c_int),
            "SDL_QuitSubSystem": ([ctypes.c_uint32], None),
            "SDL_PumpEvents": ([], None),
            "SDL_FlushEvents": ([ctypes.c_uint32, ctypes.c_uint32], None),
            "SDL_NumJoysticks": ([], ctypes.c_int),
            "SDL_IsGameController": ([ctypes.c_int], ctypes.c_int),
            "SDL_GameControllerOpen": ([ctypes.c_int], ctypes.c_void_p),
            "SDL_GameControllerClose": ([ctypes.c_void_p], None),
            "SDL_GameControllerGetAttached": ([ctypes.c_void_p], ctypes.c_int),
            "SDL_GameControllerGetButton": ([ctypes.c_void_p, ctypes.c_int], ctypes.c_uint8),
            "SDL_GameControllerGetAxis": ([ctypes.c_void_p, ctypes.c_int], ctypes.c_int16),
            "SDL_GameControllerAddMapping": ([ctypes.c_char_p], ctypes.c_int),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.sdl, name)
            function.argtypes, function.restype = arguments, result

    @property
    def connected(self):
        return self.controller is not None

    def suspend(self):
        if self.controller is not None:
            self.sdl.SDL_GameControllerClose(self.controller)
            self.controller = None
        if self.running:
            self.sdl.SDL_QuitSubSystem(self.FLAGS)
            self.running = False
        self.navigation.reset()
        self.next_scan = 0.

    def poll(self, active):
        if not active or self.sdl is None:
            self.suspend()
            return []
        if not self.running:
            # GTK owns the window; SDL has no focused SDL window of its own.
            # The caller gates all polling by GTK focus and game lifecycle.
            self.sdl.SDL_SetHint(b"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", b"1")
            if self.sdl.SDL_InitSubSystem(self.FLAGS) != 0:
                return []
            self.running = True
            for path in self.mapping_paths:
                try:
                    for line in path.read_bytes().splitlines():
                        if line and not line.startswith(b"#"):
                            self.sdl.SDL_GameControllerAddMapping(line)
                except OSError:
                    pass
        self.sdl.SDL_PumpEvents()
        # SDL owns no window here; discard only its own queued input so a long
        # launcher session cannot accumulate an unbounded event queue.
        self.sdl.SDL_FlushEvents(0, 0xFFFF)
        now = self.clock()
        if self.controller is not None and not self.sdl.SDL_GameControllerGetAttached(self.controller):
            self.sdl.SDL_GameControllerClose(self.controller)
            self.controller = None
            self.navigation.reset()
        if self.controller is None and now >= self.next_scan:
            self.next_scan = now + 1.
            for index in range(self.sdl.SDL_NumJoysticks()):
                if self.sdl.SDL_IsGameController(index):
                    self.controller = self.sdl.SDL_GameControllerOpen(index)
                    if self.controller:
                        break
            if not self.controller:
                self.controller = None
                return []
        if self.controller is None:
            return []
        held = {name for button, name in self.BUTTONS.items()
                if self.sdl.SDL_GameControllerGetButton(self.controller, button)}
        x = self.sdl.SDL_GameControllerGetAxis(self.controller, 0)
        y = self.sdl.SDL_GameControllerGetAxis(self.controller, 1)
        # A generous dead zone avoids drift and diagonal double navigation.
        if max(abs(x), abs(y)) >= 16000:
            held.add(("right" if x > 0 else "left") if abs(x) > abs(y)
                     else ("down" if y > 0 else "up"))
        return self.navigation.update(held, now)
