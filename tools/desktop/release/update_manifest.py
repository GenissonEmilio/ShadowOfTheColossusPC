#!/usr/bin/env python3
"""Write the launcher manifest for the verified assets of a stable release."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from model import VERSION
from updates import MAX_DOWNLOAD, NAME, REPOSITORY


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("packages", type=Path, nargs="+")
    args = parser.parse_args()
    tag = f"v{VERSION}"
    allowed = {
        f"{NAME}-{VERSION}-linux-x86_64.tar.gz",
        f"{NAME}-{VERSION}-windows-x86_64.zip",
    }
    assets = []
    for package in args.packages:
        if package.name not in allowed:
            parser.error(f"Unexpected or duplicate release package: {package.name}")
        allowed.remove(package.name)
        size = package.stat().st_size
        if not 0 < size <= MAX_DOWNLOAD:
            parser.error(f"Invalid release package size: {package.name}")
        recorded = package.with_name(package.name + ".sha256").read_text(encoding="utf-8").split()
        with package.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if recorded != [digest, package.name]:
            parser.error(f"SHA-256 record does not match: {package.name}")
        assets.append({"name": package.name, "state": "uploaded", "size": size,
                       "browser_download_url": f"https://github.com/{REPOSITORY}/releases/download/{tag}/{package.name}",
                       "digest": f"sha256:{digest}"})
    manifest = {"tag_name": tag, "draft": False, "prerelease": False, "assets": assets}
    args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
