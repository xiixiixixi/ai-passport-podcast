"""Persistent household access, administrator sessions, and one-use device pairing.

Credentials are never stored in plain text. Listening data remains in its own
database so authorizing, renaming, or revoking a device cannot erase history.
"""
import hashlib
import hmac
import re
import secrets
import sqlite3
import time
from contextlib import contextmanager
from pathlib import Path

from werkzeug.security import check_password_hash, generate_password_hash
from listening_store import ListeningError


SESSION_SECONDS = 7 * 86400
PAIR_SECONDS = 10 * 60


def digest(value):
    try:
        return hashlib.sha256(value.encode("utf-8")).hexdigest()
    except UnicodeEncodeError:
        raise ListeningError("输入中包含无法使用的字符")


def device_identifier(value):
    if not isinstance(value, str) or not re.fullmatch(r"device-[A-Za-z0-9_-]{1,80}", value):
        raise ListeningError("设备编号格式不正确")
    return value


def device_name(value):
    value = value.strip() if isinstance(value, str) else ""
    if not value or len(value) > 40 or any(ord(c) < 32 or 0xD800 <= ord(c) <= 0xDFFF for c in value):
        raise ListeningError("设备名称请填写 1 到 40 个字")
    return value


class AccessStore:
    def __init__(self, path, setup_key="", clock=time.time):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.setup_key = setup_key
        self.clock = clock
        with self.connection() as db:
            db.executescript("""
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS browser_sessions (
              token_hash TEXT PRIMARY KEY, expires INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS devices (
              id TEXT PRIMARY KEY, name TEXT NOT NULL, token_hash TEXT UNIQUE NOT NULL,
              pending_hash TEXT UNIQUE,
              created_at INTEGER NOT NULL, last_seen INTEGER NOT NULL DEFAULT 0,
              revoked INTEGER NOT NULL DEFAULT 0);
            CREATE TABLE IF NOT EXISTS pairings (
              code_hash TEXT PRIMARY KEY, expires INTEGER NOT NULL, used_for TEXT);
            CREATE TABLE IF NOT EXISTS attempts (
              category TEXT NOT NULL, subject TEXT NOT NULL, window INTEGER NOT NULL,
              count INTEGER NOT NULL, PRIMARY KEY(category,subject));
            """)
            columns = {row[1] for row in db.execute("PRAGMA table_info(devices)")}
            if "pending_hash" not in columns:
                db.execute("ALTER TABLE devices ADD COLUMN pending_hash TEXT")
                db.execute("CREATE UNIQUE INDEX IF NOT EXISTS device_pending_hash ON devices(pending_hash)")

    @contextmanager
    def connection(self, write=False):
        db = sqlite3.connect(self.path, timeout=15, isolation_level=None)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA busy_timeout=15000")
        try:
            if write:
                db.execute("BEGIN IMMEDIATE")
            yield db
            if write:
                db.execute("COMMIT")
        except BaseException:
            if write:
                db.execute("ROLLBACK")
            raise
        finally:
            db.close()

    def initialized(self):
        with self.connection() as db:
            return db.execute("SELECT 1 FROM settings WHERE key='password_hash'").fetchone() is not None

    def throttle(self, category, subject, maximum=8, seconds=300):
        # Stored counters keep retries bounded across process restarts. The global
        # claim limit also prevents guessing by rotating source addresses.
        now = int(self.clock())
        subject = digest(str(subject))
        with self.connection(write=True) as db:
            db.execute("DELETE FROM attempts WHERE window<?", (now - 86400,))
            old = db.execute("SELECT * FROM attempts WHERE category=? AND subject=?", (category, subject)).fetchone()
            if not old or now - old["window"] >= seconds:
                db.execute("INSERT OR REPLACE INTO attempts VALUES(?,?,?,1)", (category, subject, now))
                return
            if old["count"] >= maximum:
                raise ListeningError("尝试次数较多，请稍等几分钟再试", 429)
            db.execute("UPDATE attempts SET count=count+1 WHERE category=? AND subject=?", (category, subject))

    def _session(self, db):
        token = secrets.token_hex(32)
        now = int(self.clock())
        db.execute("DELETE FROM browser_sessions WHERE expires<=?", (now,))
        db.execute("INSERT INTO browser_sessions VALUES(?,?)", (digest(token), now + SESSION_SECONDS))
        return token

    def setup(self, key, password, remote):
        self.throttle("setup", remote)
        if not self.setup_key:
            raise ListeningError("后台还没有安装码，请先运行项目提供的安装脚本", 503)
        if not isinstance(key, str) or not hmac.compare_digest(digest(key), digest(self.setup_key)):
            raise ListeningError("安装码不正确，请从本机首次设置入口重新打开页面", 403)
        if not isinstance(password, str) or not 10 <= len(password) <= 256:
            raise ListeningError("管理口令请设置为 10 到 256 个字符")
        digest(password)  # Reject invalid Unicode before the password hasher.
        password_hash = generate_password_hash(password, method="pbkdf2:sha256:600000")
        with self.connection(write=True) as db:
            if db.execute("SELECT 1 FROM settings WHERE key='password_hash'").fetchone():
                raise ListeningError("首次设置已经完成，请登录", 409)
            db.execute("INSERT INTO settings VALUES('password_hash',?)", (password_hash,))
            return self._session(db)

    def login(self, password, remote):
        self.throttle("login", remote)
        with self.connection() as db:
            row = db.execute("SELECT value FROM settings WHERE key='password_hash'").fetchone()
        if not row:
            raise ListeningError("请先完成首次设置", 409)
        if isinstance(password, str):
            digest(password)
        if not isinstance(password, str) or len(password) > 256 or not check_password_hash(row["value"], password):
            raise ListeningError("管理口令不正确", 401)
        with self.connection(write=True) as db:
            return self._session(db)

    def administrator(self, token):
        if not isinstance(token, str) or not re.fullmatch(r"[0-9a-f]{64}", token):
            return False
        with self.connection() as db:
            return db.execute("SELECT 1 FROM browser_sessions WHERE token_hash=? AND expires>?",
                              (digest(token), int(self.clock()))).fetchone() is not None

    def logout(self, token):
        if isinstance(token, str):
            with self.connection(write=True) as db:
                db.execute("DELETE FROM browser_sessions WHERE token_hash=?", (digest(token),))

    def pairing(self):
        now = int(self.clock())
        with self.connection(write=True) as db:
            # Only the latest displayed code is valid; a new click cannot leave
            # forgotten active codes around the household network.
            previous = db.execute("SELECT code_hash FROM pairings LIMIT 1").fetchone()
            code = f"{secrets.randbelow(1000000):06d}"
            while previous and digest(code) == previous["code_hash"]:
                code = f"{secrets.randbelow(1000000):06d}"
            db.execute("DELETE FROM pairings")
            db.execute("INSERT INTO pairings VALUES(?,?,NULL)", (digest(code), now + PAIR_SECONDS))
        return {"code": code, "expires_at": now + PAIR_SECONDS}

    def register_existing_device(self, identifier, name, token):
        """Owner-host migration only; HTTP never exposes credential registration.

        A new random credential retains the device's old listening identity. It
        stays pending until the updated device makes its first authenticated
        request. Repeating the same registration is safe; conflicting credentials
        or a revoked registration must go through normal administrator pairing.
        """
        identifier = device_identifier(identifier)
        name = device_name(name)
        if not isinstance(token, str) or not re.fullmatch(r"[0-9a-f]{64}", token) or len(set(token)) < 8:
            raise ListeningError("设备迁移凭据必须由安全随机方式生成，不能使用占位值")
        token_hash, now = digest(token), int(self.clock())
        with self.connection(write=True) as db:
            row = db.execute("SELECT * FROM devices WHERE id=?", (identifier,)).fetchone()
            if row:
                if not row["revoked"] and token_hash in {row["token_hash"], row["pending_hash"]}:
                    return {"device_id": identifier, "pending": bool(row["pending_hash"]), "existing": True}
                raise ListeningError("设备已经登记过其他凭据或已被移除，请使用管理员配对流程", 409)
            conflict = db.execute("SELECT 1 FROM devices WHERE token_hash=? OR pending_hash=?", (token_hash, token_hash)).fetchone()
            if conflict:
                raise ListeningError("这份迁移凭据已经属于另一台设备，请重新生成", 409)
            db.execute("INSERT INTO devices(id,name,token_hash,pending_hash,created_at,last_seen,revoked) VALUES(?,?,?,?,?,0,0)",
                       (identifier, name, digest(secrets.token_hex(32)), token_hash, now))
        return {"device_id": identifier, "pending": True, "existing": False}

    def claim(self, code, identifier, name, remote):
        self.throttle("claim-ip", remote, maximum=8)
        self.throttle("claim-global", "household", maximum=30)
        identifier = device_identifier(identifier)
        name = device_name(name or "我的播客机")
        if not isinstance(code, str) or not re.fullmatch(r"[0-9]{6}", code):
            raise ListeningError("配对码应为六位数字")
        token, now = secrets.token_hex(32), int(self.clock())
        with self.connection(write=True) as db:
            row = db.execute("SELECT * FROM pairings WHERE code_hash=?", (digest(code),)).fetchone()
            if not row or row["expires"] <= now or row["used_for"] is not None:
                raise ListeningError("配对码已失效，请在后台生成一个新的配对码", 401)
            db.execute("UPDATE pairings SET used_for=? WHERE code_hash=? AND used_for IS NULL", (identifier, digest(code)))
            # A device must save its new settings before using the token. Keeping
            # the current token until that first request protects the old working
            # setup from a flash-save failure or power loss after this response.
            # Pending credentials have no short-code expiry: a safe saved token
            # remains usable after an offline reboot. A newer claim replaces it.
            db.execute("INSERT INTO devices(id,name,token_hash,pending_hash,created_at,last_seen,revoked) VALUES(?,?,?,?,?,0,0) "
                       "ON CONFLICT(id) DO UPDATE SET name=excluded.name,pending_hash=excluded.pending_hash",
                       (identifier, name, digest(secrets.token_hex(32)), digest(token), now))
        return {"device_id": identifier, "token": token}

    def device(self, token):
        if not isinstance(token, str) or not re.fullmatch(r"[0-9a-f]{64}", token):
            return None
        now = int(self.clock())
        with self.connection(write=True) as db:
            token_hash = digest(token)
            row = db.execute("SELECT * FROM devices WHERE pending_hash=? OR (token_hash=? AND revoked=0)", (token_hash, token_hash)).fetchone()
            if row:
                if row["pending_hash"] == token_hash:
                    # Activation and revocation serialize in the same transaction;
                    # a revoked pending token cannot race this code and resurrect.
                    db.execute("UPDATE devices SET token_hash=pending_hash,pending_hash=NULL,revoked=0,last_seen=? WHERE id=?",
                               (now, row["id"]))
                # Do not write SQLite for every audio range or polling request.
                elif now - row["last_seen"] >= 60:
                    db.execute("UPDATE devices SET last_seen=? WHERE id=?", (now, row["id"]))
                return row["id"]
        return None

    def devices(self):
        with self.connection() as db:
            return [dict(row) for row in db.execute("SELECT id,name,created_at,last_seen,revoked,pending_hash IS NOT NULL AS pending FROM devices ORDER BY created_at,id")]

    def pairing_status(self):
        with self.connection() as db:
            row = db.execute("SELECT expires,used_for FROM pairings LIMIT 1").fetchone()
        return {"active": bool(row and row["expires"] > self.clock() and not row["used_for"]),
                "expires_at": row["expires"] if row else 0,
                "claimed_device_id": row["used_for"] if row else None}

    def rename(self, identifier, name):
        name = device_name(name)
        with self.connection(write=True) as db:
            if db.execute("UPDATE devices SET name=? WHERE id=?", (name, identifier)).rowcount != 1:
                raise ListeningError("没有找到这台设备", 404)

    def revoke(self, identifier):
        with self.connection(write=True) as db:
            if db.execute("UPDATE devices SET revoked=1,pending_hash=NULL WHERE id=?", (identifier,)).rowcount != 1:
                raise ListeningError("没有找到这台设备", 404)
