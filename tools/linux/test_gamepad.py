#!/usr/bin/env python3
"""Local USB integration: virtual devices -> GLFW -> real DualShock packet.

Requires python-evdev, a display, /dev/uinput access, and the gamepad.cpp probe.
Creates only temporary game controllers; never injects keyboard or mouse input.
"""
import argparse
import os
from pathlib import Path
import subprocess
import time


def main():
    from evdev import UInput, AbsInfo, ecodes as e
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=Path('artifacts/fidelity/gamepad.log'))
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    axes = [(axis, AbsInfo(128, 0, 255, 0, 0, 0))
            for axis in (e.ABS_X, e.ABS_Y, e.ABS_Z, e.ABS_RX, e.ABS_RY)]
    axes += [(e.ABS_HAT0X, AbsInfo(0, -1, 1, 0, 0, 0)),
             (e.ABS_HAT0Y, AbsInfo(0, -1, 1, 0, 0, 0))]
    caps = {e.EV_KEY: list(range(e.BTN_JOYSTICK, e.BTN_JOYSTICK + 12)), e.EV_ABS: axes}
    with UInput(caps, name='Shadow synthetic unmapped USB', vendor=0xfeed,
                product=0x9901, version=0x0107, bustype=e.BUS_USB):
        with UInput(caps, name='USB Gamepad', vendor=0x0079, product=0x0006,
                    version=0x0107, bustype=e.BUS_USB) as pad:
            time.sleep(.6)
            env = dict(os.environ)
            env.pop('PS2X_INPUT_SCRIPT', None)
            with args.out.open('w') as log:
                process = subprocess.Popen([str(args.probe.resolve())], env=env,
                                           stdout=log, stderr=subprocess.STDOUT)
                try:
                    time.sleep(1)
                    pad.write(e.EV_KEY, e.BTN_JOYSTICK + 2, 1)
                    pad.write(e.EV_ABS, e.ABS_HAT0Y, -1)
                    pad.write(e.EV_ABS, e.ABS_X, 255)
                    pad.syn()
                    time.sleep(1)
                    pad.write(e.EV_KEY, e.BTN_JOYSTICK + 2, 0)
                    pad.write(e.EV_ABS, e.ABS_HAT0Y, 0)
                    pad.write(e.EV_ABS, e.ABS_X, 128)
                    pad.syn()
                    code = process.wait(timeout=12)
                finally:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
            print(args.out.read_text()[-1500:])
            return code


if __name__ == '__main__':
    raise SystemExit(main())
