#!/usr/bin/env python3
"""Select and verify the prior stable compiler package for a launcher build."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
from urllib.error import HTTPError
from urllib.request import Request, urlopen
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from updates import MAX_METADATA, NAME, REPOSITORY, download, https_context, release_package, release_version

TAG = re.compile(r"v[0-9]{1,5}\.[0-9]{1,5}\.[0-9]{1,5}")
DIGEST = re.compile(r"[0-9a-f]{64}")
BUILD_ID = re.compile(r"[0-9a-f]{24}")
SYSTEMS = ("linux-x86_64", "windows-x86_64")
LINUX_WORKFLOW = ".github/workflows/linux.yml"
WINDOWS_WORKFLOW = ".github/workflows/windows.yml"


def launcher_only_path(path, system):
    if path.startswith("tools/desktop/"):
        # The pipeline controls game recompilation and consumes these disc helpers.
        return path not in {"tools/desktop/pipeline.py", "tools/desktop/import_disc.py"}
    if path.startswith(("tests/", "docs/", ".github/workflows/")):
        return True
    if path in {"README.md", ".recomp.json", "recomplauncher.json", "tools/linux/reuse_compiler.py"}:
        return True
    if system == "linux-x86_64" and path.startswith("tools/windows/"):
        return True
    if system == "windows-x86_64" and path.startswith("tools/linux/"):
        return path not in {"tools/linux/stage_source.py", "tools/linux/input_tool.cpp", "tools/linux/video_tool.cpp"}
    return path in {"tools/windows/windows_bootstrap.py", "tools/windows/launcher.cpp",
                    "tools/windows/launcher.rc", "tools/windows/make_icon.py", "tools/windows/LEIA-ME.txt"}


def git(*args):
    return subprocess.check_output(["git", *args], encoding="utf-8")


def windows_recipe(ref):
    """Compare the Windows kit configuration embedded in the CI workflow."""
    workflow = git("show", f"{ref}:{WINDOWS_WORKFLOW}")
    pieces = (
        r"^    runs-on: .+$",
        r"^      - uses: msys2/setup-msys2@[^\n]+\n(?:(?!^      - ).*\n)*",
        r"^      - name: Configure the native Windows build\n(?:(?!^      - ).*\n)*",
        r"^ +targets\+=\(.*\)$",
        r"^ +cmake --build build/windows .+$",
    )
    matches = [re.search(piece, workflow, re.MULTILINE) for piece in pieces]
    return tuple(match.group() for match in matches) if all(matches) else None


def changed_inputs(base, system):
    changed = git("diff", "--name-only", "--no-renames", "-z", base, "HEAD").split("\0")
    inputs = [path for path in changed if path and not launcher_only_path(path, system)]
    if system == "windows-x86_64" and WINDOWS_WORKFLOW in changed:
        previous, current = windows_recipe(base), windows_recipe("HEAD")
        if previous is None or previous != current:
            inputs.append(WINDOWS_WORKFLOW)
    if system == "linux-x86_64" and LINUX_WORKFLOW in changed:
        native_run = "        run: tools/linux/build.sh\n"
        if any(git("show", f"{ref}:{LINUX_WORKFLOW}").count(native_run) != 1 for ref in (base, "HEAD")):
            inputs.append(LINUX_WORKFLOW)
    return inputs


def official_release(tag):
    suffix = f"tags/{tag}" if tag else "latest"
    url = f"https://api.github.com/repos/{REPOSITORY}/releases/{suffix}"
    request = Request(url, headers={"Accept": "application/vnd.github+json", "User-Agent": f"{NAME}-compiler-ci"})
    if token := os.environ.get("GH_TOKEN"):
        request.add_unredirected_header("Authorization", f"Bearer {token}")
    try:
        with urlopen(request, timeout=20, context=https_context()) as response:
            if response.geturl() != url:
                raise ValueError("GitHub redirected the stable release metadata")
            raw = response.read(MAX_METADATA + 1)
    except HTTPError as error:
        if error.code != 404 or tag:
            raise
        error.close()
        return None
    if len(raw) > MAX_METADATA:
        raise ValueError("GitHub stable release metadata is too large")
    release = json.loads(raw)
    if (not isinstance(release, dict) or release.get("draft") is not False
            or release.get("prerelease") is not False or not isinstance(release.get("tag_name"), str)
            or (tag is not None and release["tag_name"] != tag)
            or not isinstance(release.get("assets"), list)
            or not all(isinstance(asset, dict) for asset in release["assets"])):
        raise ValueError("Invalid GitHub stable release metadata")
    return release


def archived_build_id(archive, tag, system):
    metadata = {f"{NAME}/release.json", f"{NAME}/compiler/manifest.json"}
    values = {}
    if system == "windows-x86_64":
        with zipfile.ZipFile(archive) as bundle:
            for name in sorted(metadata):
                entry = bundle.getinfo(name)
                if entry.file_size > MAX_METADATA:
                    raise ValueError(f"Archive metadata is too large: {name}")
                values[name] = json.loads(bundle.read(entry))
    else:
        with tarfile.open(archive, "r|gz") as bundle:
            for entry in bundle:
                if entry.name not in metadata:
                    continue
                if not entry.isfile() or entry.size > MAX_METADATA:
                    raise ValueError(f"Invalid archive metadata: {entry.name}")
                with bundle.extractfile(entry) as stream:
                    values[entry.name] = json.load(stream)
                if values.keys() == metadata:
                    break
    manifest = values.get(f"{NAME}/compiler/manifest.json")
    build_id = manifest.get("build_id") if isinstance(manifest, dict) else None
    if (values.get(f"{NAME}/release.json") != {"version": tag[1:], "platform": system}
            or not isinstance(build_id, str) or not BUILD_ID.fullmatch(build_id)):
        raise ValueError("Previous package version, platform or compiler build_id mismatch")
    return build_id


def select(system, archive, *, tag="", digest="", build_id="", force_build=False):
    manual = bool(tag or digest or build_id)
    if manual and (force_build or not (TAG.fullmatch(tag) and DIGEST.fullmatch(digest)
                                      and BUILD_ID.fullmatch(build_id))):
        raise ValueError("Provide all three prior compiler inputs without forcing a new build")
    if force_build:
        print("New compiler build requested")
        return None
    release = official_release(tag or None)
    if release is None or not TAG.fullmatch(release["tag_name"]):
        print("No standard stable release found; building a new compiler kit")
        return None
    tag = release["tag_name"]
    package = release_package(release, release_version(tag), system, strict=True)
    if package is None:
        if manual:
            raise ValueError(f"No official {system} package in {tag}")
        print(f"No official {system} package in {tag}; building a new compiler kit")
        return None
    if manual and digest != package.sha256:
        raise ValueError("Requested SHA-256 differs from GitHub's official package digest")

    git("fetch", "--no-tags", "--depth=1", f"https://github.com/{REPOSITORY}.git", f"refs/tags/{tag}")
    if inputs := changed_inputs("FETCH_HEAD", system):
        reason = "Compiler inputs changed since " + tag + ": " + ", ".join(inputs)
        if manual:
            raise ValueError(reason)
        print(reason)
        return None

    with tempfile.TemporaryDirectory(prefix=".sotc-compiler-", dir=archive.parent) as folder:
        prior = Path(folder) / package.name
        download(package, prior, lambda _progress: None)
        previous_id = archived_build_id(prior, tag, system)
        if manual and build_id != previous_id:
            raise ValueError("Requested build_id differs from the verified package manifest")
        prior.rename(archive)
    print(f"Selected {tag} {system} compiler kit {previous_id}")
    return {"COMPILER_VERSION": tag, "COMPILER_SHA256": package.sha256, "COMPILER_BUILD_ID": previous_id}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("system", choices=SYSTEMS)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--force-build", action="store_true")
    args = parser.parse_args()
    prior = select(args.system, args.archive, tag=os.environ.get("COMPILER_VERSION", ""),
                   digest=os.environ.get("COMPILER_SHA256", ""), build_id=os.environ.get("COMPILER_BUILD_ID", ""),
                   force_build=args.force_build)
    with Path(os.environ["GITHUB_ENV"]).open("a", encoding="utf-8") as environment:
        for key, value in (prior or {}).items():
            environment.write(f"{key}={value}\n")


if __name__ == "__main__":
    main()
