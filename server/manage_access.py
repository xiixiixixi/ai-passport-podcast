"""Recover administrator access on the owner's backend host; preserve listening."""
import argparse
import os
import re
import stat
from pathlib import Path

from access_store import AccessStore
from listening_store import ListeningError


def private_token(path):
    """Read an owner-only file without putting a credential in process arguments."""
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or not 64 <= info.st_size <= 66:
            raise ListeningError("凭据文件必须是只含一份设备迁移凭据的普通文件")
        if os.name == "posix" and (info.st_mode & 0o077 or info.st_uid != os.geteuid()):
            raise ListeningError("凭据文件必须属于当前系统账号，且只有该账号可读写")
        with os.fdopen(fd, "r", encoding="ascii", closefd=False) as stream:
            value = stream.read(67).strip()
        if not re.fullmatch(r"[0-9a-f]{64}", value):
            raise ListeningError("凭据文件内容格式不正确")
        return value
    finally:
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description="在自己的后台主机上管理授权，不修改订阅、音频缓存或收听资料")
    parser.add_argument("action", choices=["reset-admin", "register-device"])
    parser.add_argument("--confirm", action="store_true", help="确认退出所有管理网页登录，并重新进行首次设置")
    parser.add_argument("--data-dir", type=Path, default=Path(__file__).parent / "data", help="已备份的后台资料目录")
    parser.add_argument("--device-id", help="沿用设备原有的持久编号")
    parser.add_argument("--device-name", default="我的播客机", help="设备显示名称")
    parser.add_argument("--token-file", type=Path, help="只限当前系统账号读取的随机设备凭据文件")
    args = parser.parse_args()
    if not args.confirm:
        parser.error("请加上 --confirm 确认所选授权管理操作；节目、缓存和收听记录均保留")
    path = args.data_dir / "access.sqlite3"
    if args.action == "register-device":
        if not args.device_id or not args.token_file:
            parser.error("迁移登记需要 --device-id 和 --token-file；凭据不要放在命令参数里")
        if not args.data_dir.is_dir() or args.data_dir.is_symlink() or path.is_symlink():
            parser.error("资料目录必须已存在，且资料目录与授权数据库不能是符号链接")
        try:
            result = AccessStore(path).register_existing_device(args.device_id, args.device_name, private_token(args.token_file))
        except (ListeningError, OSError, UnicodeError):
            # Do not echo paths, credentials, or caller-provided data in errors.
            parser.error("迁移登记未完成。请核验私密文件权限、设备编号及既有授权；资料未删除")
        print("设备迁移登记已保存；首次使用新凭据时生效。订阅、缓存和收听资料均保留。")
        print("已有相同登记，保持原记录。" if result["existing"] else "已增加设备授权，尚未改变管理员设置。")
        return
    if not os.environ.get("PODCAST_SETUP_KEY"):
        parser.error("请从项目的组合启动配置运行恢复工具，保留原安装码")
    if not path.is_file():
        parser.error("后台尚未安装，不需要恢复")
    store = AccessStore(path)
    with store.connection(write=True) as db:
        db.execute("DELETE FROM settings WHERE key='password_hash'")
        db.execute("DELETE FROM browser_sessions")
        db.execute("DELETE FROM pairings")
    print("管理员设置已恢复。重新运行安装脚本打开首次设置页面，设一个新的管理口令。")
    print("已配对的设备、订阅、缓存和收听记录均保留。")


if __name__ == "__main__":
    main()
