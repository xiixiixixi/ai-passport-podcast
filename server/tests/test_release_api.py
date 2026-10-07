"""Real cached audio and shared HTTP contracts, isolated from household listening data."""
import copy
import json
import io
import shutil
import subprocess
import unittest
import xml.etree.ElementTree as ET
from urllib.parse import unquote, urljoin
from pathlib import Path
from unittest import mock

import test_pcm
import source_manager
from listening_store import ListeningStore, ListeningError
from public_feeds import PublicFeedAdapter

RSS = b'''<?xml version="1.0"?><rss version="2.0" xmlns:itunes="http://www.itunes.com/dtds/podcast-1.0.dtd"><channel><title>New podcast</title><itunes:author>Author</itunes:author><item><guid>episode1</guid><title>Latest episode</title><pubDate>Sun, 04 Oct 2026 00:00:00 GMT</pubDate><itunes:duration>100</itunes:duration><enclosure url="https://example.org/audio.mp3" type="audio/mpeg" length="10000"/></item></channel></rss>'''


@unittest.skipUnless(shutil.which("ffmpeg"), "ffmpeg is required for audio integration tests")
class ReleaseAPITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_pcm.PCMEndpointTests.setUpClass.__func__(cls)

    @classmethod
    def tearDownClass(cls):
        test_pcm.PCMEndpointTests.tearDownClass.__func__(cls)

    def setUp(self):
        test_pcm.PCMEndpointTests.setUp(self)
        self.now = 1800000000.0
        self.server.LISTENING = ListeningStore(self.root / "history.sqlite3", clock=lambda: self.now)
        self.server._public_feeds = PublicFeedAdapter(self.root / "public-feeds", {}, ttl=300)
        self.server._prepare_jobs.clear()
        self.server.save_json(self.server.EP_CACHE, {"fixture": {"id": "fixture", "name": "中文节目", "count": 2,
                                      "episodes": [{"id": "episode1", "title": "第一集", "duration": 100, "published": "2026-10-04T00:00:00Z", "audio": "https://example.org/one.mp3"},
                                                   {"id": "episode2", "title": "第二集", "duration": 100, "published": "2026-10-03T00:00:00Z", "audio": "https://example.org/two.mp3"}]}})

    request = test_pcm.PCMEndpointTests.request
    prepare = test_pcm.PCMEndpointTests.prepare

    def start(self):
        response = self.request("/api/listening/sessions", method="POST", json={"client_id": "web", "request_id": "start1", "show_id": "fixture", "episode_id": "episode1"})
        self.assertEqual(response.status_code, 200)
        return response.json

    def test_shared_marks_recent_stats_and_compact_device_contract(self):
        session = self.start()
        self.now += 10
        response = self.request("/api/listening/events", method="POST", json={"session_id": session["session_id"], "seq": 1, "position_ms": 10000, "listened_ms": 10000, "state": "paused"})
        self.assertEqual(response.status_code, 200)
        detail = self.request("/api/episodes/fixture/episode1").json
        self.assertEqual(detail["progress"]["status"], "in_progress")
        lite = self.request("/api/episodes/fixture/episode1?lite=1").json
        self.assertEqual((lite["p"], lite["l"], lite["c"]), (10000, 10000, 1))
        self.assertNotIn("progress", lite)
        recent = self.request("/api/listening/recent?limit=1&lite=1").json["s"][0]
        self.assertEqual((recent["i"], recent["e"], recent["p"]), ("fixture", "episode1", 10000))
        page = self.request("/api/shows/fixture/episodes?status=unplayed&lite=1").json
        self.assertEqual((page["total"], page["episodes"][0]["id"], page["episodes"][0]["c"]), (1, "episode2", 0))
        show = self.request("/api/shows_lite").json["s"][0]
        self.assertEqual((show["l"], show["u"], show["c"]), (10000, 1, 0))
        stats = self.request("/api/listening/stats").json
        self.assertEqual(stats["timezone"], "Asia/Shanghai")
        self.assertEqual(stats["summary"]["listened_ms"], 10000)
        mark = self.request("/api/listening/mark", method="POST", json={"show_id": "fixture", "episode_id": "episode1", "status": "completed"})
        self.assertEqual(mark.json["progress"]["listened_ms"], 10000)

    def test_legacy_http_import_and_web_page_preserve_full_metadata_without_fabricating_stats(self):
        payload = {"client_id": "web", "request_id": "legacy1", "episodes": [{"show_id": "fixture", "episode_id": "episode2", "position_ms": 25000},
                                                                                         {"show_id": "missing", "episode_id": "episode", "position_ms": 1000}]}
        first = self.request("/api/listening/import", method="POST", json=payload)
        second = self.request("/api/listening/import", method="POST", json=payload)
        self.assertEqual(first.json, second.json)
        self.assertEqual((first.json["imported"], first.json["skipped"]), (1, 1))
        progress = self.request("/api/shows/fixture/episodes?web=1").json["episodes"][1]["progress"]
        self.assertTrue(progress["legacy"])
        self.assertEqual(progress["play_count"], 0)
        self.assertEqual(progress["position_ms"], 25000)
        self.assertEqual(self.request("/api/listening/stats").json["summary"]["listened_ms"], 0)

    def test_completion_uses_actual_prepared_audio_duration_instead_of_inaccurate_rss_duration(self):
        self.prepare()
        session = self.start()
        duration = self.target.stat().st_size * 1000 // 32000
        self.now += duration / 1000
        response = self.request("/api/listening/events", method="POST", json={"session_id": session["session_id"], "seq": 1, "position_ms": duration, "listened_ms": duration, "state": "ended"})
        self.assertEqual(response.json["progress"]["status"], "completed")

    def test_invalid_requests_are_explained_without_internal_errors(self):
        for path, body in [("/api/listening/sessions", []), ("/api/listening/sessions", {"client_id": []}),
                           ("/api/listening/events", {"session_id": "missing", "seq": 1, "position_ms": 0, "listened_ms": 0, "state": []}),
                           ("/api/listening/mark", {"show_id": "fixture", "episode_id": "episode1", "status": []}),
                           ("/api/sources", {}), ("/api/sources/preview", {})]:
            with self.subTest(path=path, body=body):
                result = self.request(path, method="POST", json=body)
                self.assertLess(result.status_code, 500)
                self.assertIn("error", result.json)
        self.assertEqual(self.request("/api/shows/fixture/episodes?status=unknown").status_code, 400)
        self.assertEqual(self.request("/api/listening/recent?limit=100000").status_code, 400)
        self.assertEqual(self.request("/api/recommendations").status_code, 410)

    def test_new_source_preview_commit_duplicate_persistence_and_audio_does_not_prepare_on_read(self):
        with mock.patch.object(source_manager, "read_public", return_value=(RSS, "https://example.org/feed.xml")), \
                mock.patch.object(self.server, "queue_preparation", return_value="queued"):
            preview = self.request("/api/sources/preview", method="POST", json={"url": "https://example.org/feed.xml"})
            self.assertEqual(preview.status_code, 200)
            self.assertFalse(preview.json["duplicate"])
            token = preview.json["preview_id"]
            added = self.request("/api/sources", method="POST", json={"preview_id": token})
            duplicate = self.request("/api/sources", method="POST", json={"preview_id": token})
        self.assertEqual(added.status_code, 200)
        self.assertFalse(added.json["duplicate"])
        self.assertTrue(duplicate.json["duplicate"])
        sid = added.json["show_id"]
        self.assertEqual(self.request("/api/shows/" + sid).json["name"], "New podcast")
        reopened = ListeningStore(self.root / "history.sqlite3")
        self.assertEqual(reopened.sources()[0]["id"], sid)
        with mock.patch.object(self.server, "transcode", side_effect=AssertionError("read triggered preparation")):
            self.assertEqual(self.request("/api/episodes/" + sid + "/episode1").status_code, 200)

    def test_added_source_prepares_real_audio_and_reuses_complete_mp3_after_failure(self):
        with mock.patch.object(source_manager, "read_public", return_value=(RSS, "https://example.org/feed.xml")), \
                mock.patch.object(self.server, "queue_preparation", return_value="queued"):
            preview = self.request("/api/sources/preview", method="POST", json={"url": "https://example.org/feed.xml"})
            added = self.request("/api/sources", method="POST", json={"preview_id": preview.json["preview_id"]})
        self.assertEqual(added.status_code, 200)
        sid = added.json["show_id"]
        self.assertNotIn(sid, self.server.SHOWS)
        self.assertTrue(self.server.known_show(sid))
        payload = {"show_id": sid, "episode_id": "episode1"}
        key = sid + "/episode1"
        detail_url = "/api/episodes/" + sid + "/episode1?lite=1"
        stream_url = "/pcm/" + sid + "/episode1/stream.pcm"

        def fetch_fixture(url, target):
            shutil.copyfile(self.fixture, target)
            return target.stat().st_size

        with mock.patch.object(self.server, "fetch_audio", side_effect=fetch_fixture) as download:
            preparing = self.request("/api/prepare", method="POST", json=payload)
            self.assertEqual(preparing.status_code, 200)
            self.assertTrue(self.server._prepare_jobs[key].result(timeout=20))
            download.assert_called_once()
        detail = self.request(detail_url)
        self.assertEqual(detail.status_code, 200)
        self.assertEqual((detail.json["status"], detail.json["ready"], detail.json["segment_count"]), ("ready", True, 1))
        head = self.request(stream_url, method="HEAD")
        self.assertEqual(head.status_code, 200)
        total = int(head.headers["Content-Length"])
        self.assertGreater(total, 0)
        self.assertEqual(head.headers["X-Audio-Segment-Count"], "1")
        part = self.request(stream_url, headers={"Range": "bytes=32000-"})
        self.assertEqual(part.status_code, 206)
        self.assertEqual(part.headers["Content-Range"], f"bytes 32000-{total - 1}/{total}")
        self.assertEqual(len(part.data), total - 32000)

        # Failed/interrupted PCM preparation keeps the complete MP3. Retrying
        # repairs just the missing derived audio and never downloads it again.
        directory = self.server.ep_dir(sid, "episode1")
        original_mp3 = (directory / "seg_000.mp3").read_bytes()
        (directory / self.server.PCM_CACHE_VERSION / "seg_000.pcm").unlink()
        self.server.set_state(key, status="failed", pcm_complete=False, error="interrupted")
        self.assertEqual(self.request(detail_url).json["status"], "failed")
        with mock.patch.object(self.server, "fetch_audio", side_effect=AssertionError("complete MP3 was downloaded again")):
            retry = self.request("/api/prepare", method="POST", json=payload)
            self.assertEqual(retry.status_code, 200)
            self.assertTrue(self.server._prepare_jobs[key].result(timeout=20))
            self.assertEqual(self.request("/api/prepare", method="POST", json=payload).json["status"], "ready")
        self.assertEqual((directory / "seg_000.mp3").read_bytes(), original_mp3)
        self.assertTrue(self.request(detail_url).json["ready"])
        self.assertEqual(self.request(stream_url, method="HEAD").status_code, 200)

    def test_subscription_changes_invalidate_warm_catalogue_without_waiting(self):
        with mock.patch.object(self.server.time, "time", return_value=1800000000), \
                mock.patch.object(source_manager, "read_public", return_value=(RSS, "https://example.org/feed.xml")), \
                mock.patch.object(self.server, "queue_preparation", return_value="queued"):
            self.assertEqual([s["id"] for s in self.server.configured_shows()], ["fixture"])
            preview = self.request("/api/sources/preview", method="POST", json={"url": "https://example.org/feed.xml"})
            added = self.request("/api/sources", method="POST", json={"preview_id": preview.json["preview_id"]})
            self.assertEqual(added.status_code, 200)
            sid = added.json["show_id"]
            self.assertTrue(self.server.known_show(sid))
            self.assertEqual(self.request("/api/shows/" + sid).status_code, 200)
            removed = self.request("/api/sources/" + sid, method="DELETE")
            self.assertEqual(removed.status_code, 200)
            self.assertFalse(self.server.known_show(sid))
            self.assertEqual(self.request("/api/shows/" + sid).status_code, 404)

    def test_subscription_export_matches_original_eleven_and_immediately_includes_persistent_addition(self):
        original = copy.deepcopy(self.server.CFG["shows"])
        self.addCleanup(self.server.CFG.__setitem__, "shows", original)
        production = json.loads((Path(__file__).parents[1] / "config.json").read_text())["shows"]
        self.server.CFG["shows"] = production
        expected = {urljoin("http://localhost/", show["feed"]) for show in production}
        before = self.request("/subscriptions.opml")
        entries = ET.fromstring(before.data).findall("body/outline")
        self.assertEqual(len(entries), 11)
        self.assertEqual({entry.get("xmlUrl") for entry in entries}, expected)
        self.assertNotIn("晚点聊", {entry.get("text") for entry in entries})
        self.assertEqual(ET.fromstring(before.data).findtext("head/title"), "已订阅播客清单")
        self.assertIn("已订阅播客清单.opml", unquote(before.headers["Content-Disposition"]))
        self.assertEqual(before.headers["Cache-Control"], "no-store")
        with mock.patch.object(source_manager, "read_public", return_value=(RSS, "https://example.org/feed.xml")), \
                mock.patch.object(self.server, "queue_preparation", return_value="queued"):
            preview = self.request("/api/sources/preview", method="POST", json={"url": "https://example.org/feed.xml"})
            added = self.request("/api/sources", method="POST", json={"preview_id": preview.json["preview_id"]})
        self.assertEqual(added.status_code, 200)
        after = ET.fromstring(self.request("/subscriptions.opml").data).findall("body/outline")
        self.assertEqual(len(after), 12)
        self.assertEqual({entry.get("xmlUrl") for entry in after}, expected | {"https://example.org/feed.xml"})
        self.server.LISTENING = ListeningStore(self.root / "history.sqlite3")
        reopened = ET.fromstring(self.request("/subscriptions.opml").data).findall("body/outline")
        self.assertEqual({entry.get("xmlUrl") for entry in reopened}, expected | {"https://example.org/feed.xml"})

    def test_subscription_export_round_trips_unicode_xml_characters_and_local_adapter_absolute_url(self):
        original = copy.deepcopy(self.server.CFG["shows"])
        self.addCleanup(self.server.CFG.__setitem__, "shows", original)
        name = '中文 & <经济> "播客" 🎧'
        feed = 'https://example.org/feed.xml?one=1&two=2'
        self.server.CFG["shows"] = [{"id": "escaped", "name": name + "\x01", "feed": feed},
                                   {"id": "adapter", "name": "公开节目", "feed": "/feeds/xiaoyuzhou/111111111111111111111111.xml"}]
        response = self.request("/subscriptions.opml", base_url="http://192.168.1.50:8899/")
        self.assertTrue(response.headers["Content-Type"].startswith("application/xml"))
        self.assertIn("🎧".encode(), response.data)
        self.assertNotIn(b"\x01", response.data)
        entries = ET.fromstring(response.data).findall("body/outline")
        self.assertEqual(entries[0].get("text"), name)
        self.assertEqual(entries[0].get("title"), name)
        self.assertEqual(entries[0].get("xmlUrl"), feed)
        self.assertEqual(entries[1].get("xmlUrl"), "http://192.168.1.50:8899/feeds/xiaoyuzhou/111111111111111111111111.xml")

    def test_subscription_export_works_with_no_subscriptions_without_reading_legacy_recommendation_file(self):
        original = copy.deepcopy(self.server.CFG["shows"])
        self.addCleanup(self.server.CFG.__setitem__, "shows", original)
        self.server.CFG["shows"] = []
        response = self.request("/subscriptions.opml")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(ET.fromstring(response.data).findall("body/outline"), [])

    def test_verified_platform_identities_merge_apple_and_xiaoyuzhou_without_replacing_existing_feed(self):
        configured = self.server.CFG["shows"][0]
        before = copy.deepcopy(configured)
        configured["identities"] = {"apple": ["1552904790"], "xiaoyuzhou": ["5e4ee557418a84a0466737b7"]}
        self.addCleanup(configured.update, before)
        self.addCleanup(configured.clear)
        base = {"source_url": "https://elsewhere.example.org/program", "feed_url": "https://elsewhere.example.org/feed.xml",
                "source_type": "apple", "canonical": "other", "show": {"name": "中文节目", "author": "", "art": "", "episodes": 1},
                "warning": "", "episodes": [{"id": "other", "title": "Different catalogue ID", "audio": "https://example.org/a.mp3"}]}
        for fields in ({"apple_id": "1552904790"}, {"public_pid": "5e4ee557418a84a0466737b7"}):
            payload = {**base, **fields}
            found = self.server.source_duplicate(payload)
            self.assertEqual(found, {"show_id": "fixture", "reason": "verified_platform_identity"})
            with mock.patch.object(self.server, "resolve_source", return_value={**payload, "show": {**payload["show"], "latest": payload["episodes"][0]}}), \
                    mock.patch.object(self.server, "queue_preparation", return_value="queued"):
                preview = self.request("/api/sources/preview", method="POST", json={"url": "https://example.org/shared"})
                added = self.request("/api/sources", method="POST", json={"preview_id": preview.json["preview_id"]})
            self.assertTrue(preview.json["duplicate"])
            self.assertEqual(added.json["show_id"], "fixture")
            self.assertTrue(added.json["duplicate"])
            self.assertEqual(self.server.LISTENING.sources(), [])
            self.assertEqual(configured["feed"], before["feed"])
        wrong_name = {**base, "apple_id": "1552904790", "show": {"name": "另一个同名也不对的节目"}}
        self.assertIsNone(self.server.source_duplicate(wrong_name))

    def test_same_name_alone_or_one_episode_never_merges_but_three_correlated_episodes_do(self):
        known = [{"id": f"old{index}", "title": f"第{index}期一个具体的经济事件讨论", "published": f"2026-10-0{index}T00:00:00Z", "duration": 3600}
                 for index in (1, 2, 3)]
        cache = self.server.load_json(self.server.EP_CACHE, {})
        cache["fixture"]["episodes"] = known
        self.server.save_json(self.server.EP_CACHE, cache)
        payload = {"source_url": "https://different.example.org/show", "feed_url": "https://different.example.org/rss",
                   "show": {"name": " 中文节目 "}, "episodes": [{**episode, "id": "new" + episode["id"]} for episode in known]}
        self.assertEqual(self.server.source_duplicate(payload), {"show_id": "fixture", "reason": "matching_episode_history"})
        for episodes in ([], payload["episodes"][:1],
                         [{**episode, "title": "不相干的另一个节目内容" + episode["id"]} for episode in known],
                         [{**episode, "published": "2020-01-01T00:00:00Z"} for episode in known],
                         [{**episode, "duration": 50} for episode in known]):
            with self.subTest(episodes=episodes):
                self.assertIsNone(self.server.source_duplicate({**payload, "episodes": episodes}))

    def test_show_lite_pagination_is_bounded_and_default_keeps_original_response(self):
        configured_before = copy.deepcopy(self.server.CFG["shows"])
        self.addCleanup(self.server.CFG.__setitem__, "shows", configured_before)
        self.server.CFG["shows"] = [{"id": f"show{index}", "name": f"节目{index}", "feed": ""} for index in range(32)]
        self.server.save_json(self.server.EP_CACHE, {show["id"]: {"id": show["id"], "name": show["name"], "episodes": [{"id": "episode", "title": "测试", "published": "2026-10-04T00:00:00Z"}]} for show in self.server.CFG["shows"]})
        complete = self.request("/api/shows_lite").json
        self.assertEqual(len(complete["s"]), 32)
        self.assertNotIn("total", complete)
        rows = []
        for offset in range(0, 32, 8):
            page = self.request(f"/api/shows_lite?offset={offset}&limit=8").json
            self.assertEqual((page["total"], page["offset"], len(page["s"])), (32, offset, 8))
            rows.extend(page["s"])
        self.assertEqual(rows, complete["s"])
        for query in ("offset=-1", "limit=9", "limit=0", "offset=2147483648", "offset=text", "limit=1.5"):
            self.assertEqual(self.request("/api/shows_lite?" + query).status_code, 400)

    def test_full_browser_audio_real_duration_ranges_and_no_work_on_http(self):
        shutil.copyfile(self.fixture, self.directory / "seg_001.mp3")
        self.server.save_json(self.server.STATE, {"fixture/episode1": {"status": "ready", "segments": 2}})
        self.assertTrue(self.server.transcode("fixture", "episode1", "https://example.org/one.mp3"))
        detail = self.request("/api/episodes/fixture/episode1").json
        self.assertTrue(detail["browser_audio_ready"])
        self.assertEqual(detail["browser_audio_status"], "ready")
        path = detail["browser_audio_url"]
        with mock.patch.object(self.server.subprocess, "run", side_effect=AssertionError("HTTP invoked ffmpeg")), \
                mock.patch.object(self.server, "fetch_audio", side_effect=AssertionError("HTTP downloaded audio")):
            full = self.request(path)
            partial = self.request(path, headers={"Range": "bytes=2-1025"})
            head = self.request(path, method="HEAD")
        self.assertEqual(partial.status_code, 206)
        self.assertEqual(partial.data, full.data[2:1026])
        self.assertEqual(int(head.headers["Content-Length"]), len(full.data))
        whole = self.directory / "stream.mp3"
        probe = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "json", str(whole)], capture_output=True, text=True, check=True)
        self.assertAlmostEqual(float(json.loads(probe.stdout)["format"]["duration"]), 10, delta=.15)
        old = self.source.stat()
        import os
        os.utime(self.source, ns=(old.st_atime_ns, old.st_mtime_ns + 10000000))
        self.assertFalse(self.request("/api/episodes/fixture/episode1").json["browser_audio_ready"])
        self.assertEqual(self.request(path).status_code, 503)

    def test_browser_preparation_failure_keeps_hardware_ready_and_retry_recovers(self):
        self.prepare()
        (self.directory / "stream.mp3").unlink()
        with mock.patch.object(self.server, "prepare_browser_audio", side_effect=OSError("disk failure")):
            self.assertTrue(self.server.transcode("fixture", "episode1", "https://example.org/one.mp3"))
        detail = self.request("/api/episodes/fixture/episode1").json
        self.assertTrue(detail["ready"])
        self.assertEqual(detail["browser_audio_status"], "failed")
        self.assertTrue(self.server.transcode("fixture", "episode1", "https://example.org/one.mp3"))
        self.assertTrue(self.request("/api/episodes/fixture/episode1").json["browser_audio_ready"])


class PublicSourceTests(unittest.TestCase):
    def setUp(self):
        source_manager._search_cache.clear()
        source_manager._apple_requests.clear()

    def test_local_ip_dns_and_credentials_never_connect(self):
        for url in ("file:///etc/passwd", "https://localhost/feed", "http://127.0.0.1/feed", "https://192.168.1.50/", "http://[::1]/", "https://user:password@example.org/feed", "http://example.org:8899/feed", "https://example.org\\@localhost/", "https://224.0.0.1/feed"):
            with self.subTest(url=url), self.assertRaises(ListeningError):
                source_manager.public_url(url)
        with mock.patch.object(source_manager.socket, "getaddrinfo", return_value=[(2, 1, 6, "", ("127.0.0.1", 443))]), \
                mock.patch.object(source_manager.http.client, "HTTPSConnection", side_effect=AssertionError("private address connected")):
            with self.assertRaises(ListeningError):
                source_manager.open_public("https://example.org/feed")

    def test_redirect_to_private_address_is_rejected_and_response_closed(self):
        response = mock.Mock(status=302)
        response.getheader.return_value = "http://192.168.1.50/private"
        connection = mock.Mock()
        connection.getresponse.return_value = response
        with mock.patch.object(source_manager.socket, "getaddrinfo", return_value=[(2, 1, 6, "", ("93.184.215.14", 443))]), \
                mock.patch.object(source_manager.http.client, "HTTPSConnection", return_value=connection):
            with self.assertRaises(ListeningError):
                source_manager.open_public("https://example.org/feed")
        response.close.assert_called_once()
        connection.close.assert_called_once()

    def test_apple_share_lookup_returns_rss_and_single_episode_link_is_rejected(self):
        def read(url, maximum=0):
            if "itunes.apple.com/lookup" in url:
                return json.dumps({"results": [{"collectionId": 123456, "feedUrl": "https://example.org/feed.xml"}]}).encode(), url
            return RSS, url
        with mock.patch.object(source_manager, "read_public", side_effect=read) as requests:
            resolved = source_manager.resolve_source("https://podcasts.apple.com/cn/podcast/title/id123456")
            self.assertEqual(resolved["source_type"], "apple")
            self.assertEqual(resolved["show"]["name"], "New podcast")
            self.assertIn("country=CN", requests.call_args_list[0].args[0])
            with self.assertRaises(ListeningError):
                source_manager.resolve_source("https://podcasts.apple.com/cn/podcast/title/id123456?i=999")

    def test_search_is_actual_apple_directory_and_cached_not_curated_recommendations(self):
        rows = [{"collectionName": "Example", "artistName": "Author", "feedUrl": "https://example.org/feed", "collectionViewUrl": "https://podcasts.apple.com/cn/podcast/example/id123", "trackCount": 12}]
        with mock.patch.object(source_manager, "read_public", return_value=(json.dumps({"results": rows}).encode(), "url")) as read:
            first = source_manager.search_apple("中文节目")
            second = source_manager.search_apple("中文节目")
        self.assertEqual(first, second)
        self.assertEqual(read.call_count, 1)
        self.assertIn("entity=podcast", read.call_args.args[0])
        self.assertEqual(first[0]["episodes"], 12)

    def test_any_valid_public_xiaoyuzhou_program_is_supported_but_paid_content_is_excluded(self):
        fixture = Path(__file__).parent / "fixtures" / "wechat-talk-public-page.json"
        podcast = json.loads(fixture.read_text())
        pid = "111111111111111111111111"
        podcast["pid"] = pid
        for episode in podcast["episodes"]:
            episode["pid"] = pid
        podcast["episodes"][0]["payType"] = "PAID"
        html = ('<script id="__NEXT_DATA__">' + json.dumps({"props": {"pageProps": {"podcast": podcast}}}) + '</script>').encode()
        with mock.patch.object(source_manager, "read_public", return_value=(html, "url")):
            resolved = source_manager.resolve_source("https://www.xiaoyuzhoufm.com/podcast/" + pid)
        self.assertEqual(resolved["public_pid"], pid)
        self.assertEqual(resolved["show"]["episodes"], 9)
        self.assertIn("历史", resolved["warning"])
        with self.assertRaises(ListeningError):
            source_manager.resolve_source("https://www.xiaoyuzhoufm.com/episode/" + pid)


if __name__ == "__main__":
    unittest.main()
