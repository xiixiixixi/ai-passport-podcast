"""Temporary Git repositories test staged privacy and non-destructive hooks."""
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
GUARD = "scripts/check-public-commit.py"
INSTALLER = "scripts/install-git-hooks.py"
POLICY = "scripts/export-release.py"
FILE_LIST = ".github/public-files.json"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    sys.modules[name] = value
    spec.loader.exec_module(value)
    return value


installer = module("public_hook_installer_tests", ROOT / INSTALLER)


class PublicCommitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        environment = mock.patch.dict(os.environ, {"GIT_CONFIG_GLOBAL": os.devnull,
                                                    "GIT_CONFIG_NOSYSTEM": "1",
                                                    "PYTHONDONTWRITEBYTECODE": "1"})
        environment.start()
        self.addCleanup(environment.stop)
        self.git("init", "--quiet")
        self.git("config", "user.name", "Public fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("config", "core.autocrlf", "false")
        for name in (GUARD, INSTALLER, POLICY):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, path)
        (self.root / "README.md").write_text("public fixture\n")
        (self.root / ".gitignore").write_text(".local/\n")
        self.files = {GUARD, INSTALLER, POLICY, FILE_LIST, "README.md", ".gitignore"}
        self.manifest()
        self.git("add", "--all")

    def git(self, *args, check=True):
        result = subprocess.run(["git", *args], cwd=self.root, capture_output=True)
        if check:
            self.assertEqual(result.returncode, 0, "temporary Git fixture operation failed")
        return result

    def manifest(self, stage=False):
        path = self.root / FILE_LIST
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"schema_version": 1, "files": sorted(self.files)}))
        if stage:
            self.git("add", "--", FILE_LIST)

    def registered(self, name, data="public fixture\n"):
        self.files.add(name)
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data if isinstance(data, bytes) else data.encode())
        self.manifest(stage=True)
        self.git("add", "--", name)

    def guard(self, *args):
        return subprocess.run([sys.executable, str(self.root / GUARD), *args], cwd=self.root,
                              capture_output=True, text=True)

    def test_unborn_repository_checks_index_not_working_copy(self):
        self.assertEqual(self.guard().returncode, 0)
        path = self.root / "README.md"
        secret = "fictional-fixture-secret"
        path.write_text(f'WIFI_PASSWORD = "{secret}"\n')
        self.assertEqual(self.guard().returncode, 0)  # Unstaged content is not submitted.
        self.git("add", "README.md")
        path.write_text("clean working copy\n")
        result = self.guard()
        self.assertEqual(result.returncode, 1)
        self.assertNotIn(secret, result.stderr)

    def test_forced_private_files_and_unreviewed_new_files_refuse(self):
        for name in (".local/private.txt", "server/.env", "server/data/records.sqlite3",
                     "server/media/audio.mp3", "firmware/build/image.bin",
                     "firmware/managed_components/example/source.c",
                     "firmware/assets/images/home.jpg", "firmware/docs/brand/ai-passport-front.png",
                     "docs/claude-fix-report.md", "server/cache/state.json", "server/runtime.log",
                     "docs/new-internal-note.md"):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fictional private fixture\n")
            self.git("add", "-f", "--", name)
        self.assertEqual(self.guard("--all").returncode, 1)

    def test_manifest_cannot_authorize_private_paths_and_staged_manifest_wins(self):
        name = ".local/private.txt"
        path = self.root / name
        path.parent.mkdir()
        path.write_text("fictional fixture")
        self.files.add(name)
        self.manifest(stage=True)
        self.git("add", "-f", name)
        self.files.remove(name)
        self.manifest()  # Fixing only the working manifest must not hide staged content.
        self.assertNotEqual(self.guard("--all").returncode, 0)

    def test_manifest_cannot_authorize_runtime_database_sidecars(self):
        for name in ("server/runtime.sqlite-wal", "server/runtime.sqlite-shm"):
            with self.subTest(sidecar=name):
                path = self.root / name
                path.parent.mkdir(exist_ok=True)
                path.write_text("fictional runtime fixture")
                self.files.add(name)
                self.manifest(stage=True)
                self.git("add", "-f", "--", name)
                self.assertNotEqual(self.guard("--all").returncode, 0)
                self.git("rm", "--cached", "--", name)
                path.unlink()
                self.files.remove(name)
                self.manifest(stage=True)

    def test_local_private_values_and_sensitive_file_names_are_not_echoed(self):
        value = "fictional-private-label"
        path = self.root / ".local/export-private-values.json"
        path.parent.mkdir()
        path.write_text(json.dumps([value]))
        hidden = self.root / "docs" / (value + ".md")
        hidden.parent.mkdir()
        hidden.write_text("public-looking fixture")
        self.git("add", "-f", "--", str(hidden.relative_to(self.root)))
        result = self.guard("--all")
        self.assertEqual(result.returncode, 1)
        self.assertNotIn(value, result.stdout + result.stderr)
        hidden.unlink()
        self.git("rm", "--cached", "--", str(hidden.relative_to(self.root)))
        self.registered("server/example.py", value)
        result = self.guard("--all")
        self.assertEqual(result.returncode, 1)
        self.assertNotIn(value, result.stdout + result.stderr)

    def test_public_variants_and_exact_cache_stub_pass_internal_images_refuse(self):
        policy = module("public_policy_fixture_tests", ROOT / POLICY)
        for name, data in policy.PUBLIC_DOCUMENT_VARIANTS.items():
            self.registered(name, data)
        stub = "firmware/tests/podcast_cover_stubs/src/misc/cache/instance/lv_image_cache.h"
        self.registered(stub)
        self.assertEqual(self.guard("--all").returncode, 0)
        name = next(iter(policy.PUBLIC_DOCUMENT_VARIANTS))
        (self.root / name).write_text("internal catalogue fixture")
        self.git("add", "--", name)
        self.assertEqual(self.guard("--all").returncode, 1)
        self.registered(name, policy.PUBLIC_DOCUMENT_VARIANTS[name])
        self.registered("firmware/docs/README.md", '<img src="../assets/images/home.jpg">')
        self.assertEqual(self.guard("--all").returncode, 1)

    def test_all_mode_finds_previously_tracked_private_file_and_types_refuse(self):
        path = self.root / ".local/tracked.txt"
        path.parent.mkdir()
        path.write_text("fictional fixture")
        self.git("add", "-f", ".local/tracked.txt")
        self.git("commit", "--quiet", "-m", "synthetic baseline")
        self.assertEqual(self.guard().returncode, 0)
        self.assertEqual(self.guard("--all").returncode, 1)
        self.git("rm", "--cached", ".local/tracked.txt")
        self.registered("docs/link.md")
        link = self.root / "docs/link.md"
        link.unlink()
        link.symlink_to(self.root / "README.md")
        self.git("add", "--", "docs/link.md")
        self.assertEqual(self.guard().returncode, 1)

    def test_original_hook_runs_first_and_failure_code_is_preserved(self):
        original = self.root / ".git/hooks/pre-commit"
        text = '#!/bin/sh\nmkdir -p .local\nprintf "fictional fixture" >.local/from-old.txt\ngit add -f .local/from-old.txt\n'
        original.write_text(text)
        original.chmod(0o755)
        self.assertTrue(installer.install(self.root))
        self.assertEqual(original.read_text(), text)
        managed = Path(self.git("config", "--get", "core.hooksPath").stdout.decode().strip())
        result = self.git("commit", "--quiet", "-m", "must be rejected", check=False)
        self.assertEqual(result.returncode, 1)
        self.assertNotEqual(self.git("rev-parse", "--verify", "HEAD", check=False).returncode, 0)
        for name in ("pre-merge-commit", "pre-applypatch"):
            result = subprocess.run([str(managed / name)], cwd=self.root, capture_output=True)
            self.assertEqual(result.returncode, 1)
        original.write_text("#!/bin/sh\nexit 23\n")
        result = subprocess.run([str(managed / "pre-commit")], cwd=self.root, capture_output=True)
        self.assertEqual(result.returncode, 23)

    def test_custom_hooks_configuration_and_stdin_preserved_repeat_is_idempotent(self):
        hooks = self.root / ".custom-hooks"
        hooks.mkdir()
        original = hooks / "pre-push"
        text = '#!/bin/sh\ncat >hook-input.txt\nprintf "%s" "$1" >hook-argument.txt\n'
        original.write_text(text)
        original.chmod(0o755)
        self.git("config", "core.hooksPath", ".custom-hooks")
        self.assertTrue(installer.install(self.root))
        values = self.git("config", "--get-all", "core.hooksPath").stdout.decode().splitlines()
        self.assertEqual(values[0], ".custom-hooks")
        managed = Path(values[-1])
        self.assertFalse(installer.install(self.root))
        self.assertEqual(self.git("config", "--get-all", "core.hooksPath").stdout.decode().splitlines(), values)
        result = subprocess.run([str(managed / "pre-push"), "fixture-argument"], cwd=self.root,
                                input="fixture-input", capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertEqual((self.root / "hook-input.txt").read_text(), "fixture-input")
        self.assertEqual((self.root / "hook-argument.txt").read_text(), "fixture-argument")
        self.assertEqual(original.read_text(), text)

    def test_public_commit_succeeds_with_installed_guard(self):
        self.assertTrue(installer.install(self.root))
        result = self.git("commit", "--quiet", "-m", "public fixture", check=False)
        self.assertEqual(result.returncode, 0, "an actual public Git commit must succeed")
        self.assertEqual(self.git("log", "-1", "--format=%s").stdout.decode().strip(), "public fixture")

    def test_install_failure_after_configuration_append_restores_original(self):
        hooks = self.root / ".custom-hooks"
        hooks.mkdir()
        original = hooks / "pre-commit"
        text = "#!/bin/sh\nexit 23\n"
        original.write_text(text)
        original.chmod(0o755)
        self.git("config", "core.hooksPath", ".custom-hooks")
        original_git = installer.git
        injected = False

        def append_then_fail(root, *args, **kwargs):
            nonlocal injected
            result = original_git(root, *args, **kwargs)
            if not injected and args[:4] == ("config", "--local", "--add", "core.hooksPath"):
                injected = True
                raise installer.InstallError("simulated post-write failure")
            return result

        with mock.patch.object(installer, "git", side_effect=append_then_fail):
            with self.assertRaises(installer.InstallError):
                installer.install(self.root)
        self.assertEqual(self.git("config", "--get-all", "core.hooksPath").stdout.decode().splitlines(),
                         [".custom-hooks"])
        self.assertEqual(original.read_text(), text)
        self.assertFalse((self.root / ".git/passport-public-hooks").exists())

    def test_current_public_tree_remains_allowed(self):
        result = subprocess.run(["git", "archive", "HEAD"], cwd=ROOT, capture_output=True)
        public_names = set()
        if result.returncode == 0:
            with tarfile.open(fileobj=io.BytesIO(result.stdout)) as package:
                sources = {item.name: package.extractfile(item).read() for item in package.getmembers()
                           if item.isfile()}
        else:
            policy = module("public_policy_snapshot_fixture_tests", ROOT / POLICY)
            sources = policy.source_snapshot(ROOT)
        for name, data in sources.items():
            self.assertFalse(name.startswith("/") or ".." in Path(name).parts)
            public_names.add(name)
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        for name in (GUARD, INSTALLER, "tests/test_public_commit.py"):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, path)
        self.files = public_names | {GUARD, INSTALLER, "tests/test_public_commit.py", FILE_LIST}
        self.manifest()
        self.git("add", "-f", "--all")
        indexed = self.git("ls-files", "-z").stdout.decode().split("\0")
        self.assertEqual(set(filter(None, indexed)), self.files)
        result = self.guard("--all")
        self.assertEqual(result.returncode, 0, "the real public tree must remain permitted")


if __name__ == "__main__":
    unittest.main()
