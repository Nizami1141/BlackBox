"""Run after installing dependencies: python registry/test_smoke.py."""
import os
import tempfile
from pathlib import Path

from fastapi.testclient import TestClient

with tempfile.TemporaryDirectory() as directory:
    os.environ["DB_PATH"] = str(Path(directory) / "devices.db")
    os.environ["DEVICE_API_KEY"] = "test-device-key"
    import server

    with TestClient(server.app) as client:
        key = {"X-API-Key": "test-device-key"}
        register = {"device_id": "GATE-01", "region": "astara", "name": "Post"}
        assert client.post("/api/v1/devices/register", json=register).status_code == 401
        assert client.post("/api/v1/devices/register", json=register, headers=key).status_code == 200
        assert client.post("/api/v1/devices/GATE-01/heartbeat",
                           json={"ip": "172.16.6.52", "fw_version": "cc2-1.0"},
                           headers=key).status_code == 200
        assert client.post("/api/v1/devices/GATE-01/logs",
                           json={"logs": [{"level": "info", "message": "boot"}]},
                           headers=key).json()["stored"] == 1
        devices = client.get("/api/v1/devices").json()
        assert devices[0]["status"] == "online"
        assert devices[0]["heartbeat_count"] == 1
        assert client.get("/api/v1/regions").json()[0]["online"] == 1
        assert client.get("/api/v1/logs").json()[0]["message"] == "boot"
        assert "boot" in client.get("/api/v1/logs/export.csv").text
        assert client.get("/").status_code == 200
        assert client.get("/health").json()["ok"] is True

print("Device Registry smoke test: OK")
