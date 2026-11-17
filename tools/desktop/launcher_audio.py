"""Optional synthesized feedback for launcher actions."""
from array import array
import ctypes
import math
import sys
import time

from sdl import load_sdl


_RATE = 24000
_SDL_INIT_AUDIO = 0x10
_AUDIO_S16SYS = 0x8010 if sys.byteorder == "little" else 0x9010
# Start and end frequencies, duration in seconds, amplitude relative to full scale.
_CUES = {
    "focus": (580, 678, .038, .042),
    "confirm": (525, 785, .096, .075),
    "back": (553, 360, .075, .065),
    "error": (286, 197, .106, .071),
}


class _AudioSpec(ctypes.Structure):
    _fields_ = [
        ("freq", ctypes.c_int),
        ("format", ctypes.c_uint16),
        ("channels", ctypes.c_uint8),
        ("silence", ctypes.c_uint8),
        ("samples", ctypes.c_uint16),
        ("padding", ctypes.c_uint16),
        ("size", ctypes.c_uint32),
        ("callback", ctypes.c_void_p),
        ("userdata", ctypes.c_void_p),
    ]


def _synthesize(start, end, duration, amplitude):
    frames = round(_RATE * duration)
    samples = array("h")
    for n in range(frames):
        t = n / _RATE
        phase = math.tau * (start * t + (end - start) * t * t / (2 * duration))
        # Ease in to avoid a click; taper to silence with a short, soft tail.
        attack = min(1., t / .004)
        decay = (1. - n / (frames - 1)) ** 2
        tone = (.87 * math.sin(phase) + .13 * math.sin(phase * 2))
        samples.append(round(32767 * amplitude * attack * decay * tone))
    return samples.tobytes()


class LauncherAudio:
    """Queue brief cues on a private SDL playback device when available."""

    def __init__(self, program, enabled=True, clock=time.monotonic):
        self.program = program
        self.enabled = enabled
        self.clock = clock
        self._sdl = None
        self._device = 0
        self._failed = False
        self._last_cue = None
        self._last_at = float("-inf")
        self._samples = {}

    def _open(self):
        sdl = load_sdl(self.program)
        if sdl is None:
            return False
        uint32 = ctypes.c_uint32
        spec = ctypes.POINTER(_AudioSpec)
        signatures = {
            "SDL_InitSubSystem": ([uint32], ctypes.c_int),
            "SDL_QuitSubSystem": ([uint32], None),
            "SDL_OpenAudioDevice": ([ctypes.c_char_p, ctypes.c_int, spec, spec, ctypes.c_int], uint32),
            "SDL_PauseAudioDevice": ([uint32, ctypes.c_int], None),
            "SDL_CloseAudioDevice": ([uint32], None),
            "SDL_GetQueuedAudioSize": ([uint32], uint32),
            "SDL_ClearQueuedAudio": ([uint32], None),
            "SDL_QueueAudio": ([uint32, ctypes.c_void_p, uint32], ctypes.c_int),
        }
        try:
            for name, (arguments, result) in signatures.items():
                function = getattr(sdl, name)
                function.argtypes, function.restype = arguments, result
        except AttributeError:
            return False
        if sdl.SDL_InitSubSystem(_SDL_INIT_AUDIO) != 0:
            return False
        desired = _AudioSpec(freq=_RATE, format=_AUDIO_S16SYS, channels=1, samples=512)
        # SDL converts to the hardware format when it cannot use the requested format.
        device = sdl.SDL_OpenAudioDevice(None, 0, ctypes.byref(desired), None, 0)
        if not device:
            sdl.SDL_QuitSubSystem(_SDL_INIT_AUDIO)
            return False
        self._sdl, self._device = sdl, device
        sdl.SDL_PauseAudioDevice(device, 0)
        return True

    def play(self, cue):
        if not self.enabled or self._failed or cue not in _CUES:
            return
        now = self.clock()
        elapsed = now - self._last_at
        if ((cue == "focus" and elapsed < .085)
                or (cue == self._last_cue and elapsed < .060)):
            return
        if not self._device and not self._open():
            self._failed = True
            return
        # Keep at most one cue waiting in SDL. Action cues can preempt focus;
        # a new focus event never interrupts an action.
        if self._sdl.SDL_GetQueuedAudioSize(self._device):
            if cue == "focus" or self._last_cue != "focus":
                return
            self._sdl.SDL_ClearQueuedAudio(self._device)
        if cue not in self._samples:
            self._samples[cue] = _synthesize(*_CUES[cue])
        samples = self._samples[cue]
        if self._sdl.SDL_QueueAudio(self._device, samples, len(samples)) != 0:
            self.close()
            self._failed = True
            return
        self._last_cue, self._last_at = cue, now

    def set_enabled(self, enabled):
        if self.enabled != enabled:
            self.enabled = enabled
            self._failed = False
            if not enabled:
                self.close()

    def close(self):
        """Release playback now; an enabled instance can reopen on a later cue."""
        if self._device:
            self._sdl.SDL_ClearQueuedAudio(self._device)
            self._sdl.SDL_CloseAudioDevice(self._device)
            self._sdl.SDL_QuitSubSystem(_SDL_INIT_AUDIO)
            self._device, self._sdl = 0, None
        self._last_cue = None
        self._last_at = float("-inf")
