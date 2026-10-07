"""真实音频、跨段全局续传、准备失败与优先队列的隔离验证。"""
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import threading
import time
import unittest
from concurrent.futures import Future
from unittest import mock

import test_pcm


@unittest.skipUnless(shutil.which("ffmpeg"), "ffmpeg is required for audio integration tests")
class ContinuousAudioTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_pcm.PCMEndpointTests.setUpClass.__func__(cls)

    @classmethod
    def tearDownClass(cls):
        test_pcm.PCMEndpointTests.tearDownClass.__func__(cls)

    def setUp(self):
        test_pcm.PCMEndpointTests.setUp(self)
        self.server._prepare_jobs.clear()
        self.episodes = [
            {"id": "episode1", "title": "最新中文集", "duration": 5,
             "published": "2026-10-04T00:00:00Z", "audio": "https://example.invalid/latest.mp3"},
            {"id": "episode2", "title": "同日较早集", "duration": 5,
             "published": "2026-10-03T23:00:00Z", "audio": "https://example.invalid/older.mp3"},
            {"id": "episode3", "title": "最早集", "duration": 5,
             "published": "2026-10-02T02:00:00+08:00", "audio": "https://example.invalid/oldest.mp3"},
        ]
        self.catalog = {"fixture": {"id": "fixture", "name": "测试播客", "count": 3,
                                     "episodes": self.episodes}}
        self.server.save_json(self.server.EP_CACHE, self.catalog)
        self.key = "fixture/episode1"

    request = test_pcm.PCMEndpointTests.request
    prepare = test_pcm.PCMEndpointTests.prepare

    def add_second_segment(self):
        shutil.copyfile(self.fixture, self.directory / "seg_001.mp3")
        self.server.save_json(self.server.STATE, {self.key: {"status": "ready", "segments": 2}})

    def test_legacy_mp3_and_existing_pcm_migrate_without_download_or_reencode(self):
        self.add_second_segment()
        self.server.pcm_segment("fixture", "episode1", "seg_000.pcm")
        before_pcm = self.target.read_bytes()
        before_mp3 = [p.read_bytes() for p in self.server.seg_files("fixture", "episode1")]
        self.assertFalse(self.request("/api/episodes/fixture/episode1").json["ready"])
        original_run = subprocess.run
        commands = []

        def record(cmd, **kwargs):
            commands.append(cmd)
            return original_run(cmd, **kwargs)

        with mock.patch.object(self.server, "fetch_audio", side_effect=AssertionError("redownloaded old episode")), \
                mock.patch.object(self.server.subprocess, "run", side_effect=record):
            self.assertTrue(self.server.transcode("fixture", "episode1", self.episodes[0]["audio"]))
        pcm_commands = [cmd for cmd in commands if cmd[0] == "ffmpeg" and "concat" not in cmd]
        browser_commands = [cmd for cmd in commands if cmd[0] == "ffmpeg" and "concat" in cmd]
        self.assertEqual(len(pcm_commands), 1)
        self.assertEqual(len(browser_commands), 1)
        self.assertEqual(browser_commands[0][browser_commands[0].index("-c:a") + 1], "copy")
        self.assertEqual(self.target.read_bytes(), before_pcm)
        self.assertEqual([p.read_bytes() for p in self.server.seg_files("fixture", "episode1")], before_mp3)
        detail = self.request("/api/episodes/fixture/episode1").json
        self.assertTrue(detail["ready"])
        self.assertEqual(len(detail["segments"]), 2)
        self.assertTrue(all(s["bytes"] > 0 and s["bytes"] % 2 == 0 for s in detail["segments"]))

    def test_partial_even_old_pcm_is_not_trusted_as_complete(self):
        self.cache_dir.mkdir()
        self.target.write_bytes(b"\0\0" * 20)
        self.prepare()
        self.assertAlmostEqual(self.target.stat().st_size / 32000, 5, delta=0.03)
        self.target.write_bytes(b"\0\0" * 20)
        with mock.patch.object(self.server.subprocess, "run", side_effect=AssertionError("HTTP converted audio")):
            self.assertFalse(self.request("/api/episodes/fixture/episode1").json["ready"])
            self.assertEqual(self.request("/pcm/fixture/episode1/stream.pcm").status_code, 503)

    def test_pcm_failure_keeps_mp3_and_completed_part_and_retry_only_fills_missing_part(self):
        self.add_second_segment()
        original = self.server.pcm_segment

        def fail_second(sid, eid, filename, **kwargs):
            if filename == "seg_001.pcm":
                raise OSError("test disk failure")
            return original(sid, eid, filename, **kwargs)

        with mock.patch.object(self.server, "fetch_audio", side_effect=AssertionError("unexpected download")), \
                mock.patch.object(self.server, "pcm_segment", side_effect=fail_second):
            self.assertFalse(self.server.transcode("fixture", "episode1", self.episodes[0]["audio"]))
        failed = self.server.get_state()[self.key]
        self.assertEqual(failed["status"], "failed")
        self.assertTrue(failed["mp3_complete"])
        self.assertFalse(failed["pcm_complete"])
        self.assertEqual(failed["pcm_ready_segments"], 1)
        before = self.target.read_bytes()
        with mock.patch.object(self.server, "fetch_audio", side_effect=AssertionError("unexpected download")), \
                mock.patch.object(self.server, "pcm_segment", wraps=original) as convert:
            self.assertTrue(self.server.transcode("fixture", "episode1", self.episodes[0]["audio"]))
        self.assertEqual(convert.call_count, 1)
        self.assertEqual(convert.call_args.args[2], "seg_001.pcm")
        self.assertEqual(self.target.read_bytes(), before)

    def test_disk_failure_never_marks_ready_or_downloads(self):
        self.server.save_json(self.server.STATE, {})
        usage = shutil._ntuple_diskusage(100, 100, 0)
        with mock.patch.object(self.server.shutil, "disk_usage", return_value=usage), \
                mock.patch.object(self.server, "fetch_audio", side_effect=AssertionError("disk full downloaded")):
            self.assertFalse(self.server.transcode("fixture", "episode1", self.episodes[0]["audio"]))
        self.assertEqual(self.server.get_state()[self.key]["status"], "failed")
        self.assertFalse(self.request("/api/episodes/fixture/episode1").json["ready"])

    def test_new_download_is_completely_pcm_prepared_before_ready(self):
        self.server.save_json(self.server.STATE, {})

        def fetch_fixture(url, target):
            shutil.copyfile(self.fixture, target)
            return target.stat().st_size

        with mock.patch.object(self.server, "SEG_SEC", 2), \
                mock.patch.object(self.server, "fetch_audio", side_effect=fetch_fixture) as fetch:
            self.assertTrue(self.server.transcode("fixture", "episode1", self.episodes[0]["audio"]))
        fetch.assert_called_once()
        detail = self.request("/api/episodes/fixture/episode1").json
        self.assertTrue(detail["ready"])
        self.assertGreater(len(detail["segments"]), 1)
        with mock.patch.object(self.server.subprocess, "run", side_effect=AssertionError("playback transcoded")):
            stream = self.request("/pcm/fixture/episode1/stream.pcm")
        self.assertEqual(stream.status_code, 200)
        self.assertEqual(len(stream.data), sum(s["bytes"] for s in detail["segments"]))

    def test_logical_stream_head_global_ranges_cross_segment_and_suffix(self):
        self.add_second_segment()
        self.assertTrue(self.server.transcode("fixture", "episode1", self.episodes[0]["audio"]))
        parts = [p.read_bytes() for p in sorted(self.cache_dir.glob("seg_*.pcm"))]
        expected = b"".join(parts)
        path = "/pcm/fixture/episode1/stream.pcm"
        with mock.patch.object(self.server.subprocess, "run", side_effect=AssertionError("HTTP converted audio")):
            full = self.request(path)
            head = self.request(path, method="HEAD")
            start = len(parts[0]) - 8
            cross = self.request(path, headers={"Range": f"bytes={start}-{start + 31}"})
            resume = self.request(path, headers={"Range": f"bytes={start}-"})
            suffix = self.request(path, headers={"Range": "bytes=-17"})
        self.assertEqual(full.data, expected)
        self.assertEqual(head.data, b"")
        self.assertEqual(int(head.headers["Content-Length"]), len(expected))
        self.assertEqual(head.headers["X-Audio-Segment-Count"], "2")
        self.assertEqual(head.headers["X-Audio-Segment-Lengths"], ",".join(str(len(p)) for p in parts))
        self.assertEqual(cross.status_code, 206)
        self.assertEqual(cross.data, expected[start:start + 32])
        self.assertEqual(cross.headers["Content-Range"], f"bytes {start}-{start + 31}/{len(expected)}")
        self.assertEqual(resume.data, expected[start:])
        self.assertEqual(suffix.data, expected[-17:])
        for bad in (f"bytes={len(expected)}-", "bytes=10-2", "bytes=-0", "bytes=0-1,3-5", "items=0-1"):
            result = self.request(path, headers={"Range": bad})
            self.assertEqual(result.status_code, 416)
            self.assertEqual(result.headers["Content-Range"], f"bytes */{len(expected)}")

    def test_lite_detail_bounded_at_max_segments_and_omits_segment_array(self):
        self.prepare()
        first_pcm = self.target.read_bytes()
        manifest = {}
        for index in range(self.server.MAX_EPISODE_SEGMENTS):
            source = self.directory / f"seg_{index:03d}.mp3"
            if index:
                shutil.copyfile(self.fixture, source)
            target = self.cache_dir / f"seg_{index:03d}.pcm"
            target.write_bytes(first_pcm)
            manifest[target.name] = self.server.pcm_manifest_entry(source, target.stat())
        self.server.save_json(self.directory / self.server.PCM_MANIFEST_NAME,
                              {"version": self.server.PCM_CACHE_VERSION, "files": manifest})
        self.server.set_state(self.key, segments=self.server.MAX_EPISODE_SEGMENTS)
        with mock.patch.object(self.server.subprocess, "run", side_effect=AssertionError("detail converted audio")):
            response = self.request("/api/episodes/fixture/episode1?lite=1")
        self.assertEqual(response.status_code, 200)
        self.assertTrue(response.json["ready"])
        self.assertEqual(response.json["segment_count"], 256)
        self.assertEqual(response.json["total"], 3)
        self.assertNotIn("segments", response.json)
        self.assertNotIn("audio", response.json)
        self.assertLess(len(response.data), 8192)
        self.assertEqual(self.request("/api/episodes/fixture/episode1").json["segment_count"], 256)

    def test_prepare_returns_immediately_deduplicates_and_promotes_old_cache(self):
        self.server.save_json(self.server.STATE, {self.key: {"status": "ready", "segments": 1}})
        future = Future()
        with mock.patch.object(self.server._pool, "submit", return_value=future) as submit, \
                mock.patch.object(self.server._pool, "promote") as promote, \
                mock.patch.object(self.server, "fetch_audio", side_effect=AssertionError("request downloaded")):
            self.assertEqual(self.server.queue_preparation("fixture", self.episodes[0], priority=10), "queued")
            response = self.request("/api/prepare", method="POST", json={"show_id": "fixture", "episode_id": "episode1"})
        self.assertEqual(response.json["status"], "queued")
        self.assertEqual(submit.call_count, 1)
        promote.assert_called_once_with(future, 0)
        self.assertTrue(self.server.get_state()[self.key]["mp3_complete"])

    def test_automatic_latest_is_bounded_deduplicated_and_failure_isolated(self):
        self.server.CFG["shows"].append({"id": "other", "name": "另一个", "feed": ""})
        self.addCleanup(self.server.CFG["shows"].pop)
        self.catalog["other"] = {"episodes": self.episodes, "name": "另一个", "id": "other", "count": 3}
        self.server.save_json(self.server.EP_CACHE, self.catalog)
        self.server.save_json(self.server.SETTINGS_FILE, {"prefetch_latest": 2})
        calls = []

        def enqueue(sid, episode, priority):
            calls.append((sid, episode["id"], priority))
            if sid == "fixture":
                raise OSError("one show's failure")
            return "queued"

        with mock.patch.object(self.server, "queue_preparation", side_effect=enqueue):
            self.server.queue_latest_episodes()
        # 每档最多预缓 settings 指定的集数，最新一集 priority 10、次新 11；
        # 一档的失败不影响另一档。
        self.assertEqual(calls, [("fixture", "episode1", 10), ("fixture", "episode2", 11),
                                 ("other", "episode1", 10), ("other", "episode2", 11)])

    def test_startup_recovers_partial_pcm_without_changing_episode_selection(self):
        self.server.save_json(self.server.STATE, {self.key: {"status": "transcoding", "segments": 1,
                                                            "mp3_complete": True}})
        with mock.patch.object(self.server, "queue_preparation", return_value="queued") as enqueue:
            self.server.recover_prepared_episodes()
        enqueue.assert_called_once_with("fixture", self.episodes[0], priority=8)

    def test_refresh_and_startup_enqueue_latest_after_directory_work(self):
        calls = []
        with mock.patch.object(self.server, "fetch_show", side_effect=lambda show: calls.append("fetch") or {"ok": True}), \
                mock.patch.object(self.server, "queue_latest_episodes", side_effect=lambda: calls.append("latest")):
            self.server.refresh_all()
        self.assertEqual(calls, ["fetch", "latest"])
        targets = []
        with mock.patch.object(self.server.threading, "Thread", side_effect=lambda **kwargs: targets.append(kwargs["target"]) or mock.Mock()):
            self.server.start_background()
        calls.clear()
        with mock.patch.object(self.server, "recover_prepared_episodes", side_effect=lambda: calls.append("recover")), \
                mock.patch.object(self.server, "queue_latest_episodes", side_effect=lambda: calls.append("latest")), \
                mock.patch.object(self.server, "refresh_all", side_effect=lambda: calls.append("refresh")):
            targets[1]()
        self.assertEqual(calls, ["recover", "latest", "refresh"])

    def test_next_prefetch_route_uses_background_priority_and_http_does_not_run_audio_work(self):
        with mock.patch.object(self.server, "queue_preparation", return_value="queued") as enqueue, \
                mock.patch.object(self.server, "transcode", side_effect=AssertionError("request blocked for conversion")):
            result = self.request("/api/prepare", method="POST", json={"show_id": "fixture", "episode_id": "episode1", "prefetch": True})
        self.assertEqual(result.status_code, 200)
        enqueue.assert_called_once_with("fixture", self.episodes[0], priority=3)

    def test_manual_refresh_real_thread_merges_requests_and_publishes_result_or_failure(self):
        started, release = threading.Event(), threading.Event()
        results = {"fixture": {"ok": True, "count": 3}, "broken": {"ok": False, "error": "source failed"}}

        def blocked_refresh():
            started.set()
            self.assertTrue(release.wait(5))
            return results

        with mock.patch.object(self.server, "refresh_all", side_effect=blocked_refresh) as refresh:
            first = self.request("/api/refresh")
            self.assertEqual(first.status_code, 202)
            self.assertTrue(started.wait(2))
            second = self.request("/api/refresh")
            self.assertEqual(second.json, first.json)
            self.assertEqual(self.request("/api/refresh/status").json["status"], "running")
            release.set()
            self.server._refresh_thread.join(timeout=5)
            self.assertFalse(self.server._refresh_thread.is_alive())
        refresh.assert_called_once()
        complete = self.request("/api/refresh/status").json
        self.assertEqual(complete["status"], "complete")
        self.assertEqual(complete["job_id"], first.json["job_id"])
        self.assertEqual(complete["results"], results)
        self.assertIsInstance(complete["finished_at"], int)
        with mock.patch.object(self.server, "refresh_all", side_effect=RuntimeError("internal failure")):
            failed = self.request("/api/refresh")
            self.assertNotEqual(failed.json["job_id"], first.json["job_id"])
            self.server._refresh_thread.join(timeout=5)
        failure = self.request("/api/refresh/status").json
        self.assertEqual(failure["status"], "failed")
        self.assertEqual(failure["results"], {})
        self.assertTrue(failure["error"])
        # 自动刷新调用不会改变手动任务的结果和编号。
        with mock.patch.object(self.server, "fetch_show", return_value={"ok": True}), \
                mock.patch.object(self.server, "queue_latest_episodes"):
            self.server.refresh_all()
        self.assertEqual(self.request("/api/refresh/status").json, failure)

    def test_dates_stable_order_focus_neighbors_and_same_timestamp(self):
        self.episodes[1]["published"] = self.episodes[0]["published"]
        self.catalog["fixture"]["episodes"] = [self.episodes[2], self.episodes[0], self.episodes[1]]
        self.server.save_json(self.server.EP_CACHE, self.catalog)
        newest = self.request("/api/shows/fixture/episodes?limit=3").json
        oldest = self.request("/api/shows/fixture/episodes?order=oldest&episode_id=episode2&limit=1").json
        self.assertEqual([e["id"] for e in newest["episodes"]], ["episode1", "episode2", "episode3"])
        self.assertEqual((oldest["focus"], oldest["offset"]), (1, 1))
        detail = self.request("/api/episodes/fixture/episode2").json
        self.assertEqual((detail["newest_index"], detail["oldest_index"]), (1, 1))
        self.assertEqual((detail["newer"]["id"], detail["older"]["id"]), ("episode1", "episode3"))
        self.assertEqual(detail["pub_date"], "2026-10-04")
        self.assertGreater(detail["published_at"], 0)
        self.assertIsNone(self.request("/api/episodes/fixture/episode1").json["newer"])
        self.assertIsNone(self.request("/api/episodes/fixture/episode3").json["older"])
        self.assertEqual(self.request("/api/shows/fixture/episodes?order=invalid").status_code, 400)

    def test_show_order_uses_latest_actual_episode_not_config_or_refresh_time(self):
        old_cfg = self.server.CFG["shows"]
        self.server.CFG["shows"] = [{"id": "older", "name": "older", "feed": ""}, old_cfg[0]]
        self.addCleanup(self.server.CFG.__setitem__, "shows", old_cfg)
        self.catalog["older"] = {"id": "older", "name": "older", "count": 1,
                                   "updated": "2099-12-31", "episodes": [self.episodes[2]]}
        self.server.save_json(self.server.EP_CACHE, self.catalog)
        result = self.request("/api/shows_lite").json["s"]
        self.assertEqual([s["i"] for s in result], ["fixture", "older"])
        self.assertGreater(result[0]["p"], result[1]["p"])
        self.assertEqual(result[0]["d"], "2026-10-04")

    def test_priority_queue_reserves_user_slot_and_promotes_queued_background(self):
        queue = self.server.PreparationQueue()
        self.addCleanup(queue.shutdown)
        background_started = threading.Event()
        release_background = threading.Event()
        user_started = threading.Event()
        release_user = threading.Event()
        promoted_started = threading.Event()

        def background():
            background_started.set()
            self.assertTrue(release_background.wait(5))

        def user():
            user_started.set()
            self.assertTrue(release_user.wait(5))

        bg = queue.submit(background, priority=10)
        self.assertTrue(background_started.wait(2))
        waiting = queue.submit(lambda: promoted_started.set(), priority=10)
        foreground = queue.submit(user, priority=0)
        self.assertTrue(user_started.wait(2), "user waited behind the automatic queue")
        self.assertFalse(waiting.running())
        queue.promote(waiting, 0)
        release_user.set()
        self.assertTrue(promoted_started.wait(2))
        release_background.set()
        for future in (bg, waiting, foreground):
            future.result(timeout=5)

    def test_next_prefetch_uses_only_background_slot_and_precedes_automatic_latest(self):
        queue = self.server.PreparationQueue()
        self.addCleanup(queue.shutdown)
        active = threading.Event()
        release = threading.Event()
        called = []

        def background():
            active.set()
            self.assertTrue(release.wait(5))

        running = queue.submit(background, priority=10)
        self.assertTrue(active.wait(2))
        latest = queue.submit(lambda: called.append("latest"), priority=10)
        next_episode = queue.submit(lambda: called.append("next"), priority=3)
        user = queue.submit(lambda: called.append("user"), priority=0)
        user.result(timeout=2)
        self.assertFalse(next_episode.running(), "prefetch occupied the reserved user slot")
        release.set()
        for future in (running, next_episode, latest):
            future.result(timeout=5)
        self.assertEqual(called, ["user", "next", "latest"])


if __name__ == "__main__":
    unittest.main()
