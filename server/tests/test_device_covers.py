"""Real fixed-size artwork, cache invalidation and nonblocking device reads."""
import hashlib
import shutil
import struct
import tempfile
import threading
import time
import unittest
import zlib
from pathlib import Path
from unittest import mock

from device_covers import (DeviceCoverCache, PIXEL_BYTES, WIRE_BYTES,
                           convert_image, pack_pixels, valid_wire)
from listening_store import ListeningError


def ppm(red, green, blue, width=80, height=40):
    return f"P6\n{width} {height}\n255\n".encode() + bytes((red, green, blue)) * width * height


class DeviceCoverTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="passport-art-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def cache(self, reader, **kwargs):
        cache = DeviceCoverCache(self.root, reader, **kwargs)
        self.addCleanup(cache.close)
        return cache

    def wait_ready(self, cache, url, show="src_dynamic"):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            blob = cache.snapshot(show, url)
            if blob:
                return blob
            time.sleep(.01)
        self.fail("artwork did not become ready")

    @unittest.skipUnless(shutil.which("ffmpeg"), "server image tool is required")
    def test_real_image_is_center_cropped_fixed_rgb565_and_self_validating(self):
        blob = convert_image(ppm(255, 0, 0))
        self.assertEqual(len(blob), WIRE_BYTES)
        self.assertEqual(struct.unpack("<4sHHII", blob[:16]),
                         (b"PDC1", 52, 52, PIXEL_BYTES, zlib.crc32(blob[32:])))
        self.assertEqual(blob[16:32], hashlib.sha256(blob[32:]).digest()[:16])
        self.assertTrue(valid_wire(blob))
        # Red is in the high five bits, independent of host endianness.
        pixel = int.from_bytes(blob[32:34], "little")
        self.assertGreater(pixel >> 11, 28)
        self.assertLess(pixel & 31, 3)
        for invalid in (blob[:-1], blob + b"x", b"FAIL" + blob[4:],
                        blob[:20] + bytes([blob[20] ^ 1]) + blob[21:],
                        blob[:-1] + bytes([blob[-1] ^ 1])):
            self.assertFalse(valid_wire(invalid))

    @unittest.skipUnless(shutil.which("ffmpeg"), "server image tool is required")
    def test_new_source_automatic_background_conversion_reboot_and_changed_url(self):
        reader = mock.Mock(side_effect=lambda url, limit: (ppm(255, 0, 0) if url.endswith("red")
                                                         else ppm(0, 0, 255), url))
        cache = self.cache(reader)
        red = self.wait_ready(cache, "https://public.example/red")
        self.assertEqual(reader.call_count, 1)
        self.assertEqual(cache.snapshot("src_dynamic", "https://public.example/red"), red)
        self.assertEqual(reader.call_count, 1)
        cache.close()
        reopened = self.cache(mock.Mock(side_effect=AssertionError("redownloaded a persisted cover")))
        self.assertEqual(reopened.snapshot("src_dynamic", "https://public.example/red"), red)
        blue = self.wait_ready(cache := self.cache(reader), "https://public.example/blue")
        self.assertNotEqual(red[16:32], blue[16:32])
        self.assertEqual(reader.call_count, 2)
        self.assertEqual(cache.original("src_dynamic", "https://public.example/blue").read_bytes(), ppm(0, 0, 255))

    @unittest.skipUnless(shutil.which("ffmpeg"), "server image tool is required")
    def test_adopts_browser_art_and_repairs_corrupt_browser_cache(self):
        (self.root / "art_src_dynamic.img").write_bytes(ppm(0, 255, 0))
        reader = mock.Mock(return_value=(ppm(0, 0, 255), "https://public.example/art"))
        first = self.cache(reader)
        self.wait_ready(first, "https://public.example/art")
        reader.assert_not_called()
        (self.root / "art_other.img").write_bytes(b"not an image")
        self.wait_ready(first, "https://public.example/art", "other")
        self.assertEqual(reader.call_count, 1)

    def test_slow_fetch_never_blocks_read_and_old_request_cannot_overwrite_new_logo(self):
        started, release = threading.Event(), threading.Event()
        def reader(url, limit):
            if url.endswith("old"):
                started.set()
                release.wait(3)
            return (url.encode(), url)
        converter = lambda data: pack_pixels(bytes([1 if data.endswith(b"old") else 2]) * PIXEL_BYTES)
        cache = self.cache(reader, converter=converter)
        self.addCleanup(release.set)
        self.assertIsNone(cache.snapshot("src_dynamic", "https://public.example/old"))
        self.assertTrue(started.wait(2))
        before = time.monotonic()
        self.assertIsNone(cache.snapshot("src_dynamic", "https://public.example/new"))
        self.assertLess(time.monotonic() - before, .1)
        release.set()
        blob = self.wait_ready(cache, "https://public.example/new")
        self.assertEqual(blob[32], 2)
        self.assertIsNone(cache._ready("src_dynamic", "https://public.example/old"))

    def test_bad_network_data_retries_later_and_identifiers_cannot_escape_cache(self):
        clock = [1]
        reader = mock.Mock(side_effect=ListeningError("blocked public image"))
        cache = self.cache(reader, converter=lambda data: pack_pixels(data), clock=lambda: clock[0])
        cache.request("src_dynamic", "https://public.example/art")
        deadline = time.monotonic() + 2
        while cache.pending and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertIsNone(cache.snapshot("src_dynamic", "https://public.example/art"))
        self.assertEqual(reader.call_count, 1)
        for show in ("../escape", "a/b", "a" * 24, "", None):
            self.assertFalse(cache.request(show, "https://public.example/art"))
        outside = self.root / "outside"
        outside.write_bytes(b"keep")
        (cache.directory / "linked.bin").symlink_to(outside)
        cache.request("linked", "https://public.example/art")
        cache.close()
        self.assertEqual(outside.read_bytes(), b"keep")


if __name__ == "__main__":
    unittest.main()
