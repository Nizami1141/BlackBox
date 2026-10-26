"""Watch autonomous ESP posts and keep a TXT report of connectivity faults."""
from __future__ import annotations

import asyncio
import logging
import os
import time
from datetime import datetime, timezone
from pathlib import Path

log = logging.getLogger(__name__)

CHECK_INTERVAL_S = 3
OFFLINE_AFTER_S = 90


class SystemMonitor:
    def __init__(self, db, devices, report_path: Path,
                 started_at: float | None = None):
        self.db = db
        self.device_ids = tuple(
            d.device_id for d in devices
            if d.link.get("type", "http") in ("http", "none")
        )
        self.report_path = Path(report_path)
        self.started_at = time.time() if started_at is None else started_at

    def check_once(self, now: float | None = None) -> list[str]:
        """Check every post; never write the TXT while all ESP posts respond."""
        now = time.time() if now is None else now
        if now - self.started_at < OFFLINE_AFTER_S:
            return []  # allow the ESP to boot and send its first heartbeat

        last_seen = {row["device_id"]: row["last_seen"] or 0
                     for row in self.db.list_devices()}
        offline = [device_id for device_id in self.device_ids
                   if now - last_seen.get(device_id, 0) > OFFLINE_AFTER_S]
        if not offline:
            return []

        checked_at = datetime.fromtimestamp(now, timezone.utc).isoformat()
        lines = [f"checked_at_utc={checked_at}", "system=degraded",
                 "server=online"]
        for device_id in offline:
            seen = last_seen.get(device_id, 0)
            seen_text = (datetime.fromtimestamp(seen, timezone.utc).isoformat()
                         if seen else "never")
            lines.append(f"device={device_id} status=offline last_seen_utc={seen_text}")

        self.report_path.parent.mkdir(parents=True, exist_ok=True)
        temp_path = self.report_path.with_suffix(self.report_path.suffix + ".tmp")
        try:
            temp_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            os.replace(temp_path, self.report_path)
        except OSError:
            log.exception("Failed to write system status to %s", self.report_path)
        return offline

    async def run(self) -> None:
        while True:
            await asyncio.sleep(CHECK_INTERVAL_S)
            self.check_once()
