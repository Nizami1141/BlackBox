"""The central AKS path is never allowed to fall back to local face matching."""
from __future__ import annotations

import asyncio
import os
import sys

import httpx
import pytest
from fastapi.testclient import TestClient

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import api.central_aks as upstream_mod  # noqa: E402
import api.server as server_mod  # noqa: E402
import core.service as service_mod  # noqa: E402
from core.config import Config, DeviceCfg  # noqa: E402
from core.flow import CheckpointFlow  # noqa: E402
from core.service import CheckpointService  # noqa: E402
from link import LinkRegistry  # noqa: E402

from .test_flow import StubEngine, _face, _jpeg  # noqa: E402


def test_central_client_uses_fixed_multipart_endpoints_and_key(monkeypatch):
    seen = []

    def handler(request: httpx.Request) -> httpx.Response:
        seen.append(request)
        if request.url.path.endswith("/recognize"):
            return httpx.Response(200, json={
                "status": "success", "faces": [{
                    "match": True, "log_id": 145,
                    "user": {"first_name": "Elnur", "last_name": "Abushov"},
                }],
            })
        return httpx.Response(200, json={"status": "success", "message": "saved"})

    original = httpx.AsyncClient
    transport = httpx.MockTransport(handler)
    monkeypatch.setattr(upstream_mod.httpx, "AsyncClient",
                        lambda **kwargs: original(transport=transport, **kwargs))
    client = upstream_mod.CentralAksClient("http://192.168.250.106/api", "secret", 10)
    result = asyncio.run(client.recognize(b"\xff\xd8test\xff\xd9"))
    assert result["faces"][0]["log_id"] == 145
    asyncio.run(client.log_cargo(145, "TR-123"))
    assert [req.url.path for req in seen] == ["/api/recognize", "/api/log_cargo"]
    assert all(req.headers["X-API-Key"] == "secret" for req in seen)
    assert all(req.headers["content-type"].startswith("multipart/form-data")
               for req in seen)
    assert b'name="file"' in seen[0].content
    assert b'name="log_id"' in seen[1].content
    assert b'name="cargo_barcode"' in seen[1].content


def test_central_client_reports_whitelist_failure(monkeypatch):
    original = httpx.AsyncClient
    transport = httpx.MockTransport(
        lambda request: httpx.Response(403, json={"detail": "forbidden"}))
    monkeypatch.setattr(upstream_mod.httpx, "AsyncClient",
                        lambda **kwargs: original(transport=transport, **kwargs))
    client = upstream_mod.CentralAksClient("http://192.168.250.106/api", "secret", 10)
    with pytest.raises(upstream_mod.CentralAksError) as exc:
        asyncio.run(client.recognize(b"jpeg"))
    assert exc.value.reason == "CENTRAL_IP_NOT_ALLOWED"
    assert exc.value.upstream_status == 403


def test_central_client_rejects_match_without_name_and_surname(monkeypatch):
    original = httpx.AsyncClient
    transport = httpx.MockTransport(lambda request: httpx.Response(200, json={
        "status": "success", "faces": [{
            "match": True, "log_id": 145,
            "user": {"first_name": "Elnur"},
        }],
    }))
    monkeypatch.setattr(upstream_mod.httpx, "AsyncClient",
                        lambda **kwargs: original(transport=transport, **kwargs))
    client = upstream_mod.CentralAksClient("http://192.168.250.106/api", "secret", 10)
    with pytest.raises(upstream_mod.CentralAksError) as exc:
        asyncio.run(client.recognize(b"jpeg"))
    assert exc.value.reason == "CENTRAL_INVALID_MATCH_DATA"


@pytest.fixture()
def central_test_client(monkeypatch, tmp_path):
    monkeypatch.setattr(service_mod, "FaceEngine", StubEngine)

    class FakeAks:
        fail = False
        deny = False
        scanned = []

        def __init__(self, *args):
            pass

        async def recognize(self, jpeg):
            if self.fail:
                raise upstream_mod.CentralAksError(502, "CENTRAL_UNREACHABLE")
            if self.deny:
                return {"status": "success", "faces": [{
                    "match": False, "message": "Tanınmadı",
                }]}
            return {"status": "success", "faces": [{
                "match": True, "log_id": 145,
                "user": {"id": 999, "qr_id": "3633",
                         "first_name": "Elnur", "last_name": "Abushov"},
            }]}

        async def log_cargo(self, log_id, barcode):
            self.scanned.append((log_id, barcode))
            return {"status": "success", "message": "saved"}

    fake = FakeAks()
    monkeypatch.setattr(server_mod, "CentralAksClient", lambda *args: fake)

    cfg = Config()
    cfg.data_dir = tmp_path
    cfg.db_file = tmp_path / "test.db"
    cfg.models_dir = tmp_path
    cfg.web_dir = tmp_path / "no-web"
    cfg.devices = [DeviceCfg("GATE-01", "device-key")]
    cfg.aks_api_key = "local-key"
    cfg.aks_ip_devices = {"testclient": "GATE-01"}
    cfg.central_aks_enabled = True
    cfg.central_aks_base_url = "http://192.168.250.106/api"
    service = CheckpointService(cfg)
    registry = LinkRegistry()
    flow = CheckpointFlow(service, registry)
    app = server_mod.create_app(service, flow, registry, cfg)
    with TestClient(app) as client:
        yield client, fake, service


def _recognize(client):
    boundary = b"CentralTestBoundary"
    body = (
        b"--" + boundary + b"\r\n"
        b'Content-Disposition: form-data; name="file"; filename="face.jpg"\r\n'
        b"Content-Type: image/jpeg\r\n\r\n" + _jpeg(_face((200, 60, 40))) +
        b"\r\n--" + boundary + b"--\r\n"
    )
    return client.post("/api/recognize", content=body, headers={
        "X-API-Key": "local-key",
        "Content-Type": "multipart/form-data; boundary=CentralTestBoundary",
    })


def test_central_mode_returns_real_log_id_and_never_uses_local_match(central_test_client):
    client, fake, service = central_test_client
    assert client.get("/api/v1/health").json()["recognition_source"] == "central_aks"
    response = _recognize(client)
    assert response.status_code == 200, response.text
    assert response.json()["faces"][0]["log_id"] == 145
    assert service.index.size == 0  # no person exists in the local face database

    cargo = client.post("/api/log_cargo", headers={"X-API-Key": "local-key"},
                        data={"log_id": "145", "cargo_barcode": "TR-123"})
    assert cargo.status_code == 200, cargo.text
    assert fake.scanned == [(145, "TR-123")]
    assert client.post("/api/log_cargo", headers={"X-API-Key": "local-key"},
                       data={"log_id": "145", "cargo_barcode": "TR-123"}).status_code == 409


def test_central_failure_is_not_local_fallback(central_test_client):
    client, fake, _ = central_test_client
    fake.fail = True
    response = _recognize(client)
    assert response.status_code == 502
    assert response.json()["reason"] == "CENTRAL_UNREACHABLE"


def test_central_region_denial_cannot_advance_to_cargo(central_test_client):
    client, fake, _ = central_test_client
    fake.deny = True
    response = _recognize(client)
    assert response.status_code == 200, response.text
    assert response.json()["faces"] == [{"match": False, "message": "Tanınmadı"}]
    cargo = client.post("/api/log_cargo", headers={"X-API-Key": "local-key"},
                        data={"log_id": "145", "cargo_barcode": "TR-DENIED"})
    assert cargo.status_code == 409
    assert fake.scanned == []
