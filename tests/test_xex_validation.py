"""Regression checks for corruption the former size-only validator accepted."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("check_xex", ROOT / "xbox360/check_xex.py")
check_xex = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(check_xex)


class XexValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = ROOT / "xbox360/out/remote360_loader_probe.xex"
        if not cls.fixture.exists():
            raise unittest.SkipTest("Build the Xbox diagnostic probe first")
        cls.original = cls.fixture.read_bytes()

    def assert_rejected(self, data, message):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "corrupted.xex"
            path.write_bytes(data)
            with self.assertRaisesRegex(ValueError, message):
                check_xex.xex_image_size(path)

    def test_generated_images_pass(self):
        for path in sorted((ROOT / "xbox360/out").glob("*.xex")):
            with self.subTest(image=path.name):
                self.assertGreater(check_xex.xex_image_size(path), 0)

    def test_truncated_image_is_rejected(self):
        self.assert_rejected(self.original[:-1], "truncated image")

    def test_modified_code_is_rejected(self):
        data = bytearray(self.original)
        payload = struct.unpack_from(">I", data, 8)[0]
        data[payload + 0x10000] ^= 1
        self.assert_rejected(data, "image integrity hash mismatch")

    def test_modified_header_is_rejected(self):
        data = bytearray(self.original)
        payload = struct.unpack_from(">I", data, 8)[0]
        data[payload - 1] ^= 1
        self.assert_rejected(data, "header integrity hash mismatch")

    def test_invalid_security_offset_is_rejected(self):
        data = bytearray(self.original)
        struct.pack_into(">I", data, 16, 0xffff0000)
        self.assert_rejected(data, "invalid header bounds")


if __name__ == "__main__":
    unittest.main()
