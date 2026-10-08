#!/usr/bin/env python3
"""本仓库启用公开提交保护，委托执行原有钩子，不覆盖原脚本。"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

HOOKS = {"applypatch-msg", "pre-applypatch", "post-applypatch", "pre-commit", "pre-merge-commit",
         "prepare-commit-msg", "commit-msg", "post-commit", "pre-rebase", "post-checkout", "post-merge",
         "pre-push", "pre-receive", "update", "proc-receive", "post-receive", "post-update",
         "reference-transaction", "push-to-checkout", "pre-auto-gc", "post-rewrite", "sendemail-validate",
         "fsmonitor-watchman", "p4-changelist", "p4-prepare-changelist", "p4-post-changelist",
         "p4-pre-submit", "post-index-change"}
STATE_NAME = "installation-state.json"
GUARDED_HOOKS = {"pre-commit", "pre-merge-commit", "pre-applypatch"}


class InstallError(Exception):
    pass


def git(root, *args, optional=False):
    result = subprocess.run(["git", *args], cwd=root, capture_output=True)
    if result.returncode and not optional:
        raise InstallError("无法安全配置本仓库的提交钩子；未显示内部错误内容。")
    return result.stdout.decode("utf-8").strip() if result.returncode == 0 else ""


def delegation_script(original):
    quoted = shlex.quote(original.as_posix())
    return f"""run_original_hook() {{
    if [ -x {quoted} ]; then
        {quoted} "$@"
        return $?
    fi
    return 0
}}
"""


def hook_script(name, original, python):
    start = "#!/bin/sh\n# passport-public-commit-hooks v1\n" + delegation_script(original)
    if name not in GUARDED_HOOKS:
        return start + 'run_original_hook "$@"\nexit $?\n'
    # Original hooks may stage generated files, so the privacy guard runs last.
    return start + f'''run_original_hook "$@" || exit $?
root=$(git rev-parse --show-toplevel) || exit 2
if [ ! -f "$root/scripts/check-public-commit.py" ]; then
    echo '缺少公开提交保护程序，提交已停止。' >&2
    exit 2
fi
exec {shlex.quote(python.as_posix())} "$root/scripts/check-public-commit.py" --all
'''


def restore_hook_config(root, prior_local, managed):
    current = git(root, "config", "--local", "--get-all", "core.hooksPath", optional=True).splitlines()
    if current == prior_local:
        return True
    if current != prior_local + [str(managed)]:
        return False  # Preserve a concurrent owner change and its active directory.
    git(root, "config", "--local", "--unset-all", "core.hooksPath")
    for value in prior_local:
        git(root, "config", "--local", "--add", "core.hooksPath", value)
    return True


def install(root):
    root = Path(git(root, "rev-parse", "--show-toplevel"))
    if not (root / "scripts/check-public-commit.py").is_file():
        raise InstallError("请先准备本项目的公开提交检查程序。")
    managed = Path(git(root, "rev-parse", "--git-path", "passport-public-hooks"))
    if not managed.is_absolute():
        managed = root / managed
    managed = managed.resolve()
    effective = git(root, "config", "--path", "--get", "core.hooksPath", optional=True)
    effective_path = Path(effective) if effective else Path(git(root, "rev-parse", "--git-path", "hooks"))
    if not effective_path.is_absolute():
        effective_path = root / effective_path
    effective_path = effective_path.resolve()
    if managed.exists():
        try:
            state = json.loads((managed / STATE_NAME).read_text())
            valid = (state.get("schema_version") == 1 and state.get("kind") == "passport-public-commit-hooks"
                     and effective_path == managed and isinstance(state.get("wrappers"), dict)
                     and "pre-commit" in state["wrappers"])
            valid = valid and all((managed / name).is_file() and not (managed / name).is_symlink()
                                  and os.access(managed / name, os.X_OK) and
                                  hashlib.sha256((managed / name).read_bytes()).hexdigest() == digest
                                  for name, digest in state["wrappers"].items())
        except (OSError, ValueError, KeyError, TypeError):
            valid = False
        if not valid:
            raise InstallError("已有钩子包装或配置与安装记录不一致；为保留原设置，未覆盖。")
        return False
    if managed == effective_path:
        raise InstallError("原钩子目录与保护目录冲突；未覆盖。")
    names = set(HOOKS)
    if effective_path.is_dir():
        names.update(path.name for path in effective_path.iterdir()
                     if path.is_file() and os.access(path, os.X_OK) and not path.name.endswith(".sample"))
    prior_local = git(root, "config", "--local", "--get-all", "core.hooksPath", optional=True).splitlines()
    managed.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".passport-hooks-", dir=managed.parent))
    published = False
    try:
        wrappers = {}
        for name in sorted(names):
            text = hook_script(name, effective_path / name, Path(sys.executable))
            path = stage / name
            path.write_text(text)
            path.chmod(0o755)
            wrappers[name] = hashlib.sha256(path.read_bytes()).hexdigest()
        state = {"schema_version": 1, "kind": "passport-public-commit-hooks",
                 "original_hooks_dir": str(effective_path), "prior_local_hooks_path": prior_local,
                 "wrappers": wrappers}
        (stage / STATE_NAME).write_text(json.dumps(state, indent=2) + "\n")
        (stage / STATE_NAME).chmod(0o600)
        stage.rename(managed)
        published = True
        # Retain prior local/global settings; only append this repository's entry.
        git(root, "config", "--local", "--add", "core.hooksPath", str(managed))
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        if published:
            # Keep an active delegating directory if rollback cannot be completed.
            try:
                restored = restore_hook_config(root, prior_local, managed)
            except (InstallError, OSError, UnicodeError):
                restored = False
            if restored:
                shutil.rmtree(managed)
        raise
    return True


def main(argv=None):
    argparse.ArgumentParser(description=__doc__).parse_args(argv)
    try:
        changed = install(Path.cwd())
    except (InstallError, OSError, UnicodeError, ValueError) as error:
        print(str(error) if isinstance(error, InstallError) else "钩子安装未完成；未显示内部错误内容。", file=sys.stderr)
        return 2
    print("已启用公开提交保护，并保留原钩子与原配置项。" if changed else "公开提交保护已启用，原钩子和配置未重复改动。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
