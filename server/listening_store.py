"""Shared listening history. Positions are milliseconds; time comes from actual playback.

SQLite transactions serialize ownership changes and event acknowledgements. A late
event may add genuine listening time, but cannot overwrite a newer session's position.
"""
import hashlib
import json
import math
import re
import sqlite3
import time
import uuid
from contextlib import contextmanager
from datetime import datetime, timezone, timedelta
from pathlib import Path


class ListeningError(ValueError):
    def __init__(self, message, status=400):
        super().__init__(message)
        self.status = status


def integer(value, field, maximum=315360000000):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
        raise ListeningError(f"{field} 必须是有效的非负整数")
    return value


def identifier(value, field):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,96}", value):
        raise ListeningError(f"{field} 格式不正确")
    return value


def _merge(intervals, start, end):
    if end <= start:
        return intervals
    merged = []
    for left, right in sorted(intervals + [[start, end]]):
        if merged and left <= merged[-1][1]:
            merged[-1][1] = max(right, merged[-1][1])
        else:
            merged.append([left, right])
    return merged


class ListeningStore:
    def __init__(self, path, clock=time.time):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.clock = clock
        with self.connection() as db:
            db.executescript("""
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value INTEGER NOT NULL);
            INSERT OR IGNORE INTO meta VALUES ('revision',0);
            CREATE TABLE IF NOT EXISTS progress (
              show_id TEXT NOT NULL, episode_id TEXT NOT NULL, duration_ms INTEGER NOT NULL DEFAULT 0,
              position_ms INTEGER NOT NULL DEFAULT 0, listened_ms INTEGER NOT NULL DEFAULT 0,
              play_count INTEGER NOT NULL DEFAULT 0, status TEXT NOT NULL DEFAULT 'unplayed',
              updated_at INTEGER NOT NULL DEFAULT 0, revision INTEGER NOT NULL DEFAULT 0,
              owner TEXT, coverage TEXT NOT NULL DEFAULT '[]', manual INTEGER NOT NULL DEFAULT 0,
              legacy INTEGER NOT NULL DEFAULT 0,
              recent_order INTEGER NOT NULL DEFAULT 0,
              PRIMARY KEY(show_id,episode_id));
            CREATE TABLE IF NOT EXISTS sessions (
              id TEXT PRIMARY KEY, client_id TEXT NOT NULL, request_id TEXT NOT NULL,
              show_id TEXT NOT NULL, episode_id TEXT NOT NULL, started_ms INTEGER NOT NULL,
              last_seq INTEGER NOT NULL DEFAULT 0, listened_ms INTEGER NOT NULL DEFAULT 0,
              position_ms INTEGER NOT NULL DEFAULT 0, state TEXT NOT NULL DEFAULT 'playing',
              counted INTEGER NOT NULL DEFAULT 0, closed INTEGER NOT NULL DEFAULT 0,
              start_payload TEXT NOT NULL, start_reply TEXT NOT NULL,
              UNIQUE(client_id,request_id));
            CREATE TABLE IF NOT EXISTS events (
              session_id TEXT NOT NULL, seq INTEGER NOT NULL, payload TEXT NOT NULL,
              reply TEXT NOT NULL, PRIMARY KEY(session_id,seq));
            CREATE TABLE IF NOT EXISTS days (
              date TEXT NOT NULL, show_id TEXT NOT NULL, listened_ms INTEGER NOT NULL DEFAULT 0,
              play_count INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(date,show_id));
            CREATE TABLE IF NOT EXISTS sources (
              id TEXT PRIMARY KEY, canonical TEXT UNIQUE NOT NULL, payload TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS source_previews (
              id TEXT PRIMARY KEY, expires INTEGER NOT NULL, payload TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS imports (
              client_id TEXT NOT NULL, request_id TEXT NOT NULL, payload TEXT NOT NULL,
              reply TEXT NOT NULL, PRIMARY KEY(client_id,request_id));
            """)
        # The migration and its sequence assignment are one transaction. Reopening
        # an upgraded database must not reseed old records and reorder recent items.
        with self.connection(write=True) as db:
            columns = {row[1] for row in db.execute("PRAGMA table_info(progress)")}
            if "legacy" not in columns:
                db.execute("ALTER TABLE progress ADD COLUMN legacy INTEGER NOT NULL DEFAULT 0")
            db.execute("INSERT OR IGNORE INTO meta VALUES ('recent_sequence',0)")
            if "recent_order" not in columns:
                db.execute("ALTER TABLE progress ADD COLUMN recent_order INTEGER NOT NULL DEFAULT 0")
                previous = list(db.execute("SELECT rowid FROM progress WHERE updated_at>0 ORDER BY updated_at ASC,revision ASC,rowid ASC"))
                for index, row in enumerate(previous, start=1):
                    db.execute("UPDATE progress SET recent_order=? WHERE rowid=?", (index, row[0]))
                db.execute("UPDATE meta SET value=? WHERE key='recent_sequence'", (len(previous),))

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

    def remove_source(self, show_id):
        """退订：删除网页添加的订阅行；返回这行原本是否存在。"""
        identifier(show_id, "show_id")
        with self.connection(write=True) as db:
            row = db.execute("SELECT id FROM sources WHERE id=?", (show_id,)).fetchone()
            db.execute("DELETE FROM sources WHERE id=?", (show_id,))
            return row is not None

    def forget_show(self, show_id):
        """退订：删除这档节目的进度、会话、事件与统计行。"""
        identifier(show_id, "show_id")
        with self.connection(write=True) as db:
            db.execute("DELETE FROM events WHERE session_id IN (SELECT id FROM sessions WHERE show_id=?)", (show_id,))
            db.execute("DELETE FROM sessions WHERE show_id=?", (show_id,))
            db.execute("DELETE FROM progress WHERE show_id=?", (show_id,))
            db.execute("DELETE FROM days WHERE show_id=?", (show_id,))
            self._revision(db)

    def _revision(self, db):
        db.execute("UPDATE meta SET value=value+1 WHERE key='revision'")
        return db.execute("SELECT value FROM meta WHERE key='revision'").fetchone()[0]

    def _recent_sequence(self, db):
        db.execute("UPDATE meta SET value=value+1 WHERE key='recent_sequence'")
        return db.execute("SELECT value FROM meta WHERE key='recent_sequence'").fetchone()[0]

    @staticmethod
    def _public(row):
        if row is None:
            return {"status": "unplayed", "position_ms": 0, "listened_ms": 0,
                    "play_count": 0, "updated_at": 0, "revision": 0, "legacy": False}
        return {**{key: row[key] for key in ("status", "position_ms", "listened_ms", "play_count", "updated_at", "revision")},
                "legacy": bool(row["legacy"])}

    def progress(self, sid, eid):
        with self.connection() as db:
            return self._public(db.execute("SELECT * FROM progress WHERE show_id=? AND episode_id=?", (sid, eid)).fetchone())

    def all_progress(self):
        with self.connection() as db:
            return {(row["show_id"], row["episode_id"]): self._public(row)
                    for row in db.execute("SELECT * FROM progress")}

    def cleanup_protection(self):
        """缓存清理的保护名单：活跃会话、未听完进度、近两周听过。"""
        with self.connection() as db:
            active = {(row["show_id"], row["episode_id"]) for row in db.execute(
                "SELECT show_id,episode_id FROM sessions WHERE closed=0 AND state IN ('playing','paused')")}
            in_progress = {(row["show_id"], row["episode_id"]) for row in db.execute(
                "SELECT show_id,episode_id FROM progress WHERE status='in_progress'")}
            recent = {(row["show_id"], row["episode_id"]) for row in db.execute(
                "SELECT show_id,episode_id FROM progress WHERE listened_ms>0 AND updated_at>?",
                (int(self.clock()) - 14 * 86400,))}
        return {"active": active, "in_progress": in_progress, "recent": recent}

    def start(self, body, duration_ms):
        client = identifier(body.get("client_id"), "client_id")
        request_id = identifier(body.get("request_id"), "request_id")
        sid, eid = identifier(body.get("show_id"), "show_id"), identifier(body.get("episode_id"), "episode_id")
        duration = integer(duration_ms, "duration_ms")
        explicit = body.get("position_ms")
        if explicit is not None:
            integer(explicit, "position_ms")
        if "restart" in body and not isinstance(body["restart"], bool):
            raise ListeningError("restart 必须是布尔值")
        if "base_revision" in body:
            integer(body["base_revision"], "base_revision", 9223372036854775807)
        offline_elapsed = integer(body.get("offline_elapsed_ms", 0), "offline_elapsed_ms", 7 * 86400000)
        if offline_elapsed and "base_revision" not in body:
            raise ListeningError("离线收听记录必须携带开始时看到的版本")
        payload = json.dumps(body, sort_keys=True, separators=(",", ":"))
        now = int(self.clock() * 1000)
        with self.connection(write=True) as db:
            old = db.execute("SELECT * FROM sessions WHERE client_id=? AND request_id=?", (client, request_id)).fetchone()
            if old:
                if old["start_payload"] != payload:
                    raise ListeningError("同一开始编号不能用于不同播放请求", 409)
                return json.loads(old["start_reply"])
            db.execute("INSERT OR IGNORE INTO progress(show_id,episode_id) VALUES(?,?)", (sid, eid))
            row = db.execute("SELECT * FROM progress WHERE show_id=? AND episode_id=?", (sid, eid)).fetchone()
            position = 0 if body.get("restart") else (explicit if explicit is not None else row["position_ms"])
            if row["status"] == "completed" and explicit is None:
                position = 0
            position = min(position, max(0, duration - 1)) if duration else position
            session = uuid.uuid4().hex
            stale = "base_revision" in body and body["base_revision"] != row["revision"]
            revision = row["revision"] if stale else self._revision(db)
            # Claiming playback is not evidence that audio was actually heard.
            if not stale:
                db.execute("UPDATE progress SET owner=?,duration_ms=?,position_ms=?,revision=? WHERE show_id=? AND episode_id=?",
                           (session, duration, position, revision, sid, eid))
            progress = self._public(db.execute("SELECT * FROM progress WHERE show_id=? AND episode_id=?", (sid, eid)).fetchone())
            reply = {"session_id": session, "position_ms": position, "next_seq": 1, "stale": stale, "progress": progress}
            db.execute("INSERT INTO sessions(id,client_id,request_id,show_id,episode_id,started_ms,position_ms,start_payload,start_reply) VALUES(?,?,?,?,?,?,?,?,?)",
                       (session, client, request_id, sid, eid, now - offline_elapsed, position, payload, json.dumps(reply)))
            return reply

    def event(self, body):
        session_id = identifier(body.get("session_id"), "session_id")
        seq = integer(body.get("seq"), "seq", 2147483647)
        if not seq:
            raise ListeningError("seq 从 1 开始")
        position = integer(body.get("position_ms"), "position_ms")
        listened = integer(body.get("listened_ms"), "listened_ms")
        state = body.get("state")
        if not isinstance(state, str) or state not in {"playing", "paused", "stopped", "ended"}:
            raise ListeningError("播放状态不正确")
        if "seek" in body and not isinstance(body["seek"], bool):
            raise ListeningError("seek 必须是布尔值")
        rate = body.get("rate", 1)
        if isinstance(rate, bool) or not isinstance(rate, (int, float)) or not math.isfinite(rate) or not 0.5 <= rate <= 3:
            raise ListeningError("播放速度必须在 0.5 到 3 倍之间")
        elapsed = body.get("elapsed_ms")
        if elapsed is not None:
            integer(elapsed, "elapsed_ms", 7 * 86400000)
            if listened > elapsed + 30000:
                raise ListeningError("实际收听时长超过本机会话经过的时间")
        payload = json.dumps(body, sort_keys=True, separators=(",", ":"))
        now = int(self.clock() * 1000)
        with self.connection(write=True) as db:
            session = db.execute("SELECT * FROM sessions WHERE id=?", (session_id,)).fetchone()
            if session is None:
                raise ListeningError("播放会话不存在，请重新开始", 404)
            previous = db.execute("SELECT * FROM events WHERE session_id=? AND seq=?", (session_id, seq)).fetchone()
            if previous:
                if previous["payload"] != payload:
                    raise ListeningError("同一序号不能对应不同播放记录", 409)
                reply = json.loads(previous["reply"])
                reply["duplicate"] = True
                current = db.execute("SELECT * FROM progress WHERE show_id=? AND episode_id=?", (session["show_id"], session["episode_id"])).fetchone()
                reply["progress"] = self._public(current)
                reply["stale"] = current["owner"] != session_id
                return reply
            if seq <= session["last_seq"]:
                raise ListeningError("播放记录序号已经过期", 409)
            # The audio worker can finish accounting its output buffer after
            # the controller has already sent a terminal snapshot. Keep that
            # final cumulative observation deliverable without reopening the
            # session or discarding its real listening time. Reboot recovery
            # may describe a previously ended generation as stopped.
            terminal_tail = bool(session["closed"])
            if terminal_tail and state not in {"stopped", "ended"}:
                raise ListeningError("播放会话已结束，请建立新会话", 409)
            if listened < session["listened_ms"]:
                raise ListeningError("实际收听累计不能倒退", 409)
            if terminal_tail and session["state"] == "ended":
                state = "ended"
            started_ms = min(session["started_ms"], now - elapsed) if elapsed is not None else session["started_ms"]
            if listened > max(0, now - started_ms) + 30000:
                raise ListeningError("收听时长超过此会话实际经过的时间")
            sid, eid = session["show_id"], session["episode_id"]
            row = db.execute("SELECT * FROM progress WHERE show_id=? AND episode_id=?", (sid, eid)).fetchone()
            duration = row["duration_ms"]
            position = min(position, duration) if duration else position
            delta = listened - session["listened_ms"]
            counted = int(delta > 0 and not session["counted"])
            stale = row["owner"] != session_id
            total = row["listened_ms"] + delta
            status, coverage = row["status"], json.loads(row["coverage"])
            if not stale or not row["manual"]:
                advance = position - session["position_ms"]
                if delta > 0 and not body.get("seek") and 0 < advance <= delta * rate * 1.1 + 1500:
                    coverage = _merge(coverage, max(session["position_ms"], int(position - delta * rate)), position)
                if delta > 0 and status == "unplayed":
                    status = "in_progress"
                covered = sum(end - start for start, end in coverage)
                if (state == "ended" and duration > 0 and position >= max(0, duration - 2000)
                        and covered >= duration * 0.9):
                    status = "completed"
            revision = self._revision(db)
            # Late acknowledgements never reorder recent listening or change the current owner.
            latest = db.execute("SELECT id FROM sessions ORDER BY rowid DESC LIMIT 1").fetchone()[0]
            promote_recent = delta > 0 and not terminal_tail and not stale and latest == session_id
            updated = now // 1000 if promote_recent else row["updated_at"]
            recent_order = self._recent_sequence(db) if promote_recent else row["recent_order"]
            if delta > 0 and not updated:
                updated = started_ms // 1000
            db.execute("UPDATE progress SET listened_ms=?,play_count=play_count+?,status=?,position_ms=?,updated_at=?,revision=?,coverage=?,recent_order=? WHERE show_id=? AND episode_id=?",
                       (total, counted, status, row["position_ms"] if stale else position, updated, revision,
                        json.dumps(coverage), recent_order, sid, eid))
            if delta or counted:
                date = datetime.fromtimestamp(now / 1000, timezone(timedelta(hours=8))).strftime("%Y-%m-%d")
                db.execute("INSERT INTO days(date,show_id,listened_ms,play_count) VALUES(?,?,?,?) ON CONFLICT(date,show_id) DO UPDATE SET listened_ms=listened_ms+excluded.listened_ms,play_count=play_count+excluded.play_count",
                           (date, sid, delta, counted))
            db.execute("UPDATE sessions SET last_seq=?,listened_ms=?,position_ms=?,state=?,counted=max(counted,?),closed=?,started_ms=? WHERE id=?",
                       (seq, listened, position, state, counted, int(state in {"stopped", "ended"}), started_ms, session_id))
            progress = self._public(db.execute("SELECT * FROM progress WHERE show_id=? AND episode_id=?", (sid, eid)).fetchone())
            reply = {"accepted": True, "duplicate": False, "stale": stale, "progress": progress}
            db.execute("INSERT INTO events(session_id,seq,payload,reply) VALUES(?,?,?,?)", (session_id, seq, payload, json.dumps(reply)))
            return reply

    def mark(self, sid, eid, status, duration_ms):
        if not isinstance(status, str) or status not in {"completed", "unplayed"}:
            raise ListeningError("只能标记为已完成或未播放")
        with self.connection(write=True) as db:
            db.execute("INSERT OR IGNORE INTO progress(show_id,episode_id) VALUES(?,?)", (sid, eid))
            revision = self._revision(db)
            # Marking changes presentation, never fabricates listening time or playback counts.
            db.execute("UPDATE progress SET status=?,position_ms=?,owner=NULL,coverage='[]',manual=1,legacy=CASE WHEN ?='unplayed' THEN 0 ELSE legacy END,revision=? WHERE show_id=? AND episode_id=?",
                       (status, duration_ms if status == "completed" else 0, status, revision, sid, eid))
            return self._public(db.execute("SELECT * FROM progress WHERE show_id=? AND episode_id=?", (sid, eid)).fetchone())

    def recent(self, limit=20):
        with self.connection() as db:
            return [{"show_id": row["show_id"], "episode_id": row["episode_id"], "progress": self._public(row)}
                    for row in db.execute("SELECT * FROM progress WHERE updated_at>0 AND (listened_ms>0 OR legacy=1) ORDER BY updated_at DESC,recent_order DESC,rowid DESC LIMIT ?", (limit,))]

    def stats(self):
        with self.connection() as db:
            shows = [dict(row) for row in db.execute("SELECT show_id,sum(listened_ms) AS listened_ms,sum(listened_ms>0 OR status='completed' OR legacy=1) AS played_episodes,sum(status='completed') AS completed_episodes,sum(play_count) AS play_count FROM progress GROUP BY show_id")]
            summary = {key: sum(row[key] for row in shows) for key in ("listened_ms", "played_episodes", "completed_episodes", "play_count")}
            days = [dict(row) for row in db.execute("SELECT date,sum(listened_ms) AS listened_ms,sum(play_count) AS play_count FROM days GROUP BY date ORDER BY date DESC LIMIT 366")]
            legacy = db.execute("SELECT count(*) FROM progress WHERE legacy=1").fetchone()[0]
            return {"summary": summary, "shows": shows, "days": days, "legacy_episodes": legacy}

    def import_progress(self, body, resolved):
        client = identifier(body.get("client_id"), "client_id")
        request_id = identifier(body.get("request_id"), "request_id")
        payload = json.dumps(body, sort_keys=True, separators=(",", ":"))
        with self.connection(write=True) as db:
            previous = db.execute("SELECT * FROM imports WHERE client_id=? AND request_id=?", (client, request_id)).fetchone()
            if previous:
                if previous["payload"] != payload:
                    raise ListeningError("同一导入编号不能用于不同历史记录", 409)
                return json.loads(previous["reply"])
            results = []
            now = int(self.clock())
            for item in resolved:
                sid, eid = item.get("show_id"), item.get("episode_id")
                result = {"show_id": sid, "episode_id": eid, "status": item.get("error", "invalid")}
                if not item.get("error"):
                    row = db.execute("SELECT * FROM progress WHERE show_id=? AND episode_id=?", (sid, eid)).fetchone()
                    if row and (row["revision"] or row["owner"] or row["listened_ms"] or row["status"] != "unplayed"):
                        result.update(status="skipped_existing", progress=self._public(row))
                    elif item["position_ms"] <= 0 and not item.get("completed"):
                        result["status"] = "invalid"
                    else:
                        db.execute("INSERT OR IGNORE INTO progress(show_id,episode_id) VALUES(?,?)", (sid, eid))
                        revision = self._revision(db)
                        status = "completed" if item.get("completed") else "in_progress"
                        position = min(item["position_ms"], item["duration_ms"]) if item["duration_ms"] else item["position_ms"]
                        updated = min(now, item.get("updated_at") or now)
                        recent_order = self._recent_sequence(db)
                        db.execute("UPDATE progress SET status=?,position_ms=?,duration_ms=?,updated_at=?,legacy=1,revision=?,recent_order=? WHERE show_id=? AND episode_id=?",
                                   (status, position, item["duration_ms"], updated, revision, recent_order, sid, eid))
                        result.update(status="imported", progress=self._public(db.execute("SELECT * FROM progress WHERE show_id=? AND episode_id=?", (sid, eid)).fetchone()))
                results.append(result)
            imported = sum(item["status"] == "imported" for item in results)
            reply = {"imported": imported, "skipped": len(results) - imported, "results": results}
            db.execute("INSERT INTO imports VALUES(?,?,?,?)", (client, request_id, payload, json.dumps(reply)))
            return reply

    def sources(self):
        with self.connection() as db:
            return [json.loads(row[0]) for row in db.execute("SELECT payload FROM sources ORDER BY rowid")]

    def save_preview(self, payload):
        token, expires = uuid.uuid4().hex, int(self.clock()) + 600
        with self.connection(write=True) as db:
            db.execute("DELETE FROM source_previews WHERE expires<?", (int(self.clock()),))
            db.execute("DELETE FROM source_previews WHERE id IN (SELECT id FROM source_previews ORDER BY rowid DESC LIMIT -1 OFFSET 31)")
            db.execute("INSERT INTO source_previews VALUES(?,?,?)", (token, expires, json.dumps(payload, ensure_ascii=False)))
        return token, expires

    def commit_source(self, token, configured, maximum=32):
        identifier(token, "preview_id")
        with self.connection(write=True) as db:
            preview = db.execute("SELECT * FROM source_previews WHERE id=?", (token,)).fetchone()
            if not preview or preview["expires"] < self.clock():
                raise ListeningError("预览已过期，请重新检查链接", 409)
            payload = json.loads(preview["payload"])
            existing = db.execute("SELECT * FROM sources WHERE canonical=?", (payload["canonical"],)).fetchone()
            if existing:
                return json.loads(existing["payload"]), True
            for source in configured:
                if source.get("feed") == payload["feed_url"] or source.get("public_pid") == payload.get("public_pid") and payload.get("public_pid"):
                    return source, True
            if len(configured) + db.execute("SELECT count(*) FROM sources").fetchone()[0] >= maximum:
                raise ListeningError(f"目前最多订阅 {maximum} 档节目，请先整理订阅", 409)
            sid = "src_" + hashlib.sha256(payload["canonical"].encode()).hexdigest()[:16]
            source = {"id": sid, "name": payload["show"]["name"], "feed": payload["feed_url"],
                      "source_type": payload["source_type"], "source_url": payload["source_url"]}
            identities = {}
            if payload.get("apple_id"):
                identities["apple"] = [payload["apple_id"]]
            if payload.get("public_pid"):
                identities["xiaoyuzhou"] = [payload["public_pid"]]
            if identities:
                source["identities"] = identities
            if payload.get("public_pid"):
                source["public_pid"] = payload["public_pid"]
                source["author"] = payload["show"].get("author", "")
            db.execute("INSERT INTO sources VALUES(?,?,?)", (sid, payload["canonical"], json.dumps(source, ensure_ascii=False)))
            return source, False
