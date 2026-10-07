"""Resolve public RSS, Apple podcast directory links, and public Xiaoyuzhou pages.

External HTTP connections are pinned to a checked public IP. Redirects get the same
checks. This module does not use private app APIs or claim to provide a recommendation SDK.
"""
import hashlib
import http.client
import ipaddress
import json
import re
import socket
import ssl
import threading
import time
from urllib.parse import urlencode, urljoin, urlsplit, urlunsplit, parse_qs, quote

import feedparser
from listening_store import ListeningError
from public_feeds import podcast_from_page, build_rss, PROGRAMS

HEADERS = {"User-Agent": "Mozilla/5.0 PodcastRelay/2.0", "Accept": "*/*", "Accept-Encoding": "identity"}
MAX_FEED_BYTES = 12 * 1024 * 1024
_search_lock = threading.Lock()
_search_cache = {}
_apple_requests = []


def public_url(value):
    if not isinstance(value, str) or not value.strip() or len(value) > 4096:
        raise ListeningError("请粘贴完整的公开节目链接或订阅地址")
    value = value.strip()
    if any(ord(character) < 32 or character == "\\" for character in value):
        raise ListeningError("链接包含无效字符")
    try:
        parsed = urlsplit(value)
        port = parsed.port
    except ValueError as error:
        raise ListeningError("链接格式不正确") from error
    if (parsed.scheme not in {"http", "https"} or not parsed.hostname or parsed.username or parsed.password
            or port not in {None, 80, 443}):
        raise ListeningError("只支持公开的 http 或 https 地址，不支持登录凭据或本地地址")
    hostname = parsed.hostname.lower().rstrip(".")
    if hostname in {"localhost", "localhost.localdomain"} or hostname.endswith((".local", ".localhost", ".internal")):
        raise ListeningError("不能添加本机或局域网地址")
    try:
        address = ipaddress.ip_address(hostname)
    except ValueError:
        address = None
    if address and (not address.is_global or address.is_multicast):
        raise ListeningError("不能添加本机或局域网地址")
    try:
        hostname = hostname.encode("idna").decode("ascii")
    except UnicodeError as error:
        raise ListeningError("节目网址的域名格式不正确") from error
    netloc = "[" + hostname + "]" if ":" in hostname else hostname
    if port and port != (443 if parsed.scheme == "https" else 80):
        netloc += ":" + str(port)
    return urlunsplit((parsed.scheme, netloc, quote(parsed.path or "/", safe="/%:@!$&'()*+,;=-._~"),
                       quote(parsed.query, safe="=&%/:?@!$'()*+,;=-._~"), ""))


def open_public(value, timeout=15, redirects=4):
    """Return (response, connection, resolved URL); callers must close both objects."""
    url = public_url(value)
    for hop in range(redirects + 1):
        parsed = urlsplit(url)
        port = parsed.port or (443 if parsed.scheme == "https" else 80)
        try:
            addresses = socket.getaddrinfo(parsed.hostname, port, type=socket.SOCK_STREAM)
        except OSError as error:
            raise ListeningError("无法解析节目地址，请检查链接或稍后重试", 502) from error
        ips = {result[4][0] for result in addresses}
        if not ips or any(not ipaddress.ip_address(ip).is_global or ipaddress.ip_address(ip).is_multicast for ip in ips):
            raise ListeningError("该链接指向本机或局域网，无法作为公开订阅")
        pinned_ip = sorted(ips, key=lambda ip: (ipaddress.ip_address(ip).version, ip))[0]
        connection = (http.client.HTTPSConnection(parsed.hostname, port, timeout=timeout, context=ssl.create_default_context())
                      if parsed.scheme == "https" else http.client.HTTPConnection(parsed.hostname, port, timeout=timeout))
        connection._create_connection = lambda address, timeout=timeout, source_address=None, ip=pinned_ip, target_port=port: socket.create_connection((ip, target_port), timeout, source_address)
        try:
            connection.request("GET", parsed.path + ("?" + parsed.query if parsed.query else ""), headers=HEADERS)
            response = connection.getresponse()
        except (OSError, http.client.HTTPException) as error:
            connection.close()
            raise ListeningError("节目网站暂时无法读取，请稍后重试", 502) from error
        if response.status in {301, 302, 303, 307, 308}:
            location = response.getheader("Location")
            response.close()
            connection.close()
            if not location or hop == redirects:
                raise ListeningError("节目链接重定向过多或无效", 502)
            url = public_url(urljoin(url, location))
            continue
        if response.status != 200:
            status = response.status
            response.close()
            connection.close()
            raise ListeningError(f"节目网站返回 {status}，请检查链接或稍后重试", 502)
        return response, connection, url
    raise ListeningError("无法读取节目链接", 502)


def read_public(url, maximum=MAX_FEED_BYTES):
    response, connection, final_url = open_public(url)
    try:
        length = response.getheader("Content-Length")
        if length and length.isdecimal() and int(length) > maximum:
            raise ListeningError("节目元信息过大，请使用节目官方订阅地址")
        content = response.read(maximum + 1)
        if len(content) > maximum:
            raise ListeningError("节目元信息过大，请使用节目官方订阅地址")
        if response.getheader("Content-Encoding") not in {None, "identity"}:
            raise ListeningError("节目网站返回了不支持的压缩格式，请稍后重试", 502)
        return content, final_url
    except (OSError, http.client.HTTPException) as error:
        raise ListeningError("节目读取中断，请稍后重试", 502) from error
    finally:
        response.close()
        connection.close()


def _duration(value):
    try:
        parts = [int(part) for part in str(value or "0").split(":")]
        result = 0
        for part in parts:
            result = result * 60 + part
        return max(0, min(31536000, result))
    except (TypeError, ValueError):
        return 0


def parse_feed(raw, fallback="播客"):
    parsed = feedparser.parse(raw)
    if not parsed.feed.get("title") or not parsed.entries:
        raise ListeningError("没有找到可播放的播客订阅，请粘贴 RSS（订阅源）或节目主页链接")
    episodes = []
    for entry in parsed.entries:
        audio = next((link.get("href") for link in entry.get("links", []) if link.get("type", "").startswith("audio")), None)
        if not audio:
            audio = next((enclosure.get("href") for enclosure in entry.get("enclosures", []) if enclosure.get("href")), None)
        if not audio:
            continue
        try:
            audio = public_url(audio)
        except ListeningError:
            continue
        identity = str(entry.get("id") or entry.get("link") or audio)
        eid = re.sub(r"[^a-zA-Z0-9]", "", identity)[-40:] or hashlib.sha256(identity.encode()).hexdigest()[:40]
        published = ""
        for field in ("published_parsed", "updated_parsed", "created_parsed"):
            if entry.get(field):
                published = time.strftime("%Y-%m-%dT%H:%M:%S", entry[field]) + "+00:00"
                break
        episodes.append({"id": eid, "title": str(entry.get("title") or "未命名单集").strip(), "published": published,
                         "duration": _duration(entry.get("itunes_duration") or entry.get("duration")), "audio": audio,
                         "type": (entry.get("enclosures") or [{}])[0].get("type", ""),
                         "desc": re.sub(r"<[^>]+>", "", str(entry.get("summary") or ""))[:400]})
    if not episodes:
        raise ListeningError("订阅中没有可公开播放的音频；暂不支持需要登录或付费的节目")
    # Repeated RSS entries must not create duplicate episode rows or replay loops.
    episodes = list({episode["id"]: episode for episode in episodes}.values())
    episodes.sort(key=lambda episode: episode["published"], reverse=True)
    art = ""
    for key in ("image", "itunes_image"):
        image = parsed.feed.get(key)
        if isinstance(image, dict):
            candidate = image.get("href") or image.get("url")
            if isinstance(candidate, str):
                try:
                    art = public_url(candidate)
                    break
                except ListeningError:
                    pass
    show = {"name": str(parsed.feed.get("title") or fallback), "author": str(parsed.feed.get("author") or ""),
            "art": art, "episodes": len(episodes), "link": str(parsed.feed.get("link") or ""), "latest": episodes[0]}
    return show, episodes


def _apple_json(url):
    with _search_lock:
        now = time.time()
        _apple_requests[:] = [stamp for stamp in _apple_requests if now - stamp < 60]
        if len(_apple_requests) >= 18:
            raise ListeningError("苹果播客查询较频繁，请稍等一分钟重试", 429)
        _apple_requests.append(now)
    raw, _ = read_public(url, 2 * 1024 * 1024)
    try:
        result = json.loads(raw)
        if not isinstance(result, dict) or not isinstance(result.get("results"), list):
            raise ValueError
        return result["results"]
    except (TypeError, ValueError) as error:
        raise ListeningError("苹果播客目录暂时无法解析，请稍后重试", 502) from error


def resolve_source(value):
    source_url = public_url(value)
    parsed = urlsplit(source_url)
    hostname = parsed.hostname.lower()
    source_type, public_pid, apple_id, warning = "rss", None, None, ""
    requested_feed_url = source_url
    if hostname == "podcasts.apple.com":
        if "i" in parse_qs(parsed.query):
            raise ListeningError("这是苹果播客单集链接，请进入节目主页，再分享节目链接")
        match = re.search(r"(?:^|/)id([0-9]{1,20})(?:/|$)", parsed.path)
        if not match or "/podcast/" not in parsed.path:
            raise ListeningError("请使用苹果播客的节目主页分享链接")
        region = parsed.path.strip("/").split("/", 1)[0]
        apple_id = match[1]
        country = region.upper() if re.fullmatch(r"[A-Za-z]{2}", region) else "US"
        rows = _apple_json("https://itunes.apple.com/lookup?" + urlencode({"id": match[1], "entity": "podcast", "country": country}))
        row = next((item for item in rows if str(item.get("collectionId")) == match[1] and item.get("feedUrl")), None)
        if not row:
            raise ListeningError("苹果播客未提供这个节目的公开订阅源，请尝试节目官网 RSS（订阅源）", 422)
        feed_url, source_type = public_url(row["feedUrl"]), "apple"
        requested_feed_url = feed_url
        raw, feed_url = read_public(feed_url)
    elif hostname in {"www.xiaoyuzhoufm.com", "xiaoyuzhoufm.com"}:
        match = re.fullmatch(r"/podcast/([a-f0-9]{24})/?", parsed.path)
        if not match:
            raise ListeningError("请分享小宇宙的播客节目主页（/podcast/），暂不支持单集链接（/episode/）")
        public_pid, source_type = match[1], "xiaoyuzhou"
        source_url = "https://www.xiaoyuzhoufm.com/podcast/" + public_pid
        raw, _ = read_public(source_url, 4 * 1024 * 1024)
        try:
            podcast = podcast_from_page(raw.decode("utf-8"), public_pid)
            program = {"name": str(podcast.get("title") or "播客"), "author": str(podcast.get("author") or ""), "legacy": None}
            raw = build_rss(podcast, public_pid, program=program)
        except (UnicodeError, ValueError, KeyError, TypeError) as error:
            raise ListeningError("小宇宙公开页格式无法读取，请尝试节目官方 RSS（订阅源）", 422) from error
        feed_url = "/feeds/xiaoyuzhou/" + public_pid + ".xml"
        requested_feed_url = feed_url
        warning = "小宇宙公开页面只提供当前可见的免费单集，历史集数可能不完整；服务会保留以后抓到的公开单集。可取得官方 RSS（订阅源）时优先使用它。"
    else:
        raw, feed_url = read_public(source_url)
    show, episodes = parse_feed(raw)
    canonical = "xiaoyuzhou:" + public_pid if public_pid else feed_url
    return {"source_type": source_type, "source_url": source_url, "feed_url": feed_url,
            "public_pid": public_pid, "apple_id": apple_id, "canonical": canonical, "show": show, "warning": warning,
            "requested_feed_url": requested_feed_url,
            "episodes": episodes, "raw_feed": raw.decode("utf-8", errors="replace") if public_pid else None}


def search_apple(query):
    if not isinstance(query, str) or not 1 <= len(query.strip()) <= 80:
        raise ListeningError("请输入 1 到 80 个字的节目名称")
    query = query.strip()
    with _search_lock:
        cached = _search_cache.get(query)
        if cached and time.time() - cached[0] < 300:
            return cached[1]
    rows = _apple_json("https://itunes.apple.com/search?" + urlencode({"term": query, "media": "podcast", "entity": "podcast", "country": "CN", "limit": 15}))
    results = []
    for row in rows:
        if not row.get("feedUrl") or not row.get("collectionViewUrl"):
            continue
        try:
            url = public_url(row["collectionViewUrl"])
            if urlsplit(url).hostname != "podcasts.apple.com":
                continue
        except ListeningError:
            continue
        try:
            count = max(0, int(row.get("trackCount") or 0))
        except (TypeError, ValueError, OverflowError):
            count = 0
        results.append({"name": str(row.get("collectionName") or "播客"), "author": str(row.get("artistName") or ""),
                        "art": str(row.get("artworkUrl600") or row.get("artworkUrl100") or ""),
                        "url": url, "episodes": count})
    with _search_lock:
        if len(_search_cache) >= 64:
            _search_cache.pop(next(iter(_search_cache)))
        _search_cache[query] = (time.time(), results)
    return results
