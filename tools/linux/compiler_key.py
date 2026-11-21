"""Keep GUI/docs updates independent from the compiled game's cache."""
import hashlib


def compiler_key(directory):
    digest = hashlib.sha256()
    for path in sorted(directory.rglob("*")):
        if not path.is_file() or path.name == "manifest.json" or "__pycache__" in path.parts or path.suffix == ".pyc":
            continue
        relative = path.relative_to(directory).as_posix()
        if relative == "bin/sotc-input-tool":
            continue  # controller setup does not alter generated guest code or runtime
        if relative.startswith(("licenses/", "source/tools/linux/", "source/docs/", "source/tests/")):
            continue
        if relative.startswith("source/tools/desktop/") and relative != "source/tools/desktop/pipeline.py":
            continue
        if relative in ("packages.txt", "source/README.md", "source/LICENSE"):
            continue
        digest.update(relative.encode())
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()[:24]
