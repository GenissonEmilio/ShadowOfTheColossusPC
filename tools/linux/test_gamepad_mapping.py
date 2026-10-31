#!/usr/bin/env python3
"""Local end-to-end test: an unknown USB joystick is calibrated and used in-game.

Only virtual gamepad events are sent; requires python-evdev and /dev/uinput.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    from evdev import UInput, AbsInfo, ecodes as e
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool', type=Path, required=True)
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--out', type=Path, default=Path('artifacts/controller-roundtrip'))
    args = p.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    profile, status = out / 'gamecontrollerdb.txt', out / 'status.json'
    axes = [(a, AbsInfo(128, 0, 255, 0, 0, 0)) for a in (e.ABS_X, e.ABS_Y, e.ABS_Z, e.ABS_RX, e.ABS_RY)]
    axes += [(e.ABS_HAT0X, AbsInfo(0, -1, 1, 0, 0, 0)), (e.ABS_HAT0Y, AbsInfo(0, -1, 1, 0, 0, 0))]
    caps = {e.EV_KEY: list(range(e.BTN_JOYSTICK, e.BTN_JOYSTICK + 12)), e.EV_ABS: axes}
    events = [(e.EV_KEY, e.BTN_JOYSTICK + i, 1, 0) for i in range(12)]
    events += [(e.EV_ABS, e.ABS_HAT0Y, -1, 0), (e.EV_ABS, e.ABS_HAT0Y, 1, 0),
               (e.EV_ABS, e.ABS_HAT0X, -1, 0), (e.EV_ABS, e.ABS_HAT0X, 1, 0)]
    events += [(e.EV_ABS, a, 255, 128) for a in (e.ABS_X, e.ABS_Y, e.ABS_RX, e.ABS_RY)]
    with UInput(caps, name='Shadow unmapped placeholder', vendor=0xfeed, product=0x9901,
                version=0x0107, bustype=e.BUS_USB):
        with UInput(caps, name='Shadow custom generic USB', vendor=0xfeed, product=0x9902,
                    version=0x0107, bustype=e.BUS_USB) as pad:
            time.sleep(.6)
            status.unlink(missing_ok=True)
            with (out / 'setup.log').open('w') as log:
                process = subprocess.Popen([str(args.tool.resolve()), '--output', str(profile), '--device', '1',
                    '--status', str(status), '--screenshot', str(out / 'setup.png')], stdout=log, stderr=subprocess.STDOUT)
                deadline, last = time.monotonic() + 35, -1
                try:
                    while process.poll() is None and time.monotonic() < deadline:
                        try:
                            step = json.loads(status.read_text())['step']
                        except (OSError, ValueError):
                            time.sleep(.03)
                            continue
                        if 0 <= step < len(events) and step != last:
                            time.sleep(.4)  # release/neutral gate before the next binding
                            kind, code, value, rest = events[step]
                            pad.write(kind, code, value)
                            pad.syn()
                            time.sleep(.15)
                            pad.write(kind, code, rest)
                            pad.syn()
                            last = step
                        time.sleep(.03)
                    code = process.wait(timeout=2)
                    assert code == 0, (code, (out / 'setup.log').read_text()[-2000:])
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=3)
            mapping = profile.read_text()
            assert 'Shadow custom generic USB' in mapping and 'a:b0,' in mapping, mapping
            with (out / 'gamepad.log').open('w') as log:
                env = dict(os.environ, PS2X_GAMEPAD_MAPPINGS=str(profile))
                env.pop('PS2X_INPUT_SCRIPT', None)
                env.pop('SDL_GAMECONTROLLERCONFIG', None)
                process = subprocess.Popen([str(args.probe.resolve())], env=env, stdout=log, stderr=subprocess.STDOUT)
                try:
                    time.sleep(1)
                    pad.write(e.EV_KEY, e.BTN_JOYSTICK, 1)
                    pad.write(e.EV_ABS, e.ABS_HAT0Y, -1)
                    pad.write(e.EV_ABS, e.ABS_X, 255)
                    pad.syn()
                    time.sleep(1)
                    pad.write(e.EV_KEY, e.BTN_JOYSTICK, 0)
                    pad.write(e.EV_ABS, e.ABS_HAT0Y, 0)
                    pad.write(e.EV_ABS, e.ABS_X, 128)
                    pad.syn()
                    code = process.wait(timeout=12)
                    assert code == 0, (out / 'gamepad.log').read_text()[-2000:]
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=3)
            result = {'passed': True, 'scope': 'unknown USB joystick -> setup wizard -> persisted mapping -> native DualShock packet',
                      'mapping': mapping.strip(), 'last_step': last}
            (out / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
            print(json.dumps(result))


if __name__ == '__main__':
    main()
