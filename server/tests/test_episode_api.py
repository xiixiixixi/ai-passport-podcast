"""硬件用分页、单集详情、准备恢复与续播协议验证。"""
import json
import io
import os
from pathlib import Path
import shutil
import subprocess
import unittest
from concurrent.futures import Future, ThreadPoolExecutor
from unittest import mock

import test_pcm


@unittest.skipUnless(shutil.which("ffmpeg"), "ffmpeg is required for audio integration tests")
class EpisodeAPITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_pcm.PCMEndpointTests.setUpClass.__func__(cls)

    @classmethod
    def tearDownClass(cls):
        test_pcm.PCMEndpointTests.tearDownClass.__func__(cls)

    def setUp(self):
        test_pcm.PCMEndpointTests.setUp(self)
        self.server._prepare_jobs.clear()
        self.episodes = [{"id": f"episode{index + 1}", "title": f"第{index + 1}集：中文标题" + "内容" * 100,
                          "duration": 5, "published": "2026-10-02T23:30:00+00:00",
                          "audio": "https://example.invalid/audio.mp3"} for index in range(12)]
        self.server.save_json(self.server.EP_CACHE, {
            "fixture": {"id": "fixture", "name": "fixture", "count": 12, "episodes": self.episodes}})
        self.server.save_json(self.server.STATE, {"fixture/episode1": {"status": "ready", "segments": 1}})
        self.assertTrue(self.server.transcode("fixture", "episode1", self.episodes[0]["audio"]))

    request = test_pcm.PCMEndpointTests.request

    def test_public_feed_route_handles_origin_status_without_audio_work(self):
        path = "/feeds/xiaoyuzhou/65f02690587b754dbe358a7d.xml"
        with mock.patch.object(self.server._public_feeds, "get", return_value=(b"<rss/>", True)):
            response = self.request(path)
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.data, b"<rss/>")
        self.assertTrue(response.headers["Content-Type"].startswith("application/rss+xml"))
        self.assertEqual(response.headers["X-Feed-Stale"], "true")
        with mock.patch.object(self.server._public_feeds, "get", side_effect=OSError("offline")):
            response = self.request(path)
        self.assertEqual(response.status_code, 502)
        self.assertEqual(response.json, {"error": "公开节目页暂时无法读取，请稍后刷新"})
        with mock.patch.object(self.server._public_feeds, "get", side_effect=AssertionError("network")):
            self.assertEqual(self.request("/feeds/xiaoyuzhou/unknown.xml").status_code, 404)

    def test_confirmed_public_retraction_updates_episode_catalog_but_origin_failures_preserve_it(self):
        show = {"id": "wechat-talk", "name": "微信公开TALK",
                "feed": "http://192.168.1.50:8899/feeds/xiaoyuzhou/65f02690587b754dbe358a7d.xml"}
        prior = {"wechat-talk": {"id": "wechat-talk", "count": 1, "episodes": [self.episodes[0]]}}
        empty = '<?xml version="1.0" encoding="utf-8"?><rss version="2.0"><channel><title>微信公开TALK</title></channel></rss>'.encode()
        self.server.save_json(self.server.EP_CACHE, prior)
        with mock.patch.object(self.server._public_feeds, "get", return_value=(empty, False)):
            self.assertEqual(self.server.fetch_show(show), {"ok": True, "count": 0})
        self.assertEqual(self.server.load_json(self.server.EP_CACHE, {})["wechat-talk"]["episodes"], [])
        for result in (OSError("offline"), (empty, True)):
            self.server.save_json(self.server.EP_CACHE, prior)
            patch = {"side_effect": result} if isinstance(result, Exception) else {"return_value": result}
            with mock.patch.object(self.server._public_feeds, "get", **patch):
                self.assertFalse(self.server.fetch_show(show)["ok"])
            self.assertEqual(self.server.load_json(self.server.EP_CACHE, {}), prior)
        ordinary = dict(show, feed="https://example.invalid/source.xml")
        with mock.patch.object(self.server.urllib.request, "urlopen", return_value=io.BytesIO(empty)):
            self.assertFalse(self.server.fetch_show(ordinary)["ok"])
        self.assertEqual(self.server.load_json(self.server.EP_CACHE, {}), prior)

    def test_default_page_preserves_chinese_and_small_payload(self):
        response = self.request("/api/shows/fixture/episodes")
        body = response.json
        self.assertEqual(response.status_code, 200)
        self.assertEqual((body["show_id"], body["total"], body["offset"]), ("fixture", 12, 0))
        self.assertEqual(len(body["episodes"]), 5)
        first = body["episodes"][0]
        self.assertEqual(first["id"], "episode1")
        self.assertEqual(first["title"], self.episodes[0]["title"][:160])
        self.assertEqual(first["pub_date"], "2026-10-03")
        self.assertEqual(first["duration"], 5)
        self.assertIs(first["ready"], True)
        self.assertEqual(first["segments"], 1)
        self.assertIs(body["episodes"][1]["ready"], False)
        self.assertEqual(body["episodes"][1]["segments"], 0)
        self.assertNotIn("audio", first)
        self.assertNotIn(b"\\u", response.data)
        self.assertLess(len(response.data), 3500)

    def test_page_boundaries_and_bad_parameters(self):
        for query, status, ids in [
            ("offset=10&limit=10", 200, ["episode11", "episode12"]),
            ("offset=12&limit=1", 200, []),
            ("offset=999&limit=5", 200, []),
            ("offset=0&limit=10", 200, [f"episode{i}" for i in range(1, 11)]),
            ("offset=-1", 400, None), ("offset=no", 400, None),
            ("offset=1.5", 400, None), ("limit=0", 400, None),
            ("limit=11", 400, None), ("limit=abc", 400, None),
            ("offset=2147483648", 400, None),
        ]:
            with self.subTest(query=query):
                response = self.request("/api/shows/fixture/episodes?" + query)
                self.assertEqual(response.status_code, status)
                if ids is not None:
                    self.assertEqual([e["id"] for e in response.json["episodes"]], ids)

    def test_focus_episode_chooses_its_page_and_neighbor_offsets(self):
        response = self.request("/api/shows/fixture/episodes?episode_id=episode8&limit=3")
        self.assertEqual(response.status_code, 200)
        self.assertEqual((response.json["focus"], response.json["offset"]), (7, 6))
        self.assertEqual([e["id"] for e in response.json["episodes"]], ["episode7", "episode8", "episode9"])
        last = self.request("/api/shows/fixture/episodes?episode_id=episode12&limit=5")
        self.assertEqual((last.json["focus"], last.json["offset"]), (11, 10))
        self.assertEqual(self.request("/api/shows/fixture/episodes?episode_id=missing&limit=3").status_code, 404)
        self.assertNotIn("focus", self.request("/api/shows/fixture/episodes?offset=3&limit=3").json)

    def test_unknown_ids_and_known_empty_show(self):
        for path in ("/api/shows/unknown/episodes", "/api/episodes/unknown/episode1",
                     "/api/episodes/fixture/missing"):
            self.assertEqual(self.request(path).status_code, 404)
        self.server.save_json(self.server.EP_CACHE, {})
        body = self.request("/api/shows/fixture/episodes").json
        self.assertEqual(body, {"show_id": "fixture", "total": 0, "offset": 0, "order": "newest", "episodes": []})

    def test_ready_detail_never_probes_converts_or_downloads(self):
        original = subprocess.run
        commands = []

        def metadata_only(cmd, **kwargs):
            commands.append(cmd)
            self.assertEqual(cmd[0], "ffprobe")
            return original(cmd, **kwargs)

        with mock.patch.object(self.server.subprocess, "run", side_effect=metadata_only), \
                mock.patch.object(self.server, "fetch_audio", side_effect=AssertionError("unexpected download")):
            first = self.request("/api/episodes/fixture/episode1")
            second = self.request("/api/episodes/fixture/episode1")
        self.assertEqual(first.status_code, 200)
        self.assertEqual(first.json, second.json)
        self.assertIs(first.json["ready"], True)
        self.assertEqual(first.json["status"], "ready")
        segment = first.json["segments"][0]
        self.assertEqual((segment["index"], segment["bytes"]), (0, self.target.stat().st_size))
        self.assertAlmostEqual(segment["duration_ms"], 5000, delta=150)
        self.assertEqual(len(commands), 0)

    def test_cached_pcm_reports_exact_bytes_and_duration_without_probe(self):
        audio = self.request()
        with mock.patch.object(self.server.subprocess, "run", side_effect=AssertionError("unexpected probe")):
            detail = self.request("/api/episodes/fixture/episode1")
        self.assertEqual(detail.json["segments"], [{"index": 0, "bytes": len(audio.data),
                                                  "duration_ms": len(audio.data) * 1000 // 32000}])

    def test_stale_pcm_is_not_reported_as_exact(self):
        self.request()
        os.utime(self.target, (1, 1))
        detail = self.request("/api/episodes/fixture/episode1")
        self.assertEqual(detail.json["segments"], [])
        self.assertIs(detail.json["ready"], False)

    def test_changed_mp3_invalidates_prepared_manifest_without_http_probe(self):
        self.request("/api/episodes/fixture/episode1")
        old = self.source.stat()
        os.utime(self.source, ns=(old.st_atime_ns, old.st_mtime_ns + 1000000000))
        with mock.patch.object(self.server.subprocess, "run", wraps=subprocess.run) as probe:
            self.assertEqual(self.request("/api/episodes/fixture/episode1").status_code, 200)
        self.assertEqual(probe.call_count, 0)
        self.assertIs(self.request("/api/episodes/fixture/episode1").json["ready"], False)

    def test_legacy_mp3_ready_is_not_pcm_ready_and_detail_never_probes(self):
        shutil.rmtree(self.cache_dir)
        self.server.save_json(self.server.STATE, {"fixture/episode1": {"status": "ready", "segments": 1}})
        with mock.patch.object(self.server.subprocess, "run", side_effect=subprocess.TimeoutExpired("ffprobe", 10)):
            response = self.request("/api/episodes/fixture/episode1")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json["segments"], [])
        self.assertIs(response.json["ready"], False)
        self.assertFalse(self.cache_dir.exists())

    def test_ready_requires_full_contiguous_nonempty_cache_and_success_state(self):
        prepared = self.server.get_state()["fixture/episode1"]
        for state, invalidate in [
            ({"status": "none", "segments": 1}, None),
            ({"status": "queued", "segments": 1}, None),
            ({"status": "downloading", "segments": 1}, None),
            ({"status": "transcoding", "segments": 1}, None),
            ({"status": "failed", "segments": 1}, None),
            ({"status": "ready", "segments": 2}, None),
            ({"status": "ready", "segments": 1}, "empty"),
            ({"status": "ready", "segments": 1}, "symlink"),
        ]:
            with self.subTest(state=state, invalidate=invalidate):
                if self.source.exists() or self.source.is_symlink():
                    self.source.unlink()
                if invalidate == "symlink":
                    self.source.symlink_to(self.fixture)
                elif invalidate == "empty":
                    self.source.write_bytes(b"")
                else:
                    shutil.copyfile(self.fixture, self.source)
                self.server.save_json(self.server.STATE, {"fixture/episode1": {**prepared, **state}})
                page = self.request("/api/shows/fixture/episodes").json
                detail = self.request("/api/episodes/fixture/episode1").json
                self.assertIs(page["episodes"][0]["ready"], False)
                self.assertEqual(page["episodes"][0]["segments"], 0)
                self.assertIs(detail["ready"], False)
                self.assertEqual(detail["segments"], [])
                self.assertEqual(detail["status"], "missing" if state["status"] == "ready" else state["status"])

    def test_prepare_deduplicates_active_job_and_preserves_status_api(self):
        self.server.save_json(self.server.STATE, {})
        future = Future()
        payload = {"show_id": "fixture", "episode_id": "episode1"}
        with mock.patch.object(self.server._pool, "submit", return_value=future) as submit:
            first = self.request("/api/prepare", method="POST", json=payload)
            second = self.request("/api/prepare", method="POST", json=payload)
        # 点播响应附带排队估算（任务2c）；设备端 cJSON 忽略未知字段，旧字段不变。
        self.assertEqual(first.json, {"status": "queued", "show_id": "fixture", "episode_id": "episode1",
                                      "pinned": True, "queue_position": 0, "eta_seconds": 60})
        self.assertEqual(second.json, first.json)
        self.assertEqual(submit.call_count, 1)
        self.assertEqual(self.request("/api/status").json["fixture/episode1"]["status"], "queued")
        self.assertEqual(self.request("/api/episodes/fixture/episode1").json["status"], "queued")

    def test_prepare_keeps_ready_episode_and_recovers_lost_or_failed_job(self):
        payload = {"show_id": "fixture", "episode_id": "episode1"}
        with mock.patch.object(self.server._pool, "submit") as submit:
            self.assertEqual(self.request("/api/prepare", method="POST", json=payload).json["status"], "ready")
            submit.assert_not_called()
        for status in ("ready", "queued", "downloading", "transcoding", "failed"):
            with self.subTest(status=status):
                self.server._prepare_jobs.clear()
                self.server.save_json(self.server.STATE, {"fixture/episode1": {"status": status, "segments": 2}})
                with mock.patch.object(self.server._pool, "submit", return_value=Future()) as submit:
                    response = self.request("/api/prepare", method="POST", json=payload)
                self.assertEqual(response.json["status"], "queued")
                self.assertEqual(submit.call_count, 1)

    def test_prepare_rejects_bad_payload_and_unknown_episode(self):
        for payload, status in [([], 400), ({"show_id": []}, 400), ({}, 400),
                                ({"show_id": "fixture", "episode_id": "missing"}, 404)]:
            self.assertEqual(self.request("/api/prepare", method="POST", json=payload).status_code, status)

    def test_partial_old_segments_are_replaced_by_a_complete_preparation(self):
        shutil.copyfile(self.fixture, self.directory / "seg_001.mp3")
        self.server.save_json(self.server.STATE, {"fixture/episode1": {"status": "failed", "segments": 2}})

        def fetch_fixture(url, target):
            shutil.copyfile(self.fixture, target)
            return target.stat().st_size

        with mock.patch.object(self.server, "fetch_audio", side_effect=fetch_fixture):
            self.assertTrue(self.server.transcode("fixture", "episode1", "https://example.invalid/audio.mp3"))
        self.assertFalse((self.directory / "seg_001.mp3").exists())
        self.assertEqual(self.server.get_state()["fixture/episode1"]["segments"], 1)
        self.assertIs(self.request("/api/episodes/fixture/episode1").json["ready"], True)

    def test_pcm_head_resume_tail_and_end_of_file_ranges(self):
        audio = self.request()
        total = len(audio.data)
        head = self.request(method="HEAD")
        self.assertEqual(head.status_code, 200)
        self.assertEqual(int(head.headers["Content-Length"]), total)
        self.assertEqual(total % 2, 0)
        for name, value in {"X-Audio-Sample-Rate": "16000", "X-Audio-Channels": "1",
                            "X-Audio-Bits": "16", "X-Audio-Format": "s16le"}.items():
            self.assertEqual(head.headers[name], value)
        offset = 32000
        tail = self.request(headers={"Range": f"bytes={offset}-"})
        self.assertEqual(tail.status_code, 206)
        self.assertEqual(tail.headers["Content-Range"], f"bytes {offset}-{total - 1}/{total}")
        self.assertEqual(int(tail.headers["Content-Length"]), total - offset)
        self.assertEqual(tail.data, audio.data[offset:])
        for offset in (total, total + 32000):
            beyond = self.request(headers={"Range": f"bytes={offset}-"})
            self.assertEqual(beyond.status_code, 416)
            self.assertEqual(beyond.headers["Content-Range"], f"bytes */{total}")


if __name__ == "__main__":
    unittest.main()
