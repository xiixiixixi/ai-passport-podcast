import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("podcast_product", Path(__file__).resolve().parents[1] / "tools/check_podcast_product.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ProductSizeTests(unittest.TestCase):
    def test_source_layout_and_wrong_recovery_type(self):
        source = Path(__file__).resolve().parents[1] / "partitions.csv"
        module.check_layout(source)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "partitions.csv"
            path.write_text(source.read_text().replace("app,  test", "app,  ota_0"))
            with self.assertRaises(ValueError):
                module.check_layout(path)
            path.write_text(source.read_text().replace("0x650000", "0x7f0000"))
            with self.assertRaises(ValueError):
                module.check_layout(path)

    def test_real_files_at_release_boundary(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "app.bin"
            for size in (1, module.MAX_APP_BYTES - 1, module.MAX_APP_BYTES):
                with path.open("wb") as out:
                    out.truncate(size)
                self.assertEqual(module.check_app(path), size)
            for size in (0, module.MAX_APP_BYTES + 1, 6698384):
                with path.open("wb") as out:
                    out.truncate(size)
                with self.assertRaises(ValueError):
                    module.check_app(path)

    def test_missing_artifact_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(FileNotFoundError):
                module.check_app(Path(tmp) / "missing.bin")


if __name__ == "__main__":
    unittest.main()
