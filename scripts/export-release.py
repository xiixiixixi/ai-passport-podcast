#!/usr/bin/env python3
"""Export an explicitly selected, verified archive and a clean public source tree.

This tool never builds, installs, flashes, uploads, commits or pushes. A retained
archive must be selected by a project-relative path; no "latest build" guessing.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import uuid
import zipfile

ROOT_TREES = {"firmware", "server", "scripts", "tests", "docs", "assets", ".github"}
ROOT_FILES = {"README.md", "README.en.md", "README.zh_CN.md", "LICENSE", ".gitignore", "install.command", "install.ps1"}
EXCLUDED_DIRS = {".git", "build", "managed_components", ".env", ".local", "data", "media", "__pycache__", "node_modules", ".venv", "venv", ".pytest_cache", "dist", "logs", "private", "backups", "backup", "cache", ".cache", ".idea", ".vscode", ".agents", ".codex", "__macosx"}
EXCLUDED_FILES = {".DS_Store", "sdkconfig", "sdkconfig.old", "compile_commands.json", "CMakeCache.txt", "flash_args"}
EXCLUDED_SUFFIXES = {".pyc", ".pyo", ".elf", ".map", ".bin", ".db", ".sqlite", ".sqlite3", ".sqlite-wal", ".sqlite-shm", ".log", ".pid", ".dump", ".zip", ".tar", ".gz"}
# This is a checked-in interface stub needed by the clean source host tests,
# not runtime cache data. Do not exempt any other cache directory or file.
PUBLIC_SOURCE_EXCEPTIONS = {
    "firmware/tests/podcast_cover_stubs/src/misc/cache/instance/lv_image_cache.h",
}
# Internal repair history and externally sourced design reference are not
# public documentation/assets. Keep these files locally, out of both archives.
EXCLUDED_SOURCE_PATHS = {
    "docs/claude-fix-report.md",
    "firmware/assets/images/podcast-radio-selected.png",
    "firmware/docs/brand/README.md",
    "firmware/docs/brand/README.zh_CN.md",
    "firmware/assets/README.md",
    "firmware/assets/README.zh_CN.md",
    "server/static/assets/station-art-20261004.jpg",
}
FIRMWARE_IMAGE_ASSETS = Path("firmware/assets/images")
PODCAST_COVER_ASSETS = Path("firmware/assets/images/podcast-covers")
PUBLIC_COVER_METADATA = {"README.md", "README.zh_CN.md", "manifest.json"}
BRAND_REFERENCE_ASSETS = Path("firmware/docs/brand")
PUBLIC_BRAND_DOCUMENTS = {"brand-and-product.md", "brand-and-product.zh_CN.md"}
# Public pointer documents replace the internal reference-image catalogues.
# Original files and images stay untouched in the author's working directory.
PUBLIC_DOCUMENT_VARIANTS = {
    "firmware/assets/README.md": (
        '[简体中文](README.zh_CN.md) · **English**\n\n# Public assets\n\n'
        'The [Noto Sans SC font license](fonts/LICENSE-NotoSansSC.txt) is retained with '
        'the font and generated glyph data. [Neutral podcast covers](images/podcast-covers/README.md) '
        'are original programmatically generated geometry, with no publisher source images.\n\n'
        'Internal design references, template product images, and device QR codes are not '
        'distributed in this package. Publication assets live under `assets/publication/`; '
        'see [public product information](../docs/brand/brand-and-product.md).\n'
    ).encode(),
    "firmware/assets/README.zh_CN.md": (
        '**简体中文** · [English（英文）](README.md)\n\n# 公开素材\n\n'
        '[思源黑体许可](fonts/LICENSE-NotoSansSC.txt)与字库及生成的字形数据一同保留。'
        '[中性播客封面](images/podcast-covers/README.zh_CN.md)使用自行绘制的几何图案，'
        '不包含节目原始图片。\n\n'
        '内部设计参考、模板产品图片和设备身份二维码不进入公开包。发布素材位于'
        '`assets/publication/`（公开原创素材）；产品信息见'
        '[公开品牌说明](../docs/brand/brand-and-product.zh_CN.md)。\n'
    ).encode(),
    "firmware/docs/brand/README.md": (
        '[简体中文](README.zh_CN.md) · **English**\n\n# Public brand references\n\n'
        'Internal product reference images and device QR codes are excluded from this package. '
        'Use [public product information](brand-and-product.md) and '
        '[the official product website](https://ai-passport.folotoy.cn/) for hardware facts. '
        'Project publication artwork is original neutral artwork under `assets/publication/`.\n'
    ).encode(),
    "firmware/docs/brand/README.zh_CN.md": (
        '**简体中文** · [English（英文）](README.md)\n\n# 公开品牌参考\n\n'
        '本公开包不包含内部产品参考图或设备身份二维码。硬件信息见'
        '[公开产品说明](brand-and-product.zh_CN.md)与'
        '[厂商公开网站](https://ai-passport.folotoy.cn/)。'
        '本项目的发布图为 `assets/publication/`（公开原创素材）中的原创中性图案。\n'
    ).encode(),
}
RELEASE_FILES = {
    "app": ("FoloToy-AI-Passport.bin", "FoloToy-AI-Passport.bin", 0x10000),
    "full": ("FoloToy-AI-Passport-full.bin", "FoloToy-AI-Passport-full.bin", 0),
    "bootloader": ("bootloader/bootloader.bin", "bootloader.bin", 0),
    "partition_table": ("partition_table/partition-table.bin", "partition-table.bin", 0x8000),
}
REQUIRED_SOURCE = {
    ".github/public-files.json", "scripts/check-public-commit.py", "scripts/install-git-hooks.py",
    "README.md", "README.en.md", "README.zh_CN.md", "LICENSE", "install.command",
    "docs/install.md", "docs/install.zh_CN.md", "docs/use.md", "docs/use.zh_CN.md",
    "docs/publish.md", "docs/publish.zh_CN.md", "docs/third-party.md", "docs/third-party.zh_CN.md",
    "scripts/flash-device.py", "scripts/flash-requirements.txt", "scripts/install-server.sh", "scripts/install-server.ps1",
    "server/server.py", "server/requirements.txt", "server/Dockerfile", "server/compose.yaml", "server/.dockerignore",
    "server/config.json", "server/access_store.py", "server/listening_store.py", "server/source_manager.py", "server/public_feeds.py", "server/manage_access.py", "server/runtime_release.py", "server/device_covers.py",
    "server/static/index.html", "server/static/access.html", "server/static/assets/player.js", "server/static/assets/player-core.js", "server/static/assets/player.css", "server/static/assets/devices.js", "server/static/assets/access.js", "server/static/assets/access.css",
    "server/static/assets/icons/LICENSE", "firmware/LICENSE",
    "firmware/assets/fonts/LICENSE-NotoSansSC.txt", "firmware/main/minimp3.h",
    *PUBLIC_SOURCE_EXCEPTIONS,
}


# Actual linked dependency notices accompany public artifacts.
REQUIRED_SOURCE.update({
    'docs/licenses/README.md',
    'docs/licenses/README.zh_CN.md',
    'docs/licenses/cjson/LICENSE',
    'docs/licenses/esp-idf/I2S-SOURCE-NOTICE.txt',
    'docs/licenses/esp-idf/LICENSE',
    'docs/licenses/esp-idf/esp_coex.LICENSE',
    'docs/licenses/esp-idf/esp_phy.LICENSE',
    'docs/licenses/esp-idf/esp_wifi.LICENSE',
    'docs/licenses/espressif/button-4.2.0-SOURCE-NOTICE.txt',
    'docs/licenses/espressif/esp_codec_dev-1.6.2-SOURCE-NOTICE.txt',
    'docs/licenses/espressif/esp_codec_dev-1.6.2.LICENSE',
    'docs/licenses/espressif/esp_lvgl_port-2.9.0-SOURCE-NOTICE.txt',
    'docs/licenses/freertos/LICENSE.md',
    'docs/licenses/freertos/SOURCE-NOTICE.txt',
    'docs/licenses/gcc/COPYING.RUNTIME',
    'docs/licenses/gcc/COPYING3',
    'docs/licenses/gcc/SOURCE-NOTICE.txt',
    'docs/licenses/http_parser/LICENSE.txt',
    'docs/licenses/lvgl-fonts/FontAwesome5-LICENSE.txt',
    'docs/licenses/lvgl-fonts/Montserrat-OFL.txt',
    'docs/licenses/lvgl/LICENCE.txt',
    'docs/licenses/lvgl/LICENSE_SPRINTF.txt',
    'docs/licenses/lvgl/LICENSE_TLSF.txt',
    'docs/licenses/lvgl/TLSF-SOURCE-NOTICE.txt',
    'docs/licenses/lwip/COPYING',
    'docs/licenses/mbedtls/LICENSE',
    'docs/licenses/mbedtls/SOURCE-NOTICE.txt',
    'docs/licenses/newlib/COPYING.NEWLIB',
    'docs/licenses/wpa_supplicant/COPYING',
    'docs/licenses/wpa_supplicant/README',
})

class ExportError(Exception):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def relative_inside(root, value, field):
    value = Path(value)
    if value.is_absolute() or ".." in value.parts:
        raise ExportError(f"{field} must be an explicit project-relative path")
    resolved = (root / value).resolve()
    try:
        resolved.relative_to(root)
    except ValueError as error:
        raise ExportError(f"{field} escapes the project") from error
    return resolved


USER_ABSOLUTE_PATHS = (
    re.compile(r"""(?<![\w/])/(?:Users|home)/[^/\s\\:"'<>${}%]+(?:/|(?=$|[\s"'<>]))""", re.I),
    re.compile(r"""(?:\b[A-Z]:[\\/]+|\\{2,}[^\\/\s]+[\\/]+)Users[\\/]+[^\\/\r\n"'<>${}%]+""", re.I),
)


def load_private_values(root, private_blocklist=None):
    """Load literal UTF-8 strings from a local-only JSON array, never public source."""
    root = Path(root).resolve()
    candidate = Path(private_blocklist) if private_blocklist is not None else Path(".local/export-private-values.json")
    if not candidate.is_absolute():
        candidate = root / candidate
    try:
        relative = candidate.relative_to(root)
    except ValueError as error:
        raise ExportError("Private blocklist must be inside project .local/") from error
    if not relative.parts or relative.parts[0] != ".local" or ".." in relative.parts:
        raise ExportError("Private blocklist must be inside project .local/")
    current = root
    for part in relative.parts:
        current /= part
        if current.is_symlink():
            raise ExportError("Private blocklist must not use symlinks")
    if private_blocklist is None and not candidate.exists():
        return ()
    try:
        if not candidate.is_file() or candidate.stat().st_size > 65536:
            raise ExportError("Private blocklist must be a small local JSON file")
        values = json.loads(candidate.read_text(encoding="utf-8"))
        if not isinstance(values, list) or any(not isinstance(value, str) or not value.strip() or "\0" in value for value in values):
            raise ExportError("Private blocklist must be a JSON array of nonempty strings")
        return tuple(dict.fromkeys(value.encode("utf-8") for value in values))
    except (OSError, UnicodeError, ValueError) as error:
        raise ExportError("Private blocklist is missing, unreadable or invalid JSON") from error


def sensitive_reason(data, relative_name, *, private_values=()):
    if any(value in data for value in private_values):
        return "private blocklist value"
    text = data.decode("utf-8", "replace")
    if any(pattern.search(text) for pattern in USER_ABSOLUTE_PATHS):
        return "user absolute path"
    pem = r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----\s+([A-Za-z0-9+/=\r\n]{120,})\s*-----END (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"
    if (re.search(pem, text) or re.search(r"\bAKIA[0-9A-Z]{16}\b", text)
            or re.search(r"\bsk-(?:proj-)?[A-Za-z0-9_-]{32,}\b", text)
            or re.search(r"\b(?:ghp_|github_pat_)[A-Za-z0-9_]{20,}\b", text)):
        return "private key or access credential"
    # Test-only fake credentials are needed to exercise auth. Concrete user
    # paths, local private values and complete key/token patterns remain blocked
    # in tests too. Runtime/source credentials have no exception.
    if "tests" not in Path(relative_name).parts:
        assignments = re.findall(r'(?<![A-Za-z0-9_\-\"\'])[\"\']?\b(WIFI_SSID|SECRET_KEY|API_KEY|[A-Z0-9_]*(?:TOKEN|PASSWORD|SECRET|SECRET_KEY|API_KEY))[\"\']?\s*(?:=|:|\s)\s*[\"\']([^\"\'\r\n]+)', text, re.I)
        for name, value in assignments:
            if value.upper() != name.upper() and not value.lower().startswith(("example", "placeholder", "changeme", "your_", "your-", "<", "${")):
                return "hardcoded configuration credential"
    return None


def check_public_file(name, data, private_values, context="export"):
    # A private file name must not be echoed as part of its own rejection.
    if sensitive_reason(name.encode("utf-8"), name, private_values=private_values):
        raise ExportError("Sensitive public file name; export stopped")
    reason = sensitive_reason(data, name, private_values=private_values)
    if reason:
        raise ExportError(f"Sensitive {reason}; refusing {context}: {name}")


def allowed_source(relative):
    parts = relative.parts
    if not parts:
        return False
    if len(parts) == 1:
        return relative.name in ROOT_FILES
    if relative.as_posix() in PUBLIC_SOURCE_EXCEPTIONS:
        return True
    if relative.as_posix() in EXCLUDED_SOURCE_PATHS:
        return False
    if parts[0] not in ROOT_TREES or any(p.lower() in EXCLUDED_DIRS or p.startswith(("02", "._")) for p in parts[:-1]):
        return False
    name = relative.name
    if name.startswith("._") or name in EXCLUDED_FILES or name.lower() == ".env" or (name.lower().startswith(".env.") and name.lower() != ".env.example"):
        return False
    if PODCAST_COVER_ASSETS in relative.parents and (relative.parent != PODCAST_COVER_ASSETS or name not in PUBLIC_COVER_METADATA):
        return False
    if FIRMWARE_IMAGE_ASSETS in relative.parents and PODCAST_COVER_ASSETS not in relative.parents:
        return False
    if BRAND_REFERENCE_ASSETS in relative.parents and (relative.parent != BRAND_REFERENCE_ASSETS or name not in PUBLIC_BRAND_DOCUMENTS):
        return False
    if relative.suffix.lower() in EXCLUDED_SUFFIXES or name.endswith((".sqlite-wal", ".sqlite-shm")):
        return False
    return True


def source_snapshot(root, *, private_values=None):
    if private_values is None:
        private_values = load_private_values(root)
    snapshot = {}
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if not allowed_source(relative):
            continue
        if path.is_symlink():
            raise ExportError(f"Source symlink not permitted: {relative.as_posix()}")
        if not path.is_file():
            continue
        if path.stat().st_size > 64 * 1024 * 1024:
            raise ExportError(f"Unexpected oversized source: {relative.as_posix()}")
        data = path.read_bytes()
        check_public_file(relative.as_posix(), data, private_values)
        if relative == PODCAST_COVER_ASSETS / "manifest.json":
            try:
                artwork = json.loads(data)
            except (ValueError, UnicodeError) as error:
                raise ExportError("Invalid public neutral artwork metadata") from error
            if not isinstance(artwork, dict) or artwork.get("artwork_policy") != "original-neutral" or artwork.get("artworks") != []:
                raise ExportError("Podcast artwork must be replaced with original neutral artwork before export")
        if relative.as_posix() in {"firmware/docs/README.md", "firmware/docs/README.zh_CN.md"}:
            text = data.decode("utf-8")
            def omit_internal_image(match):
                paragraph = match.group(0)
                return "" if re.search(r'(?:src|srcset)=[\"\']\.\./assets/images/', paragraph) else paragraph
            data = re.sub(r'<p\s+align=[\"\']center[\"\']>.*?</p>', omit_internal_image,
                          text, flags=re.S).encode()
        snapshot[relative.as_posix()] = data
    for name, data in PUBLIC_DOCUMENT_VARIANTS.items():
        check_public_file(name, data, private_values, "public replacement")
    snapshot.update(PUBLIC_DOCUMENT_VARIANTS)
    missing = sorted(REQUIRED_SOURCE - snapshot.keys())
    if missing:
        raise ExportError("Missing public package inputs: " + ", ".join(missing))
    return snapshot


def load_flash_tool(root):
    path = root / "scripts/flash-device.py"
    spec = importlib.util.spec_from_file_location("public_release_flash_validation", path)
    if not spec or not spec.loader:
        raise ExportError("Cannot load local upgrade validation")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)  # No USB enumeration/open on import.
    return module


def verify_archive(root, archive, runner=subprocess.run, *, private_values=()):
    verifier = root / "firmware/tools/archive_firmware.py"
    if not verifier.is_file():
        raise ExportError("Missing firmware archive verifier")
    result = runner([sys.executable, str(verifier), "verify", str(archive)],
                    cwd=root, capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise ExportError("Retained firmware archive verification failed; no old-build fallback")
    try:
        metadata = json.loads((archive / "manifest.json").read_text())
    except (OSError, ValueError) as error:
        raise ExportError("Invalid retained firmware manifest") from error
    if not isinstance(metadata, dict) or metadata.get("schema_version") != 1 or metadata.get("target") != "esp32c3" or metadata.get("flash_size_bytes") != 8388608 or not isinstance(metadata.get("files"), dict) or not isinstance(metadata.get("image_offsets"), dict):
        raise ExportError("Retained archive target/layout metadata is unsupported")
    expected = metadata.get("full_bin_sha256")
    if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected) or archive.name != expected:
        raise ExportError("Archive directory is not bound to its full image SHA256")
    images = {}
    for role, (input_name, output_name, offset) in RELEASE_FILES.items():
        path = archive / input_name
        if path.is_symlink() or not path.is_file():
            raise ExportError(f"Missing/unsafe archived image: {input_name}")
        data = path.read_bytes()
        record = metadata.get("files", {}).get(input_name, {})
        if not isinstance(record, dict) or not data or len(data) != record.get("size") or digest(data) != record.get("sha256"):
            raise ExportError(f"Archived image hash/size mismatch: {input_name}")
        if role != "full" and metadata.get("image_offsets", {}).get(input_name) != offset:
            raise ExportError(f"Unsupported archived image offset: {input_name}")
        images[role] = {"path": output_name, "offset": offset, "size": len(data), "sha256": digest(data), "data": data}
    if images["full"]["sha256"] != expected or images["app"]["size"] > 6291456:
        raise ExportError("Wrong full image identity or app exceeds public six-MiB limit")
    for role in ("app", "bootloader", "partition_table"):
        record = images[role];start = record["offset"]
        if images["full"]["data"][start:start + record["size"]] != record["data"]:
            raise ExportError(f"Merged image differs from archived {role}")
    tool = load_flash_tool(root)
    try:
        tool.validate_app_image(images["app"]["data"])
        tool.validate_modern(tool.parse_partition_table(images["partition_table"]["data"]))
    except tool.SafetyError as error:
        raise ExportError("Retained firmware is not compatible with the public upgrade contract") from error
    descriptor = metadata.get("app_descriptor", {})
    if not isinstance(descriptor, dict) or descriptor.get("idf_version") != "v5.5.3":
        raise ExportError("Retained firmware does not report the supported ESP-IDF version")
    for record in images.values():
        reason = sensitive_reason(record["data"], "release/" + record["path"], private_values=private_values)
        if reason:
            raise ExportError(f"Sensitive {reason} in retained firmware; export stopped")
    return images, descriptor


def release_manifest(images, descriptor):
    return {"schema_version": 1, "chip": "esp32c3", "flash_size_bytes": 8388608,
        "images": {role: {k: v for k, v in record.items() if k != "data"} for role, record in images.items()},
        "app_descriptor": {key: descriptor.get(key) for key in ("project_name", "version", "idf_version")},
        "verification": {"retained_archive": "PASS", "image_hash_layout": "PASS", "app_six_mib_limit": "PASS", "device_tests": "NOT_RUN"},
        "installation_boundaries": {"usb": "app-only; existing modern layout and valid permanent Recovery required",
            "initialize": "unsupported; no verified old identity migration",
            "permanent_recovery_binary_included": False,
            "merged_phone_install": "may erase ordinary NVS/store data; not a settings-preserving upgrade",
            "usb_tool_installs_new_bootloader_hook": False}}


def zip_files(destination, files):
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as package:
        for name, data in sorted(files.items()):
            entry = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = (0o755 if name.endswith((".sh", ".command")) else 0o644) << 16
            package.writestr(entry, data)


def export_release(root, archive_relative, output_relative, replace=False, runner=subprocess.run, *, private_blocklist=None):
    root = Path(root).resolve()
    archive = relative_inside(root, archive_relative, "archive")
    output = relative_inside(root, output_relative, "output")
    if output == root or not output.relative_to(root).parts or output.relative_to(root).parts[0] != "dist":
        raise ExportError("Output must be inside project dist/")
    if output.exists():
        try:
            old_state = json.loads((output / "export-state.json").read_text())
        except (OSError, ValueError):
            old_state = None
        if not replace or not isinstance(old_state, dict) or old_state.get("schema_version") != 1 or old_state.get("kind") != "public_export":
            raise ExportError("Output exists; use a new dist path or --replace for a prior exporter output")
    private_values = load_private_values(root, private_blocklist)
    images, descriptor = verify_archive(root, archive, runner, private_values=private_values)
    sources = source_snapshot(root, private_values=private_values)
    # Preserve the exact source bytes selected by the privacy check even if
    # another editor modifies files while zip/copy work proceeds.
    manifest = release_manifest(images, descriptor)
    manifest_bytes = (json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode()
    installation = dict(sources)
    installation["release/manifest.json"] = manifest_bytes
    for record in images.values():
        installation["release/" + record["path"]] = record["data"]
    alias = json.loads(manifest_bytes)
    for record in alias["images"].values():
        record["path"] = "release/" + record["path"]
    installation["firmware-manifest.json"] = (json.dumps(alias, indent=2, sort_keys=True) + "\n").encode()
    image_checks = "".join(f"{record['sha256']}  {record['path']}\n" for record in images.values())
    installation["release/sha256s.txt"] = (image_checks + f"{digest(manifest_bytes)}  manifest.json\n").encode()
    for name, data in installation.items():
        check_public_file(name, data, private_values, "public output")
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".public-export-", dir=output.parent))
    try:
        for name, data in installation.items():
            path = stage / name;path.parent.mkdir(parents=True, exist_ok=True);path.write_bytes(data)
            if name.endswith((".sh", ".command")):
                path.chmod(0o755)
        zip_files(stage / "installation.zip", installation)
        zip_files(stage / "source.zip", sources)
        checks = [f"{record['sha256']}  release/{record['path']}" for record in images.values()]
        checks += [f"{digest((stage/name).read_bytes())}  {name}" for name in ("installation.zip", "source.zip")]
        (stage / "sha256s.txt").write_text("\n".join(checks) + "\n")
        state = {"schema_version": 1, "kind": "public_export", "full_sha256": images["full"]["sha256"],
            "app_sha256": images["app"]["sha256"], "source_files": len(sources),
            "source_manifest": {name: digest(data) for name, data in sorted(sources.items())},
            "excluded": "git/build/dependencies/runtime/env/data/media/private/history/debug artifacts; internal repair report; external artwork references; podcast source images; Apple metadata",
            "publishing": "NOT_RUN", "flash": "NOT_RUN"}
        (stage / "export-state.json").write_text(json.dumps(state, indent=2, sort_keys=True) + "\n")
        # Final local upgrade-manifest checks are file-only, never USB.
        tool = load_flash_tool(root)
        tool.load_package(stage / "release/manifest.json")
        tool.load_package(stage / "firmware-manifest.json")
        if output.exists():
            previous = output.with_name(output.name + ".previous-" + uuid.uuid4().hex[:8])
            output.rename(previous)  # Keep previous output recoverable, never delete unknown user files.
        stage.rename(output)
        return state
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, help="显式指定本次已验证归档的项目相对目录，不猜最新版本")
    parser.add_argument("--output", default="dist/public-preview", help="项目dist下的相对输出目录")
    parser.add_argument("--replace", action="store_true", help="替换本工具的旧输出并保留可恢复旧目录")
    parser.add_argument("--private-blocklist", help="可选.local内的JSON字符串数组，默认.local/export-private-values.json；仅在本机使用，不进入公开包")
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parent.parent
    try:
        result = export_release(root, args.archive, args.output, args.replace, private_blocklist=args.private_blocklist)
        print("公开包已导出；未上传、烧录或发布。")
        print(f"完整镜像SHA256：{result['full_sha256']}")
        print(f"输出：{args.output}；installation.zip（安装包）、source.zip（源码包）。")
        return 0
    except (ExportError, OSError, UnicodeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"导出停止：{error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
