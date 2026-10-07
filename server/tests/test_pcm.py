"""真实 5 秒音频验证：python -m unittest discover -s tests -v。"""
import array
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from unittest import mock


@unittest.skipUnless(shutil.which("ffmpeg"), "ffmpeg is required for audio integration tests")
class PCMEndpointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # 把服务加载到临时目录：不会启动订阅抓取，也不写工程里的真实数据。
        cls.app_temp = tempfile.TemporaryDirectory(prefix="passport-pcm-app-")
        cls.app_root = Path(cls.app_temp.name)
        # A real deployment may set its public URL. Keep the fixture request host
        # independent of that environment while allowing individual tests to
        # explicitly exercise a configured public URL.
        cls.fixture_environment = mock.patch.dict(os.environ, {
            "PODCAST_NO_BG": "1", "PODCAST_PUBLIC_URL": "", "PODCAST_SETUP_KEY": ""})
        cls.fixture_environment.start()
        cls.addClassCleanup(cls.fixture_environment.stop)
        original = Path(__file__).resolve().parents[1] / "server.py"
        copied = cls.app_root / "server.py"
        copied.write_bytes(original.read_bytes())
        (cls.app_root / "config.json").write_text(json.dumps({"shows": [
            {"id": "fixture", "name": "fixture", "feed": ""}]}))
        spec = importlib.util.spec_from_file_location("relay_pcm_fixture", copied)
        cls.server = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.server)
        cls.server.app.config["TESTING"] = True
        cls.server.app.config["PODCAST_TEST_AUTH_DISABLED"] = True
        cls.fixture = cls.app_root / "source.mp3"
        subprocess.run([
            "ffmpeg", "-y", "-nostdin", "-v", "error", "-f", "lavfi", "-i",
            "aevalsrc=0.95*sin(2*PI*440*t)|0.95*sin(2*PI*440*t):s=44100:d=5",
            "-acodec", "libmp3lame", "-b:a", "64k", str(cls.fixture)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.app_temp.cleanup()

    def setUp(self):
        # This class's loaded server is shared; each fixture uses fresh storage.
        self.server.invalidate_configured_shows()
        self.temp = tempfile.TemporaryDirectory(prefix="passport-pcm-case-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.media = self.root / "media"
        self.directory = self.media / "fixture" / "episode1"
        self.directory.mkdir(parents=True)
        self.source = self.directory / "seg_000.mp3"
        shutil.copyfile(self.fixture, self.source)
        self.server.MEDIA = self.media
        self.server.STATE = self.root / "state.json"
        self.server.EP_CACHE = self.root / "episodes.json"
        self.server.DEVICE_COVERS = self.server.DeviceCoverCache(self.root,
            lambda url, maximum: (b"", url))  # Ordinary audio fixtures never fetch remote artwork.
        self.addCleanup(self.server.DEVICE_COVERS.close)
        self.cache_dir = self.directory / self.server.PCM_CACHE_VERSION
        self.target = self.cache_dir / "seg_000.pcm"
        self.url = "/pcm/fixture/episode1/seg_000.pcm"

    def prepare(self):
        self.server.save_json(self.server.STATE, {"fixture/episode1": {"status": "ready", "segments": 1}})
        self.assertTrue(self.server.transcode("fixture", "episode1", "https://example.invalid/audio.mp3"))

    def request(self, path=None, method="GET", **kwargs):
        with self.server.app.test_client() as client:
            response = client.open(path or self.url, method=method, buffered=True, **kwargs)
            response.get_data()
            response.close()
            return response

    def integrated_loudness(self, target):
        meter = subprocess.run([
            "ffmpeg", "-hide_banner", "-nostdin", "-f", "s16le", "-ar", "16000", "-ac", "1",
            "-i", str(target), "-af", "ebur128", "-f", "null", "-"],
            check=True, capture_output=True, text=True)
        measured = re.search(r"Integrated loudness:\s+I:\s+(-?[\d.]+) LUFS", meter.stderr)
        self.assertIsNotNone(measured, meter.stderr[-1000:])
        return float(measured.group(1))

    def test_audio_duration_gain_and_format_headers(self):
        self.prepare()
        response = self.request()
        self.assertEqual(response.status_code, 200)
        for name, value in {"X-Audio-Sample-Rate": "16000", "X-Audio-Channels": "1",
                            "X-Audio-Bits": "16", "X-Audio-Format": "s16le"}.items():
            self.assertEqual(response.headers[name], value)
        self.assertEqual(response.headers["Content-Type"], "application/octet-stream")
        self.assertEqual(int(response.headers["Content-Length"]), len(response.data))
        samples = array.array("h")
        samples.frombytes(response.data)
        if sys.byteorder != "little":
            samples.byteswap()
        self.assertAlmostEqual(len(samples) / 16000, 5.0, delta=0.03)
        peak = max(abs(value) for value in samples) / 32768
        self.assertGreater(peak, 0)
        self.assertLess(peak, 0.43)  # Headroom after normalization and volume trim.
        loudness = self.integrated_loudness(self.target)
        self.assertGreater(loudness, -28)
        self.assertLess(loudness, -20)
        self.assertEqual(response.data, self.target.read_bytes())

    def test_normalization_keeps_quiet_and_loud_recordings_at_similar_loudness(self):
        self.prepare()
        quieter = self.media / "fixture" / "quieter"
        quieter.mkdir()
        subprocess.run([
            "ffmpeg", "-y", "-nostdin", "-v", "error", "-i", str(self.fixture),
            "-af", "volume=0.1", "-c:a", "libmp3lame", "-b:a", "64k", str(quieter / "seg_000.mp3")],
            check=True)
        target = self.server.pcm_segment("fixture", "quieter", "seg_000.pcm")
        self.assertAlmostEqual(self.integrated_loudness(self.target), self.integrated_loudness(target), delta=1.0)
        self.assertEqual(target.stat().st_size, self.target.stat().st_size)
        samples = array.array("h")
        samples.frombytes(target.read_bytes())
        if sys.byteorder != "little":
            samples.byteswap()
        self.assertLess(max(abs(value) for value in samples) / 32768, 0.43)

    def test_cached_get_head_and_range_do_not_convert_again(self):
        self.prepare()
        first = self.request()
        before = self.target.stat().st_mtime_ns
        with mock.patch.object(self.server.subprocess, "run", side_effect=AssertionError("unexpected conversion")):
            cached = self.request()
            head = self.request(method="HEAD")
            partial = self.request(headers={"Range": "bytes=2-1025"})
        self.assertEqual(cached.data, first.data)
        self.assertEqual(self.target.stat().st_mtime_ns, before)
        self.assertEqual(head.status_code, 200)
        self.assertEqual(head.data, b"")
        self.assertEqual(head.headers["Content-Length"], first.headers["Content-Length"])
        self.assertEqual(head.headers["X-Audio-Sample-Rate"], "16000")
        self.assertEqual(partial.status_code, 206)
        self.assertEqual(partial.data, first.data[2:1026])
        self.assertEqual(partial.headers["Content-Length"], "1024")
        self.assertEqual(partial.headers["Content-Range"], f"bytes 2-1025/{len(first.data)}")

    def test_stale_or_incomplete_cache_is_rejected_then_worker_rebuilds(self):
        self.cache_dir.mkdir()
        for bad in (b"", b"odd", b"outdated"):
            with self.subTest(cache=bad):
                self.target.write_bytes(bad)
                os.utime(self.target, (1, 1))
                response = self.request()
                self.assertEqual(response.status_code, 503)
                self.prepare()
                response = self.request()
                self.assertEqual(response.status_code, 200)
                self.assertAlmostEqual(len(response.data) / 32000, 5.0, delta=0.03)

    def test_four_concurrent_preparation_calls_convert_only_once(self):
        barrier = threading.Barrier(4)
        original_run = subprocess.run

        def slow_convert(*args, **kwargs):
            time.sleep(0.05)
            return original_run(*args, **kwargs)

        def get():
            barrier.wait(timeout=5)
            target = self.server.pcm_segment("fixture", "episode1", "seg_000.pcm")
            return target.read_bytes()

        with mock.patch.object(self.server.subprocess, "run", side_effect=slow_convert) as run:
            with ThreadPoolExecutor(max_workers=4) as pool:
                responses = list(pool.map(lambda _: get(), range(4)))
        self.assertEqual(run.call_count, 1)
        self.assertTrue(all(data == responses[0] for data in responses))
        self.assertEqual(self.server._pcm_locks, {})
        self.assertEqual(list(self.cache_dir.glob("*.tmp")), [])

    def test_rejects_invalid_names_and_paths_without_conversion(self):
        paths = [
            "/pcm/fixture/episode1/seg_000.mp3",
            "/pcm/fixture/episode1/seg_00.pcm",
            "/pcm/fixture/episode1/seg_-01.pcm",
            "/pcm/fixture/episode1/seg_0000.pcm",
            "/pcm/fixture/episode1/other.pcm",
            "/pcm/fixture/../seg_000.pcm",
            "/pcm/../episode1/seg_000.pcm",
            "/pcm/fixture/episode1/%2e%2e%2fseg_000.pcm",
            "/pcm/fixture/episode%5c1/seg_000.pcm",
        ]
        with mock.patch.object(self.server.subprocess, "run", side_effect=AssertionError("invalid request converted")):
            for path in paths:
                with self.subTest(path=path):
                    self.assertIn(self.request(path).status_code, (400, 404))
        self.assertFalse(self.cache_dir.exists())

    def test_missing_segment_or_show_never_fetches_new_audio(self):
        with mock.patch.object(self.server, "fetch_audio", side_effect=AssertionError("unexpected download")):
            for path in ("/pcm/fixture/episode1/seg_001.pcm",
                         "/pcm/unknown/episode1/seg_000.pcm",
                         "/pcm/fixture/missing/seg_000.pcm"):
                with self.subTest(path=path):
                    self.assertEqual(self.request(path).status_code, 404)
        self.assertFalse(self.cache_dir.exists())

    def test_rejects_source_symlink(self):
        self.source.unlink()
        self.source.symlink_to(self.fixture)
        self.assertEqual(self.request().status_code, 400)
        self.assertFalse(self.cache_dir.exists())

    def test_rejects_episode_directory_symlink(self):
        real = self.root / "real_episode"
        self.directory.rename(real)
        self.directory.symlink_to(real, target_is_directory=True)
        self.assertEqual(self.request().status_code, 400)

    def test_rejects_show_directory_symlink(self):
        show_dir = self.media / "fixture"
        real = self.root / "real_show"
        show_dir.rename(real)
        show_dir.symlink_to(real, target_is_directory=True)
        self.assertEqual(self.request().status_code, 400)

    def test_rejects_media_root_symlink(self):
        real = self.root / "real_media"
        self.media.rename(real)
        self.media.symlink_to(real, target_is_directory=True)
        self.assertEqual(self.request().status_code, 400)

    def test_rejects_cache_directory_symlink(self):
        outside = self.root / "outside"
        outside.mkdir()
        self.cache_dir.symlink_to(outside, target_is_directory=True)
        self.assertEqual(self.request().status_code, 400)
        self.assertEqual(list(outside.iterdir()), [])

    def test_rejects_cache_file_symlink(self):
        self.cache_dir.mkdir()
        outside = self.root / "outside.pcm"
        outside.write_bytes(b"protected")
        self.target.symlink_to(outside)
        self.assertEqual(self.request().status_code, 400)
        self.assertEqual(outside.read_bytes(), b"protected")

    def test_invalid_mp3_fails_without_publishing_partial_pcm(self):
        self.source.write_bytes(b"this is not audio")
        with self.assertRaises(RuntimeError):
            self.server.pcm_segment("fixture", "episode1", "seg_000.pcm")
        self.assertEqual(self.request().status_code, 503)
        self.assertFalse(self.target.exists())
        self.assertEqual(list(self.cache_dir.iterdir()), [])
        self.assertEqual(self.server._pcm_locks, {})

    def test_conversion_timeout_cleans_partial_pcm_and_can_retry(self):
        def timeout(cmd, **kwargs):
            Path(cmd[-1]).write_bytes(b"half written")
            raise subprocess.TimeoutExpired(cmd, kwargs["timeout"])

        with mock.patch.object(self.server.subprocess, "run", side_effect=timeout):
            with self.assertRaises(subprocess.TimeoutExpired):
                self.server.pcm_segment("fixture", "episode1", "seg_000.pcm")
        self.assertFalse(self.target.exists())
        self.assertEqual(list(self.cache_dir.iterdir()), [])
        self.assertEqual(self.server._pcm_locks, {})
        self.prepare()
        self.assertEqual(self.request().status_code, 200)

    def test_failed_refresh_keeps_existing_cache_untouched(self):
        self.prepare()
        first = self.request()
        os.utime(self.target, (1, 1))
        with mock.patch.object(self.server.subprocess, "run", return_value=subprocess.CompletedProcess([], 1)):
            with self.assertRaises(RuntimeError):
                self.server.pcm_segment("fixture", "episode1", "seg_000.pcm")
        self.assertEqual(self.request().status_code, 503)
        self.assertEqual(self.target.read_bytes(), first.data)
        self.assertEqual(list(self.cache_dir.glob("*.tmp")), [])

    def test_original_mp3_playback_remains_available(self):
        self.prepare()
        self.assertEqual(self.request().status_code, 200)
        response = self.request("/media/fixture/episode1/seg_000.mp3")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.data, self.source.read_bytes())
        self.assertEqual(response.headers["Content-Type"], "audio/mpeg")


if __name__ == "__main__":
    unittest.main()
