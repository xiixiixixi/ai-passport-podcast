"""/api/storage 只读用量接口验证：不改动任何缓存状态。"""
import shutil
import unittest

import test_pcm


@unittest.skipUnless(shutil.which("ffmpeg"), "ffmpeg is required for audio integration tests")
class StorageAPITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_pcm.PCMEndpointTests.setUpClass.__func__(cls)

    @classmethod
    def tearDownClass(cls):
        test_pcm.PCMEndpointTests.tearDownClass.__func__(cls)

    def setUp(self):
        test_pcm.PCMEndpointTests.setUp(self)
        self.server._prepare_jobs.clear()

    request = test_pcm.PCMEndpointTests.request

    def prepare(self):
        self.server.save_json(self.server.STATE, {"fixture/episode1": {"status": "ready", "segments": 1}})
        self.assertTrue(self.server.transcode("fixture", "episode1", "https://example.invalid/audio.mp3"))

    def test_storage_reports_usage_without_touching_state(self):
        episodes = [{"id": "episode1", "title": "缓存用量测试集", "duration": 5, "published": "2026-10-02T00:00:00+00:00",
                     "audio": "https://example.invalid/audio.mp3"}]
        self.server.save_json(self.server.EP_CACHE,
                              {"fixture": {"id": "fixture", "name": "fixture", "count": 1, "episodes": episodes}})
        self.prepare()
        before = self.server.load_json(self.server.STATE, {})
        response = self.request("/api/storage")
        self.assertEqual(response.status_code, 200)
        body = response.get_json()
        self.assertEqual(body["ready_episodes"], 1)
        self.assertGreater(body["media_bytes"], 0)
        self.assertGreater(body["disk_free_bytes"], 0)
        self.assertGreater(body["disk_total_bytes"], body["disk_free_bytes"])
        # 只读接口：查询前后状态文件不应变化。
        self.assertEqual(self.server.load_json(self.server.STATE, {}), before)

    def test_storage_counts_only_ready_episodes(self):
        self.server.save_json(self.server.STATE, {"fixture/a": {"status": "ready"}, "fixture/b": {"status": "failed"}})
        response = self.request("/api/storage")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.get_json()["ready_episodes"], 0)  # ready 但文件缺失按 missing 计。


if __name__ == "__main__":
    unittest.main()
