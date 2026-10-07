"""USB installer tests; all device IO is fake, never enumerate or open ports."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import importlib.metadata
import zlib

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/flash-device.py"
SPEC = importlib.util.spec_from_file_location("passport_flash_device", SCRIPT)
flash = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = flash
SPEC.loader.exec_module(flash)

# Explicit published protocol-four fixture, not generated from tool constants.
MODERN_ROWS = [
    ("nvs", 1, 2, 0x9000, 0x6000),
    ("phy_init", 1, 1, 0xF000, 0x1000),
    ("factory", 0, 0, 0x10000, 0x650000),
    ("store", 1, 2, 0x660000, 0x4000),
    ("netcfg", 1, 2, 0x6BC000, 0x4000),
    ("recovery", 0, 0x20, 0x6C0000, 0x140000),
]
LEGACY_ROWS = [
    ("nvs", 1, 2, 0x9000, 0x6000), ("phy_init", 1, 1, 0xF000, 0x1000),
    ("factory", 0, 0, 0x10000, 0x300000),
    ("cardid", 1, 2, 0x356000, 0x4000), ("recovery", 0, 0x20, 0x700000, 0x100000),
]


def table(rows=MODERN_ROWS, with_md5=True):
    raw = b"".join(struct.pack("<HBBII16sI", 0x50AA, kind, subtype, offset, size,
                               label.encode(), 0) for label, kind, subtype, offset, size in rows)
    if with_md5:
        raw += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(raw).digest()
    return raw.ljust(0x1000, b"\xff")


def app_image(chip=5, descriptor=True, payload=b"public-app"):
    common = struct.pack("<BBBBI", 0xE9, 1, 2, 0x30, 0x40380000)
    extended = struct.pack("<BBBBHBHHBBBBB", 0xEE, 0, 0, 0, chip, 0, 0, 65535, 0, 0, 0, 0, 1)
    segment = struct.pack("<I", 0xABCD5432 if descriptor else 0) + bytes(252) + payload
    raw = common + extended + struct.pack("<II", 0x3C000020, len(segment)) + segment
    checksum = 0xEF
    for byte in segment:
        checksum ^= byte
    raw += bytes((15 - len(raw) % 16) % 16) + bytes([checksum])
    return raw + hashlib.sha256(raw).digest()


class FakeBackend:
    def __init__(self, partition=table(), marker=b"\xff" * 4, recovery=None, fail_write=False):
        self.partition = partition
        self.marker = marker
        self.recovery = (app_image(payload=b"recovery") if recovery is None else recovery).ljust(0x140000, b"\xff")
        self.reads, self.writes = [], []
        self.connected = self.closed = self.rebooted = False
        self.fail_write = fail_write

    def connect(self):
        self.connected = True

    def read(self, offset, size):
        self.reads.append((offset, size))
        return {(0x8000, 0x1000): self.partition, (0x6B8000, 4): self.marker,
                (0x6C0000, 0x140000): self.recovery}[(offset, size)]

    def write(self, app):
        self.writes.append(app)
        if self.fail_write:
            raise OSError("simulated unknown delivery")

    def reboot(self):
        self.rebooted = True

    def close(self):
        self.closed = True


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.app = app_image()
        self.manifest = self.root / "firmware-manifest.json"
        self.write_manifest()
        self.package = flash.load_package(self.manifest)
        self.port = flash.Port("test-podcast-A", "test USB device", 0x303A, 0x1001)

    def tearDown(self):
        self.temp.cleanup()

    def write_manifest(self, adjust=None):
        (self.root / "app.bin").write_bytes(self.app)
        (self.root / "partition-table.bin").write_bytes(table())
        data = {"schema_version": 1, "chip": "esp32c3", "flash_size_bytes": 8388608,
                "images": {"app": {"path": "app.bin", "offset": 65536,
                                   "size": len(self.app), "sha256": hashlib.sha256(self.app).hexdigest()},
                           "partition_table": {"path": "partition-table.bin", "offset": 32768,
                                               "size": 4096, "sha256": hashlib.sha256(table()).hexdigest()}}}
        if adjust:
            adjust(data)
        self.manifest.write_text(json.dumps(data))

    def test_valid_modern_md5_and_half_open_sector_plan(self):
        p = flash.upgrade_plan(self.package, flash.parse_partition_table(table()))
        self.assertEqual((p.write_start, p.write_end), (0x10000, 0x10000 + len(self.app)))
        self.assertEqual(p.erase_end % 4096, 0)
        self.assertGreaterEqual(p.erase_end, p.write_end)
        self.assertFalse(flash.overlaps(0x10000, 0x356000, 0x356000, 0x35A000))

    def test_md5_missing_corrupt_truncated_and_empty_stop(self):
        bad = bytearray(table());bad[34] ^= 1
        for raw in (table(with_md5=False), bad, table()[:2048], b"\xff" * 4096):
            with self.subTest(raw=len(raw)), self.assertRaises(flash.SafetyError):
                flash.parse_partition_table(raw)

    def test_duplicate_overlap_bounds_alignment_and_label_control_stop(self):
        fixtures = [MODERN_ROWS + [MODERN_ROWS[0]],
            MODERN_ROWS + [("extra", 1, 2, 0x10000, 0x1000)],
            MODERN_ROWS + [("extra", 1, 2, 0x800000, 0x1000)],
            MODERN_ROWS + [("extra", 1, 2, 0x664001, 0x1000)],
            MODERN_ROWS + [("bad\nlabel", 1, 2, 0x664000, 0x1000)]]
        for rows in fixtures:
            with self.subTest(rows=rows[-1]), self.assertRaises(flash.SafetyError):
                flash.parse_partition_table(table(rows))

    def test_legacy_cardid_overlap_is_specific_and_never_written(self):
        large = flash.Image("modern-app.bin", 0x10000, "0" * 64, bytes(0x400000))
        package = flash.Package(large, self.package.table, self.package.partitions)
        backend = FakeBackend(partition=table(LEGACY_ROWS))
        with self.assertRaisesRegex(flash.SafetyError, "cardid.*0x356000"):
            flash.upgrade(package, self.port, backend, ask=lambda _: "UPGRADE", emit=lambda _: None)
        self.assertEqual(backend.writes, [])
        self.assertEqual(backend.reads, [(0x8000, 0x1000)])
        self.assertTrue(backend.closed)

    def test_small_legacy_app_still_does_not_guess_layout(self):
        with self.assertRaisesRegex(flash.SafetyError, "不是支持的现代"):
            flash.upgrade_plan(self.package, flash.parse_partition_table(table(LEGACY_ROWS)))

    def test_all_current_data_and_test_partitions_protected(self):
        large = flash.Image("app.bin", 0x10000, "0" * 64, bytes(0x400000))
        package = flash.Package(large, self.package.table, self.package.partitions)
        for kind, subtype in ((1, 2), (0, 0x20)):
            rows = [("factory", 0, 0, 0x10000, 0x300000), ("other", kind, subtype, 0x320000, 0x10000)]
            with self.subTest(kind=kind), self.assertRaisesRegex(flash.SafetyError, "数据或恢复"):
                flash.upgrade_plan(package, flash.parse_partition_table(table(rows)))

    def test_exact_six_mib_is_allowed_one_more_byte_is_rejected(self):
        for size, allowed in ((6291456, True), (6291457, False)):
            image = flash.Image("size-test", 0x10000, "0" * 64, bytes(size))
            p = flash.Package(image, self.package.table, self.package.partitions)
            if allowed:
                self.assertEqual(flash.upgrade_plan(p, self.package.partitions).erase_end, 0x610000)
            else:
                with self.assertRaises(flash.SafetyError):
                    flash.upgrade_plan(p, self.package.partitions)

    def test_missing_recovery_and_unfinished_install_marker_stop(self):
        for backend in (FakeBackend(recovery=bytes(64)), FakeBackend(marker=b"IPR4")):
            with self.subTest(marker=backend.marker), self.assertRaises(flash.SafetyError):
                flash.upgrade(self.package, self.port, backend, ask=lambda _: "UPGRADE", emit=lambda _: None)
            self.assertEqual(backend.writes, [])
            self.assertTrue(backend.closed)

    def test_app_malformed_wrong_chip_bootloader_digest_and_tail_stop(self):
        corrupt = bytearray(self.app);corrupt[-1] ^= 1
        for data in (self.app[:10], app_image(chip=9), app_image(descriptor=False), corrupt,
                     self.app + b"merged-other-content"):
            with self.subTest(length=len(data)), self.assertRaises(flash.SafetyError):
                flash.validate_app_image(data)
        self.assertEqual(flash.validate_app_image(self.app + b"\xff" * 512, True), len(self.app))

    def test_valid_recovery_unused_partition_tail_need_not_be_erased(self):
        backend = FakeBackend(recovery=app_image(payload=b"recovery") + b"unused old tail")
        self.assertFalse(flash.upgrade(self.package, self.port, backend, inspect_only=True, emit=lambda _: None))
        self.assertEqual(backend.writes, [])

    def test_manifest_hash_size_offset_and_boolean_version_stop(self):
        edits = [lambda d: d.update(schema_version=True),
                 lambda d: d["images"]["app"].update(sha256="0" * 64),
                 lambda d: d["images"]["app"].update(size=len(self.app) + 1),
                 lambda d: d["images"]["app"].update(offset=0),
                 lambda d: d["images"]["app"].update(size=True)]
        for edit in edits:
            self.write_manifest(edit)
            with self.assertRaises(flash.SafetyError):
                flash.load_package(self.manifest)

    def test_path_escape_windows_drive_and_outside_symlink_stop(self):
        for path in ("../app.bin", "C:\\app.bin", "/tmp/app.bin"):
            self.write_manifest(lambda d: d["images"]["app"].update(path=path))
            with self.subTest(path=path), self.assertRaises(flash.SafetyError):
                flash.load_package(self.manifest)
        outside = self.root.parent / (self.root.name + "-outside.bin")
        outside.write_bytes(self.app)
        try:
            (self.root / "escape.bin").symlink_to(outside)
            self.write_manifest(lambda d: d["images"]["app"].update(path="escape.bin"))
            with self.assertRaises(flash.SafetyError):
                flash.load_package(self.manifest)
        finally:
            outside.unlink()

    def test_duplicate_manifest_fields_stop(self):
        self.manifest.write_text('{"schema_version":1,"schema_version":1}')
        with self.assertRaisesRegex(flash.SafetyError, "重复"):
            flash.load_package(self.manifest)

    def test_manifest_size_and_path_resolution_failure_are_safe_errors(self):
        self.manifest.write_bytes(b" " * (1024 * 1024 + 1))
        with self.assertRaisesRegex(flash.SafetyError, "超过1MiB"):
            flash.load_package(self.manifest)
        loop = self.root / "loop.bin";loop.symlink_to(loop.name)
        self.write_manifest(lambda d: d["images"]["app"].update(path="loop.bin"))
        with self.assertRaises(flash.SafetyError):
            flash.load_package(self.manifest)

    def test_multiple_ports_and_one_port_both_need_explicit_selection(self):
        other = flash.Port("test-podcast-B", "second USB", 0x303A, 0x1001)
        ports = [self.port, other, flash.Port("test-display", "display", 1, 2)]
        with self.assertRaises(flash.SafetyError):
            flash.select_port(ports, None, ask=lambda _: "", emit=lambda _: None)
        with self.assertRaises(flash.SafetyError):
            flash.select_port([self.port], None, ask=lambda _: "", emit=lambda _: None)
        self.assertEqual(flash.select_port(ports, None, ask=lambda _: "2", emit=lambda _: None), other)
        with self.assertRaises(flash.SafetyError):
            flash.select_port(ports, "test-display")

    def test_inspect_cancel_and_confirm_have_exact_write_boundary(self):
        for inspect_only, answer, expected in ((True, "UPGRADE", 0), (False, "no", 0), (False, "UPGRADE", 1)):
            backend = FakeBackend();output = []
            result = flash.upgrade(self.package, self.port, backend, inspect_only, ask=lambda _: answer, emit=output.append)
            self.assertEqual(len(backend.writes), expected)
            self.assertEqual(result, bool(expected))
            self.assertEqual(backend.reads, [(0x8000, 0x1000), (0x6B8000, 4), (0x6C0000, 0x140000)])
            self.assertTrue(backend.closed)
            if expected:
                self.assertEqual(backend.writes[0].offset, 0x10000)
                self.assertEqual(backend.writes[0].data, self.app)
                self.assertTrue(backend.rebooted)
            self.assertTrue(any("只写主程序" in line for line in output))

    def test_disk_changed_after_confirmation_still_writes_verified_snapshot(self):
        backend = FakeBackend()
        def confirm(_):
            (self.root / "app.bin").write_bytes(b"unverified replacement")
            return "UPGRADE"
        flash.upgrade(self.package, self.port, backend, ask=confirm, emit=lambda _: None)
        self.assertEqual(backend.writes[0].data, self.app)

    def test_unknown_write_failure_is_once_and_no_reboot(self):
        backend = FakeBackend(fail_write=True)
        with self.assertRaisesRegex(flash.SafetyError, "结果可能不完整.*不自动重试"):
            flash.upgrade(self.package, self.port, backend, ask=lambda _: "UPGRADE", emit=lambda _: None)
        self.assertEqual(len(backend.writes), 1)
        self.assertFalse(backend.rebooted)
        self.assertTrue(backend.closed)

    def test_interrupt_during_write_is_not_reported_as_clean_cancel(self):
        backend = FakeBackend()
        backend.write = mock.Mock(side_effect=KeyboardInterrupt)
        with self.assertRaisesRegex(flash.SafetyError, "写入中被取消.*结果可能不完整"):
            flash.upgrade(self.package, self.port, backend, ask=lambda _: "UPGRADE", emit=lambda _: None)
        backend.write.assert_called_once()
        self.assertTrue(backend.closed)

    def test_initialize_and_local_check_never_enumerate_or_connect(self):
        with mock.patch.object(flash, "installed_ports", side_effect=AssertionError("must not enumerate")), mock.patch.object(flash, "EspBackend", side_effect=AssertionError("must not connect")), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(flash.main(["--initialize"]), 2)
            self.assertEqual(flash.main(["--check-package", "--manifest", str(self.manifest)]), 0)

    def test_noninteractive_cannot_auto_confirm_or_guess_port(self):
        with mock.patch.object(flash, "installed_ports", side_effect=AssertionError("must not enumerate")), mock.patch.object(sys.stdin, "isatty", return_value=False), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(flash.main(["--manifest", str(self.manifest), "--port", "test-podcast-A"]), 2)
            self.assertEqual(flash.main(["--manifest", str(self.manifest), "--inspect-only"]), 2)

    def test_cancel_exit_code_is_distinct_from_upgrade_success(self):
        with mock.patch.object(flash, "installed_ports", return_value=[self.port]), mock.patch.object(sys.stdin, "isatty", return_value=True), mock.patch.object(flash, "upgrade", return_value=False):
            self.assertEqual(flash.main(["--manifest", str(self.manifest), "--port", self.port.device]), 3)

    def test_real_adapter_checks_chip_exact_flash_and_security_before_flash_read(self):
        for chip, capacity, secure, allowed in (("ESP32-C3", 23, False, True),
             ("ESP32-S3", 23, False, False), ("ESP32-C3", 22, False, False),
             ("ESP32-C3", 24, False, False), ("ESP32-C3", 23, True, False)):
            device = SimpleNamespace(CHIP_NAME=chip, secure_download_mode=secure,
                get_secure_boot_enabled=lambda: False, get_flash_encryption_enabled=lambda: False,
                flash_spi_attach=mock.Mock(), flash_id=lambda: (capacity << 16) | 0x4020)
            device.run_stub = mock.Mock(return_value=device)
            detect = mock.Mock(return_value=device)
            module = SimpleNamespace(detect_chip=detect)
            backend = flash.EspBackend(self.port)
            with mock.patch.dict(sys.modules, {"esptool": module}):
                if allowed:
                    backend.connect()
                else:
                    with self.assertRaises(flash.SafetyError):
                        backend.connect()
            self.assertEqual(detect.call_args.kwargs["port"], "test-podcast-A")
            self.assertEqual(detect.call_args.kwargs["connect_attempts"], 1)
            self.assertFalse(detect.call_args.kwargs["trace_enabled"])
            if chip != "ESP32-C3" or secure:
                device.run_stub.assert_not_called()

    def test_library_ids_and_unknown_failure_details_are_never_logged(self):
        token = "private-device-identifier-not-for-log"
        def library_output():
            print(token)
            print(token, file=sys.stderr)
            return 123
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            self.assertEqual(flash.quiet_call(library_output), 123)
        self.assertNotIn(token, stdout.getvalue() + stderr.getvalue())
        backend = FakeBackend();backend.connect = mock.Mock(side_effect=RuntimeError(token))
        with self.assertRaises(flash.SafetyError) as error:
            flash.upgrade(self.package, self.port, backend, ask=lambda _: "UPGRADE", emit=lambda _: None)
        self.assertNotIn(token, str(error.exception));self.assertEqual(backend.writes, [])
        output = []
        private_port = flash.Port("test-podcast-A", token, 0x303A, 0x1001)
        flash.upgrade(self.package, private_port, FakeBackend(), inspect_only=True, emit=output.append)
        self.assertNotIn(token, "".join(output))

    def test_reboot_or_close_failure_does_not_replay_verified_write(self):
        backend = FakeBackend();backend.reboot = mock.Mock(side_effect=OSError("disconnected"))
        backend.close = mock.Mock(side_effect=OSError("closed"));output = []
        self.assertTrue(flash.upgrade(self.package, self.port, backend, ask=lambda _: "UPGRADE", emit=output.append))
        self.assertEqual(len(backend.writes), 1)
        self.assertTrue(any("不要因此重复写入" in line for line in output))

    def test_pinned_esptool_adapter_disables_both_retry_layers_and_only_passes_app(self):
        loader = SimpleNamespace(WRITE_BLOCK_ATTEMPTS=3)
        module = SimpleNamespace(loader=loader, write_flash=mock.Mock(), verify_flash=mock.Mock())
        backend = flash.EspBackend(self.port);backend.esp = SimpleNamespace(WRITE_FLASH_ATTEMPTS=3)
        with mock.patch.dict(sys.modules, {"esptool": module, "esptool.loader": loader}):
            backend.write(self.package.app)
        self.assertEqual(loader.WRITE_BLOCK_ATTEMPTS, 1)
        self.assertEqual(backend.esp.WRITE_FLASH_ATTEMPTS, 1)
        module.write_flash.assert_called_once();module.verify_flash.assert_called_once()
        args = module.write_flash.call_args.args[1]
        self.assertEqual(len(args.addr_filename), 1)
        self.assertEqual(args.addr_filename[0][0], 0x10000)
        self.assertEqual(args.addr_filename[0][1].getvalue(), self.app)
        self.assertFalse(args.erase_all or args.force or args.encrypt)
        self.assertEqual((args.flash_size, args.flash_mode, args.flash_freq), ("keep", "keep", "keep"))


def pinned_tool_available():
    try:
        return importlib.metadata.version("esptool") == "4.12.0"
    except importlib.metadata.PackageNotFoundError:
        return False


@unittest.skipUnless(pinned_tool_available(), "Optional pinned esptool 4.12.0 API acceptance")
class ActualEsptoolApiTests(unittest.TestCase):
    """Run real esptool code against an in-memory ESP endpoint, no serial IO."""
    def endpoint(self, fail=False):
        import serial
        class MemoryEsp:
            CHIP_NAME = "ESP32-C3"
            IMAGE_CHIP_ID = 5
            IS_STUB = True
            secure_download_mode = False
            BOOTLOADER_FLASH_OFFSET = 0
            FLASH_SECTOR_SIZE = 4096
            FLASH_WRITE_SIZE = 128
            FLASH_ENCRYPTED_WRITE_ALIGN = 32
            WRITE_FLASH_ATTEMPTS = 3
            def __init__(self):
                self.begins, self.blocks, self.finishes, self.digests = [], [], [], []
                self.written = bytearray()
            def get_secure_boot_enabled(self):return False
            def get_encrypted_download_disabled(self):return False
            def get_flash_encryption_enabled(self):return False
            def get_chip_revision(self):return 4
            def flash_id(self):return (23 << 16) | 0x4020
            def flash_defl_begin(self, size, compressed_size, address, encrypted_write=False):
                self.begins.append((size, compressed_size, address, encrypted_write))
                self.inflate = zlib.decompressobj()
                return (compressed_size + self.FLASH_WRITE_SIZE - 1) // self.FLASH_WRITE_SIZE
            def flash_defl_block(self, data, sequence, timeout=None):
                self.blocks.append(sequence)
                if fail:raise serial.SerialException("simulated unknown ACK")
                self.written.extend(self.inflate.decompress(data))
            def flash_defl_finish(self, reboot=False, timeout=None):
                self.finishes.append(reboot)
                self.written.extend(self.inflate.flush())
            def flash_md5sum(self, address, size):
                self.digests.append((address, size))
                return hashlib.md5(self.written[:size]).hexdigest()
            def connect(self, *args, **kwargs):raise AssertionError("must not reconnect")
        return MemoryEsp()

    def test_actual_pinned_write_and_verify_only_erase_write_app(self):
        data = app_image(payload=bytes(range(256)) * 3)
        image = flash.Image("public-app.bin", 0x10000, hashlib.sha256(data).hexdigest(), data)
        backend = flash.EspBackend(flash.Port("fake-no-serial", "fake", 0x303A, 0x1001))
        backend.esp = self.endpoint()
        backend.write(image)
        self.assertEqual(len(backend.esp.begins), 1)
        self.assertEqual(backend.esp.begins[0][0], len(data))
        self.assertEqual(backend.esp.begins[0][2:], (0x10000, False))
        self.assertEqual(bytes(backend.esp.written), data)
        self.assertEqual(backend.esp.finishes, [False])
        self.assertTrue(all(offset == 0x10000 for offset, _ in backend.esp.digests))

    def test_actual_pinned_unknown_ack_never_reconnects_or_replays(self):
        import serial
        data = app_image()
        backend = flash.EspBackend(flash.Port("fake-no-serial", "fake", 0x303A, 0x1001))
        backend.esp = self.endpoint(fail=True)
        with self.assertRaises(serial.SerialException):
            backend.write(flash.Image("public-app.bin", 0x10000, "0" * 64, data))
        self.assertEqual(len(backend.esp.begins), 1)
        self.assertEqual(backend.esp.blocks, [0])
        self.assertEqual(backend.esp.finishes, [])


if __name__ == "__main__":
    unittest.main()
