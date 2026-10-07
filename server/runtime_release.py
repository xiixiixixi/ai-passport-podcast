"""Fingerprint running backend code without including household configuration."""
import hashlib
import json
import re
from pathlib import Path


def source_release(directory, release_id=""):
    directory = Path(directory)
    paths = sorted([*directory.glob("*.py"), directory / "requirements.txt",
                    *(directory / "static").rglob("*")], key=lambda path: path.relative_to(directory).as_posix())
    files = {}
    for path in paths:
        if path.is_file() and not path.is_symlink():
            files[path.relative_to(directory).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    content = json.dumps(files, sort_keys=True, separators=(",", ":")).encode("utf-8")
    fingerprint = hashlib.sha256(content).hexdigest()
    # Deployment labels may identify a build, but cannot replace its actual code
    # fingerprint or leak arbitrary operator-supplied environment content.
    if not isinstance(release_id, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,79}", release_id):
        release_id = "source-" + fingerprint[:16]
    return {"id": release_id, "source_sha256": fingerprint, "files": len(files), "schema": 1}
