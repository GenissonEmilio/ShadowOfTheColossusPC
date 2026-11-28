#!/usr/bin/env python3
"""Restore the compiler and its notices from a verified Linux release."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import tarfile
import tempfile


PRODUCT = "ShadowOfTheColossusPC"
PLATFORM = "linux-x86_64"


def reuse_compiler(archive, version, expected_hash, build_id, output):
    if not (re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", version)
            and re.fullmatch(r"[0-9a-f]{64}", expected_hash)
            and re.fullmatch(r"[0-9a-f]{24}", build_id)):
        raise ValueError("Provide the prior tag, SHA-256 and compiler build_id together")

    targets = ("compiler", "distribution-licenses/native", "third-party-sources.tar.gz")
    if any((output / name).exists() or (output / name).is_symlink() for name in targets):
        raise ValueError("Compiler reuse needs a clean artifacts directory")

    with archive.open("rb") as source:
        if hashlib.file_digest(source, "sha256").hexdigest() != expected_hash:
            raise ValueError("Previous Linux package SHA-256 mismatch")
        source.seek(0)
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".compiler-reuse-", dir=output) as folder:
            staging = Path(folder)
            with tarfile.open(fileobj=source, mode="r|gz") as package:
                for member in package:
                    path = PurePosixPath(member.name)
                    if path.parts[:2] == (PRODUCT, "compiler"):
                        relative = path.parts[1:]
                    elif path.parts[:3] == (PRODUCT, "licenses", "native"):
                        relative = ("distribution-licenses", *path.parts[2:])
                    elif path.parts in ((PRODUCT, "release.json"), (PRODUCT, "third-party-sources.tar.gz")):
                        relative = path.parts[1:]
                    else:
                        continue
                    if ".." in relative or not (member.isdir() or member.isfile()):
                        raise ValueError(f"Unexpected link or path in compiler kit: {member.name}")
                    destination = staging.joinpath(*relative)
                    if member.isdir():
                        destination.mkdir(parents=True, exist_ok=True)
                    else:
                        destination.parent.mkdir(parents=True, exist_ok=True)
                        with package.extractfile(member) as data, destination.open("xb") as target:
                            shutil.copyfileobj(data, target)
                        destination.chmod(member.mode & 0o777)
            release = json.loads((staging / "release.json").read_bytes())
            manifest = json.loads((staging / "compiler/manifest.json").read_bytes())
            if (release != {"version": version[1:], "platform": PLATFORM}
                    or manifest.get("build_id") != build_id):
                raise ValueError("Previous Linux package version, platform or compiler build_id mismatch")
            for name in ("compiler/bin/ps2_recomp", "distribution-licenses/native/SOURCES.txt",
                         "third-party-sources.tar.gz"):
                if not (staging / name).is_file():
                    raise ValueError(f"Missing compiler kit file: {name}")
            for name in targets:
                destination = output / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                (staging / name).rename(destination)

    print(f"Reused Linux compiler build_id {build_id} from {version}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("version")
    parser.add_argument("sha256")
    parser.add_argument("build_id")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    reuse_compiler(args.archive, args.version, args.sha256, args.build_id, args.output)
