"""
Путь автономного поста: плата ходит к серверу сама по HTTP.

Именно этим путём работает пост на границе. Проверяется то же, что и для
сценария с BLE, но со стороны устройства:

  * без ключа устройства сервер не отвечает ничего полезного;
  * незнакомому человеку токен не выдаётся, а значит и записать груз
    не по чему;
  * токен привязан к посту и одноразовый;
  * в ответе есть готовые строки для экрана 1602 на азербайджанском —
    плата их не сочиняет, а показывает как есть.

Модель заменена той же заглушкой, что и в test_flow.py: проверяется порядок
и права, а не качество распознавания.

Запуск:  python -m pytest tests/test_device_api.py -v    (из папки server/)
"""
from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

import cv2
import pytest
from fastapi.testclient import TestClient

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import core.service as service_mod                      # noqa: E402
from core.config import Config, DeviceCfg               # noqa: E402
from core.flow import CheckpointFlow                    # noqa: E402
from core.service import CheckpointService              # noqa: E402
from link import LinkRegistry                           # noqa: E402

from .test_flow import StubEngine, _face, _jpeg         # noqa: E402

KEY1 = "key-gate-01"
KEY2 = "key-gate-02"
AKS_KEY = "test-aks-key"

LCD = {
    "ok2": "Kodu skan edin",
    "deny1": "Giriş qadağandır",
    "deny2": "Bazada yoxdur",
    "saved1": "Yük qeyd edildi",
    "err1": "Xəta",
}


@pytest.fixture()
def client(monkeypatch):
    monkeypatch.setattr(service_mod, "FaceEngine", StubEngine)

    tmp = Path(tempfile.mkdtemp())
    cfg = Config()
    cfg.data_dir = tmp
    cfg.db_file = tmp / "t.db"
    cfg.models_dir = tmp
    cfg.web_dir = tmp / "nonexistent"     # сайт в этом тесте не нужен
    cfg.threshold = 0.95
    cfg.session_ttl = 30
    cfg.session_max_scans = 1
    cfg.keep_enroll_thumbnail = False
    cfg.lcd = dict(LCD)
    cfg.devices = [
        DeviceCfg("GATE-01", KEY1, "полоса 1"),
        DeviceCfg("GATE-02", KEY2, "полоса 2"),
    ]
    cfg.aks_api_key = AKS_KEY
    cfg.aks_ip_devices = {"testclient": "GATE-01"}
    cfg.aks_allow_local_test = True

    service = CheckpointService(cfg)
    emp_id, _ = service.enroll("100", "Əliyev Rəşad Nemət", [_face((200, 60, 40))])

    registry = LinkRegistry()             # управляемых постов нет — все автономны
    flow = CheckpointFlow(service, registry, lcd=cfg.lcd)

    from api.server import create_app
    app = create_app(service, flow, registry, cfg)

    with TestClient(app) as c:
        c.__dict__["_service"] = service
        c.__dict__["_emp_id"] = emp_id
        yield c


def _hdr(device="GATE-01", key=KEY1):
    return {"X-Device-Id": device, "X-Device-Key": key}


KNOWN = None
STRANGER = None


def setup_module():
    global KNOWN, STRANGER
    KNOWN = _jpeg(_face((200, 60, 40)))
    STRANGER = _jpeg(_face((40, 200, 90)))


# --------------------------------------------------------------------- права
def test_no_key_is_rejected(client):
    r = client.post("/api/v1/verify", content=KNOWN)
    assert r.status_code == 401


def test_ethernet_web_trigger_requires_online_post_and_device_key(client):
    # A post that has not polled cannot be sent a browser command.
    r = client.post("/api/web/trigger", json={"device_id": "GATE-01"})
    assert r.status_code == 409

    assert client.get("/api/device/trigger").status_code == 401
    device_headers = {"X-API-Key": AKS_KEY}
    assert client.get("/api/device/trigger", headers=device_headers).json() == {
        "check": False,
    }

    state = client.get("/api/web/state").json()
    gate = next(d for d in state["devices"] if d["device_id"] == "GATE-01")
    assert gate["triggerable"] is True

    r = client.post("/api/web/trigger", json={"device_id": "GATE-01"})
    assert r.status_code == 200
    assert client.post("/api/web/trigger", json={"device_id": "GATE-01"}).status_code == 409
    assert client.get("/api/device/trigger", headers=device_headers).json() == {
        "check": True,
    }
    assert client.get("/api/device/trigger", headers=device_headers).json() == {
        "check": False,
    }


def test_wrong_key_is_rejected(client):
    # Ключ только из ASCII — заголовки HTTP другого не переносят.
    # Это не придирка теста: если вписать в api_key кириллицу, плата
    # физически не сможет отправить такой заголовок.
    r = client.post("/api/v1/verify", content=KNOWN, headers=_hdr(key="wrong-key"))
    assert r.status_code == 403


def test_health_is_open(client):
    # /health намеренно без ключа: пост должен уметь отличить «сервер лежит»
    # от «мой ключ не подошёл», ещё до первой проверки.
    r = client.get("/api/v1/health")
    assert r.status_code == 200
    assert r.json()["ok"]


# ------------------------------------------------------------------ сценарий
def test_full_pass(client):
    v = client.post("/api/v1/verify", content=KNOWN, headers=_hdr()).json()
    assert v["matched"], v
    assert v["session_token"]
    assert v["expires_in"] > 0

    # Строки для экрана приходят готовыми — плата их не сочиняет.
    assert v["lcd2"] == LCD["ok2"]
    assert "Əliyev" in v["lcd1"], f"на экране должна быть фамилия: {v['lcd1']!r}"
    assert len(v["lcd1"]) <= 16, "строка не влезет в экран 1602"

    s = client.post("/api/v1/scan", headers=_hdr(), json={
        "session_token": v["session_token"], "barcode": "TRK-4417", "symbology": "",
    }).json()
    assert s["ok"], s
    assert s["record_id"] > 0
    assert s["lcd1"] == LCD["saved1"]
    assert s["lcd2"] == "TRK-4417"

    rows = client._service.db.list_scan_events()
    assert len(rows) == 1
    assert rows[0]["barcode"] == "TRK-4417"
    assert rows[0]["device_id"] == "GATE-01"


def test_stranger_gets_no_token(client):
    """Главное свойство: незнакомцу не выдаётся токен.

    Без токена записать груз невозможно, даже если прошивка на посту
    подменена и зажигает луч сканера когда захочет.
    """
    v = client.post("/api/v1/verify", content=STRANGER, headers=_hdr()).json()
    assert not v["matched"]
    assert "session_token" not in v
    assert v["lcd1"] == LCD["deny1"]
    assert v["lcd2"] == LCD["deny2"]

    # Попытка записать груз с выдуманным токеном.
    s = client.post("/api/v1/scan", headers=_hdr(), json={
        "session_token": "0" * 32, "barcode": "TRK-9999", "symbology": "",
    })
    assert s.status_code == 409
    assert s.json()["reason"] == "SESSION_UNKNOWN"
    assert not client._service.db.list_scan_events()


def test_token_not_accepted_from_another_post(client):
    v = client.post("/api/v1/verify", content=KNOWN, headers=_hdr()).json()
    assert v["matched"]

    s = client.post("/api/v1/scan", headers=_hdr("GATE-02", KEY2), json={
        "session_token": v["session_token"], "barcode": "TRK-1", "symbology": "",
    })
    assert s.status_code == 409
    assert s.json()["reason"] == "SESSION_DEVICE_MISMATCH"


def test_token_is_single_use(client):
    v = client.post("/api/v1/verify", content=KNOWN, headers=_hdr()).json()
    tok = v["session_token"]

    first = client.post("/api/v1/scan", headers=_hdr(), json={
        "session_token": tok, "barcode": "TRK-A", "symbology": "",
    })
    assert first.status_code == 200

    second = client.post("/api/v1/scan", headers=_hdr(), json={
        "session_token": tok, "barcode": "TRK-B", "symbology": "",
    })
    assert second.status_code == 409
    assert second.json()["reason"] == "SESSION_EXHAUSTED"


def test_no_face_reports_reason(client):
    dark = _jpeg(_face((0, 0, 0)))
    v = client.post("/api/v1/verify", content=dark, headers=_hdr()).json()
    assert not v["matched"]
    assert v["reason"] == "NO_FACE"
    # Экран всё равно должен что-то показать, а не остаться пустым.
    assert v["lcd1"]


def test_autonomous_post_visible_in_web_state(client):
    """Автономный пост должен быть виден оператору на сайте.

    Управлять им нельзя, но знать, выходил ли он на связь, оператор обязан:
    иначе «пост молчит вторые сутки» заметят только по пустому журналу.
    """
    client.post("/api/v1/verify", content=KNOWN, headers=_hdr())

    st = client.get("/api/web/state").json()
    gates = {d["device_id"]: d for d in st["devices"]}
    assert "GATE-01" in gates
    assert gates["GATE-01"]["autonomous"] is True
    assert gates["GATE-01"]["connected"] is True     # только что стучался


# ------------------------------------------------- последний кадр для отладки
#
# У автономного поста нет ни оператора рядом, ни способа показать, что он снял.
# Когда сервер отвечает NO_FACE, единственный путь понять причину — увидеть
# сам кадр. Эти три теста держат ту гарантию, ради которой endpoint и сделан:
# кадр сохраняется ПРИ ЛЮБОМ исходе, отдаётся только локально и не пишется
# на диск.
def test_last_frame_saved_even_when_not_recognised(client):
    # Незнакомец: опознание провалится — и именно такой кадр нужен для разбора.
    r = client.post("/api/v1/verify", content=STRANGER, headers=_hdr())
    assert r.status_code == 200
    assert r.json()["matched"] is False

    img = client.get("/api/web/last-frame/GATE-01.jpg")
    assert img.status_code == 200
    assert img.headers["content-type"] == "image/jpeg"
    assert img.content == STRANGER          # тот самый байт-в-байт кадр


def test_last_frame_info_reports_reason(client):
    client.post("/api/v1/verify", content=STRANGER, headers=_hdr())
    info = client.get("/api/web/last-frame/GATE-01").json()
    assert info["device_id"] == "GATE-01"
    assert info["matched"] is False
    assert info["bytes"] == len(STRANGER)
    assert info["reason"]                   # причина отказа названа


def test_last_frame_is_per_device(client):
    # Кадры постов не должны смешиваться: у каждого свой последний.
    client.post("/api/v1/verify", content=KNOWN, headers=_hdr("GATE-01", KEY1))
    client.post("/api/v1/verify", content=STRANGER, headers=_hdr("GATE-02", KEY2))

    assert client.get("/api/web/last-frame/GATE-01.jpg").content == KNOWN
    assert client.get("/api/web/last-frame/GATE-02.jpg").content == STRANGER
    # От поста, который ещё не приходил, кадра нет — и это 404, а не пустота.
    assert client.get("/api/web/last-frame/SIM-01.jpg").status_code == 404


# ---------------------------------------- contract from esp32.html / W5500
def _aks_headers(key=AKS_KEY):
    return {"X-API-Key": key}


def _aks_recognize(client, image=None, key=AKS_KEY):
    if image is None:
        image = KNOWN
    # Same wire format as firmware/src/eth_link.cpp.  Building it explicitly
    # also avoids the requests/httpx multipart incompatibility in the ml env.
    boundary = b"----ESP32BoundaryAKS7MA4Y"
    body = (
        b"--" + boundary + b"\r\n"
        b'Content-Disposition: form-data; name="file"; filename="face.jpg"\r\n'
        b"Content-Type: image/jpeg\r\n\r\n" + image +
        b"\r\n--" + boundary + b"--\r\n"
    )
    headers = _aks_headers(key)
    headers["Content-Type"] = "multipart/form-data; boundary=" + boundary.decode()
    return client.post(
        "/api/recognize",
        headers=headers,
        content=body,
    )


def test_aks_api_rejects_missing_and_wrong_key(client):
    assert _aks_recognize(client, key="").status_code == 401
    assert _aks_recognize(client, key="wrong").status_code == 401


def test_aks_api_full_pass_uses_numeric_log_id(client):
    response = _aks_recognize(client)
    assert response.status_code == 200, response.text
    face = response.json()["faces"][0]
    assert face["match"] is True
    assert isinstance(face["log_id"], int) and face["log_id"] > 0
    assert face["user"]["qr_id"] == "100"

    cargo = client.post(
        "/api/log_cargo",
        headers=_aks_headers(),
        data={"log_id": str(face["log_id"]), "cargo_barcode": "TR-123456"},
    )
    assert cargo.status_code == 200, cargo.text
    assert cargo.json()["status"] == "success"
    assert cargo.json()["record_id"] > 0
    assert client._service.db.list_scan_events()[0]["barcode"] == "TR-123456"


def test_aks_log_id_cannot_be_used_twice(client):
    face = _aks_recognize(client).json()["faces"][0]
    form = {"log_id": str(face["log_id"]), "cargo_barcode": "TR-ONE"}
    assert client.post("/api/log_cargo", headers=_aks_headers(), data=form).status_code == 200
    assert client.post("/api/log_cargo", headers=_aks_headers(), data=form).status_code == 409


def test_aks_rejected_face_cannot_log_cargo(client):
    face = _aks_recognize(client, STRANGER).json()["faces"][0]
    assert face["match"] is False
    assert face["log_id"] > 0
    cargo = client.post(
        "/api/log_cargo", headers=_aks_headers(),
        data={"log_id": str(face["log_id"]), "cargo_barcode": "TR-DENIED"},
    )
    assert cargo.status_code == 409
    assert cargo.json()["reason"] == "FACE_NOT_APPROVED"
