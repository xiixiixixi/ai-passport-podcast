#!/usr/bin/env python3
"""检查暂存区中实际将提交的内容，只允许审阅过的公开文件。"""
from __future__ import annotations

import argparse
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import types

FILE_LIST = ".github/public-files.json"
POLICY_FILE = "scripts/export-release.py"
MAX_FILE_BYTES = 64 * 1024 * 1024
INTERNAL_IMAGE_DOCUMENTS = {"firmware/docs/README.md", "firmware/docs/README.zh_CN.md"}


class CheckError(Exception):
    pass


def git(root, *args):
    result = subprocess.run(["git", *args], cwd=root, capture_output=True)
    if result.returncode:
        raise CheckError("无法读取版本管理暂存区，提交检查未完成。")
    return result.stdout


def index_entries(root):
    entries = {}
    for record in git(root, "ls-files", "--stage", "-z").split(b"\0"):
        if not record:
            continue
        try:
            metadata, raw_name = record.split(b"\t", 1)
            mode, oid, stage = metadata.decode("ascii").split()
            name = raw_name.decode("utf-8")
        except (ValueError, UnicodeError) as error:
            raise CheckError("暂存区含无法安全读取的文件名或记录。") from error
        if stage != "0":
            raise CheckError("暂存区仍有未解决的合并冲突。")
        if any(ord(character) < 32 for character in name):
            raise CheckError("暂存区含控制字符文件名；文件名未显示。")
        entries[name] = (mode, oid)
    return entries


def blob(root, entry):
    mode, oid = entry
    if mode not in {"100644", "100755"}:
        raise CheckError("公开策略或文件清单必须是普通文件。")
    size = int(git(root, "cat-file", "-s", oid))
    if size > MAX_FILE_BYTES:
        raise CheckError("暂存文件超过公开源码大小上限。")
    return git(root, "cat-file", "blob", oid)


def metadata_bytes(root, entries, name, unborn):
    if name in entries:
        return blob(root, entries[name])
    # An original development repository may not yet have its first commit.
    # Only these explicit reviewed policy files can fall back to disk there.
    if unborn:
        path = root / name
        if path.is_file() and not path.is_symlink() and path.stat().st_size <= MAX_FILE_BYTES:
            return path.read_bytes()
    raise CheckError("暂存区缺少公开策略或明确公开文件清单，不能确认可提交。")


def load_policy(root, entries, unborn):
    module = types.ModuleType("passport_public_commit_policy")
    module.__file__ = str(root / POLICY_FILE)
    sys.modules[module.__name__] = module
    try:
        code = metadata_bytes(root, entries, POLICY_FILE, unborn)
        exec(compile(code, POLICY_FILE, "exec"), module.__dict__)
    except CheckError:
        raise
    except Exception as error:
        raise CheckError("暂存的公开导出策略无法载入；未显示错误内容。") from error
    for name in ("allowed_source", "sensitive_reason", "check_public_file", "load_private_values"):
        if not callable(getattr(module, name, None)):
            raise CheckError("公开导出策略缺少必要的隐私检查。")
    return module


def load_file_list(root, entries, unborn, policy, private_values):
    try:
        data = metadata_bytes(root, entries, FILE_LIST, unborn)
        policy.check_public_file(FILE_LIST, data, private_values, "commit")
        record = json.loads(data)
    except CheckError:
        raise
    except Exception as error:
        raise CheckError("公开文件清单无效或含敏感信息；未显示清单内容。") from error
    files = record.get("files") if isinstance(record, dict) else None
    if not isinstance(record, dict) or record.get("schema_version") != 1 or not isinstance(files, list):
        raise CheckError("公开文件清单需要第一版格式和文件数组。")
    if any(not isinstance(name, str) or not name or any(ord(c) < 32 for c in name) for name in files):
        raise CheckError("公开文件清单含无效文件名。")
    if len(set(files)) != len(files) or FILE_LIST not in files or POLICY_FILE not in files:
        raise CheckError("公开文件清单重复或缺少自身及公开策略文件。")
    for name in files:
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts or path.as_posix() != name:
            raise CheckError("公开文件清单含非项目相对路径。")
        if name not in policy.PUBLIC_DOCUMENT_VARIANTS and not policy.allowed_source(path):
            raise CheckError("公开文件清单不能授权私人、内部或生成文件。")
        if policy.sensitive_reason(name.encode(), name, private_values=private_values):
            raise CheckError("公开文件清单含敏感文件名；文件名未显示。")
    return set(files)


def staged_blobs(root, names, entries):
    process = subprocess.Popen(["git", "cat-file", "--batch"], cwd=root,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        for name in names:
            process.stdin.write(entries[name][1].encode("ascii") + b"\n")
            process.stdin.flush()
            header = process.stdout.readline().split()
            if len(header) != 3 or header[1] != b"blob":
                raise CheckError("无法读取暂存文件对象。")
            size = int(header[2])
            if size > MAX_FILE_BYTES:
                raise CheckError("暂存文件超过公开源码大小上限。")
            data = process.stdout.read(size)
            if len(data) != size or process.stdout.read(1) != b"\n":
                raise CheckError("暂存文件对象不完整。")
            yield name, data
    finally:
        process.stdin.close()
        if process.poll() is None:
            process.terminate()
        process.wait()


def check(root, *, all_files=False, private_blocklist=None):
    root = Path(git(root, "rev-parse", "--show-toplevel").decode().strip())
    entries = index_entries(root)
    if not entries:
        return 0, []
    unborn = subprocess.run(["git", "rev-parse", "--verify", "HEAD"], cwd=root,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0
    policy = load_policy(root, entries, unborn)
    try:
        private_values = policy.load_private_values(root, private_blocklist)
    except policy.ExportError as error:
        raise CheckError("本机私密拦截清单无法载入，提交已停止。") from error
    public_files = load_file_list(root, entries, unborn, policy, private_values)
    if all_files:
        selected = set(entries)
    else:
        selected = {name.decode("utf-8") for name in git(root, "diff", "--cached", "--name-only", "-z",
                    "--no-renames", "--diff-filter=ACMRTUXB").split(b"\0") if name}
    failures, readable = [], []
    for name in sorted(selected):
        if name not in entries:  # Deletions are allowed, including removal of private files.
            continue
        display = name
        if policy.sensitive_reason(name.encode(), name, private_values=private_values):
            display = "〔敏感文件名已隐藏〕"
        if name not in public_files:
            failures.append((display, "未登记为必要公开文件"))
        elif entries[name][0] not in {"100644", "100755"}:
            failures.append((display, "符号链接或子模块不能公开提交"))
        elif name not in policy.PUBLIC_DOCUMENT_VARIANTS and not policy.allowed_source(PurePosixPath(name)):
            failures.append((display, "私人、内部或生成文件不能提交"))
        else:
            readable.append(name)
    for name, data in staged_blobs(root, readable, entries):
        try:
            policy.check_public_file(name, data, private_values, "commit")
        except policy.ExportError:
            failures.append((name if not policy.sensitive_reason(name.encode(), name, private_values=private_values)
                             else "〔敏感文件名已隐藏〕", "含敏感信息或凭据"))
            continue
        if name in policy.PUBLIC_DOCUMENT_VARIANTS and data != policy.PUBLIC_DOCUMENT_VARIANTS[name]:
            failures.append((name, "必须使用公开指针文档，不能提交原内部素材目录"))
        elif name in INTERNAL_IMAGE_DOCUMENTS and re.search(rb'''(?:src|srcset)\s*=\s*["']\.\./assets/images/''', data, re.I):
            failures.append((name, "仍引用应排除的内部图片"))
    return len(selected), failures


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true", help="检查整个暂存区，包括未修改的已跟踪文件")
    parser.add_argument("--private-blocklist", help="可选.local内私密JSON清单；默认沿用公开导出器配置")
    args = parser.parse_args(argv)
    try:
        count, failures = check(Path.cwd(), all_files=args.all, private_blocklist=args.private_blocklist)
    except (CheckError, OSError, UnicodeError, ValueError) as error:
        print(str(error) if isinstance(error, CheckError) else "提交检查未完成；未显示内部错误内容。", file=sys.stderr)
        return 2
    if failures:
        print("提交已阻止：请先清理暂存区或审查并更新公开文件清单。", file=sys.stderr)
        for name, reason in failures[:20]:
            print(f"- {reason}：{name}", file=sys.stderr)
        return 1
    print(f"公开提交检查通过：已检查{count}个暂存文件；提交文件内容均来自暂存区。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
