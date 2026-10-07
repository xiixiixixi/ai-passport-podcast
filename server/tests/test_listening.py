"""Persistence, truthful accounting, duplicate delivery, and concurrent device/web use."""
import json
import tempfile
import unittest
import sqlite3
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor

from listening_store import ListeningStore, ListeningError


class ListeningTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.now = 1720000000.0
        self.path = Path(self.directory.name) / "history.sqlite3"
        self.store = ListeningStore(self.path, clock=lambda: self.now)

    def start(self, client="web", request_id="one", **fields):
        return self.store.start({"client_id": client, "request_id": request_id,
                                 "show_id": "show", "episode_id": "episode", **fields}, 100000)

    def event(self, session, seq, position, heard, state="playing", **fields):
        return self.store.event({"session_id": session["session_id"], "seq": seq,
                                 "position_ms": position, "listened_ms": heard, "state": state, **fields})

    def test_start_or_failed_playback_never_counts_as_heard_and_start_retries_are_idempotent(self):
        first = self.start()
        self.assertEqual(first, self.start())
        self.assertEqual(self.store.stats()["summary"], {"listened_ms": 0, "play_count": 0, "played_episodes": 0, "completed_episodes": 0})
        self.event(first, 1, 0, 0, "stopped")
        self.assertEqual(self.store.progress("show", "episode")["status"], "unplayed")
        self.assertEqual(self.store.recent(), [])

    def test_duplicate_and_concurrent_delivery_credit_once_then_persist_after_restart(self):
        session = self.start()
        self.now += 10
        with ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda _: self.event(session, 1, 10000, 10000), range(8)))
        self.assertEqual(sum(not result["duplicate"] for result in results), 1)
        progress = self.store.progress("show", "episode")
        self.assertEqual((progress["listened_ms"], progress["play_count"], progress["position_ms"]), (10000, 1, 10000))
        reopened = ListeningStore(self.path)
        self.assertEqual(reopened.progress("show", "episode"), progress)
        self.assertEqual(reopened.stats()["summary"]["listened_ms"], 10000)

    def test_seek_to_end_never_adds_hours_or_completes_and_repeat_first_section_does_not_complete(self):
        session = self.start()
        self.now += 100
        self.event(session, 1, 10000, 10000)
        self.event(session, 2, 0, 10000, seek=True)
        self.event(session, 3, 10000, 20000)
        self.event(session, 4, 0, 20000, seek=True)
        self.event(session, 5, 10000, 30000)
        self.event(session, 6, 100000, 30000, seek=True)
        result = self.event(session, 7, 100000, 30000, "ended")
        self.assertEqual(result["progress"]["status"], "in_progress")
        self.assertEqual(result["progress"]["listened_ms"], 30000)

    def test_completion_requires_actual_coverage_and_double_speed_counts_wall_time(self):
        session = self.start()
        self.now += 50
        result = self.event(session, 1, 100000, 50000, "ended", rate=2)
        self.assertEqual(result["progress"]["status"], "completed")
        self.assertEqual(result["progress"]["listened_ms"], 50000)
        self.assertEqual(self.store.stats()["summary"]["completed_episodes"], 1)

    def test_device_resume_uses_web_position_and_old_stop_cannot_overwrite_it(self):
        old = self.start()
        self.now += 10
        self.event(old, 1, 10000, 10000)
        new = self.start("device", "two")
        self.assertEqual(new["position_ms"], 10000)
        self.now += 10
        self.event(new, 1, 20000, 10000)
        before = self.store.progress("show", "episode")
        late = self.event(old, 2, 12000, 12000, "stopped")
        self.assertTrue(late["stale"])
        after = late["progress"]
        self.assertEqual((after["position_ms"], after["updated_at"]), (before["position_ms"], before["updated_at"]))
        self.assertEqual(after["listened_ms"], 22000)
        duplicate = self.event(old, 1, 10000, 10000)
        self.assertTrue(duplicate["stale"])
        self.assertEqual(duplicate["progress"]["position_ms"], 20000)

    def test_late_offline_start_with_base_revision_does_not_claim_newer_position(self):
        web = self.start()
        self.now += 20
        self.event(web, 1, 20000, 20000)
        stale = self.start("device", "offline", base_revision=0, position_ms=50000)
        self.assertTrue(stale["stale"])
        self.now += 10
        event = self.event(stale, 1, 60000, 10000, "stopped")
        self.assertTrue(event["stale"])
        self.assertEqual(event["progress"]["position_ms"], 20000)
        self.assertEqual(event["progress"]["listened_ms"], 30000)

    def test_delayed_session_creation_accepts_monotonic_elapsed_event_without_reclaiming_position(self):
        web = self.start()
        self.now += 30
        self.event(web, 1, 30000, 30000)
        offline = self.start("device", "delayed", base_revision=0, position_ms=0)
        heard = self.event(offline, 1, 80000, 80000, elapsed_ms=80000, state="paused")
        self.assertTrue(heard["stale"])
        self.assertEqual(heard["progress"]["position_ms"], 30000)
        self.assertEqual(heard["progress"]["listened_ms"], 110000)

    def test_closed_audio_tail_credits_once_survives_reopen_and_keeps_terminal_state(self):
        session = self.start("device", "audio-tail")
        self.now += 100
        first = self.event(session, 1, 100000, 100000, "ended", elapsed_ms=100000)
        self.assertEqual(first["progress"]["status"], "completed")
        with self.store.connection() as db:
            original_recent = db.execute("SELECT updated_at,recent_order FROM progress").fetchone()
            original_recent = tuple(original_recent)
        # Exactly the controller/audio-worker ordering: a terminal snapshot is
        # accepted before its final 32ms of real output has been observed. A
        # reboot can turn that final observation into stopped instead of ended.
        self.now += 10
        with ThreadPoolExecutor(max_workers=8) as pool:
            replies = list(pool.map(lambda _: self.event(session, 2, 100032, 100032,
                                                        "stopped", elapsed_ms=100000), range(8)))
        self.assertEqual(sum(not reply["duplicate"] for reply in replies), 1)
        self.assertTrue(all(reply["accepted"] for reply in replies))
        self.assertEqual(self.store.stats()["summary"]["listened_ms"], 100032)
        self.assertEqual(self.store.stats()["summary"]["play_count"], 1)
        with self.store.connection() as db:
            self.assertEqual(tuple(db.execute("SELECT updated_at,recent_order FROM progress").fetchone()), original_recent)
            self.assertEqual(tuple(db.execute("SELECT closed,state,last_seq,listened_ms FROM sessions").fetchone()),
                             (1, "ended", 2, 100032))
            self.assertEqual(json.loads(db.execute("SELECT payload FROM events WHERE seq=2").fetchone()[0])["state"], "stopped")
        self.store = ListeningStore(self.path, clock=lambda: self.now)
        duplicate = self.event(session, 2, 100032, 100032, "stopped", elapsed_ms=100000)
        self.assertTrue(duplicate["duplicate"])
        self.assertEqual(self.store.stats()["summary"]["listened_ms"], 100032)

    def test_closed_tail_cannot_reclaim_new_owner_position_or_recent_order(self):
        old = self.start("device", "old-tail")
        self.now += 10
        self.event(old, 1, 10000, 10000, "stopped", elapsed_ms=10000)
        current = self.start("web", "new-owner")
        self.now += 2
        self.event(current, 1, 12000, 2000, "playing")
        latest = self.store.start({"client_id": "web", "request_id": "other-episode",
                                  "show_id": "another", "episode_id": "episode"}, 100000)
        self.now += 1
        self.event(latest, 1, 1000, 1000)
        before = self.store.progress("show", "episode")
        recent_before = [(row["show_id"], row["episode_id"]) for row in self.store.recent()]
        with self.store.connection() as db:
            before_owner = tuple(db.execute("SELECT owner,recent_order FROM progress WHERE show_id='show'").fetchone())
        self.now += 10
        tail = self.event(old, 2, 100032, 10032, "ended", elapsed_ms=10000)
        self.assertTrue(tail["accepted"])
        self.assertTrue(tail["stale"])
        self.assertEqual(tail["progress"]["position_ms"], before["position_ms"])
        self.assertEqual(tail["progress"]["updated_at"], before["updated_at"])
        self.assertEqual(tail["progress"]["status"], "in_progress")
        self.assertEqual(tail["progress"]["listened_ms"], before["listened_ms"] + 32)
        self.assertEqual([(row["show_id"], row["episode_id"]) for row in self.store.recent()], recent_before)
        with self.store.connection() as db:
            self.assertEqual(tuple(db.execute("SELECT owner,recent_order FROM progress WHERE show_id='show'").fetchone()), before_owner)

    def test_closed_tail_rejects_revival_conflicts_backwards_time_and_impossible_duration(self):
        session = self.start("device", "tail-validation")
        self.now += 10
        self.event(session, 1, 10000, 10000, "stopped", elapsed_ms=10000)
        before = self.store.stats()["summary"]
        for seq, position, heard, state, fields in [
                (2, 10032, 10032, "playing", {"elapsed_ms": 10000}),
                (2, 10032, 10032, "paused", {"elapsed_ms": 10000}),
                (1, 10032, 10032, "stopped", {"elapsed_ms": 10000}),
                (0, 10032, 10032, "stopped", {"elapsed_ms": 10000}),
                (2, 10000, 9999, "stopped", {"elapsed_ms": 10000}),
                (2, 50000, 50000, "ended", {"elapsed_ms": 10000}),
                (2, 50000, 50000, "stopped", {}),
        ]:
            with self.subTest(seq=seq, state=state, heard=heard, fields=fields):
                with self.assertRaises(ListeningError):
                    self.event(session, seq, position, heard, state, **fields)
                self.assertEqual(self.store.stats()["summary"], before)
        result = self.event(session, 2, 10032, 10032, "stopped", elapsed_ms=10000)
        self.assertTrue(result["accepted"])
        self.assertEqual(result["progress"]["listened_ms"], 10032)
        with self.store.connection() as db:
            self.assertEqual(tuple(db.execute("SELECT closed,state,last_seq FROM sessions").fetchone()),
                             (1, "stopped", 2))

    def test_legacy_import_is_idempotent_preserves_bookmarks_and_never_overwrites_active_records(self):
        body = {"client_id": "web", "request_id": "legacy", "episodes": [{"show_id": "show", "episode_id": "episode", "position_ms": 40000}]}
        resolved = [{"show_id": "show", "episode_id": "episode", "position_ms": 40000, "duration_ms": 100000, "updated_at": 1720000000}]
        result = self.store.import_progress(body, resolved)
        self.assertEqual(result["imported"], 1)
        self.assertEqual(self.store.import_progress(body, resolved), result)
        self.assertEqual(result["results"][0]["progress"]["position_ms"], 40000)
        self.assertTrue(result["results"][0]["progress"]["legacy"])
        self.assertEqual(self.store.stats()["summary"]["listened_ms"], 0)
        self.assertEqual(self.store.stats()["summary"]["play_count"], 0)
        self.assertEqual(self.store.stats()["legacy_episodes"], 1)
        self.assertEqual(self.store.recent()[0]["progress"]["position_ms"], 40000)
        session = self.start()
        self.assertEqual(session["position_ms"], 40000)
        other = dict(body, request_id="again")
        replay = self.store.import_progress(other, resolved)
        self.assertEqual(replay["results"][0]["status"], "skipped_existing")
        self.assertEqual(self.store.progress("show", "episode")["position_ms"], 40000)

    def test_manual_mark_preserves_real_time_and_invalidates_active_owner(self):
        session = self.start()
        self.now += 30
        self.event(session, 1, 10000, 10000)
        marked = self.store.mark("show", "episode", "unplayed", 100000)
        late = self.event(session, 2, 20000, 20000)
        self.assertTrue(late["stale"])
        self.assertEqual(late["progress"]["status"], "unplayed")
        self.assertEqual(late["progress"]["position_ms"], 0)
        self.assertEqual(late["progress"]["listened_ms"], 20000)
        complete = self.store.mark("show", "episode", "completed", 100000)
        self.assertEqual(complete["listened_ms"], 20000)
        self.assertEqual(complete["play_count"], 1)

    def test_daily_aggregation_uses_china_timezone_and_late_records_do_not_reorder_recent(self):
        self.now = 1720000000
        old = self.start()
        self.now += 10
        self.event(old, 1, 10000, 10000)
        other = self.store.start({"client_id": "device", "request_id": "other", "show_id": "another", "episode_id": "ep"}, 100000)
        self.now += 10
        self.event(other, 1, 10000, 10000)
        self.now += 1
        self.event(old, 2, 11000, 11000, "stopped")
        self.assertEqual(self.store.recent()[0]["show_id"], "another")
        self.assertEqual(self.store.stats()["days"][0]["listened_ms"], 21000)

    def test_same_second_old_episode_tail_does_not_promote_recent_but_real_relisten_does(self):
        first = self.start()
        self.now += .01
        self.event(first, 1, 10, 10)
        second = self.store.start({"client_id": "device", "request_id": "new", "show_id": "show", "episode_id": "new"}, 100000)
        self.now += .01
        self.event(second, 1, 10, 10)
        before = self.store.progress("show", "episode")
        self.assertEqual([row["episode_id"] for row in self.store.recent()], ["new", "episode"])
        stopped = self.event(first, 2, 20, 20, "stopped")
        self.assertGreater(stopped["progress"]["revision"], before["revision"])
        self.assertEqual(stopped["progress"]["listened_ms"], 20)
        self.assertEqual(stopped["progress"]["updated_at"], before["updated_at"])
        self.assertEqual([row["episode_id"] for row in self.store.recent()], ["new", "episode"])
        duplicate = self.event(first, 2, 20, 20, "stopped")
        self.assertTrue(duplicate["duplicate"])
        self.assertEqual(self.store.stats()["summary"]["listened_ms"], 30)
        self.assertEqual([row["episode_id"] for row in self.store.recent()], ["new", "episode"])
        resumed = self.start("web", "relisten")
        self.assertEqual(resumed["position_ms"], 20)
        self.assertEqual([row["episode_id"] for row in self.store.recent()], ["new", "episode"])
        self.now += .01
        self.event(resumed, 1, 30, 10)
        self.assertEqual([row["episode_id"] for row in self.store.recent()], ["episode", "new"])
        reopened = ListeningStore(self.path, clock=lambda: self.now)
        self.assertEqual([row["episode_id"] for row in reopened.recent()], ["episode", "new"])
        self.assertEqual(reopened.stats()["summary"]["listened_ms"], 40)

    def test_recent_sequence_migration_preserves_existing_order_marks_and_counters_and_is_idempotent(self):
        old_path = Path(self.directory.name) / "previous-version.sqlite3"
        with sqlite3.connect(old_path) as db:
            db.executescript("""
            CREATE TABLE meta(key TEXT PRIMARY KEY,value INTEGER NOT NULL);
            INSERT INTO meta VALUES('revision',40);
            CREATE TABLE progress (
              show_id TEXT NOT NULL,episode_id TEXT NOT NULL,duration_ms INTEGER NOT NULL DEFAULT 0,
              position_ms INTEGER NOT NULL DEFAULT 0,listened_ms INTEGER NOT NULL DEFAULT 0,
              play_count INTEGER NOT NULL DEFAULT 0,status TEXT NOT NULL DEFAULT 'unplayed',
              updated_at INTEGER NOT NULL DEFAULT 0,revision INTEGER NOT NULL DEFAULT 0,
              owner TEXT,coverage TEXT NOT NULL DEFAULT '[]',manual INTEGER NOT NULL DEFAULT 0,
              legacy INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(show_id,episode_id));
            """)
            records = [("show", "older", 100000, 50000, 40000, 2, "in_progress", 1720000000, 10, None, "[[0,40000]]", 0, 0),
                       ("show", "newer", 100000, 100000, 90000, 3, "completed", 1720000000, 20, None, "[[0,90000]]", 1, 0),
                       ("show", "legacy", 100000, 20000, 0, 0, "in_progress", 1719999999, 30, None, "[]", 0, 1)]
            db.executemany("INSERT INTO progress VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", records)
            db.executescript("""
            CREATE TABLE sources(id TEXT PRIMARY KEY,canonical TEXT UNIQUE NOT NULL,payload TEXT NOT NULL);
            CREATE TABLE events(session_id TEXT NOT NULL,seq INTEGER NOT NULL,payload TEXT NOT NULL,reply TEXT NOT NULL,PRIMARY KEY(session_id,seq));
            """)
            stored_source = {"id": "retained", "name": "保留的订阅", "feed": "https://example.org/rss"}
            db.execute("INSERT INTO sources VALUES(?,?,?)", ("retained", "https://example.org/rss", json.dumps(stored_source)))
            db.execute("INSERT INTO events VALUES(?,?,?,?)", ("old-session", 1, '{"listened_ms":1000}', '{"accepted":true}'))
        upgraded = ListeningStore(old_path, clock=lambda: self.now)
        expected = ["newer", "older", "legacy"]
        self.assertEqual([row["episode_id"] for row in upgraded.recent()], expected)
        self.assertEqual(upgraded.progress("show", "newer")["status"], "completed")
        self.assertEqual(upgraded.progress("show", "older")["position_ms"], 50000)
        self.assertEqual(upgraded.stats()["summary"]["listened_ms"], 130000)
        self.assertEqual(upgraded.sources(), [stored_source])
        with upgraded.connection() as db:
            self.assertEqual([tuple(row) for row in db.execute("SELECT show_id,episode_id,duration_ms,position_ms,listened_ms,play_count,status,updated_at,revision,owner,coverage,manual,legacy FROM progress ORDER BY episode_id")], sorted(records, key=lambda row: row[1]))
            self.assertEqual(db.execute("SELECT value FROM meta WHERE key='revision'").fetchone()[0], 40)
            self.assertEqual(tuple(db.execute("SELECT * FROM events").fetchone()), ("old-session", 1, '{"listened_ms":1000}', '{"accepted":true}'))
            sequence = db.execute("SELECT value FROM meta WHERE key='recent_sequence'").fetchone()[0]
            before_rows = [tuple(row) for row in db.execute("SELECT position_ms,listened_ms,play_count,status,updated_at,revision,coverage,manual,legacy,recent_order FROM progress ORDER BY episode_id")]
        reopened = ListeningStore(old_path, clock=lambda: self.now)
        self.assertEqual([row["episode_id"] for row in reopened.recent()], expected)
        with reopened.connection() as db:
            self.assertEqual(db.execute("SELECT value FROM meta WHERE key='recent_sequence'").fetchone()[0], sequence)
            self.assertEqual([tuple(row) for row in db.execute("SELECT position_ms,listened_ms,play_count,status,updated_at,revision,coverage,manual,legacy,recent_order FROM progress ORDER BY episode_id")], before_rows)
        current = reopened.start({"client_id": "device", "request_id": "after-migration", "show_id": "show", "episode_id": "older"}, 100000)
        self.now += 1
        reopened.event({"session_id": current["session_id"], "seq": 1, "position_ms": 51000, "listened_ms": 1000, "state": "playing"})
        self.assertEqual([row["episode_id"] for row in reopened.recent()], ["older", "newer", "legacy"])

    def test_empty_database_reopen_keeps_recent_sequence_zero_and_creating_a_session_does_not_advance_it(self):
        reopened = ListeningStore(self.path, clock=lambda: self.now)
        self.assertEqual(reopened.recent(), [])
        self.start()
        with reopened.connection() as db:
            self.assertEqual(db.execute("SELECT value FROM meta WHERE key='recent_sequence'").fetchone()[0], 0)
        again = ListeningStore(self.path, clock=lambda: self.now)
        self.assertEqual(again.recent(), [])
        with again.connection() as db:
            self.assertEqual(db.execute("SELECT value FROM meta WHERE key='recent_sequence'").fetchone()[0], 0)

    def test_sequence_payload_conflicts_and_fabricated_elapsed_time_rejected(self):
        session = self.start()
        self.event(session, 1, 1000, 1000)
        for sequence, position, heard, fields in [(1, 2000, 1000, {}), (0, 1000, 1000, {}),
                                                  (2, 1000, 0, {}), (2, 100000, 100000, {}),
                                                  (2, 2000, 2000, {"rate": float("nan")})]:
            with self.subTest(sequence=sequence, heard=heard):
                with self.assertRaises(ListeningError):
                    self.event(session, sequence, position, heard, **fields)
        with self.assertRaises(ListeningError):
            self.start(position_ms=100)


if __name__ == "__main__":
    unittest.main()
