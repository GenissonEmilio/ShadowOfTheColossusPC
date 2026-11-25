"""Exercise both packaged headless commands without a display or game data."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path, required=True, help="Extracted package folder")
    args = parser.parse_args()
    profile = json.loads((args.program / "recomplauncher.json").read_text(encoding="utf-8"))
    executable = next((args.program / name).resolve() for name in profile["start"]["program"]
                      if (args.program / name).is_file())
    assert profile["prepare"]["program"] == profile["start"]["program"]
    environment = dict(os.environ)
    environment.pop("DISPLAY", None)
    environment.pop("WAYLAND_DISPLAY", None)
    with tempfile.TemporaryDirectory(prefix="sotc-recomplauncher-") as temporary:
        home = Path(temporary)
        disc = home / "Invalid disc.iso"
        disc.write_bytes(b"not a supported disc")
        data = home / "Player data"
        for operation in ("prepare", "start"):
            command = [str(executable), *[
                argument.replace("{game}", str(disc)).replace("{local}/ShadowOfTheColossusPC", str(data))
                for argument in profile[operation]["args"]
            ]]
            result = subprocess.run(command, env=environment, cwd=home, capture_output=True,
                                    text=True, encoding="utf-8", errors="replace", timeout=20)
            assert result.returncode == 65 and result.stderr.strip(), result
            if operation == "prepare":
                assert "progress 0.05 " in result.stdout, result
        assert not (data / "launcher.json").exists()
    print("RecompLauncher descriptor and packaged headless process pipes passed")


if __name__ == "__main__":
    main()
