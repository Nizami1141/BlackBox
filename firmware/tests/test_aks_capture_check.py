"""Exercise binary JPEG export without hardware or an AKS request."""
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "aks_capture_check", Path(__file__).parents[1] / "tools" / "aks_capture_check.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class FakePort(io.BytesIO):
    def __init__(self, payload):
        super().__init__(payload)
        self.commands = []

    def open(self):
        pass

    def reset_input_buffer(self):
        pass

    def write(self, data):
        self.commands.append(data)
        return len(data)

    def read(self, size=-1):
        # Serial reads can return fewer bytes than requested, including
        # splits inside the JPEG markers and line breaks in the binary data.
        return super().read(min(size, 2))


class CaptureTest(unittest.TestCase):
    def run_capture(self, payload, destination, tls_only=False):
        port = FakePort(payload)
        args = ["capture", "--port", "COM9", "--output", destination]
        if tls_only:
            args.append("--tls-only")
        with patch.object(capture.serial, "Serial", return_value=port), \
             patch.object(capture.time, "sleep"), \
             patch("sys.argv", args):
            result = capture.main()
        self.assertEqual(port.commands, [b"DEBUG_TLS\n" if tls_only else b"DEBUG_CHECK\n"])
        return result

    def test_exports_exact_jpeg_and_aks_response(self):
        jpeg = b"\xff\xd8binary\nimage\xff\xd9"
        payload = (f"JPEG_BEGIN {len(jpeg)} 640 480\n".encode() + jpeg +
                   b'\nJPEG_END\n[eth] AKS response: {"status":"success","faces":[]}\n'
                   b"[eth] recognize: Uz tapilmadi, HTTP 200\n")
        with tempfile.TemporaryDirectory() as destination:
            self.assertEqual(self.run_capture(payload, destination), 0)
            files = list(Path(destination).glob("*.jpg"))
            self.assertEqual(len(files), 1)
            self.assertEqual(files[0].read_bytes(), jpeg)
            log = next(Path(destination).glob("*.txt")).read_text(encoding="utf-8")
            self.assertIn('"faces":[]', log)

    def test_invalid_jpeg_is_not_saved(self):
        with tempfile.TemporaryDirectory() as destination:
            with self.assertRaisesRegex(RuntimeError, "invalid JPEG boundaries"):
                self.run_capture(b"JPEG_BEGIN 4 640 480\nnope", destination)
            self.assertEqual(list(Path(destination).glob("*.jpg")), [])
            self.assertEqual(len(list(Path(destination).glob("*.txt"))), 1)

    def test_old_firmware_is_reported(self):
        with tempfile.TemporaryDirectory() as destination:
            with self.assertRaisesRegex(RuntimeError, "update the ESP32 firmware"):
                self.run_capture(b"[eth] recognize: Uz tapilmadi\n", destination)

    def test_tls_only_does_not_require_or_save_a_photo(self):
        with tempfile.TemporaryDirectory() as destination:
            self.assertEqual(self.run_capture(
                b"[cp] TLS_END passed=5 total=5\n", destination, tls_only=True), 0)
            self.assertEqual(list(Path(destination).glob("*.jpg")), [])

    def test_tls_only_reports_intermittent_failure(self):
        with tempfile.TemporaryDirectory() as destination:
            self.assertEqual(self.run_capture(
                b"[cp] TLS_END passed=3 total=5\n", destination, tls_only=True), 1)


if __name__ == "__main__":
    unittest.main()
