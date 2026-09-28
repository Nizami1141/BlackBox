"""
HTTP/WebSocket API и раздача сайта.

Три группы адресов, у каждой свой смысл и свои права:

  /api/web/*      пульт оператора. Открывается из браузера на этом же
                  компьютере. Права: запустить проверку, зарегистрировать
                  сотрудника, читать журнал.

  /api/v1/*       старый интерфейс для устройств, которые ходят по HTTP сами
                  (плата шлёт кадр и штрихкод). Оставлен ради совместимости
                  и как запасной путь, если BLE-мост не поднялся.
                  Аутентификация: X-Device-Id + X-Device-Key.

  /api/recognize, /api/log_cargo, /api/esp/register
                  основной Ethernet-контракт ESP32 из esp32.html.
                  Аутентификация: X-API-Key + whitelist исходного IP.

  /ws/ui          события в браузер (стадии проверки, статус постов)
  /ws/device/{id} канал для платы по Wi-Fi/Ethernet (см. link/ws_link.py)

Почему сайт и API на одном порту: иначе браузеру нужен CORS, а оператору —
помнить два адреса. Один uvicorn отдаёт и страницы, и данные.
"""
from __future__ import annotations

import asyncio
import csv
import hmac
import io
import logging
import socket
import threading
import time
from contextlib import asynccontextmanager
from contextlib import suppress
from typing import Optional

from fastapi import (Depends, FastAPI, File, Form, Header, HTTPException,
                     Query, Request, Response, UploadFile, WebSocket,
                     WebSocketDisconnect)
from fastapi.responses import (FileResponse, JSONResponse, PlainTextResponse,
                               StreamingResponse)
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from core.events import BUS, EV_SCAN, EV_VERIFY, Event
from core.face_engine import FaceError
from core.flow import CheckpointFlow, _short_name
from core.service import CheckpointService
from core.system_monitor import SystemMonitor
from link import LinkError
from link.ws_link import WsLink

from .ws_hub import WsHub
from .central_aks import CentralAksClient, CentralAksError

log = logging.getLogger(__name__)

API_VERSION = "2.0.0"
MAX_IMAGE_BYTES = 4 * 1024 * 1024        # 4 МБ на снимок при регистрации
MAX_ENROLL_IMAGES = 12


class ScanRequest(BaseModel):
    session_token: str = Field(..., min_length=8, max_length=64)
    barcode: str = Field(..., min_length=1, max_length=512)
    symbology: str = Field("", max_length=32)


class CheckRequest(BaseModel):
    device_id: str = Field("", max_length=64)


def create_app(service: CheckpointService, flow: CheckpointFlow,
               registry, cfg) -> FastAPI:
    hub = WsHub()
    central_aks = (CentralAksClient(cfg.central_aks_base_url, cfg.aks_api_key,
                                    cfg.central_aks_timeout_seconds)
                   if cfg.central_aks_enabled else None)
    # The central log_id is opaque. Bind it to the authenticated local post
    # before accepting its second-stage barcode, because the upstream sees
    # only the laptop's office-network IP rather than the W5500's source IP.
    central_sessions: dict[tuple[str, int], dict] = {}
    central_sessions_lock = threading.Lock()
    # The Ethernet post is an HTTP client, so a browser cannot push to it.
    # Keep only a short-lived, in-memory trigger; the board polls for it.
    pending_triggers: dict[str, float] = {}
    last_trigger_poll: dict[str, float] = {}
    trigger_lock = threading.Lock()
    trigger_ttl = 30.0

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        hub.bind_loop(asyncio.get_running_loop())
        await registry.start_all()
        monitor = SystemMonitor(service.db, cfg.devices,
                                cfg.data_dir / "system_status.txt")
        monitor_task = asyncio.create_task(monitor.run())
        log.info("Транспорты подняты: %s",
                 ", ".join(f"{l.device_id}({l.kind})" for l in registry.all()) or "нет")
        try:
            yield
        finally:
            monitor_task.cancel()
            with suppress(asyncio.CancelledError):
                await monitor_task
            await registry.stop_all()

    app = FastAPI(title="Customs Checkpoint", version=API_VERSION,
                  docs_url="/api/docs", redoc_url=None, lifespan=lifespan)
    app.state.service = service
    app.state.flow = flow
    app.state.started_at = time.time()

    # --- события наружу ----------------------------------------------------
    flow.on_event(hub.publish)

    def _on_link(kind: str, data: dict) -> None:
        hub.publish({"type": kind, **data})
        # Кнопка на самом посту запускает ту же проверку, что и кнопка
        # на сайте. Это единственный способ работать там, где рядом
        # нет ни оператора, ни компьютера.
        if kind == "event":
            flow.handle_device_event(data.get("device_id", ""), data.get("name", ""))

    registry.add_observer(_on_link)

    def _on_bus(ev: Event) -> None:
        # Шина событий работает и из рабочих потоков — hub.publish это умеет.
        hub.publish({"type": ev.kind, "ts": ev.ts, **ev.data})

    BUS.subscribe(_on_bus)

    # ---------------------------------------------------------------- права
    def _machine_ips() -> set:
        """Все адреса ЭТОГО компьютера, а не только loopback.

        Зачем. «Открыть на самом сервере» можно двумя равнозначными
        способами: http://127.0.0.1:8000 и http://192.168.10.10:8000 —
        второй адрес люди берут из ipconfig, потому что он же вписан
        в прошивку поста. Запрос при этом уходит на собственный сетевой
        адрес машины, и его source IP — не 127.0.0.1, а тот же
        192.168.10.10. Проверять только loopback значит отказывать
        человеку, сидящему за самим сервером, — ровно это и происходило.

        Запрос с ДРУГОГО компьютера принесёт чужой source IP, которого
        в этом множестве нет, — он по-прежнему получает 403.
        """
        ips = {"127.0.0.1", "::1", "localhost", "testclient"}
        try:
            for info in socket.getaddrinfo(socket.gethostname(), None):
                ips.add(str(info[4][0]).split("%")[0])
        except OSError:
            pass
        return ips

    local_ips = _machine_ips()

    def local_only(request: Request) -> None:
        """Пульт оператора не должен открываться из сети поста.

        Сайт умеет заводить сотрудников и запускать проверки — это не та
        страница, которую можно оставить доступной всем в локальной сети.
        Разрешение выдаётся явно флагом web.allow_lan в config.yaml.
        """
        nonlocal local_ips
        if cfg.web_allow_lan:
            return
        host = request.client.host if request.client else ""
        if host in local_ips:
            return
        # Адрес могли назначить адаптеру ПОСЛЕ старта сервера (на посту так
        # и бывает: сервер уже работает, а статический IP прописывают потом).
        # Прежде чем отказывать — перечитываем список своих адресов.
        local_ips = _machine_ips()
        if host in local_ips:
            return
        raise HTTPException(403, detail={
            "reason": "REMOTE_FORBIDDEN",
            "message": ("Пульт открывается только на самом сервере. "
                        "Чтобы разрешить доступ из сети, поставьте "
                        "web.allow_lan: true в config.yaml."),
        })

    def device_auth(
        request: Request,
        x_device_id: str = Header(default=""),
        x_device_key: str = Header(default=""),
    ) -> str:
        if not x_device_id or not x_device_key:
            raise HTTPException(401, detail={"reason": "NO_CREDENTIALS"})
        if not service.db.check_device(x_device_id, x_device_key):
            log.warning("Отказ в доступе: device_id=%r ip=%s", x_device_id,
                        request.client.host if request.client else "?")
            raise HTTPException(403, detail={"reason": "BAD_DEVICE_KEY"})
        service.db.touch_device(x_device_id,
                                request.client.host if request.client else "")
        return x_device_id

    def aks_auth(
        request: Request,
        x_api_key: str = Header(default="", alias="X-API-Key"),
    ) -> str:
        """Authenticate the API contract used by ESP32/W5500.

        The specification has one shared X-API-Key and identifies the
        physical checkpoint by source IP.  Localhost may use the same path
        only when allow_local_test is enabled, so /selftest exercises the
        production request parser instead of a special test endpoint.
        """
        if not x_api_key:
            raise HTTPException(401, detail={"reason": "NO_API_KEY"})
        if not cfg.aks_api_key or not hmac.compare_digest(
            cfg.aks_api_key.encode("utf-8"), x_api_key.encode("utf-8")
        ):
            raise HTTPException(401, detail={"reason": "BAD_API_KEY"})

        host = request.client.host if request.client else ""
        device_id = cfg.aks_ip_devices.get(host, "")
        if not device_id and cfg.aks_allow_local_test and host in local_ips:
            device_id = next(iter(cfg.aks_ip_devices.values()), "")
        if not device_id:
            log.warning("AKS API: IP %s отсутствует в белом списке", host or "?")
            raise HTTPException(403, detail={"reason": "IP_NOT_ALLOWED"})

        service.db.touch_device(device_id, host)
        return device_id

    # =====================================================================
    #  Пульт оператора
    # =====================================================================
    @app.get("/api/web/state", dependencies=[Depends(local_only)])
    def web_state():
        return {
            "ok": True,
            "version": API_VERSION,
            "backend": service.cfg.backend,
            "recognition_source": "central_aks" if central_aks else "local",
            "dim": service.engine.dim,
            "threshold": service.cfg.threshold,
            "liveness": service.cfg.liveness_enabled,
            "employees": service.index.size,
            # Сотрудники, чей вектор не той размерности (остались от прежнего
            # face.backend). Они исключены из поиска, и об этом надо сказать
            # вслух: молча это выглядит как «система забыла людей».
            "stale_employees": service.index.stale,
            "last_face_ms": service.engine.last_ms,
            "uptime_s": int(time.time() - app.state.started_at),
            # Посты, которыми сервер управляет (BLE, WebSocket, заглушка),
            # плюс автономные — те, что ходят по HTTP сами. Они забирают
            # разовую команду с сайта опросом, если Ethernet доступен.
            # оператор не поймёт, жив ли пост вообще.
            "devices": registry.info() + _autonomous_devices(),
            "active": {d: (s.public() if s else None)
                       for d, s in flow._active.items()},   # noqa: SLF001
            "stats": service.db.stats(),
            "aks_api": {
                "enabled": bool(cfg.aks_api_key and cfg.aks_ip_devices),
                "server_ip": cfg.aks_server_ip,
                "port": cfg.port,
                "ip_devices": cfg.aks_ip_devices,
                "allow_local_test": cfg.aks_allow_local_test,
                "recognize_path": "/api/recognize",
                "cargo_path": "/api/log_cargo",
            },
            "central_aks": {
                "enabled": bool(central_aks),
                "base_url": cfg.central_aks_base_url if central_aks else None,
            },
        }

    # ------------------------------------------------------- экран поста
    def _lcd(key: str, default: str = "") -> str:
        return str(cfg.lcd.get(key, default))

    def _lcd_verify(r) -> tuple[str, str]:
        """Что показать на экране поста по итогу опознания.

        Считает сервер, а не плата: тексты лежат в config.yaml, и поправить
        формулировку на азербайджанском можно, не перепрошивая посты.
        Плата переводит UTF-8 в знакоместа HD44780 сама.

        На экране только вердикт — ни оценки схожести, ни имён похожих
        сотрудников. Человек у поста этого видеть не должен, всё это
        уходит оператору в журнал.
        """
        if r.matched:
            return _short_name(r.full_name), _lcd("ok2", "Kodu skan edin")
        return (_lcd("deny1", "Giriş qadağandır"),
                _lcd("deny2", "Bazada yoxdur"))

    def _autonomous_devices() -> list[dict]:
        """Посты без управляемого линка: плата ходит к серверу сама.

        «На связи» здесь означает не открытое соединение, а «был недавно».
        Для кнопки на сайте нужен более свежий опрос (triggerable).
        """
        managed = {l.device_id for l in registry.all()}
        now = time.monotonic()
        out = []
        for row in service.db.list_devices():
            did = row["device_id"]
            if did in managed:
                continue
            last = row["last_seen"] or 0
            out.append({
                "device_id": did,
                "kind": "http",
                "connected": (time.time() - last) < 900,   # 15 минут
                "autonomous": True,
                "triggerable": now - last_trigger_poll.get(did, -1e9) < 15,
                "address": row["last_ip"] or "",
                "last_seen": last,
                "status": {},
            })
        return out

    @app.post("/api/web/trigger", dependencies=[Depends(local_only)])
    def web_trigger(req: CheckRequest):
        """Queue one check for an Ethernet post; no browser device key exposed."""
        device = next((d for d in cfg.devices if d.device_id == req.device_id), None)
        if device is None or device.link.get("type", "none") != "none":
            raise HTTPException(404, detail={"reason": "NOT_AUTONOMOUS"})
        now = time.monotonic()
        with trigger_lock:
            recently_polled = now - last_trigger_poll.get(req.device_id, -1e9) < 15
        if not recently_polled:
            raise HTTPException(409, detail={
                "reason": "DEVICE_OFFLINE",
                "message": "Пост не опрашивает сервер. Проверьте Ethernet-линк и питание ESP32.",
            })
        with trigger_lock:
            if pending_triggers.get(req.device_id, 0) > now:
                raise HTTPException(409, detail={
                    "reason": "CHECK_QUEUED",
                    "message": "Команда проверки уже ожидает пост.",
                })
            pending_triggers[req.device_id] = now + trigger_ttl
        return {"ok": True, "device_id": req.device_id, "expires_in": int(trigger_ttl)}

    @app.get("/api/device/trigger")
    def device_trigger(device_id: str = Depends(aks_auth)):
        """Only the authenticated, IP-allowlisted ESP32 can take its command."""
        now = time.monotonic()
        with trigger_lock:
            last_trigger_poll[device_id] = now
            expires = pending_triggers.pop(device_id, 0)
        return {"check": expires > now}

    def _pick_device(device_id: str) -> str:
        """Если сайт не указал пост явно — берём единственный подключённый."""
        if device_id:
            return device_id
        link = registry.first_connected()
        if link is None:
            raise HTTPException(409, detail={
                "reason": "NO_DEVICE_ONLINE",
                "message": "Ни один пост не на связи. Проверьте питание платы "
                           "и Bluetooth на ноутбуке.",
            })
        return link.device_id

    @app.post("/api/web/check", dependencies=[Depends(local_only)])
    async def web_check(req: CheckRequest):
        """Кнопка «Проверить». Возвращается сразу, ход показывается по WebSocket."""
        device_id = _pick_device(req.device_id)
        try:
            registry.get(device_id)
        except LinkError as e:
            raise HTTPException(409, detail={"reason": e.code, "message": e.message})

        if flow.active(device_id) is not None:
            raise HTTPException(409, detail={
                "reason": "DEVICE_BUSY",
                "message": "На этом посту уже идёт проверка",
            })

        st = flow.create(device_id)
        # Сценарий идёт фоном: он длится десятки секунд, и держать на нём
        # HTTP-запрос браузера незачем — ход показывается по WebSocket.
        asyncio.create_task(flow.run(st))
        return {"ok": True, "flow_id": st.flow_id, "device_id": device_id}

    @app.post("/api/web/cancel", dependencies=[Depends(local_only)])
    async def web_cancel(req: CheckRequest):
        device_id = _pick_device(req.device_id)
        ok = await flow.cancel(device_id)
        return {"ok": ok}

    @app.get("/api/web/flow/{flow_id}", dependencies=[Depends(local_only)])
    def web_flow(flow_id: str):
        st = flow.get(flow_id)
        if st is None:
            raise HTTPException(404, detail={"reason": "NO_FLOW"})
        return st.public()

    @app.get("/api/web/flow/{flow_id}/frame.jpg", dependencies=[Depends(local_only)])
    def web_flow_frame(flow_id: str):
        st = flow.get(flow_id)
        if st is None or not st.frame_jpeg:
            raise HTTPException(404, detail={"reason": "NO_FRAME"})
        return Response(content=st.frame_jpeg, media_type="image/jpeg")

    # ------------------------------------------------------------ сотрудники
    @app.get("/api/web/employees", dependencies=[Depends(local_only)])
    def web_employees():
        return {"items": [
            {
                "id": e.id,
                "employee_no": e.employee_no,
                "full_name": e.full_name,
                "position": e.position,
                "department": e.department,
                "active": e.active,
                "n_samples": e.n_samples,
                "created_at": e.created_at,
            }
            for e in service.db.list_employees()
        ]}

    @app.get("/api/web/employees/{emp_id}/thumb", dependencies=[Depends(local_only)])
    def web_thumb(emp_id: int):
        data = service.db.get_thumbnail(emp_id)
        if not data:
            raise HTTPException(404, detail={"reason": "NO_THUMB"})
        return Response(content=data, media_type="image/jpeg")

    @app.post("/api/web/employees/{emp_id}/active", dependencies=[Depends(local_only)])
    def web_set_active(emp_id: int, active: bool = Query(...)):
        service.set_active(emp_id, active)
        return {"ok": True}

    @app.delete("/api/web/employees/{emp_id}", dependencies=[Depends(local_only)])
    def web_delete(emp_id: int):
        service.delete_employee(emp_id)
        return {"ok": True}

    async def _read_images(files: list[UploadFile]) -> list[bytes]:
        if not files:
            raise HTTPException(400, detail={
                "reason": "NO_IMAGES",
                "message": "Нужен хотя бы один снимок",
            })
        if len(files) > MAX_ENROLL_IMAGES:
            raise HTTPException(413, detail={
                "reason": "TOO_MANY_IMAGES",
                "message": f"Не больше {MAX_ENROLL_IMAGES} снимков за раз",
            })
        blobs = []
        for f in files:
            data = await f.read()
            if len(data) > MAX_IMAGE_BYTES:
                raise HTTPException(413, detail={
                    "reason": "IMAGE_TOO_LARGE",
                    "message": f"Файл {f.filename} больше 4 МБ",
                })
            blobs.append(data)
        return blobs

    @app.post("/api/web/enroll", dependencies=[Depends(local_only)])
    async def web_enroll(
        employee_no: str = Form(...),
        full_name: str = Form(...),
        position: str = Form(""),
        department: str = Form(""),
        images: list[UploadFile] = File(...),
    ):
        """Регистрация сотрудника.

        Снимки идут с веб-камеры браузера или файлами. Важно: эталон
        считается тем же самым пайплайном, которым потом идёт опознание,
        поэтому рассинхронизации «зарегистрировали одним, ищем другим»
        быть не может.
        """
        blobs = await _read_images(images)
        try:
            emp_id, warns = await asyncio.to_thread(
                service.enroll_jpegs, employee_no.strip(), full_name.strip(),
                blobs, position.strip(), department.strip(),
            )
        except FaceError as e:
            raise HTTPException(422, detail={"reason": e.code, "message": e.message})
        except Exception as e:  # noqa: BLE001
            # Чаще всего это UNIQUE constraint на табельный номер.
            msg = str(e)
            if "UNIQUE" in msg:
                raise HTTPException(409, detail={
                    "reason": "DUPLICATE_NO",
                    "message": f"Табельный номер {employee_no} уже занят",
                }) from e
            raise
        return {"ok": True, "employee_id": emp_id, "warnings": warns,
                "samples": len(blobs)}

    @app.post("/api/web/employees/{emp_id}/samples", dependencies=[Depends(local_only)])
    async def web_add_samples(emp_id: int, images: list[UploadFile] = File(...)):
        blobs = await _read_images(images)
        try:
            warns = await asyncio.to_thread(service.append_samples_jpegs, emp_id, blobs)
        except FaceError as e:
            raise HTTPException(422, detail={"reason": e.code, "message": e.message})
        return {"ok": True, "warnings": warns}

    @app.post("/api/web/preview", dependencies=[Depends(local_only)])
    async def web_preview(image: UploadFile = File(...)):
        """Проверить кадр до сохранения: сколько лиц видит детектор."""
        data = await image.read()
        try:
            faces = await asyncio.to_thread(service.preview_faces, data)
        except FaceError as e:
            raise HTTPException(422, detail={"reason": e.code, "message": e.message})
        return {"faces": faces, "min_face_px": service.engine.min_face_px}

    # ---------------------------------------------------------------- журнал
    @app.get("/api/web/journal", dependencies=[Depends(local_only)])
    def web_journal(limit: int = Query(200, ge=1, le=5000),
                    employee_id: Optional[int] = None,
                    barcode: str = "",
                    since: Optional[float] = None,
                    until: Optional[float] = None):
        rows = service.db.list_scan_events(limit, employee_id, barcode, since, until)
        return {"items": [
            {
                "id": r["id"],
                "ts": r["ts"],
                "employee_id": r["employee_id"],
                "full_name": r["full_name"],
                "employee_no": r["employee_no"],
                "device_id": r["device_id"],
                "barcode": r["barcode"],
                "symbology": r["symbology"],
                "match_score": r["match_score"],
            }
            for r in rows
        ]}

    @app.get("/api/web/journal.csv", dependencies=[Depends(local_only)])
    def web_journal_csv(limit: int = Query(5000, ge=1, le=100000)):
        rows = service.db.list_scan_events(limit)
        buf = io.StringIO()
        # BOM — чтобы Excel на Windows открыл кириллицу без «кракозябр».
        buf.write("﻿")
        w = csv.writer(buf, delimiter=";")
        w.writerow(["Дата и время", "Табельный", "ФИО", "Пост", "Штрихкод",
                    "Тип кода", "Схожесть"])
        for r in rows:
            w.writerow([
                time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(r["ts"])),
                r["employee_no"] or "", r["full_name"] or "", r["device_id"],
                r["barcode"], r["symbology"] or "",
                f"{r['match_score']:.4f}" if r["match_score"] is not None else "",
            ])
        buf.seek(0)
        return StreamingResponse(
            iter([buf.getvalue()]),
            media_type="text/csv; charset=utf-8",
            headers={"Content-Disposition": 'attachment; filename="journal.csv"'},
        )

    @app.get("/api/web/verify-log", dependencies=[Depends(local_only)])
    def web_verify_log(limit: int = Query(200, ge=1, le=2000)):
        return {"items": [
            {
                "id": r["id"], "ts": r["ts"], "device_id": r["device_id"],
                "matched": bool(r["matched"]), "full_name": r["full_name"],
                "score": r["score"], "reason": r["reason"],
                "latency_ms": r["latency_ms"],
            }
            for r in service.db.list_verify_log(limit)
        ]}

    # =====================================================================
    #  Старый интерфейс устройства (плата ходит по HTTP сама)
    # =====================================================================

    # Последний кадр от каждого автономного поста.
    #
    # Зачем. У поста на Ethernet нет ни сценария на сервере, ни возможности
    # показать оператору, что он снял: он приходит с готовым кадром и уходит.
    # Когда сервер отвечает NO_FACE, единственный способ понять причину —
    # увидеть, что именно прислала плата: пустой кадр, размытый, тёмный,
    # или лицо просто слишком мелкое. Без картинки это гадание.
    #
    # ТОЛЬКО В ПАМЯТИ и только последний кадр на устройство. На диск не
    # пишем сознательно: кадр лица — биометрические персональные данные,
    # и storage.keep_probe_images в config.yaml выключен именно поэтому.
    # Здесь картинка живёт до следующей проверки или до перезапуска сервера
    # и отдаётся только на localhost.
    last_frames: dict[str, dict] = {}

    @app.get("/api/web/last-frame/{device_id}.jpg",
             dependencies=[Depends(local_only)])
    def web_last_frame(device_id: str):
        item = last_frames.get(device_id)
        if not item:
            raise HTTPException(404, detail={
                "reason": "NO_FRAME",
                "message": f"От поста {device_id} ещё не приходило кадров.",
            })
        return Response(content=item["jpeg"], media_type="image/jpeg",
                        headers={"Cache-Control": "no-store"})

    @app.get("/api/web/last-frame/{device_id}",
             dependencies=[Depends(local_only)])
    def web_last_frame_info(device_id: str):
        item = last_frames.get(device_id)
        if not item:
            raise HTTPException(404, detail={"reason": "NO_FRAME"})
        return {
            "device_id": device_id,
            "ts": item["ts"],
            "age_s": round(time.time() - item["ts"], 1),
            "bytes": len(item["jpeg"]),
            "matched": item["matched"],
            "reason": item["reason"],
            "url": f"/api/web/last-frame/{device_id}.jpg",
        }

    @app.get("/api/web/link-status", dependencies=[Depends(local_only)])
    def web_link_status():
        """Живое состояние связи с автономными постами.

        Проверить Ethernet «изнутри» сервер не может: пост — клиент, он
        приходит сам и между проверками спит, постучаться в него некуда.
        Зато сервер видит СЛЕДЫ каждого визита: с какого адреса пришёл,
        когда, сколько заняла обработка. По ним связь и проверяется —
        не «пингом», а фактом доставленных кадров.
        """
        managed = {l.device_id for l in registry.all()}
        now = time.time()

        # Журнал попыток — источник задержек и причин отказа. Берём с запасом
        # и раскладываем по постам: пост может быть не один.
        by_dev: dict[str, list] = {}
        for r in service.db.list_verify_log(200):
            by_dev.setdefault(r["device_id"], []).append(r)

        out = []
        for row in service.db.list_devices():
            did = row["device_id"]
            if did in managed:
                continue                    # управляемые видны в /api/web/state
            last = row["last_seen"] or 0
            tries = by_dev.get(did, [])
            ok = [t for t in tries if t["matched"]]
            lat = [t["latency_ms"] for t in tries if t["latency_ms"]]
            frame = last_frames.get(did)
            out.append({
                "device_id": did,
                "last_ip": row["last_ip"] or "",
                "last_seen": last,
                "seconds_ago": round(now - last, 1) if last else None,
                # 900 с — тот же порог, что и в шапке сайта: пост между
                # проверками спит, и молчание само по себе не поломка.
                "online": bool(last and (now - last) < 900),
                "checks_total": len(tries),
                "checks_matched": len(ok),
                "avg_latency_ms": round(sum(lat) / len(lat)) if lat else None,
                "last_reason": tries[0]["reason"] if tries else "",
                "last_matched": bool(tries[0]["matched"]) if tries else None,
                "last_frame_bytes": len(frame["jpeg"]) if frame else None,
            })
        return {"ok": True, "devices": out, "server_ts": now}

    # =====================================================================
    #  AKS compatibility API (contract from esp32.html)
    # =====================================================================
    @app.post("/api/recognize")
    async def aks_recognize(
        file: UploadFile = File(...),
        device_id: str = Depends(aks_auth),
    ):
        body = await file.read(MAX_IMAGE_BYTES + 1)
        if not body:
            raise HTTPException(400, detail={"reason": "EMPTY_IMAGE"})
        if len(body) > MAX_IMAGE_BYTES:
            raise HTTPException(413, detail={"reason": "IMAGE_TOO_LARGE"})

        if central_aks is not None:
            started = time.perf_counter()
            try:
                result = await central_aks.recognize(body)
            except CentralAksError as exc:
                log.warning("Central AKS recognize failed: %s (HTTP %s)",
                            exc.reason, exc.upstream_status or "-")
                raise HTTPException(exc.status_code, detail={
                    "reason": exc.reason,
                    "upstream_status": exc.upstream_status,
                }) from exc

            face = result["faces"][0]
            matched = face["match"]
            user = face.get("user") if matched else None
            user = user if isinstance(user, dict) else {}
            name = " ".join(str(user.get(part, "")).strip()
                            for part in ("first_name", "last_name")).strip()
            reason = "CENTRAL_MATCH" if matched else str(face.get("message", "CENTRAL_DENIED"))
            score = face.get("score")
            score = float(score) if isinstance(score, (int, float)) else None
            latency_ms = int((time.perf_counter() - started) * 1000)

            if matched:
                log_id = face["log_id"]
                remote_ttl = face.get("expires_in", cfg.session_ttl)
                ttl = min(cfg.session_ttl,
                          int(remote_ttl) if isinstance(remote_ttl, (int, float))
                          else cfg.session_ttl)
                if ttl < 1:
                    raise HTTPException(502, detail={"reason": "CENTRAL_INVALID_TTL"})
                with central_sessions_lock:
                    now = time.monotonic()
                    expired = [key for key, value in central_sessions.items()
                               if value["expires"] <= now]
                    for key in expired:
                        central_sessions.pop(key, None)
                    central_sessions[(device_id, log_id)] = {
                        "expires": now + ttl,
                        "state": "ready",
                        "full_name": name,
                        "employee_no": str(user.get("qr_id", "")),
                    }

            last_frames[device_id] = {
                "ts": time.time(), "jpeg": body,
                "matched": matched, "reason": reason,
            }
            # The central employee ID belongs to a different database: never
            # join it to a local employee with the same integer ID.
            service.db.add_verify_log(device_id, matched, None, score,
                                      reason, latency_ms)
            BUS.publish(Event(EV_VERIFY, time.time(), {
                "device_id": device_id, "matched": matched, "reason": reason,
                "message": reason, "employee_id": None,
                "full_name": name, "employee_no": str(user.get("qr_id", "")),
                "score": score, "latency_ms": latency_ms,
                "source": "central",
            }, image=body))
            return result

        r = await asyncio.to_thread(service.verify_jpeg, body, device_id)
        last_frames[device_id] = {
            "ts": time.time(), "jpeg": body,
            "matched": r.matched, "reason": r.reason,
        }

        # The external contract exposes a small integer.  Internally it maps
        # to the random, device-bound session token; the token never leaves
        # the server and log_cargo cannot be forged with an arbitrary id.
        log_id = service.db.create_aks_log(
            device_id, r.session_token if r.matched else None, r.matched,
        )
        face = {
            "match": bool(r.matched),
            "log_id": log_id,
            "score": round(r.score, 4),
            "reason": r.reason,
            "expires_in": r.expires_in if r.matched else 0,
        }
        if r.matched:
            names = r.full_name.strip().split(maxsplit=1)
            face["user"] = {
                "id": r.employee_id,
                "qr_id": r.employee_no,
                "first_name": names[0] if names else "",
                "last_name": names[1] if len(names) > 1 else "",
            }
        else:
            messages = {
                "NO_FACE": "ÜZ TAPILMADI",
                "MULTIPLE_FACES": "KADRDА BİRDƏN ÇOX ÜZ VAR",
                "NOT_FOUND": "ŞƏXS BAZADA YOXDUR",
                "LIVENESS_FAILED": "SAXTA ÜZ! (Liveness failed)",
            }
            face["message"] = messages.get(r.reason, r.message or r.reason)

        return {"status": "success", "faces": [face]}

    @app.post("/api/log_cargo")
    async def aks_log_cargo(
        request: Request,
        log_id: int = Form(...),
        cargo_barcode: str = Form(...),
        device_id: str = Depends(aks_auth),
    ):
        if central_aks is not None:
            barcode = cargo_barcode.strip()
            if not barcode or len(barcode) > 512:
                raise HTTPException(422, detail={"reason": "BAD_BARCODE"})
            key = (device_id, log_id)
            with central_sessions_lock:
                session = central_sessions.get(key)
                if session is None or session["expires"] <= time.monotonic():
                    raise HTTPException(409, detail={"reason": "CENTRAL_LOG_UNKNOWN_OR_EXPIRED"})
                if session["state"] != "ready":
                    raise HTTPException(409, detail={"reason": "CENTRAL_LOG_ALREADY_USED"})
                session["state"] = "pending"
            try:
                result = await central_aks.log_cargo(log_id, barcode)
            except CentralAksError as exc:
                # A timeout may mean the central system recorded the cargo
                # but its reply was lost. Do not silently send a duplicate.
                with central_sessions_lock:
                    session["state"] = "uncertain"
                log.warning("Central AKS log_cargo failed: %s (HTTP %s)",
                            exc.reason, exc.upstream_status or "-")
                raise HTTPException(exc.status_code, detail={
                    "reason": exc.reason,
                    "upstream_status": exc.upstream_status,
                    "log_id": log_id,
                }) from exc
            with central_sessions_lock:
                session["state"] = "done"
            BUS.publish(Event(EV_SCAN, time.time(), {
                "device_id": device_id, "ok": True,
                "record_id": result.get("record_id", log_id),
                "barcode": barcode, "symbology": "",
                "employee_id": None,
                "full_name": session["full_name"],
                "employee_no": session["employee_no"],
                "source": "central",
            }))
            # The firmware displays record_id, while the central example
            # returns only a message; log_id is the central transaction ID.
            return {**result, "record_id": result.get("record_id", log_id)}

        row = service.db.get_aks_log(log_id)
        if row is None:
            raise HTTPException(404, detail={"reason": "LOG_UNKNOWN"})
        if row["device_id"] != device_id:
            raise HTTPException(403, detail={"reason": "LOG_DEVICE_MISMATCH"})
        if not row["matched"] or not row["session_token"]:
            raise HTTPException(409, detail={"reason": "FACE_NOT_APPROVED"})

        barcode = cargo_barcode.strip()
        ok, err, record_id = await asyncio.to_thread(
            service.register_scan, row["session_token"], barcode, "", device_id,
        )
        if not ok:
            return JSONResponse(
                {"status": "error", "message": err, "reason": err},
                status_code=409,
            )
        service.db.finish_aks_log(log_id, barcode, record_id)
        return {
            "status": "success",
            "message": "Barkod şəkilli loqa əlavə edildi",
            "ip": request.client.host if request.client else "",
            "record_id": record_id,
        }

    @app.post("/api/esp/register")
    async def aks_register(
        qr_data: str = Form(...),
        file: UploadFile = File(...),
        device_id: str = Depends(aks_auth),
    ):
        if central_aks is not None:
            raise HTTPException(501, detail={
                "reason": "CENTRAL_REGISTRATION_NOT_IMPLEMENTED",
                "message": "Нельзя тихо зарегистрировать человека только в локальной базе.",
            })
        parts = qr_data.strip().split()
        if len(parts) < 4:
            raise HTTPException(422, detail={
                "reason": "BAD_QR_DATA",
                "message": "Gözlənilən format: Region qr_id Ad Soyad",
            })
        region, employee_no = parts[0], parts[1]
        full_name = " ".join(parts[2:])
        body = await file.read(MAX_IMAGE_BYTES + 1)
        if not body:
            raise HTTPException(400, detail={"reason": "EMPTY_IMAGE"})
        if len(body) > MAX_IMAGE_BYTES:
            raise HTTPException(413, detail={"reason": "IMAGE_TOO_LARGE"})
        try:
            employee_id, warnings = await asyncio.to_thread(
                service.enroll_jpegs, employee_no, full_name, [body], "", region,
            )
        except FaceError as e:
            raise HTTPException(400, detail={"reason": e.code, "message": e.message})
        return {
            "status": "success",
            "message": f"{full_name} ESP32 vasitəsilə sistemə tanıdıldı!",
            "employee_id": employee_id,
            "warnings": warnings,
            "device_id": device_id,
        }

    @app.get("/api/v1/health")
    def health():
        return {
            "ok": True, "version": API_VERSION, "backend": service.cfg.backend,
            "recognition_source": "central_aks" if central_aks else "local",
            "central_aks_url": cfg.central_aks_base_url if central_aks else None,
            "dim": service.engine.dim, "threshold": service.cfg.threshold,
            "employees": service.index.size,
            # Сотрудники, чей вектор не той размерности (остались от прежнего
            # face.backend). Они исключены из поиска, и об этом надо сказать
            # вслух: молча это выглядит как «система забыла людей».
            "stale_employees": service.index.stale,
            "last_face_ms": service.engine.last_ms,
            "uptime_s": int(time.time() - app.state.started_at),
            "liveness": service.cfg.liveness_enabled,
            "liveness_authority": "central_aks" if central_aks else "local",
        }

    @app.post("/api/esp/heartbeat")
    def esp_heartbeat(device_id: str = Depends(aks_auth)):
        return {"ok": True, "device_id": device_id}

    @app.post("/api/v1/verify")
    async def verify(request: Request, device_id: str = Depends(device_auth)):
        body = await request.body()
        if not body:
            raise HTTPException(400, detail={"reason": "EMPTY_BODY"})
        if len(body) > MAX_IMAGE_BYTES:
            raise HTTPException(413, detail={"reason": "IMAGE_TOO_LARGE"})

        r = await asyncio.to_thread(service.verify_jpeg, body, device_id)

        # Кадр держим в памяти независимо от исхода — именно неудачные
        # и надо разглядывать. Смотреть: /api/web/last-frame/GATE-01.jpg
        last_frames[device_id] = {
            "ts": time.time(), "jpeg": body,
            "matched": r.matched, "reason": r.reason,
        }

        lcd1, lcd2 = _lcd_verify(r)
        payload = {
            "matched": r.matched, "reason": r.reason, "message": r.message,
            "score": round(r.score, 4), "latency_ms": r.latency_ms,
            # Готовые строки для экрана автономного поста: он показывает их
            # как есть и ничего сам не сочиняет.
            "lcd1": lcd1, "lcd2": lcd2,
        }
        if r.matched:
            payload.update({
                "employee_id": r.employee_id, "employee_no": r.employee_no,
                "name": r.full_name, "position": r.position,
                "session_token": r.session_token, "expires_in": r.expires_in,
            })
        return JSONResponse(payload)

    @app.post("/api/v1/scan")
    async def scan(req: ScanRequest, device_id: str = Depends(device_auth)):
        ok, err, rec_id = await asyncio.to_thread(
            service.register_scan, req.session_token, req.barcode,
            req.symbology, device_id,
        )
        if not ok:
            return JSONResponse({
                "ok": False, "reason": err,
                "lcd1": _lcd("err1", "Xəta"),
                "lcd2": _lcd("deny2", ""),
            }, status_code=409)
        return {
            "ok": True, "record_id": rec_id,
            "lcd1": _lcd("saved1", "Yük qeyd edildi"),
            "lcd2": req.barcode[:16],
        }

    # =====================================================================
    #  WebSocket
    # =====================================================================
    @app.websocket("/ws/ui")
    async def ws_ui(ws: WebSocket):
        await hub.connect(ws)
        try:
            while True:
                # Браузер ничего осмысленного не шлёт; ждём только закрытия.
                await ws.receive_text()
        except WebSocketDisconnect:
            pass
        except Exception:  # noqa: BLE001
            pass
        finally:
            hub.disconnect(ws)

    @app.websocket("/ws/device/{device_id}")
    async def ws_device(ws: WebSocket, device_id: str, key: str = Query("")):
        """Канал для платы по Wi-Fi/Ethernet.

        Ключ передаётся параметром запроса, а не заголовком: на ESP32
        добавить заголовок к WebSocket-рукопожатию можно, но в половине
        библиотек это сделано криво, а строку URL умеют все. Для локальной
        сети поста этого достаточно; при выходе наружу здесь должен быть
        wss:// и клиентский сертификат — см. docs/ARCHITECTURE.md §3.3.
        """
        if not service.db.check_device(device_id, key):
            await ws.close(code=4403)
            return
        try:
            link = registry.get(device_id)
        except LinkError:
            await ws.close(code=4404)
            return
        if not isinstance(link, WsLink):
            await ws.close(code=4400)
            return

        await ws.accept()
        await link.attach(ws, ws.client.host if ws.client else "")
        service.db.touch_device(device_id, ws.client.host if ws.client else "")
        log.info("[%s] устройство подключилось по WebSocket", device_id)
        try:
            while True:
                msg = await ws.receive()
                if msg["type"] == "websocket.disconnect":
                    break
                if msg.get("bytes") is not None:
                    link.feed_binary(msg["bytes"])
                elif msg.get("text") is not None:
                    link.feed_text(msg["text"])
        except WebSocketDisconnect:
            pass
        except Exception:  # noqa: BLE001
            log.exception("[%s] ошибка WebSocket устройства", device_id)
        finally:
            await link.detach(ws)
            log.info("[%s] устройство отключилось", device_id)

    # =====================================================================
    #  Сайт
    # =====================================================================
    web_dir = cfg.web_dir
    if web_dir.exists():
        app.mount("/assets", StaticFiles(directory=str(web_dir / "assets")),
                  name="assets")

        @app.get("/", include_in_schema=False)
        def page_index():
            return FileResponse(web_dir / "index.html")

        @app.get("/enroll", include_in_schema=False)
        def page_enroll():
            return FileResponse(web_dir / "enroll.html")

        @app.get("/journal", include_in_schema=False)
        def page_journal():
            return FileResponse(web_dir / "journal.html")

        # Страница проверки серверной части: отправить кадр из файла,
        # с веб-камеры или прямо с поста и увидеть весь путь по шагам.
        @app.get("/selftest", include_in_schema=False)
        def page_selftest():
            return FileResponse(web_dir / "selftest.html")
    else:
        @app.get("/", include_in_schema=False)
        def page_missing():
            return PlainTextResponse(
                f"Папка сайта не найдена: {web_dir}\n"
                f"API работает, откройте /api/docs", status_code=500)

    @app.exception_handler(HTTPException)
    async def http_exc(_: Request, exc: HTTPException):
        detail = exc.detail if isinstance(exc.detail, dict) else {"reason": str(exc.detail)}
        return JSONResponse({"ok": False, **detail}, status_code=exc.status_code)

    return app
