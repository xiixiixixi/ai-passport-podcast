#!/usr/bin/env python3
"""
播客中转服务 (Podcast Relay)

板子（ESP32-C3, 400KB 内存, 8MB 闪存）解不动原始播客音频。
本服务负责：更新节目目录 -> 提前准备整集 PCM -> 用局域网连续音频流供板子播放。
原有 MP3 分段仍供网页播放和旧缓存迁移；音频请求不执行下载或转换。
"""
import json
import math
import os
import re
import shutil
import subprocess
import tempfile
import threading
import time
import urllib.request
import xml.etree.ElementTree as ET
import uuid
import unicodedata
from urllib.parse import urlsplit, urljoin, quote
from concurrent.futures import Future
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path

import feedparser
from flask import Flask, jsonify, request, send_file, send_from_directory, Response, g, redirect
from werkzeug.exceptions import HTTPException
from public_feeds import PublicFeedAdapter, PROGRAMS as PUBLIC_PROGRAMS
from listening_store import ListeningStore, ListeningError, identifier, integer
from source_manager import resolve_source, search_apple, read_public, open_public
from access_store import AccessStore, SESSION_SECONDS
from runtime_release import source_release
from device_covers import DeviceCoverCache, valid_id as device_cover_id

BASE = Path(__file__).resolve().parent
DATA = BASE / "data"
MEDIA = BASE / "media"
DATA.mkdir(exist_ok=True)
MEDIA.mkdir(exist_ok=True)

CFG = json.loads((BASE / "config.json").read_text(encoding="utf-8"))
SHOWS = {s["id"]: s for s in CFG["shows"]}
BITRATE = CFG.get("target_bitrate", "32k")
SEG_SEC = int(CFG.get("segment_seconds", 300))

# 原音频已经是"MP3 + 单声道 + 码率够低"时，可以跳过转码直连
DIRECT_BPS = 48000

# 板子可直接输出的固定格式：解码与重采样留在服务器，避免 C3 的软件浮点开销。
# 修改格式或增益时换版本目录，原 MP3 缓存与网页播放器保持兼容。
PCM_CACHE_VERSION = "pcm16k_v1"
PCM_SAMPLE_RATE = 16000
PCM_CHANNELS = 1
PCM_BITS = 16
PCM_FFMPEG_TIMEOUT = 90
MAX_EPISODE_SEGMENTS = 256
PCM_MANIFEST_NAME = ".pcm_complete_v1.json"
MEDIA_FREE_RESERVE = max(16, int(CFG.get("media_free_reserve_mb", 256))) * 1048576
_pcm_locks_guard = threading.Lock()
_pcm_locks = {}
_pcm_build_slots = threading.BoundedSemaphore(2)
_prepare_lock = threading.Lock()
_prepare_jobs = {}
# 点播插队的单集（priority 0）：缓存完成前清理任务不得删除。
_pinned_keys = set()
_refresh_lock = threading.Lock()
_refresh_job_lock = threading.Lock()
_refresh_thread = None
_manual_refresh = {"job_id": None, "status": "complete", "results": {},
                   "started_at": None, "finished_at": None, "error": ""}

EP_CACHE = DATA / "episodes.json"
STATE = DATA / "state.json"
# 运行时可写的缓存设置放在 data 卷里：config.json 打进镜像、重建即丢，
# 不再承载会变化的策略项。
SETTINGS_FILE = DATA / "settings.json"
SETTING_LIMITS = {"prefetch_latest": (0, 10), "keep_per_show": (1, 60), "low_free_gb": (4, 200)}
SETTING_DEFAULTS = {"prefetch_latest": 3, "keep_per_show": 3, "low_free_gb": 16}
_lock = threading.Lock()
_catalogue_lock = threading.Lock()
_UA = {"User-Agent": "Mozilla/5.0 (Macintosh) PodcastRelay/1.0"}
_public_feeds = PublicFeedAdapter(DATA / "public-feeds", _UA)
LISTENING = ListeningStore(DATA / "listening.sqlite3")
ACCESS = AccessStore(DATA / "access.sqlite3", setup_key=os.environ.get("PODCAST_SETUP_KEY", ""))
ADMIN_COOKIE = "podcast_admin"
RELEASE = source_release(BASE, os.environ.get("PODCAST_RELEASE_ID", ""))
DEVICE_COVERS = DeviceCoverCache(DATA, read_public)


def service_origin():
    """Use an explicitly configured origin or this request, never a household IP."""
    value = os.environ.get("PODCAST_PUBLIC_URL", "").strip() or request.host_url
    try:
        parsed = urlsplit(value)
        parsed.port
    except ValueError:
        raise ListeningError("后台公开地址必须是 http 或 https 的完整服务地址")
    if (parsed.scheme not in {"http", "https"} or not parsed.hostname or parsed.username or parsed.password
            or parsed.query or parsed.fragment or parsed.path not in {"", "/"}
            or "\\" in value or any(ord(c) <= 32 for c in value)):
        raise ListeningError("后台公开地址必须是 http 或 https 的完整服务地址")
    return value.rstrip("/") + "/"


_CONFIGURED_MEMO = {"at": 0.0, "shows": None}
_configured_memo_lock = threading.RLock()


def invalidate_configured_shows():
    """A saved subscription change must be visible to the next request."""
    with _configured_memo_lock:
        _CONFIGURED_MEMO.update(at=0.0, shows=None)


def configured_shows():
    """节目清单是路径校验和列表接口的地基。sources 表 3 秒内缓存复用，
    避免每次分片校验都重开数据库并逐行解析 JSON；订阅增删后立即失效。"""
    with _configured_memo_lock:
        now = time.time()
        memo = _CONFIGURED_MEMO
        if memo["shows"] is not None and now - memo["at"] < 3:
            return list(memo["shows"])
        dynamic = LISTENING.sources()
        for show in dynamic:
            if show.get("public_pid"):
                _public_feeds.add_program(show["public_pid"], show["name"], show.get("author", ""))
        removed = set(load_json(STATE, {}).get("__removed_shows__", []))
        shows = [s for s in list(CFG["shows"]) + dynamic if s.get("id") not in removed]
        memo["at"], memo["shows"] = now, shows
        return list(shows)


def known_show(show_id):
    return any(show["id"] == show_id for show in configured_shows())


def compact_progress(progress):
    return {"p": progress["position_ms"], "l": progress["listened_ms"],
            "c": {"unplayed": 0, "in_progress": 1, "completed": 2}[progress["status"]],
            "v": progress["revision"]}


def listening_fields(show_id, episode_id, lite=False, progress=None):
    progress = progress or LISTENING.progress(show_id, episode_id)
    return compact_progress(progress) if lite else {"progress": progress}


class PreparationQueue:
    """一个槽留给点播；另一个槽按优先级准备最新集，后台不占满点播槽。"""

    def __init__(self):
        self.condition = threading.Condition()
        self.tasks = {}
        self.sequence = 0
        self.started = False
        self.closed = False
        self.running = 0

    def submit(self, function, *args, priority=0):
        future = Future()
        with self.condition:
            if self.closed:
                raise RuntimeError("preparation queue is closed")
            self.sequence += 1
            self.tasks[future] = [priority, self.sequence, function, args]
            if not self.started:
                self.started = True
                for foreground_only in (True, False):
                    threading.Thread(target=self._worker, args=(foreground_only,), daemon=True).start()
            self.condition.notify_all()
        return future

    def promote(self, future, priority):
        with self.condition:
            task = self.tasks.get(future)
            if task and priority < task[0]:
                task[0] = priority
                self.condition.notify_all()

    def stats(self):
        """队列快照：点播等待数用于网页"预计约X分钟"，运行数判断两个槽是否都忙。"""
        with self.condition:
            on_demand = sum(1 for task in self.tasks.values() if task[0] == 0)
            return {"queued": len(self.tasks), "on_demand": on_demand, "running": self.running}

    def _worker(self, foreground_only):
        while True:
            with self.condition:
                while True:
                    eligible = [(future, task) for future, task in self.tasks.items()
                                if not foreground_only or task[0] == 0]
                    if eligible:
                        future, task = min(eligible, key=lambda item: item[1][:2])
                        del self.tasks[future]
                        self.running += 1
                        break
                    if self.closed:
                        return
                    self.condition.wait()
            try:
                if not future.set_running_or_notify_cancel():
                    continue
                try:
                    future.set_result(task[2](*task[3]))
                except BaseException as error:
                    future.set_exception(error)
            finally:
                with self.condition:
                    self.running -= 1

    def shutdown(self):
        with self.condition:
            self.closed = True
            for future in self.tasks:
                future.cancel()
            self.tasks.clear()
            self.condition.notify_all()


_pool = PreparationQueue()


# ---------------------------------------------------------------- 状态存取

def load_json(path, default):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return default


def save_json(path, obj):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, delete=False) as file:
            temporary = Path(file.name)
            json.dump(obj, file, ensure_ascii=False, indent=1)
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def get_state():
    return load_json(STATE, {})


def set_state(key, **kv):
    with _lock:
        s = get_state()
        cur = s.setdefault(key, {})
        cur.update(kv)
        save_json(STATE, s)


def load_settings():
    """缓存策略设置；坏值一律回落默认，不让一次手改拖垮准备与清理。"""
    stored = load_json(SETTINGS_FILE, {})
    settings = {}
    for key, (low, high) in SETTING_LIMITS.items():
        try:
            value = int(stored.get(key, SETTING_DEFAULTS[key]))
        except (TypeError, ValueError):
            value = SETTING_DEFAULTS[key]
        settings[key] = max(low, min(high, value))
    return settings


def save_settings(changes):
    settings = load_settings()
    for key, (low, high) in SETTING_LIMITS.items():
        if key not in changes:
            continue
        value = changes[key]
        if isinstance(value, bool) or not isinstance(value, int):
            raise ListeningError(f"{key} 必须是整数")
        if not low <= value <= high:
            raise ListeningError(f"{key} 必须在 {low} 到 {high} 之间")
        settings[key] = value
    save_json(SETTINGS_FILE, settings)
    return settings


# ---------------------------------------------------------------- 抓取 RSS

def fetch_audio(url, dest, timeout=90):
    r, connection, _ = open_public(url, timeout=timeout)
    try:
        expected = r.getheader("Content-Length")
        maximum = min(2 * 1024 * 1024 * 1024, max(0, shutil.disk_usage(MEDIA).free - MEDIA_FREE_RESERVE))
        if expected and expected.isdecimal() and int(expected) > maximum:
            raise OSError("原始音频过大或音频缓存空间不足")
        downloaded = 0
        with open(dest, "wb") as f:
            while True:
                chunk = r.read(1024 * 256)
                if not chunk:
                    break
                downloaded += len(chunk)
                if downloaded > maximum:
                    raise OSError("原始音频过大或音频缓存空间不足")
                f.write(chunk)
    finally:
        r.close()
        connection.close()
    size = dest.stat().st_size
    if size <= 0 or (expected and expected.isdecimal() and size != int(expected)):
        raise OSError("原始音频下载未完成")
    return size


def parse_duration(text):
    """把 01:43:35 / 6175 这类时长转成秒"""
    if not text:
        return 0
    text = str(text).strip()
    if re.fullmatch(r"\d+", text):
        return int(text)
    parts = text.split(":")
    try:
        parts = [int(p) for p in parts]
    except ValueError:
        return 0
    sec = 0
    for p in parts:
        sec = sec * 60 + p
    return sec


def extract_art(parsed):
    """从 RSS 里取封面地址。

    feedparser 的 feed.image.href 在不同源里可能是字符串，也可能是列表，
    这里两种都兼容。
    """
    cands = []

    def collect(v):
        if isinstance(v, str) and v.strip():
            cands.append(v.strip())
        elif isinstance(v, (list, tuple)):
            for x in v:
                if isinstance(x, str) and x.strip():
                    cands.append(x.strip())

    for key in ("image", "itunes_image"):
        img = parsed.feed.get(key)
        if isinstance(img, dict):
            collect(img.get("href"))
            collect(img.get("url"))
            for l in img.get("links") or []:
                if isinstance(l, dict):
                    collect(l.get("href"))
    for c in cands:
        if c.startswith("http"):
            return c
    return cands[0] if cands else ""


def iso_date(parsed_entry):
    """把 RSS 里五花八门的时间统一成 ISO 8601。

    浏览器和板子都不该去解析 'Thu, 24 Sep 2026 07:00:00 +0800' 这种格式。
    """
    for k in ("published_parsed", "updated_parsed", "created_parsed"):
        t = parsed_entry.get(k)
        if t:
            try:
                # feedparser 已把有时区的 RSS 时间归一到 UTC，不能再套服务器时区。
                return time.strftime("%Y-%m-%dT%H:%M:%S", t) + "+00:00"
            except Exception:
                pass
    return ""


def fetch_show(show):
    """抓一个节目的 RSS，写入剧集缓存

    注意：必须自己带正常 User-Agent 去拉。
    feedparser 直接拉会被小宇宙等 CDN 判定为爬虫返回 403。
    """
    public_pid = None
    try:
        source = urlsplit(show["feed"])
        public_pid = show.get("public_pid")
        if (not source.username and not source.password and not source.query and not source.fragment
                and (not source.scheme and not source.netloc or source.scheme in {"http", "https"})):
            public_pid = next((pid for pid in _public_feeds.programs
                               if source.path == "/feeds/xiaoyuzhou/" + pid + ".xml"), None)
        if public_pid:
            raw, stale = _public_feeds.get(public_pid)
        else:
            # Existing configured feeds retain their tested read path; newly added
            # sources use bounded, public-IP-pinned reads.
            if show.get("source_type"):
                raw, _ = read_public(show["feed"])
            else:
                req = urllib.request.Request(show["feed"], headers=_UA)
                with urllib.request.urlopen(req, timeout=60) as r:
                    raw = r.read(12 * 1024 * 1024 + 1)
                if len(raw) > 12 * 1024 * 1024:
                    raise ValueError("订阅元信息过大")
        d = feedparser.parse(raw)
    except Exception as e:
        return {"ok": False, "error": f"{type(e).__name__}: {e}"}
    if not d.entries:
        confirmed_empty = (public_pid is not None and not stale and not d.bozo
                           and d.feed.get("title") == _public_feeds.programs[public_pid]["name"])
        if not confirmed_empty:
            return {"ok": False, "error": "没有解析到任何单集（可能是源格式异常）"}

    eps = []
    for e in d.entries:
        audio = None
        for link in e.get("links", []):
            if link.get("type", "").startswith("audio"):
                audio = link.get("href")
                break
        if not audio:
            enc = e.get("enclosures") or []
            if enc:
                audio = enc[0].get("href")
        if not audio:
            continue
        dur = e.get("itunes_duration") or e.get("duration") or 0
        eps.append({
            "id": re.sub(r"[^a-zA-Z0-9]", "", str(e.get("id") or e.get("link") or audio))[-40:],
            "title": (e.get("title") or "").strip(),
            "published": iso_date(e),
            "duration": parse_duration(dur),
            "audio": audio,
            "type": (e.get("enclosures") or [{}])[0].get("type", "") or "",
            "desc": re.sub(r"<[^>]+>", "", (e.get("summary") or ""))[:400],
        })

    img = extract_art(d)

    entry = {
        "id": show["id"],
        "name": d.feed.get("title") or show["name"],
        "author": d.feed.get("author", ""),
        "art": img,
        "link": d.feed.get("link", ""),
        "updated": time.strftime("%Y-%m-%d %H:%M:%S"),
        "count": len(eps),
        "episodes": sorted_episodes(eps),
    }
    with _catalogue_lock:
        cache = load_json(EP_CACHE, {})
        cache[show["id"]] = entry
        save_json(EP_CACHE, cache)
    DEVICE_COVERS.request(show["id"], img)
    return {"ok": True, "count": len(eps)}


def refresh_all():
    with _refresh_lock:
        out = {}
        for s in configured_shows():
            out[s["id"]] = fetch_show(s)
        queue_latest_episodes()
        return out


# ---------------------------------------------------------------- 探测 & 转码

def probe(path):
    """返回 (codec, channels, bitrate)"""
    try:
        p = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "a:0",
             "-show_entries", "stream=codec_name,channels,bit_rate",
             "-of", "json", str(path)],
            capture_output=True, text=True, timeout=30)
        st = json.loads(p.stdout).get("streams", [{}])[0]
        return {
            "codec": st.get("codec_name", ""),
            "channels": int(st.get("channels") or 0),
            "bitrate": int(st.get("bit_rate") or 0),
        }
    except Exception:
        return {"codec": "", "channels": 0, "bitrate": 0}


def ep_dir(show_id, ep_id):
    return MEDIA / show_id / ep_id


def seg_files(show_id, ep_id):
    d = ep_dir(show_id, ep_id)
    if not d.is_dir():
        return []
    return sorted(d.glob("seg_*.mp3"))


def browser_audio_path(show_id, ep_id, sources=None):
    sources = sources or prepared_segment_sources(show_id, ep_id, get_state().get(f"{show_id}/{ep_id}", {}))
    if not sources:
        return None
    directory = ep_dir(show_id, ep_id)
    target, manifest = directory / "stream.mp3", directory / ".browser_complete_v1.json"
    if target.is_symlink() or manifest.is_symlink() or not target.is_file():
        return None
    try:
        record = load_json(manifest, {})
        fingerprints = {source.name: {"bytes": source.stat().st_size, "mtime_ns": source.stat().st_mtime_ns} for source in sources}
        stat = target.stat()
        if (stat.st_size > 0 and record.get("sources") == fingerprints
                and record.get("bytes") == stat.st_size and record.get("mtime_ns") == stat.st_mtime_ns):
            return target
    except OSError:
        pass
    return None


def prepare_browser_audio(show_id, ep_id, sources):
    """Create one seekable MP3 during preparation; the audio GET never executes ffmpeg."""
    directory = ep_dir(show_id, ep_id)
    target = directory / "stream.mp3"
    if target.is_symlink() or (directory / ".browser_complete_v1.json").is_symlink():
        raise PCMPathError("symlink is not allowed")
    with pcm_target_lock(target):
        ready = browser_audio_path(show_id, ep_id, sources)
        if ready:
            return ready
        ensure_media_space(sum(source.stat().st_size for source in sources))
        if len(sources) == 1:
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(dir=directory, suffix=".mp3", delete=False) as file:
                    temporary = Path(file.name)
                shutil.copyfile(sources[0], temporary)
                temporary.replace(target)
                stat, source_stat = target.stat(), sources[0].stat()
                save_json(directory / ".browser_complete_v1.json", {
                    "sources": {sources[0].name: {"bytes": source_stat.st_size, "mtime_ns": source_stat.st_mtime_ns}},
                    "bytes": stat.st_size, "mtime_ns": stat.st_mtime_ns})
                return target
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)
        playlist, temporary = None, None
        try:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=directory, suffix=".concat", delete=False) as file:
                playlist = Path(file.name)
                for source in sources:
                    file.write("file '" + source.name + "'\n")
            with tempfile.NamedTemporaryFile(dir=directory, suffix=".mp3", delete=False) as file:
                temporary = Path(file.name)
            result = subprocess.run(["ffmpeg", "-y", "-nostdin", "-loglevel", "error", "-f", "concat", "-safe", "1",
                                     "-i", str(playlist), "-vn", "-c:a", "copy", "-write_xing", "1", str(temporary)],
                                    capture_output=True, text=True, timeout=180)
            if result.returncode or temporary.stat().st_size <= 0:
                raise OSError("网页整集音频准备失败")
            temporary.replace(target)
            stat = target.stat()
            record = {"sources": {source.name: {"bytes": source.stat().st_size, "mtime_ns": source.stat().st_mtime_ns} for source in sources},
                      "bytes": stat.st_size, "mtime_ns": stat.st_mtime_ns}
            save_json(directory / ".browser_complete_v1.json", record)
            return target
        finally:
            if playlist is not None:
                playlist.unlink(missing_ok=True)
            if temporary is not None:
                temporary.unlink(missing_ok=True)


def prepare_browser_safely(show_id, ep_id, sources):
    set_state(f"{show_id}/{ep_id}", browser_status="preparing", browser_error="")
    try:
        prepare_browser_audio(show_id, ep_id, sources)
        set_state(f"{show_id}/{ep_id}", browser_status="ready", browser_error="")
    except (OSError, RuntimeError, subprocess.TimeoutExpired, PCMPathError):
        # Keep usable hardware audio when browser cache preparation fails.
        set_state(f"{show_id}/{ep_id}", browser_status="failed", browser_error="网页整集音频准备失败，请重试准备")


def transcode(show_id, ep_id, audio_url):
    """后台准备整集；已有完整 MP3 只补 PCM，全部校验落盘后才就绪。"""
    d = ep_dir(show_id, ep_id)
    key = f"{show_id}/{ep_id}"
    state = get_state().get(key, {})
    existing = prepared_segment_sources(show_id, ep_id, state)
    if existing:
        prepare_browser_safely(show_id, ep_id, existing)
        return True
    try:
        d.mkdir(parents=True, exist_ok=True)
        if any(part.is_symlink() for part in (MEDIA, MEDIA / show_id, d)):
            raise PCMPathError("symlink is not allowed")
        sources = mp3_segment_sources(show_id, ep_id, state)
        if not sources:
            _, episode = ep_lookup(show_id, ep_id)
            seconds = parse_duration((episode or {}).get("duration"))
            ensure_media_space(max(32 * 1048576, seconds * 48000))
            set_state(key, status="downloading", mp3_complete=False, pcm_complete=False,
                      progress="下载原始音频", error="")
            # 未完整的旧分段不能混入新尾段；已完成的旧 MP3 不进入此分支。
            for segment in d.glob("seg_*.mp3"):
                segment.unlink()
            src = d / "_source.bin"
            if src.is_symlink():
                raise PCMPathError("symlink is not allowed")
            size = fetch_audio(audio_url, src)
            set_state(key, status="transcoding", source_mb=round(size / 1048576, 1), progress="准备音频")
            info = probe(src)
            can_direct = (info["codec"] == "mp3" and info["channels"] <= 1
                          and 0 < info["bitrate"] <= DIRECT_BPS)
            cmd = ["ffmpeg", "-y", "-nostdin", "-loglevel", "error", "-threads", "1",
                   "-i", str(src), "-vn", "-threads", "1"]
            if not can_direct:
                cmd += ["-ac", "1", "-b:a", BITRATE, "-ar", "22050", "-acodec", "libmp3lame"]
            cmd += ["-f", "segment", "-segment_time", str(SEG_SEC),
                    "-reset_timestamps", "1", str(d / "seg_%03d.mp3")]
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
            if result.returncode != 0:
                raise RuntimeError((result.stderr or "ffmpeg 失败")[-400:])
            sources = seg_files(show_id, ep_id)
            if not sources or len(sources) > MAX_EPISODE_SEGMENTS:
                raise RuntimeError("无有效分段或单集过长")
            src.unlink(missing_ok=True)
            set_state(key, segments=len(sources), mp3_complete=True,
                      size_mb=round(sum(s.stat().st_size for s in sources) / 1048576, 1),
                      codec=info["codec"], direct=can_direct)
        else:
            set_state(key, segments=len(sources), mp3_complete=True, pcm_complete=False, error="")
        set_state(key, status="transcoding", pcm_cache_version=PCM_CACHE_VERSION,
                  progress="提前准备设备音频", pcm_ready_segments=0)
        manifest_path = d / PCM_MANIFEST_NAME
        if manifest_path.is_symlink():
            raise PCMPathError("symlink is not allowed")
        previous = load_json(manifest_path, {}).get("files", {})
        complete = {}
        for index, source in enumerate(sources):
            filename = f"seg_{index:03d}.pcm"
            _, _, target = pcm_paths(show_id, ep_id, filename)
            stat = cached_pcm_stat(source, target)
            cached = previous.get(filename, {})
            trusted = bool(stat and manifest_matches(source, stat, cached))
            duration_ms = 0 if trusted else probe_duration_ms(source)
            # 迁移旧 PCM 时核对完整时长，不能把一小块偶数字节误判为整段。
            plausible = bool(stat and duration_ms and abs(stat.st_size - duration_ms * 32) <= 8192)
            if not trusted and not plausible:
                ensure_media_space(max(SEG_SEC * 32000, duration_ms * 32))
                target = pcm_segment(show_id, ep_id, filename, force=True)
                stat = target.stat()
            complete[filename] = pcm_manifest_entry(source, stat)
            # 每完成一段落盘清单；重启、失败后可复用已完整的段。
            save_json(manifest_path, {"version": PCM_CACHE_VERSION, "files": complete})
            set_state(key, pcm_ready_segments=index + 1,
                      progress=f"提前准备设备音频 {index + 1}/{len(sources)}")
        set_state(key, status="ready", segments=len(sources), mp3_complete=True, pcm_complete=True,
                  pcm_cache_version=PCM_CACHE_VERSION, progress="就绪", error="")
        if not prepared_segment_sources(show_id, ep_id, get_state().get(key, {})):
            raise RuntimeError("整集音频完整性检查未通过")
        prepare_browser_safely(show_id, ep_id, sources)
        return True
    except Exception as e:
        set_state(key, status="failed", pcm_complete=False, error=str(e)[:300], progress="准备失败")
        return False


def ensure_media_space(required):
    if shutil.disk_usage(MEDIA).free < required + MEDIA_FREE_RESERVE:
        raise OSError("音频缓存空间不足，请先释放磁盘空间")


def pcm_manifest_entry(source, pcm_stat):
    stat = source.stat()
    return {"source_bytes": stat.st_size, "source_mtime_ns": stat.st_mtime_ns,
            "bytes": pcm_stat.st_size}


def manifest_matches(source, pcm_stat, entry):
    return isinstance(entry, dict) and pcm_manifest_entry(source, pcm_stat) == entry


class PCMPathError(ValueError):
    """拒绝不符合固定分段命名或包含符号链接的路径。"""


@contextmanager
def pcm_target_lock(target):
    """同一个分段只转一次；没有等待者后移除锁，避免积累条目。"""
    key = str(target)
    with _pcm_locks_guard:
        entry = _pcm_locks.setdefault(key, [threading.Lock(), 0])
        entry[1] += 1
    try:
        with entry[0]:
            yield
    finally:
        with _pcm_locks_guard:
            entry[1] -= 1
            if entry[1] == 0:
                del _pcm_locks[key]


def pcm_paths(show_id, ep_id, filename):
    """仅允许已有节目目录里的正规 MP3 分段，不跟随符号链接。"""
    if (not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", show_id)
            or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", ep_id)):
        raise PCMPathError("bad path")
    match = re.fullmatch(r"seg_([0-9]{3})\.pcm", filename)
    if not match:
        raise PCMPathError("bad segment name")
    if not known_show(show_id):
        raise FileNotFoundError("show not found")
    directory = ep_dir(show_id, ep_id)
    for part in (MEDIA, MEDIA / show_id, directory):
        if part.is_symlink():
            raise PCMPathError("symlink is not allowed")
        if not part.is_dir():
            raise FileNotFoundError("segment not found")
    source = directory / ("seg_" + match.group(1) + ".mp3")
    cache_dir = directory / PCM_CACHE_VERSION
    target = cache_dir / filename
    for part in (source, cache_dir, target):
        if part.is_symlink():
            raise PCMPathError("symlink is not allowed")
    if not source.is_file():
        raise FileNotFoundError("segment not found")
    if cache_dir.exists() and not cache_dir.is_dir():
        raise PCMPathError("invalid cache directory")
    if target.exists() and not target.is_file():
        raise PCMPathError("invalid cache file")
    return source, cache_dir, target


def pcm_segment(show_id, ep_id, filename, force=False):
    """只由准备任务调用；转换失败时不暴露半文件或覆盖旧缓存。"""
    source, cache_dir, target = pcm_paths(show_id, ep_id, filename)
    with pcm_target_lock(target):
        # 等待另一个请求期间，目录或源文件可能改变，重新验证后再访问。
        source, cache_dir, target = pcm_paths(show_id, ep_id, filename)
        if target.is_file() and not force:
            stat = target.stat()
            if stat.st_size > 0 and stat.st_size % 2 == 0 and stat.st_mtime_ns >= source.stat().st_mtime_ns:
                return target
        cache_dir.mkdir(exist_ok=True)
        if cache_dir.is_symlink() or not cache_dir.is_dir():
            raise PCMPathError("invalid cache directory")
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(
                    dir=cache_dir, prefix="." + filename + ".", suffix=".tmp", delete=False) as f:
                temporary = Path(f.name)
            cmd = ["ffmpeg", "-y", "-nostdin", "-loglevel", "error", "-threads", "1", "-i", str(source),
                   "-map", "0:a:0", "-vn", "-ac", str(PCM_CHANNELS),
                   "-ar", str(PCM_SAMPLE_RATE),
                   "-af", "loudnorm=I=-16:TP=-1.5:LRA=11,volume=0.5",
                   "-acodec", "pcm_s16le", "-threads", "1", "-f", "s16le", str(temporary)]
            with _pcm_build_slots:
                result = subprocess.run(cmd, capture_output=True, text=True,
                                        timeout=PCM_FFMPEG_TIMEOUT)
            if result.returncode != 0:
                raise RuntimeError("PCM conversion failed")
            stat = temporary.stat()
            if stat.st_size == 0 or stat.st_size % 2:
                raise RuntimeError("PCM conversion produced invalid audio")
            pcm_paths(show_id, ep_id, filename)
            temporary.replace(target)
            return target
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)


def nonnegative_int(value):
    try:
        return max(0, int(value))
    except (TypeError, ValueError, OverflowError):
        return 0


def mp3_segment_sources(show_id, ep_id, state):
    """旧版 ready 或显式完整标记可复用 MP3；磁盘完整连续是额外条件。"""
    count = nonnegative_int(state.get("segments"))
    if not (state.get("mp3_complete") or state.get("status") == "ready") or not 0 < count <= MAX_EPISODE_SEGMENTS:
        return []
    sources = []
    try:
        for index in range(count):
            source, _, _ = pcm_paths(show_id, ep_id, f"seg_{index:03d}.pcm")
            if source.stat().st_size <= 0:
                return []
            sources.append(source)
        if len(list(ep_dir(show_id, ep_id).glob("seg_*.mp3"))) != count:
            return []
    except (PCMPathError, OSError):
        return []
    return sources


_MANIFEST_MEMO = {}


def _cached_manifest(path):
    """shows_lite 每次请求会对每档就绪节目读一遍整集清单；NAS 盘上每次打开小文件要几毫秒，
    三十个节目叠加会把接口拖到秒级、拖垮设备播放。按 (mtime, size) 记忆化；
    重新准备会改写清单文件，缓存键随之变化、自动失效。"""
    try:
        stat = path.stat()
        key = (stat.st_mtime_ns, stat.st_size)
    except OSError:
        return {}
    memo = _MANIFEST_MEMO.get(path)
    if memo and memo[0] == key:
        return memo[1]
    data = load_json(path, {})
    if len(_MANIFEST_MEMO) >= 256:
        _MANIFEST_MEMO.clear()
    _MANIFEST_MEMO[path] = (key, data)
    return data


def prepared_segment_sources(show_id, ep_id, state):
    """ready 必须整集 PCM 和来源清单吻合，旧 ready 无法伪装设备已就绪。"""
    if (state.get("status") != "ready" or not state.get("pcm_complete")
            or state.get("pcm_cache_version") != PCM_CACHE_VERSION):
        return []
    sources = mp3_segment_sources(show_id, ep_id, state)
    if not sources:
        return []
    try:
        manifest_path = ep_dir(show_id, ep_id) / PCM_MANIFEST_NAME
        if manifest_path.is_symlink():
            return []
        manifest = _cached_manifest(manifest_path)
        if manifest.get("version") != PCM_CACHE_VERSION:
            return []
        entries = manifest.get("files", {})
        if not isinstance(entries, dict) or len(entries) != len(sources):
            return []
        for index, source in enumerate(sources):
            filename = f"seg_{index:03d}.pcm"
            _, _, target = pcm_paths(show_id, ep_id, filename)
            stat = cached_pcm_stat(source, target)
            if not stat or not manifest_matches(source, stat, entries.get(filename)):
                return []
    except (OSError, PCMPathError, AttributeError):
        return []
    return sources


def cached_pcm_stat(source, target):
    if target.is_symlink() or not target.is_file():
        return None
    stat = target.stat()
    if stat.st_size > 0 and stat.st_size % 2 == 0 and stat.st_mtime_ns >= source.stat().st_mtime_ns:
        return stat
    return None


def probe_duration_ms(source):
    """后台迁移旧 PCM 时读取来源时长；播放 HTTP 请求不会调用。"""
    try:
        result = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "a:0",
             "-show_entries", "stream=duration:format=duration", "-of", "json", str(source)],
            capture_output=True, text=True, timeout=10)
        if result.returncode != 0:
            return 0
        info = json.loads(result.stdout)
        streams = info.get("streams") or [{}]
        seconds = float(streams[0].get("duration") or info.get("format", {}).get("duration") or 0)
        return round(seconds * 1000) if math.isfinite(seconds) and seconds > 0 else 0
    except (OSError, subprocess.TimeoutExpired, ValueError, TypeError, IndexError):
        return 0


def episode_pub_date(episode):
    text = str(episode.get("published") or "")
    try:
        date = datetime.fromisoformat(text.replace("Z", "+00:00"))
        if date.tzinfo is None:
            date = date.replace(tzinfo=timezone.utc)
        return date.astimezone(timezone(timedelta(hours=8))).date().isoformat()
    except ValueError:
        return text[:10] if re.match(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", text) else ""


def episode_timestamp(episode):
    try:
        date = datetime.fromisoformat(str(episode.get("published") or "").replace("Z", "+00:00"))
        if date.tzinfo is None:
            date = date.replace(tzinfo=timezone.utc)
        return max(0, int(date.timestamp()))
    except (ValueError, OverflowError, OSError):
        return 0


def sorted_episodes(episodes, order="newest"):
    # 稳定排序保留同一发布时间、无日期项目的源内先后。无日期始终放在最后。
    known = [episode for episode in episodes if episode_timestamp(episode)]
    unknown = [episode for episode in episodes if not episode_timestamp(episode)]
    known.sort(key=episode_timestamp, reverse=True)
    canonical = known + unknown
    return canonical if order == "newest" else list(reversed(canonical))


def episode_brief(episode):
    if episode is None:
        return None
    return {"id": episode["id"], "title": str(episode.get("title") or "")[:160],
            "pub_date": episode_pub_date(episode), "published_at": episode_timestamp(episode),
            "duration": nonnegative_int(parse_duration(episode.get("duration")))}


def sorted_shows(cache):
    shows = []
    for configured in configured_shows():
        cached = cache.get(configured["id"], {})
        episodes = sorted_episodes(cached.get("episodes", []))
        shows.append((configured, cached, episodes))
    return sorted(shows, key=lambda item: episode_timestamp(item[2][0]) if item[2] else 0, reverse=True)


def queue_preparation(show_id, episode, priority=0):
    key = f"{show_id}/{episode['id']}"
    with _prepare_lock:
        for queued_key, job in list(_prepare_jobs.items()):
            if job.done():
                del _prepare_jobs[queued_key]
                _pinned_keys.discard(queued_key)
        state = get_state().get(key, {})
        prepared = prepared_segment_sources(show_id, episode["id"], state)
        if prepared and browser_audio_path(show_id, episode["id"], prepared):
            return "ready"
        if key in _prepare_jobs:
            _pool.promote(_prepare_jobs[key], priority)
            if priority == 0:
                _pinned_keys.add(key)
            return state.get("status", "queued")
        if len(_prepare_jobs) >= 64:
            raise RuntimeError("准备队列已满，请稍后重试")
        reusable = bool(mp3_segment_sources(show_id, episode["id"], state))
        if prepared:
            # 设备音频已完整、只差网页整集：status 保持 ready，绝不降级成
            # queued——设备端"ready 即播"的轮询依赖它（坑①）。
            set_state(key, browser_status="queued", browser_error="")
        else:
            # 标记旧版完整 MP3，queued 状态不能抹去迁移时可复用的证据。
            set_state(key, status="queued", mp3_complete=reusable, pcm_complete=False,
                      progress="准备队列中", error="", browser_status="queued", browser_error="")
        try:
            future = _pool.submit(transcode, show_id, episode["id"], episode["audio"], priority=priority)
        except Exception:
            _pinned_keys.discard(key)
            set_state(key, status="failed", progress="准备失败", error="无法加入准备队列")
            raise
        _prepare_jobs[key] = future
        if priority == 0:
            _pinned_keys.add(key)
        future.add_done_callback(lambda _finished, k=key: _pinned_keys.discard(k))
        return "ready" if prepared else "queued"


def queue_latest_episodes():
    """每档按 最新→次新 顺序预缓存设置指定的集数；更新的集优先级更高。

    priority 10 = 最新一集，11 = 第二集……上限 15；恢复(8)与点播(0)始终更靠前。
    """
    results = {}
    depth_total = load_settings()["prefetch_latest"]
    for configured, _, episodes in sorted_shows(load_json(EP_CACHE, {})):
        for depth, episode in enumerate(episodes[:depth_total]):
            key = f"{configured['id']}/{episode['id']}"
            try:
                results[key] = queue_preparation(configured["id"], episode,
                                                 priority=10 + min(depth, 5))
            except (KeyError, OSError, RuntimeError):
                results[key] = "failed"
    return results


def recover_prepared_episodes():
    """启动补齐旧 MP3/中断任务，保留现有音频，不删除、不重新下载完整缓存。"""
    for key, state in get_state().items():
        parts = key.split("/")
        if len(parts) != 2 or not known_show(parts[0]):
            continue
        sid, eid = parts
        prepared = prepared_segment_sources(sid, eid, state)
        if prepared and browser_audio_path(sid, eid, prepared):
            continue
        _, episode = ep_lookup(sid, eid)
        if episode and (prepared or mp3_segment_sources(sid, eid, state)
                        or state.get("status") in {"queued", "downloading", "transcoding"}):
            try:
                queue_preparation(sid, episode, priority=8)
            except (KeyError, OSError, RuntimeError):
                continue


def effective_status(show_id, ep_id, state):
    status = state.get("status", "none")
    return "missing" if status == "ready" and not prepared_segment_sources(show_id, ep_id, state) else status


# ---------------------------------------------------------------- 缓存估算与清理

# 实测（任务2b）：下载+MP3+PCM 全程约 40 倍速（2 小时集约 3 分钟）。
# 对网页的预计时间按 25 倍速再加 60 秒下载余量：宁可报多，不报少。
PREPARE_SPEED_DIVISOR = 25
PREPARE_BASE_SECONDS = 60


def prepare_base_seconds(duration_sec):
    return PREPARE_BASE_SECONDS + max(0, int(duration_sec or 0)) // PREPARE_SPEED_DIVISOR


def prepare_estimate(show_id, ep_id, duration_sec):
    """点播一集还要等多久：自己的准备时间 + 前面点播任务的准备时间。"""
    key = f"{show_id}/{ep_id}"
    total = prepare_base_seconds(duration_sec)
    ahead = 0
    with _prepare_lock:
        job = _prepare_jobs.get(key)
        running = job is not None and job.running()
        for queued_key, other in _prepare_jobs.items():
            if queued_key != key and queued_key in _pinned_keys and not other.done():
                ahead += 1
    if not running:
        # 自己还没开始：前面的点播任务每集按保守均值计入等待。
        total += ahead * prepare_base_seconds(7200)
    return min(total, 5400), ahead


CLEANUP_TZ = timezone(timedelta(hours=8))
CLEANUP_HOUR, CLEANUP_MINUTE = 3, 10  # 每天凌晨 3 点左右（错开整点的其他系统任务）


def next_cleanup_delay(now=None):
    now = now or datetime.now(CLEANUP_TZ)
    target = now.replace(hour=CLEANUP_HOUR, minute=CLEANUP_MINUTE, second=0, microsecond=0)
    if target <= now:
        target += timedelta(days=1)
    return max(1.0, (target - now).total_seconds())


def in_flight_keys():
    with _prepare_lock:
        return set(_prepare_jobs) | set(_pinned_keys)


def cleanup_media(reason="scheduled", keep_override=None):
    """每档保留最新 keep_per_show 集；保护名单内的集绝不删除。

    保护名单：正在播放/暂停中的会话、未听完的进度、近两周听过、
    正在排队或准备中（含点播插队）。目录被外部脚本删掉的残留 state 条目一并清走。
    """
    keep = keep_override or load_settings()["keep_per_show"]
    state = get_state()
    catalogue = load_json(EP_CACHE, {})
    protection = LISTENING.cleanup_protection()
    protected = protection["active"] | protection["in_progress"] | protection["recent"]
    deleted, freed, kept, skipped = [], 0, 0, 0
    for configured in configured_shows():
        sid = configured["id"]
        episodes = sorted_episodes(catalogue.get(sid, {}).get("episodes", []))
        for index, episode in enumerate(episodes):
            eid = episode["id"]
            directory = ep_dir(sid, eid)
            if not directory.is_dir():
                continue
            key = f"{sid}/{eid}"
            if (index < keep or (sid, eid) in protected
                    or state.get(key, {}).get("status") in {"queued", "downloading", "transcoding"}):
                kept += 1
                continue
            if key in in_flight_keys():  # 删除前复查，避免和刚点播的任务赛跑
                skipped += 1
                continue
            try:
                freed += sum(path.stat().st_size for path in directory.rglob("*")
                             if path.is_file() and not path.is_symlink())
                shutil.rmtree(directory, ignore_errors=True)
            except OSError:
                skipped += 1
                continue
            if directory.exists():
                skipped += 1
                continue
            deleted.append(key)
    # 清掉目录已不存在（外部脚本删除等）的残留条目，避免 ready 假象。
    residue = [key for key, item in state.items()
               if isinstance(item, dict) and key.count("/") == 1
               and key not in deleted and not ep_dir(*key.split("/")).is_dir()
               and key not in in_flight_keys()]
    with _lock:
        fresh = get_state()
        for key in deleted + residue:
            fresh.pop(key, None)
        save_json(STATE, fresh)
    set_state("__last_cleanup__", finished_at=int(time.time()), reason=reason, keep=keep,
              deleted=len(deleted), freed_mb=round(freed / 1048576, 1),
              protected=len(protected), skipped=skipped)
    return {"deleted": len(deleted), "freed_mb": round(freed / 1048576, 1), "keep": keep,
            "reason": reason, "finished_at": int(time.time())}


def cleanup_with_watermark(reason="scheduled"):
    """常规清理后磁盘仍低于水位线时，再收一轮（每档只保最新 1 集，保护名单依旧有效）。"""
    result = cleanup_media(reason)
    settings = load_settings()
    if shutil.disk_usage(MEDIA).free < settings["low_free_gb"] * 1024 ** 3:
        result = {"first": result, "tight": cleanup_media(reason + "-tight", keep_override=1)}
    return result


def clear_media_cache():
    """清空全部缓存：订阅与收听进度不动；正在播放和准备中的集保留，避免打断播放。"""
    active = LISTENING.cleanup_protection()["active"]
    deleted, freed = [], 0
    for show_dir in MEDIA.iterdir():
        if not show_dir.is_dir():
            continue
        for directory in show_dir.iterdir():
            if not directory.is_dir():
                continue
            key = f"{show_dir.name}/{directory.name}"
            if (show_dir.name, directory.name) in active or key in in_flight_keys():
                continue
            try:
                freed += sum(path.stat().st_size for path in directory.rglob("*")
                             if path.is_file() and not path.is_symlink())
                shutil.rmtree(directory, ignore_errors=True)
            except OSError:
                continue
            if directory.exists():
                continue
            deleted.append(key)
    with _lock:
        fresh = get_state()
        for key in deleted:
            fresh.pop(key, None)
        save_json(STATE, fresh)
    set_state("__last_cleanup__", finished_at=int(time.time()), reason="clear",
              deleted=len(deleted), freed_mb=round(freed / 1048576, 1), keep=0,
              protected=len(active), skipped=0)
    return {"deleted": len(deleted), "freed_mb": round(freed / 1048576, 1)}


def cleanup_loop():
    while True:
        time.sleep(next_cleanup_delay())
        try:
            cleanup_with_watermark("scheduled")
        except Exception:
            pass  # 单日清理失败不影响服务；第二天同一时间再试。


def compact_json(body):
    return Response(json.dumps(body, ensure_ascii=False, separators=(",", ":")), mimetype="application/json")


# ---------------------------------------------------------------- HTTP

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = 16384


@app.before_request
def household_access():
    # Compatibility tests explicitly opt out on their isolated Flask application.
    # There is no environment switch or production legacy path that bypasses auth.
    if app.testing and app.config.get("PODCAST_TEST_AUTH_DISABLED"):
        g.household_role = "administrator"
        return
    path = request.path
    public = (path in {"/healthz", "/api/health", "/setup", "/login", "/api/setup/status", "/api/setup",
                       "/api/auth/login", "/api/devices/claim"}
              or path.startswith("/static/") or path.startswith("/feeds/xiaoyuzhou/"))
    header = request.headers.get("Authorization", "")
    bearer = header[7:] if header.startswith("Bearer ") else None
    if bearer is not None and not public:
        g.device_id = ACCESS.device(bearer)
        if g.device_id:
            g.household_role = "device"
        elif not public:
            raise ListeningError("设备未配对或已被移除，请重新配对", 401)
    elif ACCESS.administrator(request.cookies.get(ADMIN_COOKIE)):
        g.household_role = "administrator"
    if request.method not in {"GET", "HEAD", "OPTIONS"} and getattr(g, "household_role", None) != "device":
        origin = request.headers.get("Origin")
        if origin and origin.rstrip("/") != request.host_url.rstrip("/"):
            raise ListeningError("请从后台自己的网页完成操作", 403)
    if public:
        return
    role = getattr(g, "household_role", None)
    if not role:
        if path == "/":
            return redirect("/login" if ACCESS.initialized() else "/setup")
        raise ListeningError("请先登录后台，或为设备完成配对", 401)
    if role == "device":
        permitted = (path.startswith(("/api/shows", "/api/episodes/", "/api/listening/", "/pcm/", "/media/", "/art/"))
                     or path in {"/api/prepare", "/api/status"})
        if not permitted:
            raise ListeningError("这项操作需要在后台管理网页完成", 403)


@app.after_request
def household_response(response):
    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["X-Frame-Options"] = "DENY"
    response.headers["Referrer-Policy"] = "same-origin"
    if request.path.startswith("/api/") or request.path in {"/setup", "/login", "/"}:
        response.headers["Cache-Control"] = "no-store"
    if not request.path.startswith(("/static/", "/feeds/")):
        response.vary.add("Cookie")
        response.vary.add("Authorization")
    return response


@app.errorhandler(ListeningError)
def listening_error(error):
    return jsonify({"error": str(error)}), error.status


@app.errorhandler(HTTPException)
def http_error(error):
    messages = {400: "请求内容格式不正确", 404: "没有找到这个页面或接口", 405: "这项操作的请求方式不正确",
                413: "请求内容太大，请缩短设备名称后重试", 415: "请求内容格式不正确"}
    # Keep protocol headers such as Content-Range on 416 and Allow on 405.
    response = error.get_response()
    response.set_data(json.dumps({"error": messages.get(error.code, "后台暂时无法完成这项请求")},
                                 ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
    response.mimetype = "application/json"
    return response


def request_object():
    body = request.get_json(force=True, silent=True)
    if not isinstance(body, dict):
        raise ListeningError("请求内容必须是一个对象")
    return body


def admin_required():
    if getattr(g, "household_role", None) != "administrator":
        raise ListeningError("请先登录管理网页", 401)


def signed_in(token):
    response = compact_json({"ok": True})
    response.set_cookie(ADMIN_COOKIE, token, max_age=SESSION_SECONDS, httponly=True,
                        samesite="Strict", secure=request.is_secure, path="/")
    return response


@app.get("/api/setup/status")
def setup_status():
    return compact_json({"initialized": ACCESS.initialized(),
                         "authenticated": getattr(g, "household_role", None) == "administrator",
                         "setup_key_available": bool(ACCESS.setup_key),
                         "library_scope": "household", "pairing_required": True})


@app.post("/api/setup")
def initial_setup():
    body = request_object()
    return signed_in(ACCESS.setup(body.get("setup_key"), body.get("password"), request.remote_addr))


@app.post("/api/auth/login")
def administrator_login():
    return signed_in(ACCESS.login(request_object().get("password"), request.remote_addr))


@app.post("/api/auth/logout")
def administrator_logout():
    admin_required()
    ACCESS.logout(request.cookies.get(ADMIN_COOKIE))
    response = compact_json({"ok": True})
    response.delete_cookie(ADMIN_COOKIE, path="/", httponly=True, samesite="Strict")
    return response


@app.get("/api/devices")
def household_devices():
    admin_required()
    return compact_json({"devices": ACCESS.devices(), "library_scope": "household", "server_url": service_origin().rstrip("/"),
                         "pairing": ACCESS.pairing_status()})


@app.post("/api/devices/pairings")
def new_pairing():
    admin_required()
    ACCESS.throttle("pairing", "administrator", maximum=20)
    return compact_json({**ACCESS.pairing(), "server_url": service_origin().rstrip("/")})


@app.post("/api/devices/claim")
def device_claim():
    if not ACCESS.initialized():
        raise ListeningError("请先在后台完成首次设置", 409)
    body = request_object()
    return compact_json(ACCESS.claim(body.get("code"), body.get("device_id"), body.get("name"), request.remote_addr))


@app.patch("/api/devices/<device_id>")
def rename_device(device_id):
    admin_required()
    ACCESS.rename(device_id, request_object().get("name"))
    return compact_json({"ok": True})


@app.delete("/api/devices/<device_id>")
def revoke_device(device_id):
    admin_required()
    ACCESS.revoke(device_id)
    return compact_json({"ok": True})


@app.get("/setup")
@app.get("/login")
def access_page():
    if getattr(g, "household_role", None) == "administrator":
        return redirect("/#devices")
    return send_from_directory(BASE / "static", "access.html")


def device_client(body):
    if getattr(g, "household_role", None) == "device" and body.get("client_id") != g.device_id:
        raise ListeningError("设备不能使用另一台设备的编号", 403)


def device_session(body):
    if getattr(g, "household_role", None) == "device":
        session = body.get("session_id")
        if not isinstance(session, str):
            raise ListeningError("播放记录编号格式不正确")
        with LISTENING.connection() as db:
            owner = db.execute("SELECT client_id FROM sessions WHERE id=?", (session,)).fetchone()
        if not owner or owner["client_id"] != g.device_id:
            raise ListeningError("设备不能写入另一台设备的播放记录", 403)


@app.post("/api/listening/sessions")
def listening_start():
    body = request_object()
    device_client(body)
    sid, eid = body.get("show_id"), body.get("episode_id")
    identifier(sid, "show_id")
    identifier(eid, "episode_id")
    _, episode = ep_lookup(sid, eid)
    if not known_show(sid) or not episode:
        raise ListeningError("找不到这一集，请刷新节目列表", 404)
    return compact_json(LISTENING.start(body, episode_duration_ms(sid, episode)))


@app.post("/api/listening/events")
def listening_event():
    body = request_object()
    device_session(body)
    return compact_json(LISTENING.event(body))


@app.get("/api/listening/progress/<show_id>/<episode_id>")
def listening_progress(show_id, episode_id):
    _, episode = ep_lookup(show_id, episode_id)
    if not known_show(show_id) or not episode:
        raise ListeningError("找不到这一集", 404)
    progress = LISTENING.progress(show_id, episode_id)
    if request.args.get("lite") == "1":
        return compact_json({**compact_progress(progress), "t": progress["updated_at"]})
    return compact_json({"progress": progress})


@app.post("/api/listening/mark")
def listening_mark():
    body = request_object()
    sid, eid = body.get("show_id"), body.get("episode_id")
    identifier(sid, "show_id")
    identifier(eid, "episode_id")
    _, episode = ep_lookup(sid, eid)
    if not known_show(sid) or not episode:
        raise ListeningError("找不到这一集", 404)
    progress = LISTENING.mark(sid, eid, body.get("status"), episode_duration_ms(sid, episode))
    return compact_json({"progress": progress})


@app.post("/api/listening/import")
def listening_import():
    body = request_object()
    device_client(body)
    entries = body.get("episodes")
    if not isinstance(entries, list) or not 1 <= len(entries) <= 100:
        raise ListeningError("每次可导入 1 到 100 条旧收听进度")
    resolved = []
    for entry in entries:
        item = {"error": "invalid"}
        if isinstance(entry, dict):
            item.update(show_id=entry.get("show_id"), episode_id=entry.get("episode_id"))
            try:
                identifier(item["show_id"], "show_id")
                identifier(item["episode_id"], "episode_id")
                position = integer(entry.get("position_ms"), "position_ms")
                if "completed" in entry and not isinstance(entry["completed"], bool):
                    raise ListeningError("completed 必须是布尔值")
                updated = integer(entry.get("updated_at", 0), "updated_at", 9223372036854775)
                _, episode = ep_lookup(item["show_id"], item["episode_id"])
                if known_show(item["show_id"]) and episode:
                    item.update(position_ms=position, duration_ms=episode_duration_ms(item["show_id"], episode),
                                completed=entry.get("completed", False), updated_at=updated)
                    item.pop("error")
                else:
                    item["error"] = "not_found"
            except ListeningError:
                pass
        resolved.append(item)
    return compact_json(LISTENING.import_progress(body, resolved))


@app.get("/api/listening/recent")
def listening_recent():
    try:
        limit = int(request.args.get("limit", "20"))
        if not 1 <= limit <= 50:
            raise ValueError
    except ValueError:
        raise ListeningError("最近收听数量必须在 1 到 50 之间")
    rows = []
    lite = request.args.get("lite") == "1"
    for item in LISTENING.recent(limit):
        show, episode = ep_lookup(item["show_id"], item["episode_id"])
        if not show or not episode:
            continue
        if lite:
            rows.append({"i": item["show_id"], "e": item["episode_id"], "n": str(show["name"])[:80],
                         "t": str(episode["title"])[:160], "d": parse_duration(episode.get("duration")),
                         **compact_progress(item["progress"])})
        else:
            rows.append({**item, "show_name": show["name"], "title": episode["title"],
                         "art": "/art/" + item["show_id"], "duration": parse_duration(episode.get("duration"))})
    return compact_json({"s" if lite else "episodes": rows, "server_time": int(time.time())})


@app.get("/api/listening/stats")
def listening_stats():
    stats = LISTENING.stats()
    existing = {row["show_id"]: row for row in stats["shows"]}
    cache = load_json(EP_CACHE, {})
    stats["shows"] = [{"show_id": show["id"], "name": cache.get(show["id"], {}).get("name", show["name"]),
                       "listened_ms": 0, "played_episodes": 0, "completed_episodes": 0, "play_count": 0,
                       **existing.get(show["id"], {})} for show in configured_shows()]
    return compact_json({**stats, "server_time": int(time.time()), "timezone": "Asia/Shanghai"})


def normalized_identity_name(value):
    value = unicodedata.normalize("NFKC", str(value or "")).casefold()
    return "".join(character for character in value if unicodedata.category(character)[0] not in {"P", "Z", "C"})


def source_content_matches(payload, cached):
    """A same-name show needs several independently matching episodes, not a title guess."""
    incoming, existing = payload.get("episodes", []), cached.get("episodes", [])
    titles = {}
    for episode in existing:
        key = normalized_identity_name(episode.get("title"))
        if len(key) >= 8:
            titles.setdefault(key, []).append(episode)
    matches = set()
    for episode in incoming:
        key = normalized_identity_name(episode.get("title"))
        timestamp = episode_timestamp(episode)
        if key in matches or not timestamp:
            continue
        for known in titles.get(key, []):
            known_timestamp = episode_timestamp(known)
            if not known_timestamp or abs(timestamp - known_timestamp) > 72 * 3600:
                continue
            duration, known_duration = parse_duration(episode.get("duration")), parse_duration(known.get("duration"))
            if duration and known_duration and abs(duration - known_duration) > max(60, min(duration, known_duration) * .2):
                continue
            matches.add(key)
            break
    return len(matches) >= 3


def source_duplicate(payload):
    cache = load_json(EP_CACHE, {})
    incoming_name = normalized_identity_name(payload.get("show", {}).get("name"))
    for show in configured_shows():
        source = urlsplit(show.get("feed", ""))
        if (show.get("feed") in {payload["feed_url"], payload.get("requested_feed_url"), payload["source_url"]}
                or payload.get("public_pid") and (show.get("public_pid") == payload["public_pid"]
                    or source.path == "/feeds/xiaoyuzhou/" + payload["public_pid"] + ".xml")):
            return {"show_id": show["id"], "reason": "same_feed"}
        cached = cache.get(show["id"], {})
        known_names = {normalized_identity_name(show.get("name")), normalized_identity_name(cached.get("name"))} - {""}
        if not incoming_name or incoming_name not in known_names:
            continue
        identities = show.get("identities", {})
        if ((payload.get("apple_id") and payload["apple_id"] in identities.get("apple", []))
                or (payload.get("public_pid") and payload["public_pid"] in identities.get("xiaoyuzhou", []))):
            return {"show_id": show["id"], "reason": "verified_platform_identity"}
        if source_content_matches(payload, cached):
            return {"show_id": show["id"], "reason": "matching_episode_history"}
    return None


def source_existing(payload):
    duplicate = source_duplicate(payload)
    return duplicate["show_id"] if duplicate else None


@app.get("/api/sources/search")
def source_search():
    return compact_json({"provider": "apple", "provider_name": "苹果播客公开目录",
                         "results": search_apple(request.args.get("q", ""))})


@app.post("/api/sources/preview")
def source_preview():
    payload = resolve_source(request_object().get("url"))
    duplicate_info = source_duplicate(payload)
    duplicate = duplicate_info["show_id"] if duplicate_info else None
    payload["existing_show_id"] = duplicate
    token, expires = LISTENING.save_preview(payload)
    show = dict(payload["show"])
    show["latest"] = {key: value for key, value in show["latest"].items() if key in {"title", "duration", "published"}}
    return compact_json({"preview_id": token, "expires_at": expires, "source_type": payload["source_type"],
                         "source_url": payload["source_url"], "feed_url": payload["feed_url"], "show": show,
                         "duplicate": bool(duplicate), "existing_show_id": duplicate,
                         "duplicate_reason": duplicate_info["reason"] if duplicate_info else None,
                         "warning": payload["warning"]})


@app.post("/api/sources")
def source_add():
    body = request_object()
    token = body.get("preview_id")
    identifier(token, "preview_id")
    # Keep the validated preview in SQLite so retries survive process restarts.
    with LISTENING.connection() as db:
        row = db.execute("SELECT * FROM source_previews WHERE id=?", (token,)).fetchone()
        if not row or row["expires"] < time.time():
            raise ListeningError("预览已过期，请重新检查链接", 409)
        payload = json.loads(row["payload"])
    existing = source_existing(payload)
    if existing:
        source = next(show for show in configured_shows() if show["id"] == existing)
        duplicate = True
        if existing in load_json(EP_CACHE, {}):
            return compact_json({"show_id": existing, "duplicate": True, "show": payload["show"]})
    else:
        source, duplicate = LISTENING.commit_source(token, CFG["shows"], maximum=64)
    if source.get("public_pid") and payload.get("public_pid") == source["public_pid"] and payload.get("raw_feed"):
        pid = source["public_pid"]
        _public_feeds.add_program(pid, source["name"], source.get("author", ""))
        save_json(_public_feeds.cache_directory / (pid + ".json"), {"updated": time.time(), "xml": payload["raw_feed"]})
    show = payload["show"]
    with _catalogue_lock:
        cache = load_json(EP_CACHE, {})
        if source["id"] not in cache:
            cache[source["id"]] = {"id": source["id"], "name": show["name"], "author": show["author"],
                                   "art": show["art"], "link": show.get("link", ""),
                                   "updated": time.strftime("%Y-%m-%d %H:%M:%S"),
                                   "count": len(payload["episodes"]), "episodes": sorted_episodes(payload["episodes"])}
            save_json(EP_CACHE, cache)
    # 重新订阅时撤销退订标记，节目重新进入节目库。
    state = load_json(STATE, {})
    if source["id"] in state.get("__removed_shows__", []):
        state["__removed_shows__"] = [x for x in state["__removed_shows__"] if x != source["id"]]
        save_json(STATE, state)
    invalidate_configured_shows()
    DEVICE_COVERS.request(source["id"], show["art"])
    if payload["episodes"]:
        try:
            queue_preparation(source["id"], payload["episodes"][0], priority=10)
        except (RuntimeError, OSError):
            # Subscription is saved; selecting an episode can retry preparation.
            pass
    return compact_json({"show_id": source["id"], "duplicate": duplicate, "show": show})


@app.delete("/api/sources/<show_id>")
def source_remove(show_id):
    """退订：移出订阅，并清掉单集缓存、封面与收听进度。"""
    if not device_cover_id(show_id):
        raise ListeningError("节目编号格式不正确", 400)
    configured = next((s for s in configured_shows() if s["id"] == show_id), None)
    if configured is None:
        raise ListeningError("没有找到这个节目", 404)
    name = configured.get("name", show_id)
    LISTENING.remove_source(show_id)
    state = load_json(STATE, {})
    removed = state.get("__removed_shows__", [])
    if show_id not in removed:
        removed.append(show_id)
    state["__removed_shows__"] = removed
    for key in [k for k in state if isinstance(k, str) and k.startswith(show_id + "/")]:
        del state[key]
    save_json(STATE, state)
    with _catalogue_lock:
        cache = load_json(EP_CACHE, {})
        if show_id in cache:
            del cache[show_id]
            save_json(EP_CACHE, cache)
    shutil.rmtree(MEDIA / show_id, ignore_errors=True)
    DEVICE_COVERS.forget(show_id)
    LISTENING.forget_show(show_id)
    invalidate_configured_shows()
    return compact_json({"ok": True, "show_id": show_id, "name": name})


def ep_lookup(show_id, ep_id):
    cache = load_json(EP_CACHE, {})
    show = cache.get(show_id)
    if not show:
        return None, None
    for e in show["episodes"]:
        if e["id"] == ep_id:
            return show, e
    return show, None


def episode_duration_ms(show_id, episode):
    sources = prepared_segment_sources(show_id, episode["id"], get_state().get(f"{show_id}/{episode['id']}", {}))
    if sources:
        try:
            return sum(pcm_paths(show_id, episode["id"], f"seg_{index:03d}.pcm")[2].stat().st_size for index in range(len(sources))) * 1000 // 32000
        except OSError:
            pass
    return max(0, parse_duration(episode.get("duration"))) * 1000


@app.get("/api/shows")
def api_shows():
    """板子启动时问的第一个接口：我订阅了啥、每档最新一集是什么"""
    cache = load_json(EP_CACHE, {})
    st = get_state()
    cached_by_show = {}
    for key, item in st.items():
        # 只按 state 计数不做文件校验：60 集逐档 stat 会把这个接口拖到秒级。
        sid, _, eid = key.partition("/")
        if eid and isinstance(item, dict) and item.get("status") == "ready":
            cached_by_show[sid] = cached_by_show.get(sid, 0) + 1
    out = []
    stats = {row["show_id"]: row for row in LISTENING.stats()["shows"]}
    for s, c, episodes in sorted_shows(cache):
        DEVICE_COVERS.request(s["id"], c.get("art", ""))
        if not c:
            out.append({"id": s["id"], "name": s["name"], "ready": False, "episodes": 0,
                        "cached_episodes": 0,
                        "listening": stats.get(s["id"], {"listened_ms": 0, "played_episodes": 0, "completed_episodes": 0, "play_count": 0})})
            continue
        latest = episodes[0] if episodes else None
        out.append({
            "id": c["id"],
            "name": c["name"],
            "author": c.get("author", ""),
            "art": f"/art/{c['id']}",
            "episodes": c["count"],
            "cached_episodes": cached_by_show.get(c["id"], 0),
            "latest": latest,
            "status": effective_status(c["id"], latest["id"], st.get(f"{c['id']}/{latest['id']}", {})) if latest else None,
            "latest_published_at": episode_timestamp(latest) if latest else 0,
            "listening": stats.get(s["id"], {"listened_ms": 0, "played_episodes": 0, "completed_episodes": 0, "play_count": 0}),
        })
    return jsonify({"shows": out, "server_time": int(time.time())})


@app.get("/api/shows_lite")
def api_shows_lite():
    """给板子用的精简接口。

    完整 /api/shows 有 10KB，其中大部分是简介和封面，板子 400KB 内存解析吃力。
    这里只返回选中和播放必需的字段，响应压到 1KB 以内。
    """
    cache = load_json(EP_CACHE, {})
    st = get_state()
    out = []
    stats = {row["show_id"]: row for row in LISTENING.stats()["shows"]}
    for s, c, episodes in sorted_shows(cache):
        DEVICE_COVERS.request(s["id"], c.get("art", ""))
        if not c or not episodes:
            continue
        latest = episodes[0]
        heard = stats.get(s["id"], {})
        out.append({
            "i": s["id"],
            "n": c["name"],
            "e": latest["id"],
            "r": 1 if prepared_segment_sources(s["id"], latest["id"], st.get(f"{s['id']}/{latest['id']}", {})) else 0,
            "p": episode_timestamp(latest),
            "d": episode_pub_date(latest),
            "l": heard.get("listened_ms", 0), "c": heard.get("completed_episodes", 0),
            "u": heard.get("played_episodes", 0),
        })
    payload = {"s": out}
    if "offset" in request.args or "limit" in request.args:
        offset_text, limit_text = request.args.get("offset", "0"), request.args.get("limit", "8")
        try:
            if not re.fullmatch(r"[0-9]+", offset_text) or not re.fullmatch(r"[0-9]+", limit_text):
                raise ValueError
            offset, limit = int(offset_text), int(limit_text)
            if not 0 <= offset <= 2147483647 or not 1 <= limit <= 8:
                raise ValueError
        except ValueError:
            return jsonify({"error": "invalid show pagination"}), 400
        payload = {"s": out[offset:offset + limit], "total": len(out), "offset": offset}
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    return Response(body, mimetype="application/json")


@app.get("/api/shows/<show_id>")
def api_show(show_id):
    if not known_show(show_id):
        return jsonify({"error": "找不到这个节目"}), 404
    cache = load_json(EP_CACHE, {})
    c = cache.get(show_id)
    if not c:
        return jsonify({"error": "该节目尚未抓取，请点刷新"}), 404
    st = get_state()
    eps = []
    for e in sorted_episodes(c["episodes"]):
        k = f"{show_id}/{e['id']}"
        s = st.get(k, {})
        eps.append({**e, **listening_fields(show_id, e["id"]), "status": effective_status(show_id, e["id"], s),
                    "segments": s.get("segments", 0), "size_mb": s.get("size_mb", 0)})
    return jsonify({**c, "episodes": eps})


@app.get("/api/shows/<show_id>/episodes")
def api_episode_page(show_id):
    if not known_show(show_id):
        return jsonify({"error": "show not found"}), 404
    offset_text, limit_text = request.args.get("offset", "0"), request.args.get("limit", "5")
    try:
        if not re.fullmatch(r"[0-9]+", offset_text) or not re.fullmatch(r"[0-9]+", limit_text):
            raise ValueError
        offset, limit = int(offset_text), int(limit_text)
        if offset > 2147483647 or not 1 <= limit <= 10:
            raise ValueError
    except ValueError:
        return jsonify({"error": "invalid pagination"}), 400
    order = request.args.get("order", "newest")
    if order not in {"newest", "oldest"}:
        return jsonify({"error": "invalid order"}), 400
    show = load_json(EP_CACHE, {}).get(show_id, {})
    episodes = sorted_episodes(show.get("episodes", []), order)
    status_filter = request.args.get("status")
    if status_filter is not None and status_filter not in {"unplayed", "in_progress", "completed"}:
        return jsonify({"error": "invalid listening status"}), 400
    heard = LISTENING.all_progress()
    if status_filter:
        episodes = [episode for episode in episodes if heard.get((show_id, episode["id"]), {"status": "unplayed"})["status"] == status_filter]
    focus = None
    if "episode_id" in request.args:
        episode_id = request.args["episode_id"]
        focus = next((index for index, episode in enumerate(episodes)
                      if episode.get("id") == episode_id), None)
        if focus is None:
            return jsonify({"error": "episode not found"}), 404
        offset = focus // limit * limit
    state = get_state()
    page = []
    for episode in episodes[offset:offset + limit]:
        sources = prepared_segment_sources(show_id, episode["id"], state.get(f"{show_id}/{episode['id']}", {}))
        fields = listening_fields(show_id, episode["id"], request.args.get("lite") == "1", heard.get((show_id, episode["id"])))
        if "progress" in fields and request.args.get("web") != "1":
            # Keep the constrained device-era page size; detailed counters are on
            # the single-episode endpoint and the per-show statistics endpoint.
            fields["progress"] = {key: value for key, value in fields["progress"].items()
                                  if key in {"status", "position_ms", "listened_ms", "revision"}}
        page.append({**episode_brief(episode), **fields,
                     "ready": bool(sources), "segments": len(sources)})
    body = {"show_id": show_id, "total": len(episodes), "offset": offset, "order": order, "episodes": page}
    if focus is not None:
        body["focus"] = focus
    return compact_json(body)


@app.get("/api/episodes/<show_id>/<ep_id>")
def api_episode_detail(show_id, ep_id):
    if not known_show(show_id):
        return jsonify({"error": "show not found"}), 404
    show, episode = ep_lookup(show_id, ep_id)
    if not episode:
        return jsonify({"error": "episode not found"}), 404
    state = get_state().get(f"{show_id}/{ep_id}", {})
    sources = prepared_segment_sources(show_id, ep_id, state)
    duration = nonnegative_int(parse_duration(episode.get("duration")))
    lite = request.args.get("lite") == "1"
    try:
        segments, duration_ms_total = [], 0
        for index, source in enumerate(sources):
            _, _, target = pcm_paths(show_id, ep_id, f"seg_{index:03d}.pcm")
            size = target.stat().st_size
            duration_ms = size * 1000 // 32000
            duration_ms_total += duration_ms
            if not lite:
                segments.append({"index": index, "bytes": size, "duration_ms": duration_ms})
    except PCMPathError:
        return jsonify({"error": "bad audio path"}), 400
    except OSError:
        return jsonify({"error": "audio metadata unavailable"}), 503
    if sources:
        duration = round(duration_ms_total / 1000)
    episodes = sorted_episodes(show["episodes"])
    index = next(i for i, candidate in enumerate(episodes) if candidate["id"] == ep_id)
    body = {**episode_brief(episode), **listening_fields(show_id, ep_id, lite), "duration": duration, "ready": bool(sources),
            "status": effective_status(show_id, ep_id, state), "segment_count": len(sources),
            "total": len(episodes), "newest_index": index, "oldest_index": len(episodes) - index - 1,
            "newer": episode_brief(episodes[index - 1]) if index else None,
            "older": episode_brief(episodes[index + 1]) if index + 1 < len(episodes) else None}
    if not lite:
        body["segments"] = segments
        browser_audio = browser_audio_path(show_id, ep_id, sources)
        body["browser_audio_url"] = f"/audio/{show_id}/{ep_id}.mp3" if browser_audio else None
        body["browser_audio_ready"] = bool(browser_audio)
        body["browser_audio_error"] = state.get("browser_error", "")
        body["browser_audio_status"] = "ready" if browser_audio else (
            "failed" if state.get("browser_error") else
            "preparing" if state.get("browser_status") in {"queued", "preparing"}
                or state.get("status") in {"queued", "downloading", "transcoding"} else "none")
        if not sources and body["status"] in {"queued", "downloading", "transcoding"}:
            body["prepare_eta_seconds"], body["queue_position"] = prepare_estimate(
                show_id, ep_id, parse_duration(episode.get("duration")))
    return compact_json(body)


@app.get("/audio/<show_id>/<ep_id>.mp3")
def browser_audio(show_id, ep_id):
    try:
        pcm_paths(show_id, ep_id, "seg_000.pcm")
        path = browser_audio_path(show_id, ep_id)
        if path is None:
            return jsonify({"error": "网页整集音频尚未准备好，请点击准备后重试"}), 503
        return send_file(path, mimetype="audio/mpeg", conditional=True)
    except (PCMPathError, OSError):
        return jsonify({"error": "音频缓存不可用"}), 503


@app.post("/api/prepare")
def api_prepare():
    """请求转码一集。板子拿到 ready 之前可以反复轮询这个接口。"""
    body = request.get_json(force=True, silent=True) or {}
    if not isinstance(body, dict):
        return jsonify({"error": "invalid episode"}), 400
    sid, eid = body.get("show_id"), body.get("episode_id")
    if not isinstance(sid, str) or not isinstance(eid, str):
        return jsonify({"error": "invalid episode"}), 400
    show, ep = ep_lookup(sid, eid)
    if not ep:
        return jsonify({"error": "找不到这一集"}), 404
    on_demand = body.get("prefetch") is not True
    try:
        status = queue_preparation(sid, ep, priority=0 if on_demand else 3)
    except (RuntimeError, OSError):
        return jsonify({"error": "音频准备队列暂时不可用"}), 503
    eta_seconds, ahead = (prepare_estimate(sid, eid, parse_duration(ep.get("duration")))
                          if on_demand and status != "ready" else (0, 0))
    return jsonify({"status": status, "show_id": sid, "episode_id": eid,
                    "pinned": on_demand, "queue_position": ahead, "eta_seconds": eta_seconds})


@app.get("/api/status")
def api_status():
    state = get_state()
    for key, item in state.items():
        parts = key.split("/")
        if len(parts) == 2:
            item["status"] = effective_status(*parts, item)
    return jsonify(state)


@app.get("/api/storage")
def api_storage():
    """缓存用量一览，只给后台网页展示；只读，不影响准备与播放流程。"""
    ready = 0
    for key, item in get_state().items():
        parts = key.split("/")
        if len(parts) == 2 and isinstance(item, dict) and effective_status(*parts, item) == "ready":
            ready += 1
    media_bytes = 0
    try:
        for path in MEDIA.rglob("*"):
            try:
                if path.is_file() and not path.is_symlink():
                    media_bytes += path.stat().st_size
            except OSError:
                continue
    except OSError:
        pass
    usage = shutil.disk_usage(MEDIA)
    return jsonify({"ready_episodes": ready, "media_bytes": media_bytes,
                    "disk_free_bytes": usage.free, "disk_total_bytes": usage.total})


@app.get("/api/settings")
def api_settings_get():
    admin_required()
    return compact_json({**load_settings(), "last_cleanup": get_state().get("__last_cleanup__", {})})


@app.put("/api/settings")
def api_settings_put():
    admin_required()
    body = request_object()
    changes = {key: body[key] for key in SETTING_LIMITS if key in body}
    if not changes:
        raise ListeningError("没有可更新的设置")
    settings = save_settings(changes)
    if "prefetch_latest" in changes:
        # 立即按新档位补预缓存；放后台线程跑，不让一次设置请求等几十个准备任务。
        threading.Thread(target=queue_latest_episodes, daemon=True).start()
    return compact_json({**settings, "last_cleanup": get_state().get("__last_cleanup__", {})})


_maintenance_lock = threading.Lock()
_maintenance = {"running": False, "result": None, "error": "", "finished_at": None}


def run_maintenance(action):
    def worker():
        try:
            _maintenance.update(result=action(), error="")
        except Exception as error:  # 维护失败只回报原因，不影响服务与播放。
            _maintenance.update(error=str(error)[:300])
        finally:
            _maintenance.update(running=False, finished_at=int(time.time()))

    with _maintenance_lock:
        if _maintenance["running"]:
            raise ListeningError("上一项缓存维护还在执行，请稍候再试", 409)
        _maintenance.update(running=True, finished_at=None)
        threading.Thread(target=worker, daemon=True).start()


@app.post("/api/maintenance/cleanup")
def api_maintenance_cleanup():
    admin_required()
    run_maintenance(lambda: cleanup_with_watermark("manual"))
    return compact_json({"ok": True, "running": True})


@app.post("/api/maintenance/clear")
def api_maintenance_clear():
    admin_required()
    if request_object().get("confirm") != "clear":
        raise ListeningError("清空缓存需要页面二次确认", 400)
    run_maintenance(clear_media_cache)
    return compact_json({"ok": True, "running": True})


@app.get("/api/maintenance")
def api_maintenance_status():
    admin_required()
    return compact_json(dict(_maintenance))


@app.get("/api/refresh")
@app.post("/api/refresh")
def api_refresh():
    # Legacy GET is retained only inside isolated compatibility tests. Browsers
    # use POST so a linked URL cannot start expensive work using an admin cookie.
    if request.method == "GET" and not (app.testing and app.config.get("PODCAST_TEST_AUTH_DISABLED")):
        return jsonify({"error": "请在网页中点击刷新订阅源"}), 405
    global _refresh_thread
    with _refresh_job_lock:
        if _refresh_thread is None or not _refresh_thread.is_alive():
            job_id = uuid.uuid4().hex
            _manual_refresh.update(job_id=job_id, status="running", results={},
                                   started_at=int(time.time()), finished_at=None, error="")
            _refresh_thread = threading.Thread(target=manual_refresh, args=(job_id,), daemon=True)
            try:
                _refresh_thread.start()
            except RuntimeError:
                _manual_refresh.update(status="failed", finished_at=int(time.time()),
                                       error="目录刷新任务无法启动，请稍后重试")
                return jsonify(dict(_manual_refresh)), 503
        snapshot = dict(_manual_refresh)
    return jsonify({"job_id": snapshot["job_id"], "status": snapshot["status"]}), 202


def manual_refresh(job_id):
    results, status, error = {}, "failed", "目录刷新任务失败，请稍后重试"
    try:
        results = refresh_all()
        status, error = "complete", ""
    except Exception:
        # 每源失败仍在 results 中；任务本身异常与完成后的每源结果分开。
        pass
    finally:
        with _refresh_job_lock:
            if _manual_refresh["job_id"] == job_id:
                _manual_refresh.update(status=status, results=results, error=error,
                                       finished_at=int(time.time()))


@app.get("/api/refresh/status")
def api_refresh_status():
    with _refresh_job_lock:
        snapshot = dict(_manual_refresh)
    return jsonify(snapshot)


@app.get("/media/<show_id>/<ep_id>/<path:filename>")
def media(show_id, ep_id, filename):
    if (not known_show(show_id) or not re.fullmatch(r"[A-Za-z0-9_-]{1,96}", ep_id)
            or not re.fullmatch(r"seg_[0-9]{3}\.mp3", filename)):
        return "bad path", 400
    f = ep_dir(show_id, ep_id) / filename
    if not f.is_file():
        return "not found", 404
    return send_file(f, mimetype="audio/mpeg", conditional=True)


@app.get("/pcm/<show_id>/<ep_id>/<filename>")
def pcm(show_id, ep_id, filename):
    """播放路径只发送完整缓存；不把等待转码塞进音频请求。"""
    try:
        if filename == "stream.pcm":
            pcm_paths(show_id, ep_id, "seg_000.pcm")
            return pcm_episode_stream(show_id, ep_id)
        _, _, target = pcm_paths(show_id, ep_id, filename)
        if not prepared_segment_sources(show_id, ep_id, get_state().get(f"{show_id}/{ep_id}", {})):
            return pcm_not_ready()
    except PCMPathError:
        return jsonify({"error": "bad audio path"}), 400
    except FileNotFoundError:
        return jsonify({"error": "segment not found"}), 404
    except (OSError, RuntimeError):
        return jsonify({"error": "audio cache unavailable"}), 503
    response = send_file(target, mimetype="application/octet-stream", conditional=True)
    return audio_format_headers(response)


def audio_format_headers(response):
    response.headers["X-Audio-Sample-Rate"] = str(PCM_SAMPLE_RATE)
    response.headers["X-Audio-Channels"] = str(PCM_CHANNELS)
    response.headers["X-Audio-Bits"] = str(PCM_BITS)
    response.headers["X-Audio-Format"] = "s16le"
    return response


def pcm_not_ready():
    response = jsonify({"error": "audio is not prepared; request /api/prepare first"})
    response.status_code = 503
    response.headers["Retry-After"] = "2"
    return response


def pcm_episode_stream(show_id, ep_id):
    sources = prepared_segment_sources(show_id, ep_id, get_state().get(f"{show_id}/{ep_id}", {}))
    if not sources:
        return pcm_not_ready()
    targets = [pcm_paths(show_id, ep_id, f"seg_{index:03d}.pcm")[2] for index in range(len(sources))]
    lengths = [target.stat().st_size for target in targets]
    lengths_header = ",".join(str(size) for size in lengths)
    if len(lengths_header) > 3072:
        return jsonify({"error": "audio segment header too large"}), 503
    total = sum(lengths)
    start, end, status = 0, total - 1, 200
    if request.headers.get("Range"):
        range_text = request.headers["Range"]
        match = re.fullmatch(r"bytes=([0-9]*)-([0-9]*)", range_text) if len(range_text) <= 128 else None
        valid = bool(match and (match[1] or match[2]))
        if valid:
            if not match[1]:
                suffix = int(match[2])
                valid = suffix > 0
                start = max(0, total - suffix)
            else:
                start = int(match[1])
                end = min(total - 1, int(match[2])) if match[2] else total - 1
                valid = start <= end and start < total
        if not valid:
            response = Response(status=416)
            response.headers["Content-Range"] = f"bytes */{total}"
            return response
        status = 206

    def chunks():
        offset, remaining = start, end - start + 1
        for target, length in zip(targets, lengths):
            if offset >= length:
                offset -= length
                continue
            with target.open("rb") as file:
                file.seek(offset)
                in_file = min(length - offset, remaining)
                while in_file:
                    chunk = file.read(min(65536, in_file))
                    if not chunk:
                        raise OSError("prepared PCM was truncated during playback")
                    yield chunk
                    in_file -= len(chunk)
                    remaining -= len(chunk)
            offset = 0
            if not remaining:
                return

    response = Response(chunks() if request.method != "HEAD" else b"", status=status,
                        mimetype="application/octet-stream", direct_passthrough=True)
    response.headers["Content-Length"] = str(end - start + 1)
    response.headers["Accept-Ranges"] = "bytes"
    response.headers["Cache-Control"] = "private, no-cache"
    response.headers["X-Audio-Segment-Lengths"] = lengths_header
    response.headers["X-Audio-Segment-Count"] = str(len(lengths))
    if status == 206:
        response.headers["Content-Range"] = f"bytes {start}-{end}/{total}"
    return audio_format_headers(response)


def artwork_url(show_id):
    if not device_cover_id(show_id):
        raise ListeningError("节目编号格式不正确", 400)
    if not known_show(show_id):
        raise ListeningError("没有找到这个节目", 404)
    url = load_json(EP_CACHE, {}).get(show_id, {}).get("art", "")
    if not isinstance(url, str) or not url:
        raise ListeningError("这个节目暂时没有封面", 404)
    return url


def artwork_pending():
    response = compact_json({"status": "preparing"})
    response.status_code = 202
    response.headers["Retry-After"] = "2"
    response.headers["Cache-Control"] = "no-store"
    return response


@app.get("/art/<show_id>/device.bin")
def device_art(show_id):
    """The hardware only receives its backend's validated fixed-size pixels."""
    blob = DEVICE_COVERS.snapshot(show_id, artwork_url(show_id))
    if blob is None:
        return artwork_pending()
    response = Response(blob, mimetype="application/octet-stream")
    response.set_etag(blob[16:32].hex())
    response.headers["Cache-Control"] = "private, no-cache"
    return response.make_conditional(request)


@app.get("/art/<show_id>")
def art(show_id):
    """Reuse background artwork; never wait for external image HTTP here."""
    p = DEVICE_COVERS.original(show_id, artwork_url(show_id))
    if p is None:
        return artwork_pending()

    # 文件名没有扩展名，send_file 猜不出类型，这里按真实内容判断
    head = p.read_bytes()[:16]
    if head.startswith(b"\xff\xd8\xff"):
        mt = "image/jpeg"
    elif head.startswith(b"\x89PNG\r\n\x1a\n"):
        mt = "image/png"
    elif head[:6] in (b"GIF87a", b"GIF89a"):
        mt = "image/gif"
    elif head[:4] == b"RIFF" and head[8:12] == b"WEBP":
        mt = "image/webp"
    else:
        mt = "image/jpeg"
    return send_file(p, mimetype=mt, conditional=True, max_age=0)


@app.get("/healthz")
@app.get("/api/health")
def healthz():
    return jsonify({"ok": True, "shows": len(configured_shows()), "history": "shared",
                    "release": RELEASE, "authentication": "required",
                    "time": time.strftime("%Y-%m-%d %H:%M:%S")})


@app.get("/feeds/xiaoyuzhou/<pid>.xml")
def public_podcast_feed(pid):
    configured_shows()
    if pid not in _public_feeds.programs:
        return jsonify({"error": "未配置此公开节目"}), 404
    try:
        xml, stale = _public_feeds.get(pid)
        return Response(xml, mimetype="application/rss+xml",
                        headers={"Cache-Control": "max-age=300", "X-Feed-Stale": str(stale).lower()})
    except Exception:
        return jsonify({"error": "公开节目页暂时无法读取，请稍后刷新"}), 502


# ---------------------------------------------------------------- 网页界面

@app.get("/")
def index():
    return send_from_directory(BASE / "static", "index.html")


@app.get("/api/recommendations")
def api_recommendations():
    return jsonify({"error": "固定推荐入口已撤下，请通过添加节目搜索苹果播客公开目录"}), 410


@app.get("/subscriptions.opml")
def subscriptions_opml():
    """Export the actual subscription registry, including durable user-added sources."""
    def xml_text(value):
        # OPML uses XML 1.0; RSS titles can contain invalid control characters.
        return "".join(character for character in str(value or "")
                       if character in "\t\n\r" or 0x20 <= ord(character) <= 0xD7FF
                       or 0xE000 <= ord(character) <= 0xFFFD or 0x10000 <= ord(character) <= 0x10FFFF)

    cache = load_json(EP_CACHE, {})
    document = ET.Element("opml", version="2.0")
    head = ET.SubElement(document, "head")
    ET.SubElement(head, "title").text = "已订阅播客清单"
    body = ET.SubElement(document, "body")
    for source in configured_shows():
        cached = cache.get(source["id"], {})
        feed = xml_text(source.get("feed"))
        if not feed:
            continue
        name = xml_text(cached.get("name") or source.get("name") or source["id"])
        attributes = {"type": "rss", "text": name, "title": name,
                      "xmlUrl": urljoin(service_origin(), feed)}
        website = xml_text(source.get("source_url") or cached.get("link"))
        if website:
            attributes["htmlUrl"] = urljoin(service_origin(), website)
        ET.SubElement(body, "outline", attributes)
    response = Response(ET.tostring(document, encoding="utf-8", xml_declaration=True),
                        mimetype="application/xml")
    response.headers["Content-Disposition"] = "attachment; filename=subscriptions.opml; filename*=UTF-8''" + quote("已订阅播客清单.opml")
    response.headers["Cache-Control"] = "no-store"
    return response




# ---------------------------------------------------------------- 启动

def start_background():
    """后台定时抓取。

    放在模块加载时启动，而不是 __main__ 里，
    这样用 gunicorn 启动时定时任务照样会跑。
    """
    def loop():
        while True:
            time.sleep(CFG.get("refresh_minutes", 180) * 60)
            try:
                refresh_all()
            except Exception:
                pass

    def initial():
        recover_prepared_episodes()
        queue_latest_episodes()  # 已有目录立即用于预缓存，源刷新失败不影响收听。
        refresh_all()

    threading.Thread(target=loop, daemon=True).start()
    threading.Thread(target=initial, daemon=True).start()
    # 缓存清理收进服务内部：保护名单依赖 state 与收听库，外部 cron 脚本看不到。
    threading.Thread(target=cleanup_loop, daemon=True).start()


if not os.environ.get("PODCAST_NO_BG"):
    start_background()

if __name__ == "__main__":
    if "--refresh" in os.sys.argv:
        print(json.dumps(refresh_all(), ensure_ascii=False, indent=1))
    else:
        print("播客中转站启动 → http://0.0.0.0:8899  (局域网访问)")
        app.run(host="0.0.0.0", port=8899, threaded=True)
