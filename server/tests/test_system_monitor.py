import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from core.system_monitor import OFFLINE_AFTER_S, SystemMonitor


class FakeDb:
    def __init__(self, last_seen):
        self.last_seen = last_seen

    def list_devices(self):
        return [{"device_id": "GATE-01", "last_seen": self.last_seen}]


class SystemMonitorTests(unittest.TestCase):
    def test_txt_is_untouched_while_esp_responds(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "system_status.txt"
            db = FakeDb(100.0)
            device = SimpleNamespace(device_id="GATE-01", link={"type": "http"})
            monitor = SystemMonitor(db, [device], report, started_at=100.0)

            self.assertEqual(monitor.check_once(now=100.0 + OFFLINE_AFTER_S - 4), [])
            self.assertFalse(report.exists())
            self.assertEqual(monitor.check_once(now=100.0 + OFFLINE_AFTER_S + 1), ["GATE-01"])
            self.assertIn("status=offline", report.read_text(encoding="utf-8"))

            db.last_seen = 100.0 + OFFLINE_AFTER_S + 3
            original = report.read_bytes()
            original_mtime = report.stat().st_mtime_ns
            self.assertEqual(monitor.check_once(now=100.0 + OFFLINE_AFTER_S + 6), [])
            self.assertEqual(report.read_bytes(), original)
            self.assertEqual(report.stat().st_mtime_ns, original_mtime)

            self.assertEqual(monitor.check_once(now=100.0 + OFFLINE_AFTER_S * 2 + 4), ["GATE-01"])
            self.assertIn("last_seen_utc=", report.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
