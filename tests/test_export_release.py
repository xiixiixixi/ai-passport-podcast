"""Public exporter acceptance using tiny synthetic images; no build/USB/network."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("passport_export_release", ROOT / "scripts/export-release.py")
exporter = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = exporter
SPEC.loader.exec_module(exporter)


def fixture_user_paths():
    unix = [str(Path("/") / base / "export-fixture-user" / "project") for base in ("Users", "home")]
    slash = chr(92)
    windows = [separator.join(("C:", "Users", "export-fixture-user", "project"))
               for separator in ("/", slash, slash * 2)]
    return unix + windows


def fixture_app():
    segment = struct.pack("<I", 0xABCD5432) + bytes(252) + b"synthetic-public-application"
    raw = struct.pack("<BBBBI", 0xE9, 1, 2, 0x30, 0x40380000)
    raw += struct.pack("<BBBBHBHHBBBBB", 0xEE, 0, 0, 0, 5, 0, 0, 65535, 0, 0, 0, 0, 1)
    raw += struct.pack("<II", 0x3C000020, len(segment)) + segment
    checksum = 0xEF
    for byte in segment:checksum ^= byte
    raw += bytes((15 - len(raw) % 16) % 16) + bytes([checksum])
    return raw + hashlib.sha256(raw).digest()


def fixture_table():
    rows = [("nvs", 1, 2, 0x9000, 0x6000), ("phy_init", 1, 1, 0xF000, 0x1000),
            ("factory", 0, 0, 0x10000, 0x650000), ("store", 1, 2, 0x660000, 0x4000),
            ("netcfg", 1, 2, 0x6BC000, 0x4000), ("recovery", 0, 0x20, 0x6C0000, 0x140000)]
    raw = b"".join(struct.pack("<HBBII16sI", 0x50AA, kind, subtype, at, size, label.encode(), 0)
                   for label, kind, subtype, at, size in rows)
    raw += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(raw).digest()
    return raw.ljust(3072, b"\xff")


class ExporterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory();self.root = Path(self.temp.name).resolve()
        for name in exporter.REQUIRED_SOURCE:
            path = self.root / name;path.parent.mkdir(parents=True, exist_ok=True);path.write_text("public fixture\n")
        shutil.copyfile(ROOT / "scripts/flash-device.py", self.root / "scripts/flash-device.py")
        (self.root / "firmware/tools").mkdir(parents=True, exist_ok=True)
        (self.root / "firmware/tools/archive_firmware.py").write_text("# mock archive verifier entry\n")
        (self.root / ".github/workflows").mkdir(parents=True)
        (self.root / ".github/workflows/check.yml").write_text("name: public checks\n")
        (self.root / ".gitignore").write_text("dist/\n")
        app, table, boot = fixture_app(), fixture_table(), b"synthetic bootloader"
        full = bytearray(b"\xff" * (65536 + len(app)))
        for offset, data in ((0, boot), (32768, table), (65536, app)):full[offset:offset + len(data)] = data
        full = bytes(full);full_sha = exporter.digest(full)
        self.archive_relative = Path("firmware/build/firmware") / full_sha
        self.archive = self.root / self.archive_relative;self.archive.mkdir(parents=True)
        data = {"FoloToy-AI-Passport.bin": app, "FoloToy-AI-Passport-full.bin": full,
                "bootloader/bootloader.bin": boot, "partition_table/partition-table.bin": table,
                "FoloToy-AI-Passport.elf": b"private debug-only fixture", "FoloToy-AI-Passport.map": b"debug-only fixture"}
        for name, value in data.items():
            path = self.archive/name;path.parent.mkdir(parents=True, exist_ok=True);path.write_bytes(value)
        metadata = {"schema_version": 1, "target": "esp32c3", "flash_size_bytes": 8388608,
            "full_bin_sha256": full_sha, "app_descriptor": {"idf_version": "v5.5.3", "project_name": "FoloToy-AI-Passport", "version": "1"},
            "image_offsets": {"FoloToy-AI-Passport.bin": 65536, "bootloader/bootloader.bin": 0, "partition_table/partition-table.bin": 32768},
            "files": {name: {"size": len(value), "sha256": exporter.digest(value)} for name, value in data.items()}}
        (self.archive/"manifest.json").write_text(json.dumps(metadata))
        self.calls = []

    def tearDown(self):self.temp.cleanup()

    def runner(self, argv, **kwargs):
        self.calls.append(argv)
        self.assertEqual(argv[-2], "verify")
        self.assertEqual(Path(argv[-1]), self.archive)
        return SimpleNamespace(returncode=0)

    def export(self, output="dist/public-preview", replace=False, *, private_blocklist=None):
        return exporter.export_release(self.root, self.archive_relative, output, replace, self.runner,
                                       private_blocklist=private_blocklist)

    def test_export_contains_exact_public_images_and_required_install_inputs(self):
        state = self.export();out = self.root/"dist/public-preview"
        self.assertEqual(len(self.calls), 1)
        manifest = json.loads((out/"release/manifest.json").read_text())
        self.assertEqual(manifest["schema_version"], 1);self.assertEqual(manifest["chip"], "esp32c3")
        self.assertEqual(manifest["images"]["partition_table"]["path"], "partition-table.bin")
        self.assertEqual(manifest["images"]["app"]["offset"], 65536)
        self.assertFalse(manifest["installation_boundaries"]["permanent_recovery_binary_included"])
        with zipfile.ZipFile(out/"installation.zip") as package:
            names = set(package.namelist())
            self.assertTrue(exporter.REQUIRED_SOURCE <= names)
            self.assertIn("release/FoloToy-AI-Passport.bin", names)
            self.assertIn(".github/workflows/check.yml", names);self.assertIn(".gitignore", names)
            self.assertFalse(any(name.endswith((".elf", ".map")) for name in names))
        with zipfile.ZipFile(out/"source.zip") as package:
            self.assertFalse(any(name.startswith("release/") or name.endswith(".bin") for name in package.namelist()))
        self.assertEqual(state["full_sha256"], self.archive.name)
        self.assertIn("installation.zip", (out/"sha256s.txt").read_text())

    def test_runtime_git_build_dependencies_old_private_and_debug_never_zip(self):
        excluded = ["server/.env", "server/data/auth.sqlite", "server/media/audio.wav", "server/.local/setup.html",
                    "firmware/build/private.log", "firmware/managed_components/private.txt", ".git/config", "02-old/private.txt",
                    "old-private/export.log", "server/__pycache__/cache.pyc", "firmware/sdkconfig", "server/debug.elf"]
        for name in excluded:
            path = self.root/name;path.parent.mkdir(parents=True, exist_ok=True);path.write_text("private fixture data")
        self.export()
        for name in ("installation.zip", "source.zip"):
            with zipfile.ZipFile(self.root/"dist/public-preview"/name) as package:
                self.assertFalse(set(excluded) & set(package.namelist()))

    def test_exact_image_cache_interface_stub_retained_runtime_caches_still_excluded(self):
        required = "firmware/tests/podcast_cover_stubs/src/misc/cache/instance/lv_image_cache.h"
        stub = b"/* public host-test interface */\ntypedef struct lv_image_cache lv_image_cache_t;\n"
        (self.root/required).write_bytes(stub)
        excluded = ["server/cache/runtime.json",
                    "firmware/tests/podcast_cover_stubs/src/misc/cache/instance/private.json",
                    "firmware/tests/podcast_cover_stubs/src/misc/cache/other.h"]
        for name in excluded:
            path = self.root/name;path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("private cache fixture\n")
        self.export()
        for name in ("installation.zip", "source.zip"):
            with zipfile.ZipFile(self.root/"dist/public-preview"/name) as package:
                self.assertEqual(package.read(required), stub)
                self.assertFalse(set(excluded) & set(package.namelist()))

    def test_internal_report_external_art_and_apple_metadata_never_zip(self):
        excluded = ["docs/claude-fix-report.md", "firmware/assets/images/podcast-radio-selected.png",
                    "server/static/assets/station-art-20261004.jpg",
                    "firmware/assets/images/home.jpg", "firmware/assets/images/logo.png",
                    "firmware/assets/images/logo-wordmark.png", "firmware/assets/images/logo-wordmark-dark.png",
                    "firmware/assets/images/readme-hardware-specs.png",
                    "firmware/docs/brand/ai-passport-back.webp",
                    "firmware/docs/brand/ai-passport-front.png",
                    "firmware/docs/brand/ai-passport-views.jpg",
                    "firmware/docs/brand/ai-passport-front-eva-00.png",
                    "firmware/docs/brand/ai-passport-front-eva-01.png",
                    "firmware/docs/brand/ai-passport-front-eva-02.png",
                    "docs/._README.md", "firmware/main/._podcast_ui.c",
                    "server/._metadata/private.txt", "server/__MACOSX/source.txt",
                    "firmware/assets/images/podcast-covers/originals/show.jpg",
                    "firmware/assets/images/podcast-covers/show-112.png"]
        for name in excluded:
            path = self.root/name;path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("private fixture: /Users/" + "export-fixture-user" + "/private\n")
        retained = ["firmware/assets/images/podcast-covers/README.md",
                    "firmware/assets/images/podcast-covers/README.zh_CN.md",
                    "firmware/assets/images/podcast-covers/manifest.json",
                    "firmware/tools/generate_podcast_covers.py",
                    "assets/publication/generate.py"]
        for name in retained:
            path = self.root/name;path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps({"artwork_policy": "original-neutral", "artworks": []})
                            if name.endswith("manifest.json") else "public neutral artwork fixture\n")
        self.export()
        for name in ("installation.zip", "source.zip"):
            with zipfile.ZipFile(self.root/"dist/public-preview"/name) as package:
                names = set(package.namelist())
                self.assertFalse(set(excluded) & names)
                self.assertTrue(set(retained) <= names)

    def test_internal_brand_catalogues_and_asset_reference_links_get_public_variants(self):
        for name in exporter.PUBLIC_DOCUMENT_VARIANTS:
            path = self.root/name;path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("private catalogue /Users/" + "export-fixture-user" + "/private\n")
        assets = self.root/"firmware/assets/README.md";assets.parent.mkdir(parents=True, exist_ok=True)
        assets.write_text("Public fonts documentation\n"
                          "| [reference](images/podcast-radio-selected.png) | internal |\n"
                          "| [`images/podcast-covers/`](images/podcast-covers/README.md) | old source art |\n")
        self.export()
        with zipfile.ZipFile(self.root/"dist/public-preview/source.zip") as package:
            for name, expected in exporter.PUBLIC_DOCUMENT_VARIANTS.items():
                self.assertEqual(package.read(name), expected)
            readme = package.read("firmware/assets/README.md")
            self.assertIn(b"Noto Sans SC font license", readme)
            self.assertIn(b"Neutral podcast covers", readme)
            self.assertNotIn(b"podcast-radio-selected.png", readme)
        self.assertIn("private catalogue", (self.root/"firmware/docs/brand/README.md").read_text())

    def test_firmware_documentation_omits_internal_images_and_preserves_guides(self):
        path = self.root/"firmware/docs/README.md";path.parent.mkdir(parents=True, exist_ok=True)
        original = ('Public guide\n<p align="center"><picture>'
                    '<img src="../assets/images/logo-wordmark.png"></picture></p>\n'
                    '<p align="center"><strong>Public product text</strong></p>\n'
                    '<p align="center"><img src="../assets/images/home.jpg"></p>\n')
        path.write_text(original)
        self.export()
        with zipfile.ZipFile(self.root/"dist/public-preview/source.zip") as package:
            data = package.read("firmware/docs/README.md")
            self.assertIn(b"Public guide", data);self.assertIn(b"Public product text", data)
            self.assertNotIn(b"../assets/images/", data)
        self.assertEqual(path.read_text(), original)

    def test_publisher_artwork_manifest_cannot_be_exported_even_without_private_addresses(self):
        path = self.root/"firmware/assets/images/podcast-covers/manifest.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"artworks": [{"id": "public-show", "original": "show.jpg"}]}))
        with self.assertRaisesRegex(exporter.ExportError, "original neutral artwork"):
            self.export()

    def test_archive_verifier_rejection_never_falls_back_or_produces_output(self):
        runner = lambda *args, **kwargs: SimpleNamespace(returncode=1)
        with self.assertRaisesRegex(exporter.ExportError, "no old-build fallback"):
            exporter.export_release(self.root, self.archive_relative, "dist/public-preview", runner=runner)
        self.assertFalse((self.root/"dist/public-preview").exists())

    def test_hash_corruption_detected_even_if_mock_archive_verifier_accepts(self):
        path = self.archive/"FoloToy-AI-Passport.bin";path.write_bytes(path.read_bytes()+b"corrupt")
        with self.assertRaisesRegex(exporter.ExportError, "hash/size"):
            self.export()
        self.assertFalse((self.root/"dist/public-preview").exists())

    def test_user_absolute_path_and_runtime_credential_refuse(self):
        values = fixture_user_paths() + ["WIFI_PASSWORD='synthetic-network-password'"]
        for value in values:
            (self.root/"server/private-fixture.py").write_text(value)
            with self.assertRaisesRegex(exporter.ExportError, "Sensitive"):
                self.export()
            self.assertFalse((self.root/"dist/public-preview").exists())

    def test_user_paths_rejected_setup_and_placeholder_examples_allowed(self):
        for value in fixture_user_paths():
            for name in ("server/example.py", "tests/fixture.py"):
                with self.subTest(path_style=fixture_user_paths().index(value), file=name):
                    self.assertEqual(exporter.sensitive_reason(value.encode(), name), "user absolute path")
        placeholders = b"/Users/<user>/project /home/${USER}/project"
        self.assertIsNone(exporter.sensitive_reason(placeholders, "docs/setup.md"))
        public_examples = b"http://192.168.4.1 http://127.0.0.1 http://192.0.2.10"
        self.assertIsNone(exporter.sensitive_reason(public_examples, "docs/setup.md"))

    def test_exact_secret_key_and_api_key_literals_are_rejected(self):
        for name in ("SECRET_KEY", "API_KEY", "APP_SECRET_KEY"):
            for declaration in (f'{name} = "nonplaceholder-real-looking-value"',
                                json.dumps({name: "nonplaceholder-real-looking-value"}),
                                f'#define {name} "nonplaceholder-real-looking-value"'):
                with self.subTest(name=name, declaration=declaration[:12]):
                    self.assertEqual(exporter.sensitive_reason(declaration.encode(), "server/config.py"),
                                     "hardcoded configuration credential")

    def test_simulated_generic_test_addresses_allowed_complete_private_keys_not(self):
        p = self.root/"tests/fixture.py";p.parent.mkdir(exist_ok=True)
        p.write_text("# synthetic fixture http://192.0.2.15\n")
        self.export()
        key = "-----BEGIN PRIVATE KEY-----\n" + "A" * 160 + "\n-----END PRIVATE KEY-----\n"
        p.write_text(key)
        with self.assertRaisesRegex(exporter.ExportError, "private key"):
            self.export("dist/key-rejected")

    def test_complete_github_credentials_rejected_even_in_tests(self):
        for prefix in ("ghp_", "github_pat_"):
            value = prefix + "A" * 32
            self.assertEqual(exporter.sensitive_reason(value.encode(), "tests/fixture.py"),
                             "private key or access credential")

    def private_values_file(self, values, name="export-private-values.json"):
        path = self.root/".local"/name
        path.parent.mkdir(exist_ok=True)
        path.write_text(json.dumps(values))
        return path.relative_to(self.root)

    def test_default_private_values_reject_source_without_echoing_value(self):
        value = "http://192.0.2.77:8899"
        self.private_values_file([value])
        (self.root/"server/private-fixture.py").write_text(value)
        with self.assertRaisesRegex(exporter.ExportError, "private blocklist") as caught:
            self.export()
        self.assertNotIn(value, str(caught.exception))
        self.assertFalse((self.root/"dist/public-preview").exists())

    def test_private_values_check_retained_firmware_without_echoing_value(self):
        value = "synthetic bootloader"
        path = self.private_values_file([value], "custom-values.json")
        with self.assertRaisesRegex(exporter.ExportError, "private blocklist.*retained firmware") as caught:
            self.export(private_blocklist=path)
        self.assertNotIn(value, str(caught.exception))
        self.assertFalse((self.root/"dist/public-preview").exists())

    def test_private_values_check_final_public_replacement(self):
        self.private_values_file(["Noto Sans SC font license"])
        with self.assertRaisesRegex(exporter.ExportError, "private blocklist.*public replacement"):
            exporter.source_snapshot(self.root)

    def test_private_values_check_generated_release_manifest(self):
        value = "fictional-generated-project"
        self.private_values_file([value])
        path = self.archive/"manifest.json"
        metadata = json.loads(path.read_text())
        metadata["app_descriptor"]["project_name"] = value
        path.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(exporter.ExportError, "private blocklist.*public output") as caught:
            self.export()
        self.assertNotIn(value, str(caught.exception))
        self.assertFalse((self.root/"dist/public-preview").exists())

    def test_private_values_check_file_names_without_echoing_them(self):
        value = "fictional-private-label"
        self.private_values_file([value])
        (self.root/"server"/(value+".py")).write_text("public fixture")
        with self.assertRaisesRegex(exporter.ExportError, "Sensitive public file name") as caught:
            self.export()
        self.assertNotIn(value, str(caught.exception))
        self.assertFalse((self.root/"dist/public-preview").exists())

    def test_private_values_use_literal_matching_and_file_never_enters_archives(self):
        value = "synthetic.example.invalid"
        path = self.private_values_file([value, value], "custom-values.json")
        source = self.root/"server/example.py"
        source.write_text("syntheticXexampleXinvalid")
        self.export(private_blocklist=path)
        self.assertEqual(exporter.load_private_values(self.root, path), (value.encode(),))
        for name in ("installation.zip", "source.zip"):
            with zipfile.ZipFile(self.root/"dist/public-preview"/name) as package:
                self.assertFalse(any(item.startswith(".local/") for item in package.namelist()))
                self.assertTrue(all(value.encode() not in package.read(item) for item in package.namelist()))
        source.write_text(value)
        with self.assertRaisesRegex(exporter.ExportError, "private blocklist"):
            self.export("dist/rejected", private_blocklist=path)

    def test_explicit_missing_private_values_preserve_prior_output(self):
        self.export()
        package = self.root/"dist/public-preview/source.zip"
        before = package.read_bytes()
        with self.assertRaisesRegex(exporter.ExportError, "Private blocklist"):
            self.export(replace=True, private_blocklist=".local/missing.json")
        self.assertEqual(package.read_bytes(), before)

    def test_invalid_private_values_refuse_and_do_not_create_output(self):
        path = self.private_values_file([])
        for index, payload in enumerate(("broken-json", "{}", "[17]", '[" "]', '["\\u0000"]')):
            (self.root/path).write_text(payload)
            with self.subTest(case=index), self.assertRaisesRegex(exporter.ExportError, "Private blocklist"):
                self.export(f"dist/invalid-{index}")
            self.assertFalse((self.root/f"dist/invalid-{index}").exists())

    def test_private_values_must_stay_local_and_cannot_use_symlinks(self):
        public = self.root/"docs/values.json"
        public.write_text('["fictional-private-value"]')
        with self.assertRaisesRegex(exporter.ExportError, "inside project .local"):
            self.export(private_blocklist=public)
        path = self.root/".local/export-private-values.json"
        path.parent.mkdir(exist_ok=True)
        path.symlink_to(public)
        with self.assertRaisesRegex(exporter.ExportError, "symlinks"):
            self.export()
        self.assertFalse((self.root/"dist/public-preview").exists())

    def test_clean_exporter_and_its_tests_pass_their_own_source_scan(self):
        for name in ("scripts/export-release.py", "tests/test_export_release.py"):
            path = self.root/name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT/name, path)
        snapshot = exporter.source_snapshot(self.root)
        self.assertIn("scripts/export-release.py", snapshot)
        self.assertIn("tests/test_export_release.py", snapshot)

    def test_cli_forwards_explicit_local_private_values(self):
        with mock.patch.object(exporter, "export_release", return_value={"full_sha256":"0"*64}) as run, mock.patch("sys.stdout"):
            self.assertEqual(exporter.main(["--archive", "fixture/archive", "--private-blocklist", ".local/values.json"]), 0)
        run.assert_called_once_with(ROOT, "fixture/archive", "dist/public-preview", False,
                                    private_blocklist=".local/values.json")

    def test_required_compose_or_instructions_missing_refuse(self):
        (self.root/"server/compose.yaml").unlink()
        with self.assertRaisesRegex(exporter.ExportError, "Missing public package"):
            self.export()

    def test_required_third_party_license_missing_refuse(self):
        (self.root/"server/static/assets/icons/LICENSE").unlink()
        with self.assertRaisesRegex(exporter.ExportError, "Missing public package"):
            self.export()

    def test_public_symlink_and_absolute_or_escaping_inputs_refuse(self):
        link = self.root/"server/linked.py";link.symlink_to(self.root/"README.md")
        with self.assertRaisesRegex(exporter.ExportError, "symlink"):
            self.export()
        link.unlink()
        for archive in (self.archive, "../other-archive"):
            with self.assertRaises(exporter.ExportError):
                exporter.export_release(self.root, archive, "dist/public-preview", runner=self.runner)
        with self.assertRaises(exporter.ExportError):
            exporter.export_release(self.root, self.archive_relative, "../outside", runner=self.runner)

    def test_existing_unknown_output_preserved_and_exporter_replace_recoverable(self):
        unknown = self.root/"dist/public-preview";unknown.mkdir(parents=True);(unknown/"user.txt").write_text("keep")
        with self.assertRaises(exporter.ExportError):self.export(replace=True)
        self.assertEqual((unknown/"user.txt").read_text(), "keep")
        (unknown/"export-state.json").write_text('{"kind":"not-an-export"}')
        with self.assertRaises(exporter.ExportError):self.export(replace=True)
        shutil.rmtree(unknown)
        self.export();before = (unknown/"source.zip").read_bytes()
        self.export(replace=True)
        previous = list(unknown.parent.glob("public-preview.previous-*"))
        self.assertEqual(len(previous), 1);self.assertEqual((previous[0]/"source.zip").read_bytes(), before)

    def test_malformed_archive_metadata_is_safe_rejection(self):
        (self.archive/"manifest.json").write_text('[]')
        with self.assertRaises(exporter.ExportError):self.export()


if __name__ == "__main__":unittest.main()
