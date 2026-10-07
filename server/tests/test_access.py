"""Real persistent authorization, one-use pairing, and authenticated audio paths."""
import copy
import json
import os
import secrets
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest import mock

from access_store import AccessStore, digest, PAIR_SECONDS, SESSION_SECONDS
from listening_store import ListeningError, ListeningStore
from device_covers import DeviceCoverCache, WIRE_BYTES, valid_wire
from test_release_api import RSS
from test_device_covers import ppm
import source_manager
import test_pcm


class AccessStoreTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.now = 1800000000
        self.path = Path(self.directory.name) / "access.sqlite3"
        self.key = "a" * 64
        self.password = "this is a private household password"
        self.store = AccessStore(self.path, self.key, clock=lambda: self.now)
        self.admin = self.store.setup(self.key, self.password, "local")

    def claim(self, identifier="device-one"):
        return self.store.claim(self.store.pairing()["code"], identifier, "卧室播客机", "local")

    def rejected(self, function, status, *args):
        with self.assertRaises(ListeningError) as caught:
            function(*args)
        self.assertEqual(caught.exception.status, status)

    def test_administrator_setup_login_password_hash_and_session_survive_restart(self):
        self.assertTrue(self.store.administrator(self.admin))
        self.rejected(self.store.setup, 409, self.key, self.password, "local")
        self.rejected(self.store.login, 401, "wrong password", "local")
        token = self.store.login(self.password, "local")
        reopened = AccessStore(self.path, self.key, clock=lambda: self.now)
        self.assertTrue(reopened.administrator(token))
        with reopened.connection() as db:
            raw = " ".join(str(tuple(row)) for table in ("settings", "browser_sessions") for row in db.execute(f"SELECT * FROM {table}"))
        for value in (self.key, self.password, self.admin, token):
            self.assertNotIn(value, raw)
        reopened.logout(token)
        self.assertFalse(self.store.administrator(token))
        self.now += SESSION_SECONDS
        self.assertFalse(self.store.administrator(self.admin))

    def test_initial_setup_cannot_be_taken_without_installation_key(self):
        fresh = AccessStore(Path(self.directory.name) / "fresh.sqlite3", self.key)
        self.rejected(fresh.setup, 403, "wrong", self.password, "local")
        self.assertFalse(fresh.initialized())
        empty = AccessStore(Path(self.directory.name) / "empty.sqlite3")
        self.rejected(empty.setup, 503, self.key, self.password, "local")
        self.rejected(fresh.setup, 400, self.key, "short", "local")

    def test_pairing_code_is_single_use_expiring_and_new_code_invalidates_old(self):
        old = self.store.pairing()["code"]
        new = self.store.pairing()["code"]
        self.rejected(self.store.claim, 401, old, "device-one", None, "local")
        token = self.store.claim(new, "device-one", None, "local")
        self.rejected(self.store.claim, 401, new, "device-two", None, "local")
        self.assertEqual(len(token["token"]), 64)
        expires = self.store.pairing()["code"]
        self.now += PAIR_SECONDS
        self.rejected(self.store.claim, 401, expires, "device-three", None, "local")

    def test_concurrent_claim_consumes_code_once(self):
        code = self.store.pairing()["code"]
        def claim(number):
            try:
                return self.store.claim(code, f"device-{number}", None, f"host-{number}")
            except ListeningError as error:
                return error.status
        with ThreadPoolExecutor(max_workers=6) as pool:
            replies = list(pool.map(claim, range(6)))
        self.assertEqual(sum(isinstance(reply, dict) for reply in replies), 1)
        self.assertEqual(sum(reply == 401 for reply in replies), 5)
        self.assertEqual(len(self.store.devices()), 1)

    def test_pending_token_does_not_break_old_configuration_and_survives_expiry_and_restart(self):
        old = self.claim()["token"]
        self.assertEqual(self.store.device(old), "device-one")
        pending = self.claim()["token"]
        self.assertEqual(self.store.device(old), "device-one")
        self.assertTrue(self.store.devices()[0]["pending"])
        self.now += PAIR_SECONDS * 3
        reopened = AccessStore(self.path, self.key, clock=lambda: self.now)
        self.assertEqual(reopened.device(old), "device-one")
        self.assertEqual(reopened.device(pending), "device-one")
        self.assertFalse(reopened.devices()[0]["pending"])
        self.assertIsNone(reopened.device(old))

    def test_new_claim_overwrites_only_pending_and_activation_is_atomic(self):
        old = self.claim()["token"]
        self.store.device(old)
        lost = self.claim()["token"]
        new = self.claim()["token"]
        self.assertIsNone(self.store.device(lost))
        self.assertEqual(self.store.device(old), "device-one")
        with ThreadPoolExecutor(max_workers=8) as pool:
            replies = list(pool.map(lambda _: self.store.device(new), range(8)))
        self.assertEqual(replies, ["device-one"] * 8)
        self.assertIsNone(self.store.device(old))

    def test_revocation_rejects_current_and_pending_without_erasing_other_device(self):
        old = self.claim()["token"]
        self.store.device(old)
        pending = self.claim()["token"]
        other = self.claim("device-two")["token"]
        self.store.revoke("device-one")
        self.assertIsNone(self.store.device(old))
        self.assertIsNone(self.store.device(pending))
        self.assertEqual(self.store.device(other), "device-two")
        restored = self.claim()["token"]
        self.assertIsNone(self.store.device(old))
        self.assertEqual(self.store.device(restored), "device-one")

    def test_revocation_and_activation_cannot_race_and_resurrect_device(self):
        for index in range(5):
            identifier = f"device-race-{index}"
            pending = self.claim(identifier)["token"]
            with ThreadPoolExecutor(max_workers=2) as pool:
                activate = pool.submit(self.store.device, pending)
                revoke = pool.submit(self.store.revoke, identifier)
                activate.result(); revoke.result()
            self.assertIsNone(self.store.device(pending))

    def test_plain_tokens_codes_and_password_never_appear_in_registry(self):
        code = self.store.pairing()["code"]
        token = self.store.claim(code, "device-one", None, "local")["token"]
        with self.store.connection() as db:
            tables = [row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")]
            raw = " ".join(str(tuple(row)) for table in tables for row in db.execute(f"SELECT * FROM {table}"))
            self.assertEqual(db.execute("SELECT code_hash FROM pairings").fetchone()[0], digest(code))
        for value in (token, self.password, self.admin):
            self.assertNotIn(value, raw)
        serialized = json.dumps(self.store.devices())
        self.assertNotIn("hash", serialized)
        self.assertNotIn("token", serialized)

    def test_guessing_limits_persist_across_restart(self):
        for _ in range(8):
            self.rejected(self.store.login, 401, "wrong", "attacker")
        reopened = AccessStore(self.path, self.key, clock=lambda: self.now)
        self.rejected(reopened.login, 429, self.password, "attacker")
        self.now += 300
        self.assertTrue(reopened.administrator(reopened.login(self.password, "attacker")))

    def test_administrator_reset_preserves_paired_devices(self):
        token = self.claim()["token"]
        self.store.device(token)
        root = Path(self.directory.name)
        (root / "data").mkdir()
        recovered = AccessStore(root / "data" / "access.sqlite3", self.key, clock=lambda:self.now)
        with self.store.connection() as source, recovered.connection() as target:
            source.backup(target)
        listening = ListeningStore(root / "data" / "listening.sqlite3")
        listening.import_progress({"client_id":"web-test","request_id":"old"}, [{"show_id":"show","episode_id":"episode","position_ms":40000,"duration_ms":100000}])
        before = listening.progress("show", "episode")
        original = Path(__file__).resolve().parents[1]
        tool = root / "manage_access.py"
        shutil.copyfile(original / "manage_access.py", tool)
        result = subprocess.run([sys.executable, str(tool), "reset-admin", "--confirm"],
                  env={**os.environ, "PODCAST_SETUP_KEY":self.key, "PYTHONPATH":str(original)}, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(self.key.encode(), result.stdout)
        self.assertFalse(recovered.initialized())
        self.assertFalse(recovered.administrator(self.admin))
        self.assertEqual(recovered.device(token), "device-one")
        self.assertEqual(listening.progress("show", "episode"), before)
        renewed = recovered.setup(self.key, "a different private password", "local")
        self.assertTrue(recovered.administrator(renewed))

    def test_owner_migration_preserves_identity_without_initializing_admin_and_retries_safely(self):
        fresh = AccessStore(Path(self.directory.name) / "migration.sqlite3", self.key, clock=lambda:self.now)
        token = secrets.token_hex(32)
        result = fresh.register_existing_device("device-retained", "原来的播客机", token)
        self.assertEqual(result, {"device_id":"device-retained", "pending":True, "existing":False})
        self.assertFalse(fresh.initialized())
        self.assertTrue(fresh.register_existing_device("device-retained", "不覆盖旧名称", token)["existing"])
        self.assertEqual(fresh.devices()[0]["name"], "原来的播客机")
        reopened = AccessStore(fresh.path, self.key, clock=lambda:self.now)
        self.assertEqual(reopened.device(token), "device-retained")
        self.assertFalse(reopened.devices()[0]["pending"])
        self.assertFalse(reopened.initialized())
        self.rejected(reopened.register_existing_device, 409, "device-retained", "旧设备", secrets.token_hex(32))
        self.rejected(reopened.register_existing_device, 409, "device-other", "其他设备", token)
        self.rejected(reopened.register_existing_device, 400, "device-weak", "占位设备", "f" * 64)
        reopened.revoke("device-retained")
        self.rejected(reopened.register_existing_device, 409, "device-retained", "旧设备", token)
        self.assertIsNone(reopened.device(token))
        with reopened.connection() as db:
            self.assertNotIn(token, " ".join(str(tuple(row)) for row in db.execute("SELECT * FROM devices")))

    def test_owner_migration_command_requires_private_file_and_preserves_listening(self):
        root = Path(self.directory.name)
        data = root / "migration-data";data.mkdir()
        listening = ListeningStore(data / "listening.sqlite3")
        listening.import_progress({"client_id":"device-retained", "request_id":"before"},
                                  [{"show_id":"show", "episode_id":"episode", "position_ms":42000, "duration_ms":100000}])
        before = listening.progress("show", "episode")
        token = secrets.token_hex(32)
        token_file = root / "private-token.txt";token_file.write_text(token + "\n");token_file.chmod(0o600)
        original = Path(__file__).resolve().parents[1]
        command = [sys.executable, str(original / "manage_access.py"), "register-device", "--data-dir", str(data),
                   "--device-id", "device-retained", "--token-file", str(token_file)]
        rejected = subprocess.run(command, capture_output=True)
        self.assertNotEqual(rejected.returncode, 0)
        for _ in range(2):
            result = subprocess.run(command + ["--confirm"], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn(token.encode(), result.stdout + result.stderr)
        registered = AccessStore(data / "access.sqlite3")
        self.assertEqual(registered.device(token), "device-retained")
        self.assertFalse(registered.initialized())
        self.assertEqual(listening.progress("show", "episode"), before)
        if os.name == "posix":
            token_file.chmod(0o644)
            invalid = subprocess.run(command + ["--confirm"], capture_output=True)
            self.assertNotEqual(invalid.returncode, 0)
            self.assertNotIn(token.encode(), invalid.stdout + invalid.stderr)


class AccessHTTPTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_pcm.PCMEndpointTests.setUpClass.__func__(cls)

    @classmethod
    def tearDownClass(cls):
        test_pcm.PCMEndpointTests.tearDownClass.__func__(cls)

    def setUp(self):
        test_pcm.PCMEndpointTests.setUp(self)
        self.server.app.config["PODCAST_TEST_AUTH_DISABLED"] = False
        self.now = 1800000000
        self.key = "b" * 64
        self.password = "a safe household password"
        self.server.ACCESS = AccessStore(self.root / "access.sqlite3", self.key, clock=lambda: self.now)
        self.server.LISTENING = ListeningStore(self.root / "listening.sqlite3", clock=lambda: self.now)
        self.server.save_json(self.server.EP_CACHE, {"fixture": {"id": "fixture", "name": "中文节目", "count": 1,
                          "episodes": [{"id": "episode1", "title": "第一集", "duration": 100, "audio": "https://example.org/audio.mp3"}]}})
        self.admin = self.server.app.test_client()
        self.anonymous = self.server.app.test_client()
        setup = self.admin.post("/api/setup", json={"setup_key": self.key, "password": self.password})
        self.assertEqual(setup.status_code, 200)
        self.setup_response = setup
        self.device_id = "device-one"
        self.token = self.claim(self.device_id)
        self.headers = {"Authorization": "Bearer " + self.token}

    request = test_pcm.PCMEndpointTests.request
    prepare = test_pcm.PCMEndpointTests.prepare

    def claim(self, identifier):
        pairing = self.admin.post("/api/devices/pairings", json={})
        response = self.anonymous.post("/api/devices/claim", json={"code": pairing.json["code"], "device_id": identifier, "name": "我的播客机"})
        self.assertEqual(response.status_code, 200)
        self.assertEqual(set(response.json), {"device_id", "token"})
        self.assertEqual(response.json["device_id"], identifier)
        self.assertEqual(int(response.headers["Content-Length"]), len(response.data))
        self.assertLess(len(response.data), 512)
        return response.json["token"]

    def start(self, identifier=None, headers=None):
        return self.anonymous.post("/api/listening/sessions", headers=headers or self.headers,
                      json={"client_id": identifier or self.device_id, "request_id": "first", "show_id": "fixture", "episode_id": "episode1"})

    def test_default_access_is_protected_without_production_legacy_switch(self):
        for path in ("/api/shows", "/api/shows_lite", "/api/listening/stats", "/api/devices", "/subscriptions.opml", "/art/fixture", "/audio/fixture/episode1.mp3", "/pcm/fixture/episode1/stream.pcm"):
            for method in ("GET", "HEAD"):
                with self.subTest(path=path, method=method):
                    self.assertEqual(self.anonymous.open(path, method=method).status_code, 401)
        self.assertEqual(self.anonymous.post("/api/prepare", json={"show_id":"fixture","episode_id":"episode1"}).status_code, 401)
        self.assertEqual(self.anonymous.get("/").status_code, 302)
        self.assertEqual(self.anonymous.get("/healthz").status_code, 200)
        with mock.patch.dict("os.environ", {"PODCAST_AUTH_DISABLED": "1", "PODCAST_LEGACY_AUTH": "1"}):
            self.assertEqual(self.anonymous.get("/api/shows_lite").status_code, 401)

    def test_registered_migration_token_reads_old_history_before_administrator_setup(self):
        self.prepare()
        session = self.start().json
        self.now += 5
        event = {"session_id":session["session_id"], "seq":1, "position_ms":4000,
                 "listened_ms":4000, "state":"paused"}
        self.assertEqual(self.anonymous.post("/api/listening/events", headers=self.headers, json=event).status_code, 200)
        before = self.server.LISTENING.stats()
        new_store = AccessStore(self.root / "migrated-access.sqlite3", self.key, clock=lambda:self.now)
        migrated = secrets.token_hex(32)
        new_store.register_existing_device(self.device_id, "原来的播客机", migrated)
        self.server.ACCESS = new_store
        headers = {"Authorization":"Bearer " + migrated}
        self.assertFalse(self.anonymous.get("/api/setup/status").json["initialized"])
        self.assertEqual(self.anonymous.get("/api/shows_lite", headers=headers).status_code, 200)
        self.assertEqual(self.anonymous.head("/pcm/fixture/episode1/stream.pcm", headers=headers).status_code, 200)
        self.assertEqual(self.anonymous.get("/pcm/fixture/episode1/stream.pcm", headers={**headers,"Range":"bytes=32000-"}).status_code, 206)
        repeat = self.anonymous.post("/api/listening/events", headers=headers, json=event)
        self.assertEqual(repeat.status_code, 200)
        self.assertTrue(repeat.json["duplicate"])
        self.assertEqual(self.server.LISTENING.stats(), before)
        self.assertEqual(self.anonymous.get("/api/shows_lite", headers={"Authorization":"Bearer " + "f"*64}).status_code, 401)
        self.assertEqual(self.anonymous.get("/api/shows_lite").status_code, 401)
        self.assertFalse(new_store.initialized())

    def test_dynamic_artwork_is_authorized_nonblocking_and_conditional_without_builtin_id(self):
        started, release = threading.Event(), threading.Event()
        png = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", "pipe:0", "-frames:v", "1",
                              "-f", "image2pipe", "-vcodec", "png", "pipe:1"], input=ppm(255,0,0),
                              capture_output=True, check=True).stdout
        def reader(url, maximum):
            started.set();release.wait(3)
            return png, url
        artwork = DeviceCoverCache(self.root, reader)
        self.addCleanup(artwork.close);self.addCleanup(release.set)
        self.server.DEVICE_COVERS = artwork
        rss = RSS.replace(b"<itunes:author>", b'<itunes:image href="https://example.org/cover.png"/><itunes:author>')
        with mock.patch.object(source_manager, "read_public", return_value=(rss, "https://example.org/feed.xml")), \
                mock.patch.object(self.server, "queue_preparation", return_value="queued"):
            preview = self.admin.post("/api/sources/preview", json={"url":"https://example.org/feed.xml"})
            added = self.admin.post("/api/sources", json={"preview_id":preview.json["preview_id"]})
        self.assertEqual(added.status_code, 200)
        sid = added.json["show_id"];self.assertNotIn(sid, self.server.SHOWS)
        self.assertTrue(started.wait(2))  # Saving the subscription queued artwork automatically.
        path = "/art/" + sid + "/device.bin"
        self.assertEqual(self.anonymous.get(path).status_code, 401)
        before = time.monotonic()
        pending = self.anonymous.get(path, headers=self.headers)
        self.assertLess(time.monotonic() - before, .2)
        self.assertEqual(pending.status_code, 202)
        self.assertEqual(pending.headers["Retry-After"], "2")
        self.assertEqual(self.anonymous.head(path, headers=self.headers).status_code, 202)
        release.set()
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            response = self.anonymous.get(path, headers=self.headers)
            if response.status_code == 200:break
            time.sleep(.01)
        self.assertEqual(response.status_code, 200)
        self.assertEqual(int(response.headers["Content-Length"]), WIRE_BYTES)
        self.assertTrue(valid_wire(response.data))
        etag = response.headers["ETag"]
        head = self.anonymous.head(path, headers=self.headers)
        self.assertEqual((head.status_code, int(head.headers["Content-Length"]), head.data), (200, WIRE_BYTES, b""))
        cached = self.anonymous.get(path, headers={**self.headers,"If-None-Match":etag})
        self.assertEqual((cached.status_code,cached.data), (304,b""))
        original = self.anonymous.get("/art/" + sid, headers=self.headers)
        self.assertEqual((original.status_code,original.content_type,original.data), (200,"image/png",png))
        for show, status in (("a"*24,400),("bad.id",400),("unknown",404),("fixture",404)):
            self.assertEqual(self.anonymous.get("/art/"+show+"/device.bin", headers=self.headers).status_code,status)
        self.server.ACCESS.revoke(self.device_id)
        self.assertEqual(self.anonymous.get(path, headers=self.headers).status_code, 401)

    def test_running_release_health_is_public_and_contains_no_household_secrets(self):
        response = self.anonymous.get("/api/health")
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json["release"], self.server.RELEASE)
        self.assertEqual(len(response.json["release"]["source_sha256"]), 64)
        for secret in (self.key,self.token,self.password):
            self.assertNotIn(secret,response.get_data(as_text=True))

    def test_administrator_cookie_headers_login_logout_and_origin_validation(self):
        cookie = self.setup_response.headers["Set-Cookie"]
        self.assertIn("HttpOnly", cookie)
        self.assertIn("SameSite=Strict", cookie)
        self.assertNotIn(self.key, self.setup_response.data.decode())
        self.assertEqual(self.admin.get("/api/setup/status").json["authenticated"], True)
        self.assertEqual(self.admin.get("/api/shows_lite").status_code, 200)
        self.assertEqual(self.admin.post("/api/devices/pairings", json={}, headers={"Origin":"http://evil.example"}).status_code, 403)
        self.assertEqual(self.admin.get("/api/refresh").status_code, 405)
        self.assertEqual(self.admin.post("/api/auth/logout", json={}).status_code, 200)
        self.assertEqual(self.admin.get("/api/shows_lite").status_code, 401)
        self.assertEqual(self.admin.post("/api/auth/login", json={"password":self.password}).status_code, 200)
        self.assertEqual(self.admin.get("/api/devices").status_code, 200)

    def test_device_cannot_manage_sources_or_other_clients_sessions(self):
        for path, method in (("/api/sources", "POST"), ("/api/sources/preview", "POST"), ("/api/devices/pairings", "POST"), ("/api/devices", "GET"), ("/api/auth/logout", "POST"), ("/api/refresh", "POST")):
            with self.subTest(path=path):
                self.assertEqual(self.anonymous.open(path, method=method, json={}, headers=self.headers).status_code, 403)
        self.assertEqual(self.start(identifier="device-other").status_code, 403)
        self.assertEqual(self.anonymous.post("/api/listening/import", json={"client_id":"device-other"}, headers=self.headers).status_code, 403)
        session = self.start()
        self.assertEqual(session.status_code, 200)
        other = self.claim("device-two")
        event = {"session_id":session.json["session_id"], "seq":1, "position_ms":1000,"listened_ms":1000,"state":"paused"}
        self.now += 1
        response = self.anonymous.post("/api/listening/events", json=event, headers={"Authorization":"Bearer " + other})
        self.assertEqual(response.status_code, 403)
        self.assertEqual(self.anonymous.post("/api/listening/events", json=event, headers=self.headers).status_code, 200)
        self.assertEqual(self.admin.get("/api/listening/stats").json["summary"]["listened_ms"], 1000)

    def test_real_pcm_get_head_range_and_revocation_are_all_authenticated(self):
        self.prepare()
        for path in (self.url, "/pcm/fixture/episode1/stream.pcm"):
            get = self.anonymous.get(path, headers=self.headers)
            self.assertEqual(get.status_code, 200)
            head = self.anonymous.head(path, headers=self.headers)
            self.assertEqual(head.status_code, 200)
            self.assertEqual(int(head.headers["Content-Length"]), len(get.data))
            ranged = self.anonymous.get(path, headers={**self.headers, "Range":"bytes=32000-63999"})
            self.assertEqual(ranged.status_code, 206)
            self.assertEqual(ranged.data, get.data[32000:64000])
        before = self.admin.get("/api/listening/stats").json
        self.assertEqual(self.admin.delete("/api/devices/device-one").status_code, 200)
        for path in (self.url, "/pcm/fixture/episode1/stream.pcm", "/api/shows_lite", "/api/listening/stats"):
            self.assertEqual(self.anonymous.get(path, headers=self.headers).status_code, 401)
            self.assertEqual(self.anonymous.head(path, headers=self.headers).status_code, 401)
        self.assertEqual(before, self.admin.get("/api/listening/stats").json)

    def test_error_body_is_bounded_with_fixed_length_and_never_echoes_codes(self):
        code = "123456"
        response = self.anonymous.post("/api/devices/claim", json={"code":code,"device_id":"device-two"})
        self.assertEqual(response.status_code, 401)
        self.assertEqual(int(response.headers["Content-Length"]), len(response.data))
        self.assertLess(len(response.data), 512)
        self.assertNotIn(code, response.data.decode())
        for kwargs in ({"data":"broken-json", "content_type":"application/json"},
                       {"data":"x"*17000,"content_type":"application/json"},
                       {"json":{"code":[], "device_id":"device-one"}}):
            result = self.anonymous.post("/api/devices/claim", **kwargs)
            self.assertIn(result.status_code, {400,413})
            self.assertEqual(int(result.headers["Content-Length"]), len(result.data))
            self.assertLess(len(result.data), 512)
            self.assertIsInstance(result.json["error"], str)

    def test_registry_no_tokens_restarts_persist_and_relative_export_uses_current_service(self):
        self.assertEqual(self.anonymous.get("/api/shows_lite", headers=self.headers).status_code, 200)
        registry = self.admin.get("/api/devices")
        self.assertNotIn(self.token, registry.data.decode())
        self.assertNotIn("hash", registry.data.decode())
        self.server.ACCESS = AccessStore(self.root / "access.sqlite3", self.key, clock=lambda:self.now)
        self.assertEqual(self.admin.get("/api/devices").status_code, 200)
        self.assertEqual(self.anonymous.get("/api/shows_lite", headers=self.headers).status_code, 200)
        original = copy.deepcopy(self.server.CFG["shows"])
        self.addCleanup(self.server.CFG.__setitem__, "shows", original)
        self.server.CFG["shows"][0]["feed"] = "/feeds/xiaoyuzhou/111111111111111111111111.xml"
        with mock.patch.dict("os.environ", {"PODCAST_PUBLIC_URL":"http://podcast.home:8999"}):
            response = self.admin.get("/subscriptions.opml")
        self.assertIn(b'http://podcast.home:8999/feeds/xiaoyuzhou/', response.data)
        self.assertNotIn(b"192.168.1.50", response.data)
