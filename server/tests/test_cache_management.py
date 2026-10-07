"""缓存管理（任务2c）：设置、预缓存顺序、点播插队与清理保护名单验证。"""
import shutil
import threading
import time
import unittest
from concurrent.futures import Future
from unittest import mock

import test_pcm


@unittest.skipUnless(shutil.which("ffmpeg"), "ffmpeg is required for audio integration tests")
class CacheManagementTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_pcm.PCMEndpointTests.setUpClass.__func__(cls)

    @classmethod
    def tearDownClass(cls):
        test_pcm.PCMEndpointTests.tearDownClass.__func__(cls)

    def setUp(self):
        test_pcm.PCMEndpointTests.setUp(self)
        self.server._prepare_jobs.clear()
        self.server._pinned_keys.clear()
        self.server.save_json(self.server.SETTINGS_FILE, {})
        # episode1 最新、编号越大越早；与产品页"最新在前"的排序一致。
        self.episodes = [
            {"id": f"episode{index + 1}", "title": f"第{index + 1}集", "duration": 3600,
             "published": f"2026-10-{7 - index:02d}T00:00:00Z", "audio": "https://example.invalid/a.mp3"}
            for index in range(7)]
        self.server.save_json(self.server.EP_CACHE, {
            "fixture": {"id": "fixture", "name": "fixture", "count": 7, "episodes": self.episodes}})

    request = test_pcm.PCMEndpointTests.request
    prepare = test_pcm.PCMEndpointTests.prepare

    def drain_jobs(self, timeout=20):
        """等真实准备任务结束，避免清理临时目录时后台线程仍在写文件。"""
        with self.server._prepare_lock:
            jobs = list(self.server._prepare_jobs.values())
        deadline = time.time() + timeout
        for job in jobs:
            try:
                job.result(timeout=max(0.1, deadline - time.time()))
            except Exception:
                pass

    def protect(self, active=(), in_progress=(), recent=()):
        patch = mock.patch.object(self.server.LISTENING, "cleanup_protection", return_value={
            "active": {tuple(item) for item in active},
            "in_progress": {tuple(item) for item in in_progress},
            "recent": {tuple(item) for item in recent}})
        patch.start()
        self.addCleanup(patch.stop)

    def occupy(self, key):
        future = Future()
        with self.server._prepare_lock:
            self.server._prepare_jobs[key] = future
            self.server._pinned_keys.add(key)
        self.addCleanup(self.server._prepare_jobs.clear)
        self.addCleanup(self.server._pinned_keys.clear)
        return future

    def test_settings_store_clamps_bad_values_and_reports_defaults(self):
        self.assertEqual(self.server.load_settings(),
                         {"prefetch_latest": 3, "keep_per_show": 3, "low_free_gb": 16})
        self.server.save_json(self.server.SETTINGS_FILE,
                              {"prefetch_latest": "broken", "keep_per_show": 999, "low_free_gb": 5})
        self.assertEqual(self.server.load_settings(),
                         {"prefetch_latest": 3, "keep_per_show": 60, "low_free_gb": 5})

    def test_settings_api_roundtrip_and_validation(self):
        with mock.patch.object(self.server, "queue_latest_episodes"):  # 不真的补预缓存
            body = self.request("/api/settings").get_json()
            self.assertEqual(body["prefetch_latest"], 3)
            self.assertEqual(body["last_cleanup"], {})
            response = self.request("/api/settings", method="PUT",
                                    json={"prefetch_latest": 5, "keep_per_show": 3})
            self.assertEqual(response.status_code, 200)
            self.assertEqual(response.get_json()["prefetch_latest"], 5)
            self.assertEqual(self.request("/api/settings").get_json()["prefetch_latest"], 5)
        for bad in ({"prefetch_latest": 11}, {"prefetch_latest": True}, {"keep_per_show": 0}, {}):
            self.assertEqual(
                self.request("/api/settings", method="PUT", json=bad).status_code, 400)
        # 恢复默认，避免影响同批其他用例。
        self.server.save_json(self.server.SETTINGS_FILE, {})

    def test_prefetch_follows_settings_and_orders_newest_first(self):
        self.server.save_json(self.server.SETTINGS_FILE, {"prefetch_latest": 2})
        calls = []
        with mock.patch.object(self.server, "queue_preparation",
                               side_effect=lambda sid, episode, priority: calls.append(
                                   (sid, episode["id"], priority)) or "queued"):
            self.server.queue_latest_episodes()
        # 最新一集 priority 10，次新 11；更老的集不自动准备。
        self.assertEqual(calls, [("fixture", "episode1", 10), ("fixture", "episode2", 11)])

    def test_queue_preparation_never_downgrades_ready_episode(self):
        """坑①：PCM 完整只缺网页整集时，一次 prepare 不能把 ready 打回 queued。"""
        self.prepare()
        key = "fixture/episode1"
        self.assertEqual(self.server.get_state()[key]["status"], "ready")
        (self.directory / "stream.mp3").unlink(missing_ok=True)
        (self.directory / ".browser_complete_v1.json").unlink(missing_ok=True)
        self.assertEqual(self.server.queue_preparation("fixture", self.episodes[0], priority=0), "ready")
        self.assertEqual(self.server.get_state()[key]["status"], "ready")  # 不降级
        self.assertEqual(self.server.get_state()[key]["browser_status"], "queued")
        with self.server._prepare_lock:
            future = self.server._prepare_jobs[key]
        self.assertTrue(future.result(timeout=30))
        self.drain_jobs()
        state = self.server.get_state()[key]
        self.assertEqual(state["status"], "ready")
        self.assertTrue((self.directory / "stream.mp3").is_file())  # 网页整集已补齐

    def test_on_demand_click_pins_episode_for_cleanup(self):
        self.occupy("fixture/episode3")  # 前面还有一个点播任务
        self.server.save_json(self.server.STATE, {"fixture/episode1": {"status": "queued"}})
        with mock.patch.object(self.server._pool, "submit", return_value=Future()) as submit:
            self.server.queue_preparation("fixture", self.episodes[0], priority=0)
        submit.assert_called_once()
        with self.server._prepare_lock:
            self.assertIn("fixture/episode1", self.server._pinned_keys)
            self.assertIn("fixture/episode1", self.server._prepare_jobs)
        eta, ahead = self.server.prepare_estimate("fixture", "episode1", 3600)
        self.assertEqual(ahead, 1)
        self.assertGreaterEqual(eta, self.server.prepare_base_seconds(3600)
                                + self.server.prepare_base_seconds(3600))

    def test_pool_promotion_reorders_waiting_tasks(self):
        gate = threading.Event()
        order = []

        def blocker_task():
            gate.wait(5)
            order.append("blocker")

        def quick_task():
            order.append("target")

        blocker = self.server._pool.submit(blocker_task, priority=10)
        deadline = time.time() + 2
        while self.server._pool.stats()["running"] < 1 and time.time() < deadline:
            time.sleep(0.02)
        target = self.server._pool.submit(quick_task, priority=10)  # 只能在后台槽后排
        self.assertEqual(self.server._pool.stats(), {"queued": 1, "on_demand": 0, "running": 1})
        self.server._pool.promote(target, 0)  # 点播提升到专用槽
        target.result(timeout=5)              # blocker 仍被闸挡住，target 已先跑完
        self.assertEqual(order, ["target"])
        gate.set()
        blocker.result(timeout=5)
        self.assertEqual(order, ["target", "blocker"])

    def test_cleanup_keeps_newest_protects_progress_and_inflight(self):
        for episode in self.episodes:
            directory = self.server.ep_dir("fixture", episode["id"])
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "seg_000.mp3").write_bytes(b"x")
        state = {f"fixture/{episode['id']}": {"status": "ready", "segments": 1}
                 for episode in self.episodes}
        state["fixture/gone"] = {"status": "ready", "segments": 1}  # 目录早已不存在的残留
        self.server.save_json(self.server.STATE, state)
        self.protect(in_progress=[("fixture", "episode5")], recent=[("fixture", "episode6")])
        self.occupy("fixture/episode7")
        result = self.server.cleanup_media()
        self.assertEqual(result["keep"], 3)
        # 只有第 4 集既不在保护名单（未听完/近两周听过/准备中）也不在最新 3 集里。
        self.assertEqual(result["deleted"], 1)
        for episode in self.episodes[:3] + self.episodes[4:7]:
            self.assertTrue(self.server.ep_dir("fixture", episode["id"]).is_dir())
        self.assertFalse(self.server.ep_dir("fixture", "episode4").is_dir())
        fresh = self.server.get_state()
        self.assertNotIn("fixture/episode4", fresh)  # 删除的集状态一并清走
        self.assertNotIn("fixture/gone", fresh)      # 残留条目也清走
        self.assertIn("fixture/episode1", fresh)
        self.assertEqual(fresh["__last_cleanup__"]["deleted"], 1)

    def test_cleanup_keep_override_for_low_disk(self):
        for episode in self.episodes:
            directory = self.server.ep_dir("fixture", episode["id"])
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "seg_000.mp3").write_bytes(b"x")
        self.server.save_json(self.server.STATE, {
            f"fixture/{episode['id']}": {"status": "ready", "segments": 1}
            for episode in self.episodes})
        self.protect()
        result = self.server.cleanup_media(keep_override=1)
        self.assertEqual(result["deleted"], 6)  # 磁盘吃紧时每档只保最新 1 集
        self.assertTrue(self.server.ep_dir("fixture", "episode1").is_dir())

    def test_clear_media_cache_keeps_active_and_inflight_only(self):
        for episode in self.episodes[:3]:
            directory = self.server.ep_dir("fixture", episode["id"])
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "seg_000.mp3").write_bytes(b"x")
        self.server.save_json(self.server.STATE, {
            f"fixture/{episode['id']}": {"status": "ready", "segments": 1}
            for episode in self.episodes[:3]})
        self.protect(active=[("fixture", "episode1")])
        self.occupy("fixture/episode2")
        result = self.server.clear_media_cache()
        self.assertEqual(result["deleted"], 1)
        self.assertTrue(self.server.ep_dir("fixture", "episode1").is_dir())   # 正在播放
        self.assertTrue(self.server.ep_dir("fixture", "episode2").is_dir())   # 准备中
        self.assertFalse(self.server.ep_dir("fixture", "episode3").is_dir())
        self.assertNotIn("fixture/episode3", self.server.get_state())

    def test_maintenance_clear_requires_confirmation_and_runs(self):
        self.assertEqual(
            self.request("/api/maintenance/clear", method="POST", json={"confirm": "yes"}).status_code, 400)
        self.protect()
        self.assertEqual(
            self.request("/api/maintenance/clear", method="POST", json={"confirm": "clear"}).status_code, 200)
        deadline = time.time() + 10
        while self.server._maintenance["running"] and time.time() < deadline:
            time.sleep(0.05)
        self.assertFalse(self.server._maintenance["running"])
        self.assertEqual(self.server._maintenance["error"], "")
        self.assertIn("deleted", self.server._maintenance["result"])
        status = self.request("/api/maintenance").get_json()
        self.assertFalse(status["running"])

    def test_prepare_endpoint_reports_eta_for_on_demand(self):
        response = self.request("/api/prepare", method="POST",
                                json={"show_id": "fixture", "episode_id": "episode2"})
        body = response.get_json()
        self.assertEqual(response.status_code, 200)
        self.assertTrue(body["pinned"])
        self.assertGreaterEqual(body["eta_seconds"], self.server.prepare_base_seconds(3600))
        prefetch = self.request("/api/prepare", method="POST",
                                json={"show_id": "fixture", "episode_id": "episode3", "prefetch": True}).get_json()
        self.assertFalse(prefetch["pinned"])
        self.assertEqual(prefetch["eta_seconds"], 0)
        self.drain_jobs()

    def test_shows_endpoint_counts_cached_episodes_from_state(self):
        self.server.save_json(self.server.STATE, {
            "fixture/episode1": {"status": "ready"}, "fixture/episode2": {"status": "ready"},
            "fixture/episode3": {"status": "transcoding"}})
        body = self.request("/api/shows").get_json()
        show = next(item for item in body["shows"] if item["id"] == "fixture")
        self.assertEqual(show["cached_episodes"], 2)

    def test_next_cleanup_delay_targets_three_am_beijing(self):
        tz = self.server.CLEANUP_TZ
        at = lambda *parts: self.server.datetime(*parts, tzinfo=tz)
        self.assertEqual(self.server.next_cleanup_delay(at(2026, 10, 7, 2, 0)), 4200)
        self.assertEqual(self.server.next_cleanup_delay(at(2026, 10, 7, 3, 5)), 300)
        self.assertAlmostEqual(self.server.next_cleanup_delay(at(2026, 10, 7, 3, 10, 1)), 86399, delta=2)

    def test_settings_change_refills_prefetch_in_background(self):
        targets = []
        with mock.patch.object(self.server.threading, "Thread",
                               side_effect=lambda **kwargs: targets.append(kwargs["target"]) or mock.Mock()), \
                mock.patch.object(self.server, "queue_latest_episodes") as refill:
            response = self.request("/api/settings", method="PUT", json={"prefetch_latest": 5})
        self.assertEqual(response.status_code, 200)
        targets[-1]()  # 后台线程目标：按新设置补预缓存
        refill.assert_called_once_with()
        self.server.save_json(self.server.SETTINGS_FILE, {})


if __name__ == "__main__":
    unittest.main()
