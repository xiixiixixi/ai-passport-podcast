"""Running source identity excludes installation keys and mutable user data."""
import tempfile
import unittest
from pathlib import Path
from runtime_release import source_release


class RuntimeReleaseTests(unittest.TestCase):
    def test_fingerprint_tracks_code_and_ui_but_not_household_settings_or_history(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "server.py").write_text("print('backend')")
            (root / "requirements.txt").write_text("dependency==1")
            (root / "static").mkdir();(root / "static" / "player.js").write_text("play()")
            initial = source_release(root)
            self.assertEqual(initial["files"], 3)
            (root / "config.json").write_text('{"private":"household address"}')
            (root / ".env").write_text("PODCAST_SETUP_KEY=private-installation-key")
            (root / "data").mkdir();(root / "data" / "history.sqlite3").write_bytes(b"private history")
            self.assertEqual(source_release(root), initial)
            (root / "static" / "player.js").write_text("updatedPlay()")
            self.assertNotEqual(source_release(root)["source_sha256"], initial["source_sha256"])
            identified = source_release(root, "backend-20261005-reviewed")
            self.assertEqual(identified["id"], "backend-20261005-reviewed")
            self.assertEqual(identified["source_sha256"], source_release(root)["source_sha256"])
            self.assertTrue(source_release(root, "private key with spaces")["id"].startswith("source-"))


if __name__ == "__main__":
    unittest.main()
