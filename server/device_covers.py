"""Bounded, background preparation of podcast artwork for the small device.

The device receives one fixed-size image from its own authorized backend;
neither image decoding nor third-party HTTP belongs on the audio/UI task.
"""
import hashlib
import json
import os
import re
import struct
import subprocess
import tempfile
import threading
import time
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from listening_store import ListeningError

SIDE = 52
PIXEL_BYTES = SIDE * SIDE * 2
WIRE_BYTES = 32 + PIXEL_BYTES
MAX_ORIGINAL = 8 * 1024 * 1024
MAX_PENDING = 32
MAX_TRACKED = 128


def valid_id(value):
    return isinstance(value, str) and bool(re.fullmatch(r"[A-Za-z0-9_-]{1,23}", value))


def pack_pixels(pixels):
    if len(pixels) != PIXEL_BYTES:
        raise ValueError("incorrect device cover dimensions")
    return (struct.pack("<4sHHII", b"PDC1", SIDE, SIDE, PIXEL_BYTES,
                        zlib.crc32(pixels)) + hashlib.sha256(pixels).digest()[:16] + pixels)


def valid_wire(blob):
    if len(blob) != WIRE_BYTES:
        return False
    magic, width, height, count, checksum = struct.unpack("<4sHHII", blob[:16])
    pixels = blob[32:]
    return (magic == b"PDC1" and width == height == SIDE and count == PIXEL_BYTES
            and checksum == zlib.crc32(pixels)
            and blob[16:32] == hashlib.sha256(pixels).digest()[:16])


def convert_image(original):
    if not original or len(original) > MAX_ORIGINAL:
        raise ValueError("original artwork is missing or too large")
    # Existing server dependency. Fixed output, one frame, bounded time and
    # individual decoder allocations; malformed input never enters the cache.
    result = subprocess.run([
        "ffmpeg", "-nostdin", "-v", "error", "-threads", "1", "-max_alloc", "16777216",
        "-i", "pipe:0", "-vf",
        "scale=52:52:force_original_aspect_ratio=increase,crop=52:52,format=rgb565le",
        "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb565le", "pipe:1"],
        input=original, capture_output=True, timeout=12, check=True)
    return pack_pixels(result.stdout)


def compress_for_web(image):
    """把原图压成网页用的小图：最长边512、质量档8；能看清且尽量小。

    压缩失败或反而更大时保留原字节，不影响设备格式转换。"""
    if not image or len(image) <= 96 * 1024:
        return image
    try:
        result = subprocess.run([
            "ffmpeg", "-nostdin", "-v", "error", "-threads", "1", "-max_alloc", "16777216",
            "-i", "pipe:0", "-vf", "scale=512:512:force_original_aspect_ratio=decrease",
            "-q:v", "8", "-frames:v", "1", "-f", "mjpeg", "pipe:1"],
            input=image, capture_output=True, timeout=12, check=True)
        return result.stdout if 0 < len(result.stdout) < len(image) else image
    except (OSError, subprocess.SubprocessError):
        return image


def atomic_write(path, content):
    if path.is_symlink():
        raise ValueError("artwork cache must not contain symbolic links")
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".cover-", delete=False) as out:
        temporary = Path(out.name)
        try:
            out.write(content)
            out.flush()
            os.fsync(out.fileno())
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)


class DeviceCoverCache:
    def __init__(self, data, reader, converter=convert_image, clock=time.monotonic):
        self.data = Path(data)
        self.directory = self.data / "device-covers-v1"
        if self.directory.is_symlink():
            raise ValueError("unsafe artwork cache directory")
        self.directory.mkdir(parents=True, exist_ok=True)
        self.reader, self.converter, self.clock = reader, converter, clock
        self.lock = threading.Lock()
        self.executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="device-art")
        self.pending, self.desired, self.retry = {}, {}, {}

    def _paths(self, show_id):
        if not valid_id(show_id):
            raise ValueError("invalid artwork show identifier")
        return (self.directory / (show_id + ".bin"),
                self.directory / (show_id + ".json"),
                self.data / ("art_" + show_id + ".img"))

    @staticmethod
    def _url_key(url):
        return hashlib.sha256(url.encode("utf-8")).hexdigest()

    def _ready(self, show_id, url):
        binary, metadata, original = self._paths(show_id)
        if any(p.is_symlink() for p in (binary, metadata, original)):
            return None
        try:
            if metadata.stat().st_size > 1024 or binary.stat().st_size != WIRE_BYTES:
                return None
            meta = json.loads(metadata.read_text())
            blob = binary.read_bytes()
            if isinstance(meta, dict) and meta.get("url_sha256") == self._url_key(url) and valid_wire(blob):
                return blob
        except (OSError, ValueError, TypeError):
            pass
        return None

    def request(self, show_id, url):
        if not valid_id(show_id) or not isinstance(url, str) or not url or len(url) > 4096:
            return False
        with self.lock:
            if show_id not in self.desired and len(self.desired) >= MAX_TRACKED:
                evict = next((key for key in self.desired if key not in self.pending), None)
                if evict is None:
                    return False
                self.desired.pop(evict)
                self.retry = {key: value for key, value in self.retry.items() if key[0] != evict}
            self.desired[show_id] = url
            if self._ready(show_id, url):
                return True
            if (show_id in self.pending or len(self.pending) >= MAX_PENDING
                    or self.retry.get((show_id, url), 0) > self.clock()):
                return False
            # Mark before submitting: a fast worker cannot leave a stale slot.
            self.pending[show_id] = url
            self.executor.submit(self._prepare, show_id, url)
        return False

    def forget(self, show_id):
        """退订节目：移除跟踪状态与已生成的封面缓存文件。"""
        if not valid_id(show_id):
            return
        with self.lock:
            self.desired.pop(show_id, None)
            self.pending.pop(show_id, None)
            self.retry = {key: value for key, value in self.retry.items() if key[0] != show_id}
        blob, metadata, original = self._paths(show_id)
        for path in (blob, metadata, original):
            try:
                path.unlink(missing_ok=True)
            except OSError:
                pass

    def snapshot(self, show_id, url):
        self.request(show_id, url)
        if not valid_id(show_id) or not url:
            return None
        with self.lock:
            return self._ready(show_id, url)

    def original(self, show_id, url):
        if self.snapshot(show_id, url) is None:
            return None
        path = self._paths(show_id)[2]
        return path if path.is_file() and not path.is_symlink() else None

    def _prepare(self, show_id, url):
        binary, metadata, original = self._paths(show_id)
        try:
            if any(p.is_symlink() for p in (binary, metadata, original)):
                raise ValueError("unsafe artwork cache entry")
            # Adopt a browser's already downloaded image once. A known changed
            # artwork URL must fetch again rather than reuse the old logo.
            adopt = not metadata.exists() and original.is_file()
            if adopt and 0 < original.stat().st_size <= MAX_ORIGINAL:
                image = original.read_bytes()
            else:
                image, _ = self.reader(url, MAX_ORIGINAL)
            if not image or len(image) > MAX_ORIGINAL:
                raise ValueError("invalid artwork size")
            image = compress_for_web(image)
            try:
                blob = self.converter(image)
            except (OSError, ValueError, subprocess.SubprocessError):
                if not adopt:
                    raise
                image, _ = self.reader(url, MAX_ORIGINAL)
                if not image or len(image) > MAX_ORIGINAL:
                    raise ValueError("invalid artwork size")
                image = compress_for_web(image)
                blob = self.converter(image)
            if not valid_wire(blob):
                raise ValueError("invalid converted artwork")
            with self.lock:
                if self.desired.get(show_id) == url:
                    atomic_write(original, image)
                    atomic_write(binary, blob)
                    atomic_write(metadata, json.dumps({"url_sha256": self._url_key(url),
                                 "version": blob[16:32].hex()}, separators=(",", ":")).encode())
        except (OSError, ValueError, ListeningError, subprocess.SubprocessError):
            with self.lock:
                self.retry[(show_id, url)] = self.clock() + 60
        finally:
            with self.lock:
                self.pending.pop(show_id, None)
                latest = self.desired.get(show_id)
            if latest and latest != url:
                self.request(show_id, latest)

    def close(self):
        self.executor.shutdown(wait=True)
