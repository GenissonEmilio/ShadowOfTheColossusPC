"""Optional SDL2 controller input, scoped to the focused launcher.

No input injection or device grabs. SDL's standard controller mappings (including
Steam virtual controllers and the user's native-game profile) supply layout names.
Unmapped joysticks remain available to the existing controller calibration tool.
"""
from __future__ import annotations

import ctypes
from pathlib import Path
import time

from sdl import load_sdl


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
        actions = [name for name in ("back", "confirm", "menu", "play", "previous", "next", "parent")
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
    BUTTONS = {0: "confirm", 1: "back", 3: "parent", 4: "menu", 6: "play", 9: "previous", 10: "next",
               11: "up", 12: "down", 13: "left", 14: "right"}

    def __init__(self, program, profile, clock=time.monotonic):
        self.clock = clock
        self.navigation = Navigation()
        self.controllers = {}
        self.ready_controllers = set()
        self.unmapped_devices = ()
        self.sdl = load_sdl(program)
        self.error = None
        self.next_scan = 0.
        self.running = False
        self.mapping_paths = (Path(__file__).resolve().parent / "assets/gamecontrollerdb.txt",
                              Path(program) / "PS2Recomp/ps2xRuntime/third_party/gamecontrollerdb/gamecontrollerdb.txt",
                              Path(profile))
        if self.sdl is None:
            self.error = "SDL2 unavailable"
            return
        signatures = {
            "SDL_SetHint": ([ctypes.c_char_p, ctypes.c_char_p], ctypes.c_int),
            "SDL_InitSubSystem": ([ctypes.c_uint32], ctypes.c_int),
            "SDL_QuitSubSystem": ([ctypes.c_uint32], None),
            "SDL_GetError": ([], ctypes.c_char_p),
            "SDL_GetPlatform": ([], ctypes.c_char_p),
            "SDL_PumpEvents": ([], None),
            "SDL_FlushEvents": ([ctypes.c_uint32, ctypes.c_uint32], None),
            "SDL_LockJoysticks": ([], None),
            "SDL_UnlockJoysticks": ([], None),
            "SDL_NumJoysticks": ([], ctypes.c_int),
            "SDL_JoystickGetDeviceInstanceID": ([ctypes.c_int], ctypes.c_int32),
            "SDL_JoystickNameForIndex": ([ctypes.c_int], ctypes.c_char_p),
            "SDL_IsGameController": ([ctypes.c_int], ctypes.c_int),
            "SDL_GameControllerOpen": ([ctypes.c_int], ctypes.c_void_p),
            "SDL_GameControllerClose": ([ctypes.c_void_p], None),
            "SDL_GameControllerGetAttached": ([ctypes.c_void_p], ctypes.c_int),
            "SDL_GameControllerGetButton": ([ctypes.c_void_p, ctypes.c_int], ctypes.c_uint8),
            "SDL_GameControllerGetAxis": ([ctypes.c_void_p, ctypes.c_int], ctypes.c_int16),
            "SDL_GameControllerAddMapping": ([ctypes.c_char_p], ctypes.c_int),
        }
        try:
            for name, (arguments, result) in signatures.items():
                function = getattr(self.sdl, name)
                function.argtypes, function.restype = arguments, result
        except AttributeError as exc:
            self.error = str(exc)
            self.sdl = None

    @property
    def connected(self):
        return bool(self.controllers)

    @property
    def device_names(self):
        return tuple(name for _, name in self.controllers.values())

    def reset_navigation(self):
        self.ready_controllers.clear()
        self.navigation.reset()

    def suspend(self):
        for controller, _ in self.controllers.values():
            self.sdl.SDL_GameControllerClose(controller)
        self.controllers.clear()
        self.unmapped_devices = ()
        if self.running:
            self.sdl.SDL_QuitSubSystem(self.FLAGS)
            self.running = False
        self.reset_navigation()
        self.next_scan = 0.

    def load_mappings(self, path, platform):
        try:
            lines = path.read_bytes().splitlines()
        except OSError:
            return False
        for line in lines:
            line = line.strip()
            if not line or line.startswith(b"#"):
                continue
            _, separator, metadata = line.partition(b",platform:")
            if not separator or metadata.split(b",", 1)[0] == platform:
                self.sdl.SDL_GameControllerAddMapping(line)
        return True

    def scan_controllers(self):
        unmapped = []
        # Indices can move on hotplug. SDL instance IDs identify already-open devices.
        self.sdl.SDL_LockJoysticks()
        try:
            for index in range(self.sdl.SDL_NumJoysticks()):
                instance = self.sdl.SDL_JoystickGetDeviceInstanceID(index)
                if instance < 0 or instance in self.controllers:
                    continue
                name = (self.sdl.SDL_JoystickNameForIndex(index) or b"SDL2").decode(errors="replace")
                if not self.sdl.SDL_IsGameController(index):
                    unmapped.append(name)
                    continue
                controller = self.sdl.SDL_GameControllerOpen(index)
                if controller:
                    self.controllers[instance] = controller, name
        finally:
            self.sdl.SDL_UnlockJoysticks()
        self.unmapped_devices = tuple(unmapped)

    def poll(self, active):
        if not active or self.sdl is None:
            self.suspend()
            return []
        now = self.clock()
        if not self.running:
            if now < self.next_scan:
                return []
            # GTK owns the window; SDL has no focused SDL window of its own.
            # The caller gates all polling by GTK focus and game lifecycle.
            self.sdl.SDL_SetHint(b"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", b"1")
            if self.sdl.SDL_InitSubSystem(self.FLAGS) != 0:
                self.error = (self.sdl.SDL_GetError() or b"SDL2 initialization failed").decode(errors="replace")
                self.next_scan = now + 1.
                return []
            self.error = None
            self.running = True
            bundled, source, profile = self.mapping_paths
            platform = self.sdl.SDL_GetPlatform()
            if not self.load_mappings(bundled, platform):
                self.load_mappings(source, platform)
            self.load_mappings(profile, platform)
        self.sdl.SDL_PumpEvents()
        # SDL owns no window here; discard only its own queued input so a long
        # launcher session cannot accumulate an unbounded event queue.
        self.sdl.SDL_FlushEvents(0, 0xFFFF)
        for instance, (controller, _) in tuple(self.controllers.items()):
            if not self.sdl.SDL_GameControllerGetAttached(controller):
                self.sdl.SDL_GameControllerClose(controller)
                del self.controllers[instance]
                self.ready_controllers.discard(instance)
        if now >= self.next_scan:
            self.next_scan = now + 1.
            self.scan_controllers()
        held = set()
        for instance, (controller, _) in self.controllers.items():
            buttons = self.held_actions(controller)
            if instance in self.ready_controllers:
                held.update(buttons)
            elif not buttons:
                self.ready_controllers.add(instance)
        if not self.ready_controllers:
            self.navigation.reset()
            return []
        # Physical and Steam virtual controllers can mirror the same press.
        return self.navigation.update(held, now)

    def held_actions(self, controller):
        held = {name for button, name in self.BUTTONS.items()
                if self.sdl.SDL_GameControllerGetButton(controller, button)}
        x = self.sdl.SDL_GameControllerGetAxis(controller, 0)
        y = self.sdl.SDL_GameControllerGetAxis(controller, 1)
        # A generous dead zone avoids drift and diagonal double navigation.
        if max(abs(x), abs(y)) >= 16000:
            held.add(("right" if x > 0 else "left") if abs(x) > abs(y)
                     else ("down" if y > 0 else "up"))
        return held
