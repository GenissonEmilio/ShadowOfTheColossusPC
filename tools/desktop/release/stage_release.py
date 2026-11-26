#!/usr/bin/env python3
"""Stage verified desktop packages in an existing GitHub draft release."""
import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tarfile
from urllib.parse import urlencode
import zipfile

from update_manifest import MAX_DOWNLOAD, NAME, REPOSITORY, VERSION


REPO_PATH = f"/repos/{REPOSITORY}"
METADATA_PATH = f"{NAME}/release.json"
PLATFORMS = {"windows": "windows-x86_64.zip", "linux": "linux-x86_64.tar.gz"}
MAX_RELEASE_METADATA = 4096
MAX_TAR_MEMBERS = 100_000
MAX_TAR_EXPANDED = 8 * 1024**3


class StageError(RuntimeError):
    pass


def github(path, *, asset=None):
    host = "api.github.com" if asset is None else "uploads.github.com"
    connection = http.client.HTTPSConnection(host, timeout=120, blocksize=1024**2)
    headers = {
        "Accept": "application/vnd.github+json",
        "Authorization": f"Bearer {os.environ['STAGING_TOKEN']}",
        "User-Agent": "SotC-Desktop-Release-Staging",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    try:
        if asset is None:
            connection.request("GET", path, headers=headers)
        else:
            headers["Content-Type"] = "application/octet-stream"
            headers["Content-Length"] = str(asset.stat().st_size)
            with asset.open("rb") as stream:
                connection.request("POST", f"{path}?{urlencode({'name': asset.name})}", stream, headers)
        response = connection.getresponse()
        expected_status = 200 if asset is None else 201
        if response.status != expected_status:
            item = path.split("?")[0] if asset is None else asset.name
            raise StageError(f"GitHub returned HTTP {response.status} for {item}")
        return json.load(response)
    except (OSError, http.client.HTTPException) as error:
        raise StageError(f"GitHub request to {host} failed ({type(error).__name__})") from None
    finally:
        connection.close()


def github_list(path):
    page = 1
    while True:
        entries = github(f"{path}?per_page=100&page={page}")
        yield from entries
        if len(entries) < 100:
            return
        page += 1


def context():
    if (os.environ.get("GITHUB_SERVER_URL") != "https://github.com"
            or os.environ.get("GITHUB_REPOSITORY") != REPOSITORY
            or os.environ.get("GITHUB_REF") != "refs/heads/main"
            or os.environ.get("GITHUB_EVENT_NAME") != "workflow_dispatch"):
        raise StageError(f"Dispatch this workflow from main in {REPOSITORY}")
    tag = os.environ["RELEASE_TAG"]
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag) or tag != f"v{VERSION}":
        raise StageError(f"The release tag must match desktop version v{VERSION}")
    commit = os.environ["GITHUB_SHA"]
    checked_out = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or checked_out != commit:
        raise StageError("The checked-out commit differs from GITHUB_SHA")
    runs = {platform: os.environ[f"{platform.upper()}_RUN_ID"] for platform in PLATFORMS}
    for platform, run_id in runs.items():
        if not re.fullmatch(r"[1-9][0-9]{0,14}", run_id):
            raise StageError(f"Invalid {platform} run ID")
    return tag, commit, runs


def verify_runs(commit, runs):
    for platform, run_id in runs.items():
        run = github(f"{REPO_PATH}/actions/runs/{run_id}")
        workflow_path = f".github/workflows/{platform}.yml"
        if (run.get("id") != int(run_id)
                or (run.get("repository") or {}).get("full_name") != REPOSITORY
                or (run.get("head_repository") or {}).get("full_name") != REPOSITORY
                or run.get("path") not in {workflow_path, f"{workflow_path}@main"}
                or run.get("head_sha") != commit
                or run.get("head_branch") != "main"
                or run.get("event") != "push"
                or run.get("status") != "completed"
                or run.get("conclusion") != "success"):
            raise StageError(f"{platform} run {run_id} must be a successful main push for {commit}")


def verify_draft(release, tag, commit):
    if (release.get("tag_name") != tag or release.get("draft") is not True
            or release.get("prerelease") is not False or release.get("target_commitish") != commit):
        raise StageError(f"Expected a stable draft release for {tag} targeting {commit}")


def draft_id(tag, commit):
    matches = [release for release in github_list(f"{REPO_PATH}/releases") if release.get("tag_name") == tag]
    if len(matches) != 1:
        raise StageError(f"Expected exactly one release for {tag}")
    release = matches[0]
    verify_draft(release, tag, commit)
    return int(release["id"])


def read_zip_metadata(package):
    with zipfile.ZipFile(package) as archive:
        members = [member for member in archive.infolist() if member.filename == METADATA_PATH]
        if len(members) != 1:
            raise StageError(f"Expected one {METADATA_PATH} in {package.name}")
        member = members[0]
        file_type = stat.S_IFMT(member.external_attr >> 16)
        if file_type not in (0, stat.S_IFREG) or not 0 < member.file_size <= MAX_RELEASE_METADATA:
            raise StageError(f"Invalid {METADATA_PATH} in {package.name}")
        return archive.read(member)


def read_tar_metadata(package):
    data = None
    expanded = 0
    with tarfile.open(package, "r|gz") as archive:
        for index, member in enumerate(archive, start=1):
            expanded += member.size
            if index > MAX_TAR_MEMBERS or expanded > MAX_TAR_EXPANDED:
                raise StageError(f"Archive exceeds inspection limits: {package.name}")
            if member.name == METADATA_PATH:
                if data is not None or not member.isfile() or not 0 < member.size <= MAX_RELEASE_METADATA:
                    raise StageError(f"Invalid {METADATA_PATH} in {package.name}")
                with archive.extractfile(member) as stream:
                    data = stream.read(MAX_RELEASE_METADATA + 1)
    if data is None:
        raise StageError(f"Missing {METADATA_PATH} in {package.name}")
    return data


def package_paths(root):
    packages = []
    sidecars = []
    metadata = []
    for platform, suffix in PLATFORMS.items():
        directory = root / platform
        package = directory / f"{NAME}-{VERSION}-{suffix}"
        sidecar = package.with_name(package.name + ".sha256")
        if set(directory.iterdir()) != {package, sidecar}:
            raise StageError(f"Unexpected files in the {platform} build artifact")
        for file, max_size in ((package, MAX_DOWNLOAD), (sidecar, 1024)):
            if file.is_symlink() or not file.is_file() or not 0 < file.stat().st_size <= max_size:
                raise StageError(f"Invalid file in the {platform} build artifact: {file.name}")
        data = read_zip_metadata(package) if platform == "windows" else read_tar_metadata(package)
        release = json.loads(data)
        platform_name = f"{platform}-x86_64"
        if (not isinstance(release, dict) or release.get("version") != VERSION
                or release.get("platform") != platform_name):
            raise StageError(f"Unexpected release metadata in {package.name}")
        packages.append(package)
        sidecars.append(sidecar)
        metadata.append({"name": package.name, "version": VERSION, "platform": platform_name})
    return packages, sidecars, metadata


def stage(root, tag, commit, runs, release_id):
    packages, sidecars, metadata = package_paths(root)
    manifest = root / "update-manifest.json"
    manifest_script = Path(__file__).with_name("update_manifest.py")
    subprocess.run([sys.executable, str(manifest_script), "--output", str(manifest), *map(str, packages)],
                   check=True)
    manifest_data = json.loads(manifest.read_text(encoding="utf-8"))
    assets = {asset["name"]: asset for asset in manifest_data["assets"]}
    for file in [*sidecars, manifest]:
        content = file.read_bytes()
        assets[file.name] = {"name": file.name, "state": "uploaded", "size": len(content),
                             "digest": f"sha256:{hashlib.sha256(content).hexdigest()}"}

    release_path = f"{REPO_PATH}/releases/{release_id}"
    assets_path = f"{release_path}/assets"
    verify_draft(github(release_path), tag, commit)
    existing = {asset["name"]: asset for asset in github_list(assets_path)}
    for name in assets.keys() & existing.keys():
        verify_asset(existing[name], assets[name])
    for file in [*packages, *sidecars, manifest]:
        if file.name in existing:
            print(f"Verified existing asset {file.name}", flush=True)
        else:
            verify_draft(github(release_path), tag, commit)
            verify_asset(github(assets_path, asset=file), assets[file.name])
            print(f"Staged {file.name}", flush=True)

    verify_draft(github(release_path), tag, commit)
    stored = {asset["name"]: asset for asset in github_list(assets_path)}
    report = {"tag_name": tag, "version": VERSION, "commit": commit, "release_id": release_id,
              **{f"{platform}_run_id": int(run_id) for platform, run_id in runs.items()},
              "packages": metadata, "assets": []}
    for name, expected in assets.items():
        asset = stored.get(name, {})
        verify_asset(asset, expected)
        report["assets"].append({field: asset[field] for field in ("name", "id", "size", "digest")})
    (root / "staging.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as summary:
        print(f"## Desktop assets staged for {tag}\n\nCommit `{commit}`\n", file=summary)
        print("| Asset | Bytes | SHA-256 |\n| --- | ---: | --- |", file=summary)
        for asset in report["assets"]:
            print(f"| {asset['name']} | {asset['size']} | `{asset['digest']}` |", file=summary)


def verify_asset(asset, expected):
    if any(asset.get(field) != expected[field] for field in ("name", "state", "size", "digest")):
        raise StageError(f"Release asset is absent or differs from the verified file: {expected['name']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("verify", "stage"))
    args = parser.parse_args()
    try:
        tag, commit, runs = context()
        release_id = draft_id(tag, commit)
        verify_runs(commit, runs)
        if args.command == "verify":
            with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as output:
                for platform, run_id in runs.items():
                    print(f"{platform}_run_id={run_id}", file=output)
        else:
            stage(Path(os.environ["RUNNER_TEMP"]) / "desktop-stage", tag, commit, runs, release_id)
    except StageError as error:
        parser.exit(1, f"{error}\n")
    except (KeyError, ValueError, OSError, zipfile.BadZipFile, tarfile.TarError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Staging failed ({type(error).__name__})\n")


if __name__ == "__main__":
    main()
