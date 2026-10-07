#!/usr/bin/env python3
"""Verified app-only USB upgrade for an existing modern ESP32-C3 Passport.

No full image, partition-table update, erase-all, identity migration or automatic
write retry is offered. Merely importing this module never enumerates/opens USB.
"""
from __future__ import annotations

import argparse
import contextlib
from dataclasses import dataclass
import hashlib
import importlib.metadata
import io
import json
from pathlib import Path, PureWindowsPath
import re
import struct
import sys
from types import SimpleNamespace

FLASH_BYTES = 0x800000
APP_OFFSET = 0x10000
APP_LIMIT = 0x600000
TABLE_OFFSET = 0x8000
TABLE_BYTES = 0xC00
SECTOR_BYTES = 0x1000
RECOVERY_OFFSET = 0x6C0000
RECOVERY_BYTES = 0x140000
JOURNAL_START, JOURNAL_END = 0x6B8000, 0x6BC000
ENTRY = struct.Struct("<HBBII16sI")
MODERN = {
    "nvs": (1, 2, 0x9000, 0x6000),
    "phy_init": (1, 1, 0xF000, 0x1000),
    "factory": (0, 0, APP_OFFSET, 0x650000),
    "store": (1, 2, 0x660000, 0x4000),
    "netcfg": (1, 2, 0x6BC000, 0x4000),
    "recovery": (0, 0x20, RECOVERY_OFFSET, RECOVERY_BYTES),
}
INITIALIZE_LIMIT = (
    "首次初始化未执行：本包不包含永久 Recovery（恢复程序），本工具也没有经过验证的旧身份/数据迁移。"
    "不能靠修改分区表、从0写完整镜像或擦全片来保留身份。"
    "仅支持已装好现代恢复系统的设备升级；旧设备应保留当前布局，使用适配旧布局的程序，"
    "或等待可核验身份备份、迁移和回读的专用流程。"
)


class SafetyError(Exception):
    """A reason shown without exposing raw serial/NVS/identity contents."""


@dataclass(frozen=True)
class Partition:
    label: str
    kind: int
    subtype: int
    offset: int
    size: int

    @property
    def end(self):
        return self.offset + self.size


@dataclass(frozen=True)
class Image:
    name: str
    offset: int
    sha256: str
    data: bytes


@dataclass(frozen=True)
class Package:
    app: Image
    table: Image
    partitions: tuple[Partition, ...]


@dataclass(frozen=True)
class Port:
    device: str
    description: str
    vid: int | None
    pid: int | None


@dataclass(frozen=True)
class Plan:
    write_start: int
    write_end: int
    erase_start: int
    erase_end: int


def overlaps(a, b, c, d):
    return a < d and c < b


def number(value, field):
    if type(value) is int:
        out = value
    elif isinstance(value, str) and re.fullmatch(r"(?:0x[0-9a-fA-F]+|[0-9]+)", value):
        out = int(value, 16 if value.startswith("0x") else 10)
    else:
        raise SafetyError(f"安装清单的 {field}（数值字段）无效。")
    if out < 0:
        raise SafetyError("安装清单含负数范围。")
    return out


def parse_partition_table(raw):
    if len(raw) < TABLE_BYTES:
        raise SafetyError("分区表不足0xC00字节，无法可信核验；未写入。")
    result, md5_found = [], False
    for cursor in range(0, TABLE_BYTES, ENTRY.size):
        magic = int.from_bytes(raw[cursor:cursor + 2], "little")
        if magic == 0xEBEB:
            if raw[cursor + 2:cursor + 16] != b"\xff" * 14:
                raise SafetyError("分区表校验标记格式错误。")
            if hashlib.md5(raw[:cursor]).digest() != raw[cursor + 16:cursor + 32]:
                raise SafetyError("分区表MD5（完整性校验）不匹配；未写入。")
            if any(byte != 255 for byte in raw[cursor + 32:TABLE_BYTES]):
                raise SafetyError("分区表校验之后还有未知内容。")
            md5_found = True
            break
        if magic == 0xFFFF:
            break
        if magic != 0x50AA:
            raise SafetyError("分区表条目格式错误；未写入。")
        _, kind, subtype, offset, size, label_bytes, flags = ENTRY.unpack_from(raw, cursor)
        try:
            label = label_bytes.split(b"\0", 1)[0].decode("ascii")
        except UnicodeError as error:
            raise SafetyError("分区名称编码无效。") from error
        if not re.fullmatch(r"[A-Za-z0-9_.-]{1,16}", label) or flags:
            raise SafetyError("分区名称或加密标记不在支持范围。")
        if not size or offset < 0x9000 or offset % SECTOR_BYTES or size % SECTOR_BYTES or offset + size > FLASH_BYTES:
            raise SafetyError("分区越界或未按闪存扇区对齐。")
        if kind == 0 and offset % 0x10000:
            raise SafetyError("应用分区未按0x10000对齐。")
        result.append(Partition(label, kind, subtype, offset, size))
    if not result or not md5_found:
        raise SafetyError("没有带有效MD5（完整性校验）的可信分区表；未写入。")
    if len({p.label for p in result}) != len(result):
        raise SafetyError("分区名称重复。")
    ordered = sorted(result, key=lambda p: p.offset)
    if any(a.end > b.offset for a, b in zip(ordered, ordered[1:])):
        raise SafetyError("分区互相重叠；未写入。")
    return tuple(result)


def validate_modern(partitions):
    by_name = {p.label: p for p in partitions}
    for name, expected in MODERN.items():
        p = by_name.get(name)
        if p is None or (p.kind, p.subtype, p.offset, p.size) != expected:
            raise SafetyError("当前布局不是支持的现代永久恢复布局；本工具不会改表或迁移身份。")
    for p in partitions:
        if p.label not in MODERN and (p.kind != 1 or overlaps(p.offset, p.end, JOURNAL_START, JOURNAL_END)):
            raise SafetyError("现代布局包含不受支持的额外应用或恢复日志冲突。")


def validate_app_image(raw, allow_partition_tail=False):
    """ESP32-C3 24-byte header, all segments, XOR checksum and appended SHA256.

    Require the ESP-IDF application descriptor, rejecting bootloaders/full
    images accidentally supplied as an app. Return the true image end. Only
    an installed Recovery partition may have unused bytes after that end;
    their contents are never interpreted as part of the application or written.
    """
    if len(raw) < 32 or raw[0] != 0xE9 or not 1 <= raw[1] <= 16:
        raise SafetyError("文件不是有效应用镜像；不能把完整镜像或启动程序当主程序。")
    if int.from_bytes(raw[12:14], "little") != 5 or raw[23] != 1:
        raise SafetyError("应用芯片不是ESP32-C3，或缺少SHA256（完整性校验）。")
    cursor, checksum = 24, 0xEF
    for segment in range(raw[1]):
        if cursor + 8 > len(raw):
            raise SafetyError("应用段头被截断。")
        _, size = struct.unpack_from("<II", raw, cursor)
        cursor += 8
        if size > len(raw) - cursor:
            raise SafetyError("应用段数据被截断。")
        if segment == 0 and (size < 256 or int.from_bytes(raw[cursor:cursor + 4], "little") != 0xABCD5432):
            raise SafetyError("缺少应用描述；该文件可能是启动程序或错误的整包。")
        for byte in raw[cursor:cursor + size]:
            checksum ^= byte
        cursor += size
    checksum_at = (cursor // 16) * 16 + 15
    end = checksum_at + 33
    if end > len(raw) or raw[checksum_at] != checksum:
        raise SafetyError("应用段校验失败。")
    if hashlib.sha256(raw[:checksum_at + 1]).digest() != raw[checksum_at + 1:end]:
        raise SafetyError("应用SHA256（完整性校验）失败。")
    if end != len(raw) and not allow_partition_tail:
        raise SafetyError("应用后面包含未知数据；拒绝当作app-only（仅主程序）写入。")
    return end


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise SafetyError("安装清单含重复字段。")
        result[key] = value
    return result


def load_image(root, record, expected_offset, maximum):
    if not isinstance(record, dict):
        raise SafetyError("安装清单缺少主程序或分区表。")
    path_text = record.get("path")
    if not isinstance(path_text, str) or not path_text or "\\" in path_text or any(ord(c) < 32 or ord(c) == 127 for c in path_text) or Path(path_text).is_absolute() or PureWindowsPath(path_text).drive:
        raise SafetyError("镜像路径必须是安装包内的相对路径。")
    try:
        path = (root / path_text).resolve()
    except (OSError, RuntimeError) as error:
        raise SafetyError("镜像路径无法安全解析。") from error
    try:
        path.relative_to(root)
    except ValueError as error:
        raise SafetyError("镜像路径逃出了安装包目录。") from error
    size = number(record.get("size"), "size")
    offset = number(record.get("offset"), "offset")
    digest = record.get("sha256")
    if not size or size > maximum or offset != expected_offset or not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
        raise SafetyError("镜像大小、位置或SHA256（完整性校验）不符合公开包约束。")
    try:
        with path.open("rb") as stream:
            data = stream.read(size + 1)
    except OSError as error:
        raise SafetyError("安装包中的镜像文件无法读取。") from error
    if len(data) != size or hashlib.sha256(data).hexdigest() != digest.lower():
        raise SafetyError("镜像文件大小或SHA256（完整性校验）与清单不符。")
    return Image(path.name, offset, digest.lower(), data)


def load_package(manifest):
    try:
        path = Path(manifest).resolve()
        with path.open("rb") as stream:
            raw = stream.read(1024 * 1024 + 1)
        if len(raw) > 1024 * 1024:
            raise SafetyError("安装清单超过1MiB，格式不符合公开安装包。")
        data = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object)
    except (OSError, RuntimeError, UnicodeError, json.JSONDecodeError) as error:
        raise SafetyError("无法读取有效安装清单，请使用完整公开安装包。") from error
    if not isinstance(data, dict) or type(data.get("schema_version")) is not int or data.get("schema_version") != 1 or data.get("chip") != "esp32c3" or number(data.get("flash_size_bytes"), "flash_size_bytes") != FLASH_BYTES:
        raise SafetyError("安装清单版本、芯片或8MiB闪存规格不支持。")
    images = data.get("images")
    if not isinstance(images, dict):
        raise SafetyError("安装清单缺少镜像记录。")
    app = load_image(path.parent, images.get("app"), APP_OFFSET, APP_LIMIT)
    table = load_image(path.parent, images.get("partition_table"), TABLE_OFFSET, SECTOR_BYTES)
    validate_app_image(app.data)
    partitions = parse_partition_table(table.data)
    validate_modern(partitions)
    return Package(app, table, partitions)


def upgrade_plan(package, current):
    start, end = APP_OFFSET, APP_OFFSET + len(package.app.data)
    erased_end = (end + SECTOR_BYTES - 1) // SECTOR_BYTES * SECTOR_BYTES
    for p in current:
        if not (p.kind == 0 and p.subtype == 0 and p.offset == APP_OFFSET) and overlaps(start, erased_end, p.offset, p.end):
            if p.label == "cardid" or p.offset == 0x356000:
                raise SafetyError("新程序会覆盖旧cardid（身份区）0x356000–0x35A000，已拒绝。先保留当前旧布局；需要迁移时必须核验身份备份、新址写入和回读，本工具未实现迁移。请使用适合旧布局且不覆盖身份的程序。")
            raise SafetyError("应用实际写入/擦除范围与已有数据或恢复分区重叠，已拒绝。")
    if overlaps(start, erased_end, JOURNAL_START, JOURNAL_END):
        raise SafetyError("应用会覆盖恢复安装日志，已拒绝。")
    validate_modern(current)
    factory = next(p for p in current if p.label == "factory")
    if len(package.app.data) > APP_LIMIT or start != factory.offset or erased_end > factory.end:
        raise SafetyError("应用或扇区擦除范围超过主程序分区。")
    return Plan(start, end, start, erased_end)


def select_port(ports, requested, ask=input, emit=print):
    candidates = [p for p in ports if p.vid == 0x303A and p.pid == 0x1001]
    if requested:
        matches = [p for p in candidates if p.device == requested]
        if len(matches) != 1:
            raise SafetyError("指定端口不是已枚举的设备原生USB串口（303A:1001）；不会尝试其它端口。")
        return matches[0]
    if not candidates:
        raise SafetyError("未找到受支持的原生USB串口。请连接设备并重试；本工具不操作未知转接器。")
    for i, p in enumerate(candidates, 1):
        emit(f"{i}. {p.device} — 原生USB串口 303A:1001")
    answer = ask("请输入要升级设备的编号（没有默认选择，空白取消）：").strip()
    if not answer.isdecimal() or not 1 <= int(answer) <= len(candidates):
        raise SafetyError("没有明确选择设备；未连接、未写入。")
    return candidates[int(answer) - 1]


def quiet_call(function, *args, **kwargs):
    # Library output may contain device identifiers or raw packets. Discard it.
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return function(*args, **kwargs)


class EspBackend:
    def __init__(self, port):
        self.esp = None
        self.port = port

    def connect(self):
        import esptool
        self.esp = quiet_call(esptool.detect_chip, port=self.port.device, baud=115200,
                              connect_attempts=1, trace_enabled=False)
        if self.esp.CHIP_NAME != "ESP32-C3":
            raise SafetyError("实际芯片不是ESP32-C3；未写入。")
        if self.esp.secure_download_mode or quiet_call(self.esp.get_secure_boot_enabled) or quiet_call(self.esp.get_flash_encryption_enabled):
            raise SafetyError("设备开启安全启动或闪存加密，当前公开工具不支持；未写入。")
        self.esp = quiet_call(self.esp.run_stub)
        quiet_call(self.esp.flash_spi_attach, 0)
        capacity = (quiet_call(self.esp.flash_id) >> 16) & 255
        if capacity != 23:
            raise SafetyError("实际闪存容量不是8MiB；未写入。")

    def read(self, offset, size):
        data = quiet_call(self.esp.read_flash, offset, size)
        if len(data) != size:
            raise SafetyError("设备读取被截断；未写入。")
        return data

    def write(self, app):
        import esptool
        import esptool.loader
        # Both high-level reconnect/replay and individual write-block retries
        # exist in pinned esptool 4.12.0. An uncertain write must stop here.
        self.esp.WRITE_FLASH_ATTEMPTS = 1
        esptool.loader.WRITE_BLOCK_ATTEMPTS = 1
        stream = io.BytesIO(app.data)
        stream.name = app.name
        args = SimpleNamespace(addr_filename=[(APP_OFFSET, stream)], compress=True,
            no_compress=False, no_stub=False, force=False, encrypt=False,
            encrypt_files=None, erase_all=False, flash_size="keep", flash_mode="keep",
            flash_freq="keep", ignore_flash_encryption_efuse_setting=False, diff="no")
        quiet_call(esptool.write_flash, self.esp, args)
        stream.seek(0)
        quiet_call(esptool.verify_flash, self.esp, args)

    def reboot(self):
        quiet_call(self.esp.hard_reset)

    def close(self):
        if self.esp is not None:
            self.esp._port.close()


def upgrade(package, port, backend, inspect_only=False, ask=input, emit=print):
    writing = False
    try:
        emit(f"已选择：{port.device} — 原生USB串口 303A:1001")
        emit("检查会使设备进入下载模式并暂停播放。正在核对芯片、闪存和现有恢复系统…")
        backend.connect()
        current = parse_partition_table(backend.read(TABLE_OFFSET, SECTOR_BYTES))
        plan = upgrade_plan(package, current)
        if backend.read(JOURNAL_START, 4) != b"\xff" * 4:
            raise SafetyError("设备存在未完成的恢复安装标记；请先完成恢复流程。本工具不清除该保护区。")
        validate_app_image(backend.read(RECOVERY_OFFSET, RECOVERY_BYTES), allow_partition_tail=True)
        emit("核验：ESP32-C3（芯片），8MiB（闪存），现代分区及既有Recovery（恢复程序）镜像有效。")
        emit(f"主程序：{package.app.name}，{len(package.app.data)}字节")
        emit(f"SHA256（程序完整性）：{package.app.sha256}")
        emit(f"只写主程序：0x{plan.write_start:06X}–0x{plan.write_end:06X}；扇区擦除：0x{plan.erase_start:06X}–0x{plan.erase_end:06X}（右端不含）。")
        emit("保留启动程序、分区表、NVS（设置/书签）、PHY（无线参数）、store（应用数据）、netcfg（网络身份）、恢复日志和Recovery（恢复程序）。")
        if inspect_only:
            emit("只检查完成，未写入。请重新上电退出下载模式。")
            return False
        answer = ask("请确认上面端口确实是您的播客机；输入 UPGRADE（确认升级）再回车，其他内容取消：").strip()
        if answer != "UPGRADE":
            emit("已取消，未写入。请重新上电退出下载模式。")
            return False
        writing = True
        emit("正在写入并核验主程序。断开连接或失败后不会自动重试。")
        backend.write(package.app)
        emit("主程序写入及设备回读校验通过。")
        try:
            backend.reboot()
            emit("设备已重启。请检查播放、按键和续听。")
        except Exception:
            emit("主程序已核验，但自动重启失败；请手动重新上电，不要因此重复写入。")
        return True
    except SafetyError:
        raise
    except KeyboardInterrupt as error:
        if writing:
            raise SafetyError("写入中被取消，结果可能不完整；已停止，不自动重试。请按恢复指引处理。") from error
        raise
    except Exception as error:
        if writing:
            raise SafetyError("写入或回读失败，结果可能不完整；已停止，不自动重试。保留本安装包，检查线缆/端口后按恢复指引处理。") from error
        raise SafetyError("设备连接或读取失败，未写入。请检查线缆、端口和占用程序后重新检查；本工具不会换端口尝试。") from error
    finally:
        with contextlib.suppress(Exception):
            backend.close()


def installed_ports():
    for distribution, expected in (("esptool", "4.12.0"), ("pyserial", "3.5")):
        try:
            version = importlib.metadata.version(distribution)
        except importlib.metadata.PackageNotFoundError as error:
            raise SafetyError("缺少安装工具依赖，请先按安装包说明安装flash-requirements.txt（依赖清单）。") from error
        if version != expected:
            raise SafetyError("工具依赖版本不匹配，请使用安装包指定的依赖清单。")
    from serial.tools import list_ports
    # Never print serial_number; on this device it can be an identifier.
    return [Port(p.device, re.sub(r"[\r\n\x00-\x1f]", " ", p.description or "USB串口"), p.vid, p.pid)
            for p in list_ports.comports()]


def main(argv=None):
    parser = argparse.ArgumentParser(description="核验并升级既有现代恢复布局的播客机；默认只写主程序。")
    parser.add_argument("--manifest", type=Path, default=Path(__file__).resolve().parent.parent / "firmware-manifest.json", help="安装包内的固件清单")
    parser.add_argument("--port", help="明确选择的本机USB串口；没有默认端口")
    parser.add_argument("--inspect-only", action="store_true", help="只检查设备和写入范围，不写入")
    parser.add_argument("--check-package", action="store_true", help="只检查安装包文件，不枚举或连接USB")
    parser.add_argument("--initialize", action="store_true", help="说明首次初始化限制；当前安全停止，不执行初始化")
    args = parser.parse_args(argv)
    try:
        if args.initialize:
            raise SafetyError(INITIALIZE_LIMIT)
        package = load_package(args.manifest)
        if args.check_package:
            print(f"安装包核验通过：ESP32-C3（芯片），主程序{len(package.app.data)}字节，现代分区。未连接USB。")
            return 0
        if not sys.stdin.isatty() and not args.inspect_only:
            raise SafetyError("升级需要在交互窗口核对设备和确认范围；不能通过管道自动确认。")
        if not sys.stdin.isatty() and not args.port:
            raise SafetyError("只检查也须通过--port（端口参数）明确选择设备。")
        port = select_port(installed_ports(), args.port)
        completed = upgrade(package, port, EspBackend(port), args.inspect_only)
        return 0 if completed or args.inspect_only else 3
    except (SafetyError, EOFError, KeyboardInterrupt) as error:
        message = str(error) if isinstance(error, SafetyError) else "用户取消，已停止。"
        print(message, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
