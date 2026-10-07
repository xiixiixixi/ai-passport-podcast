"""Turn two verified public podcast pages into local subscription feeds."""
import json
import os
import tempfile
import threading
import time
import urllib.request
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from email.utils import format_datetime, parsedate_to_datetime
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlsplit

PROGRAMS = {
    "65bb55f6513a776b57dedb32": {
        "name": "李诞", "author": "李诞、袁袁",
        "legacy": "https://feed.xyzfm.space/l3c8em3l8hfn",
    },
    "65f02690587b754dbe358a7d": {
        "name": "微信公开TALK", "author": "微信公开课", "legacy": None,
    },
}
ITUNES = "http://www.itunes.com/dtds/podcast-1.0.dtd"
ET.register_namespace("itunes", ITUNES)


class PublicFeedError(ValueError):
    pass


class _PageData(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=False)
        self.active = False
        self.parts = []

    def handle_starttag(self, tag, attrs):
        if tag == "script" and dict(attrs).get("id") == "__NEXT_DATA__":
            self.active = True

    def handle_endtag(self, tag):
        if tag == "script":
            self.active = False

    def handle_data(self, text):
        if self.active:
            self.parts.append(text)


def podcast_from_page(html, pid):
    parser = _PageData()
    parser.feed(html)
    try:
        podcast = json.loads("".join(parser.parts))["props"]["pageProps"]["podcast"]
    except (KeyError, TypeError, ValueError) as error:
        raise PublicFeedError("公开节目页格式已变化") from error
    if not isinstance(podcast, dict) or podcast.get("pid") != pid:
        raise PublicFeedError("公开页节目编号不匹配")
    return podcast


def _media_url(value):
    if not isinstance(value, str):
        return None
    try:
        url = urlsplit(value)
    except ValueError:
        return None
    if url.scheme != "https" or url.hostname != "media.xyzcdn.net" or url.username or url.password:
        return None
    return value


def _episode_key(value):
    if not isinstance(value, str):
        return None
    try:
        url = urlsplit(value)
    except ValueError:
        return None
    key = url.path.removeprefix("/episode/") if url.hostname == "www.xiaoyuzhoufm.com" else value
    return key if len(key) == 24 and all(c in "0123456789abcdef" for c in key) else None


def _legacy_media_url(value, pid):
    if _media_url(value):
        return value
    if not isinstance(value, str):
        return None
    try:
        url = urlsplit(value)
    except ValueError:
        return None
    prefix = "/track/" + pid + "/"
    if (url.scheme != "https" or url.hostname != "dts-api.xiaoyuzhoufm.com"
            or url.username or url.password or not url.path.startswith(prefix)):
        return None
    suffix = url.path[len(prefix):].split("/", 1)
    if len(suffix) != 2 or not _episode_key(suffix[0]) or not _media_url("https://" + suffix[1]):
        return None
    return value


def public_episodes(podcast, pid, program=None):
    if (program is None and pid not in PROGRAMS) or podcast.get("pid") != pid:
        raise PublicFeedError("节目不在已核验的公开源中")
    entries = podcast.get("episodes")
    if not isinstance(entries, list):
        raise PublicFeedError("公开单集列表格式已变化")
    episodes = []
    for episode in entries:
        if not isinstance(episode, dict):
            continue
        media = episode.get("media") or {}
        if not isinstance(media, dict):
            continue
        source = media.get("source") or {}
        enclosure = episode.get("enclosure") or {}
        if not isinstance(source, dict) or not isinstance(enclosure, dict):
            continue
        audio = _media_url(source.get("url"))
        if (episode.get("pid") != pid or episode.get("payType") != "FREE"
                or episode.get("status") != "NORMAL" or source.get("mode") != "PUBLIC"
                or episode.get("isPrivateMedia") or episode.get("isPreview")
                or episode.get("isTrial") or not audio or enclosure.get("url") != audio):
            continue
        eid = episode.get("eid", "")
        if not isinstance(eid, str) or len(eid) != 24 or any(c not in "0123456789abcdef" for c in eid):
            continue
        try:
            published = datetime.fromisoformat(episode["pubDate"].replace("Z", "+00:00"))
            duration = int(episode.get("duration") or 0)
        except (AttributeError, KeyError, TypeError, ValueError):
            continue
        if published.tzinfo is None or duration <= 0:
            continue
        episodes.append((episode, published, audio, duration))
    return episodes


def build_rss(podcast, pid, legacy_xml=None, program=None):
    program = program or PROGRAMS[pid]
    episodes = public_episodes(podcast, pid, program)
    eligible_ids = {entry[0]["eid"] for entry in episodes}
    rejected_ids, rejected_audio = set(), set()
    for entry in podcast["episodes"]:
        if not isinstance(entry, dict) or entry.get("eid") in eligible_ids:
            continue
        key = _episode_key(entry.get("eid"))
        if key:
            rejected_ids.add(key)
        media = entry.get("media")
        source = media.get("source") if isinstance(media, dict) else None
        enclosure = entry.get("enclosure")
        for value in (source.get("url") if isinstance(source, dict) else None,
                      enclosure.get("url") if isinstance(enclosure, dict) else None):
            if isinstance(value, str):
                rejected_audio.add(value)
    root = ET.Element("rss", version="2.0")
    channel = ET.SubElement(root, "channel")
    page = "https://www.xiaoyuzhoufm.com/podcast/" + pid
    ET.SubElement(channel, "title").text = podcast.get("title") or program["name"]
    ET.SubElement(channel, "link").text = page
    ET.SubElement(channel, "description").text = podcast.get("description") or "公开播客单集"
    ET.SubElement(channel, "language").text = "zh-cn"
    ET.SubElement(channel, "{" + ITUNES + "}author").text = program["author"]
    image = (podcast.get("image") or {}).get("picUrl")
    if image and urlsplit(image).scheme == "https":
        ET.SubElement(channel, "{" + ITUNES + "}image", href=image)
        cover = ET.SubElement(channel, "image")
        ET.SubElement(cover, "url").text = image
        ET.SubElement(cover, "title").text = channel.findtext("title")
        ET.SubElement(cover, "link").text = page
    items = []
    seen_ids, seen_audio = set(), set()
    for episode, published, audio, duration in episodes:
        item = ET.Element("item")
        link = "https://www.xiaoyuzhoufm.com/episode/" + episode["eid"]
        ET.SubElement(item, "title").text = episode.get("title") or "公开单集"
        ET.SubElement(item, "link").text = link
        ET.SubElement(item, "guid", isPermaLink="true").text = link
        ET.SubElement(item, "pubDate").text = format_datetime(published.astimezone(timezone.utc), usegmt=True)
        ET.SubElement(item, "{" + ITUNES + "}duration").text = str(duration)
        media = episode["media"]
        try:
            size = max(0, int(media.get("size") or 0))
        except (TypeError, ValueError):
            size = 0
        ET.SubElement(item, "enclosure", url=audio,
                      length=str(size),
                      type=media.get("mimeType") or "audio/mp4")
        items.append(item)
        seen_ids.add(episode["eid"])
        seen_audio.add(audio)
    history_feeds = legacy_xml if isinstance(legacy_xml, (tuple, list)) else [legacy_xml]
    for history_xml in history_feeds:
        if not history_xml:
            continue
        try:
            legacy = ET.fromstring(history_xml).find("channel")
        except ET.ParseError as error:
            raise PublicFeedError("历史订阅源格式无效") from error
        if legacy is None or legacy.findtext("title", "").strip() != program["name"]:
            raise PublicFeedError("历史订阅源身份不匹配")
        for item in legacy.findall("item"):
            enclosure = item.find("enclosure")
            if enclosure is None or not _legacy_media_url(enclosure.get("url"), pid):
                continue
            audio = enclosure.get("url")
            keys = {_episode_key(item.findtext("link")), _episode_key(item.findtext("guid"))} - {None}
            if keys.intersection(rejected_ids) or audio in rejected_audio:
                continue
            if audio in seen_audio or keys.intersection(seen_ids):
                continue
            items.append(item)
            seen_audio.add(audio)
            seen_ids.update(keys)

    def date(item):
        try:
            return parsedate_to_datetime(item.findtext("pubDate")).timestamp()
        except (TypeError, ValueError, OverflowError):
            return 0

    channel.extend(sorted(items, key=date, reverse=True))
    return ET.tostring(root, encoding="utf-8", xml_declaration=True)


class PublicFeedAdapter:
    def __init__(self, cache_directory, headers, ttl=300):
        self.cache_directory = Path(cache_directory)
        self.headers = dict(headers)
        self.ttl = ttl
        self.programs = dict(PROGRAMS)
        self.locks = {pid: threading.Lock() for pid in PROGRAMS}
        self.registry_lock = threading.Lock()

    def add_program(self, pid, name, author=""):
        if not isinstance(pid, str) or len(pid) != 24 or any(c not in "0123456789abcdef" for c in pid):
            raise PublicFeedError("节目编号格式不正确")
        with self.registry_lock:
            self.programs.setdefault(pid, {"name": str(name), "author": str(author), "legacy": None})
            self.locks.setdefault(pid, threading.Lock())

    def _read(self, url, max_bytes):
        request = urllib.request.Request(url, headers=self.headers)
        with urllib.request.urlopen(request, timeout=25) as response:
            content = response.read(max_bytes + 1)
        if len(content) > max_bytes:
            raise PublicFeedError("节目元信息超过读取上限")
        return content

    def get(self, pid):
        if pid not in self.programs:
            raise PublicFeedError("未配置此公开节目")
        with self.locks[pid]:
            path = self.cache_directory / (pid + ".json")
            try:
                cached = json.loads(path.read_text())
            except (OSError, ValueError):
                cached = {}
            if not isinstance(cached, dict):
                cached = {}
            if not isinstance(cached.get("updated"), (int, float)) or not isinstance(cached.get("xml"), str):
                cached = {}
            if time.time() - cached.get("updated", 0) < self.ttl and cached.get("xml"):
                return cached["xml"].encode(), False
            try:
                html = self._read("https://www.xiaoyuzhoufm.com/podcast/" + pid, 4 * 1024 * 1024).decode("utf-8")
                podcast = podcast_from_page(html, pid)
                history = [cached["xml"]] if cached.get("xml") else []
                program = self.programs[pid]
                if program["legacy"]:
                    try:
                        history.append(self._read(program["legacy"], 12 * 1024 * 1024))
                    except Exception:
                        # Keep already-known history during a temporary origin failure.
                        if not history:
                            raise
                xml = build_rss(podcast, pid, history, program=program)
            except Exception:
                if cached.get("xml"):
                    return cached["xml"].encode(), True
                raise
            self.cache_directory.mkdir(parents=True, exist_ok=True)
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=self.cache_directory, delete=False) as file:
                    temporary = Path(file.name)
                    json.dump({"updated": time.time(), "xml": xml.decode()}, file, ensure_ascii=False)
                os.chmod(temporary, 0o600)
                temporary.replace(path)
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)
            return xml, False
