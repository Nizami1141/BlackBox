"""
ESP32 backend: регистрация устройств по регионам + приём и хранение логов.

Запуск:
    pip install fastapi uvicorn
    DEVICE_API_KEY=devkey ADMIN_API_KEY=adminkey uvicorn server:app --host 0.0.0.0 --port 8000

Ключи:
    DEVICE_API_KEY - заголовок X-API-Key, для плат (register, heartbeat, logs)
    ADMIN_API_KEY  - заголовок X-Admin-Key, для чтения (devices, regions, logs, export)
    Если переменная пуста - проверка отключена (только для разработки).
"""
import csv
import hmac
import io
import os
import re
import sqlite3
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import List, Optional

from fastapi import Depends, FastAPI, Header, HTTPException, Query, Request
from fastapi.responses import FileResponse, StreamingResponse
from pydantic import BaseModel, Field, field_validator

DB_PATH = Path(os.getenv("DB_PATH", Path(__file__).parent / "devices.db"))
DEVICE_API_KEY = os.getenv("DEVICE_API_KEY", "")
ADMIN_API_KEY = os.getenv("ADMIN_API_KEY", "")
HEARTBEAT_TIMEOUT_S = int(os.getenv("HEARTBEAT_TIMEOUT_S", "20"))
LOG_RETENTION_DAYS = int(os.getenv("LOG_RETENTION_DAYS", "30"))  # 0 = хранить вечно
# Список разрешённых регионов через запятую, например "baku,ganja,sumqayit". Пусто = любые.
ALLOWED_REGIONS = {r.strip().lower() for r in os.getenv("ALLOWED_REGIONS", "").split(",") if r.strip()}

LEVELS = ("debug", "info", "warn", "error")
MAX_LOG_BATCH = 100
MAX_MESSAGE_LEN = 2000
ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
REGION_RE = re.compile(r"^[a-z0-9_-]{1,32}$")


# ---------------- база данных ----------------

@contextmanager
def conn():
    c = sqlite3.connect(DB_PATH, timeout=10)
    c.row_factory = sqlite3.Row
    c.execute("PRAGMA journal_mode=WAL")
    c.execute("PRAGMA foreign_keys=ON")
    try:
        yield c
        c.commit()
    finally:
        c.close()


def init_db():
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    with conn() as c:
        c.executescript(
            """
            CREATE TABLE IF NOT EXISTS devices (
                device_id TEXT PRIMARY KEY,
                region TEXT NOT NULL,
                name TEXT,
                ip TEXT,
                fw_version TEXT,
                first_seen TEXT NOT NULL,
                last_seen TEXT,
                heartbeat_count INTEGER NOT NULL DEFAULT 0
            );
            CREATE INDEX IF NOT EXISTS idx_devices_region ON devices(region);

            CREATE TABLE IF NOT EXISTS logs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                level TEXT NOT NULL,
                message TEXT NOT NULL,
                uptime_ms INTEGER,
                received_at TEXT NOT NULL,
                FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE CASCADE
            );
            CREATE INDEX IF NOT EXISTS idx_logs_device ON logs(device_id, id DESC);
            CREATE INDEX IF NOT EXISTS idx_logs_time ON logs(received_at);
            """
        )


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


# ---------------- авторизация ----------------

def _check(expected: str, given: Optional[str]):
    if expected and not (given and hmac.compare_digest(expected, given)):
        raise HTTPException(status_code=401, detail="invalid api key")


def require_device(x_api_key: Optional[str] = Header(default=None)):
    _check(DEVICE_API_KEY, x_api_key)


def require_admin(request: Request, x_admin_key: Optional[str] = Header(default=None)):
    if ADMIN_API_KEY:
        _check(ADMIN_API_KEY, x_admin_key)
    elif not request.client or request.client.host not in ("127.0.0.1", "::1", "testclient"):
        raise HTTPException(status_code=403, detail="admin API is local only")


# ---------------- модели ----------------

class RegisterPayload(BaseModel):
    device_id: str
    region: str
    name: Optional[str] = Field(default=None, max_length=100)

    @field_validator("device_id")
    @classmethod
    def _dev(cls, v):
        v = v.strip()
        if not ID_RE.match(v):
            raise ValueError("device_id: только A-Z a-z 0-9 . _ - (до 64 символов)")
        return v

    @field_validator("region")
    @classmethod
    def _region(cls, v):
        v = v.strip().lower()
        if not REGION_RE.match(v):
            raise ValueError("region: только a-z 0-9 _ - (до 32 символов)")
        if ALLOWED_REGIONS and v not in ALLOWED_REGIONS:
            raise ValueError(f"region должен быть одним из: {sorted(ALLOWED_REGIONS)}")
        return v


class HeartbeatPayload(BaseModel):
    ip: Optional[str] = Field(default=None, max_length=64)
    fw_version: Optional[str] = Field(default=None, max_length=32)


class LogEntry(BaseModel):
    level: str = "info"
    message: str
    uptime_ms: Optional[int] = None


class LogsPayload(BaseModel):
    logs: List[LogEntry] = Field(min_length=1, max_length=MAX_LOG_BATCH)


# ---------------- приложение ----------------

app = FastAPI(title="ESP32 Backend")
STATIC_DIR = Path(__file__).parent / "static"


@app.on_event("startup")
def _startup():
    init_db()
    if LOG_RETENTION_DAYS > 0:
        cutoff = (datetime.now(timezone.utc) - timedelta(days=LOG_RETENTION_DAYS)).isoformat()
        with conn() as c:
            c.execute("DELETE FROM logs WHERE received_at < ?", (cutoff,))


def _status(last_seen: Optional[str]) -> str:
    if not last_seen:
        return "never_seen"
    age = (datetime.now(timezone.utc) - datetime.fromisoformat(last_seen)).total_seconds()
    return "online" if age <= HEARTBEAT_TIMEOUT_S else "offline"


@app.get("/")
def dashboard():
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/health")
def health():
    return {"ok": True, "service": "device-registry"}


# ---------- эндпоинты для плат ----------

@app.post("/api/v1/devices/register", dependencies=[Depends(require_device)])
def register(p: RegisterPayload):
    with conn() as c:
        c.execute(
            """INSERT INTO devices (device_id, region, name, first_seen)
               VALUES (?, ?, ?, ?)
               ON CONFLICT(device_id) DO UPDATE SET
                   region = excluded.region,
                   name = COALESCE(excluded.name, devices.name)""",
            (p.device_id, p.region, p.name, now_iso()),
        )
    return {"ok": True, "device_id": p.device_id, "region": p.region}


@app.post("/api/v1/devices/{device_id}/heartbeat", dependencies=[Depends(require_device)])
def heartbeat(device_id: str, p: HeartbeatPayload, request: Request):
    ip = p.ip or (request.client.host if request.client else None)
    with conn() as c:
        cur = c.execute(
            """UPDATE devices SET last_seen = ?, ip = COALESCE(?, ip),
                   fw_version = COALESCE(?, fw_version), heartbeat_count = heartbeat_count + 1
               WHERE device_id = ?""",
            (now_iso(), ip, p.fw_version, device_id),
        )
        if cur.rowcount == 0:
            raise HTTPException(404, "device not registered - call /register first")
    return {"ok": True}


@app.post("/api/v1/devices/{device_id}/logs", dependencies=[Depends(require_device)])
def post_logs(device_id: str, p: LogsPayload):
    rows, ts = [], now_iso()
    for e in p.logs:
        level = e.level.lower()
        level = "warn" if level == "warning" else level
        if level not in LEVELS:
            level = "info"
        rows.append((device_id, level, e.message[:MAX_MESSAGE_LEN], e.uptime_ms, ts))
    with conn() as c:
        if not c.execute("SELECT 1 FROM devices WHERE device_id = ?", (device_id,)).fetchone():
            raise HTTPException(404, "device not registered - call /register first")
        c.executemany(
            "INSERT INTO logs (device_id, level, message, uptime_ms, received_at) VALUES (?,?,?,?,?)",
            rows,
        )
    return {"ok": True, "stored": len(rows)}


# ---------- эндпоинты для чтения (admin) ----------

@app.get("/api/v1/devices", dependencies=[Depends(require_admin)])
def list_devices(region: Optional[str] = None):
    since = (datetime.now(timezone.utc) - timedelta(hours=24)).isoformat()
    sql = """
        SELECT d.*,
               (SELECT COUNT(*) FROM logs l WHERE l.device_id = d.device_id) AS log_count,
               (SELECT COUNT(*) FROM logs l WHERE l.device_id = d.device_id
                    AND l.level = 'error' AND l.received_at >= ?) AS errors_24h
        FROM devices d
    """
    args = [since]
    if region:
        sql += " WHERE d.region = ?"
        args.append(region.strip().lower())
    sql += " ORDER BY d.region, d.device_id"
    with conn() as c:
        rows = [dict(r) for r in c.execute(sql, args)]
    for r in rows:
        r["status"] = _status(r["last_seen"])
    return rows


@app.get("/api/v1/regions", dependencies=[Depends(require_admin)])
def regions():
    out = {}
    with conn() as c:
        for d in c.execute("SELECT region, last_seen FROM devices"):
            r = out.setdefault(d["region"], {"region": d["region"], "total": 0, "online": 0})
            r["total"] += 1
            r["online"] += _status(d["last_seen"]) == "online"
    return sorted(out.values(), key=lambda x: x["region"])


def _query_logs(device_id, region, level, q, before_id, after_id, limit):
    where, args = [], []
    if device_id:
        where.append("l.device_id = ?"); args.append(device_id)
    if region:
        where.append("d.region = ?"); args.append(region.strip().lower())
    if level:
        allowed = LEVELS[LEVELS.index(level):]  # level=warn -> warn и error
        where.append(f"l.level IN ({','.join('?' * len(allowed))})"); args += allowed
    if q:
        esc = q.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_")
        where.append("l.message LIKE ? ESCAPE '\\'"); args.append(f"%{esc}%")
    if before_id:
        where.append("l.id < ?"); args.append(before_id)
    if after_id:
        where.append("l.id > ?"); args.append(after_id)
    sql = """SELECT l.id, l.received_at, d.region, l.device_id, l.level, l.uptime_ms, l.message
             FROM logs l JOIN devices d ON d.device_id = l.device_id"""
    if where:
        sql += " WHERE " + " AND ".join(where)
    sql += " ORDER BY l.id DESC LIMIT ?"
    args.append(limit)
    with conn() as c:
        return [dict(r) for r in c.execute(sql, args)]


def _filters(
    device_id: Optional[str] = None,
    region: Optional[str] = None,
    level: Optional[str] = Query(default=None, pattern="^(debug|info|warn|error)$"),
    q: Optional[str] = None,
):
    return dict(device_id=device_id, region=region, level=level, q=q)


@app.get("/api/v1/logs", dependencies=[Depends(require_admin)])
def get_logs(
    f: dict = Depends(_filters),
    before_id: Optional[int] = None,
    after_id: Optional[int] = None,
    limit: int = Query(default=200, ge=1, le=1000),
):
    return _query_logs(**f, before_id=before_id, after_id=after_id, limit=limit)


@app.get("/api/v1/logs/export.csv", dependencies=[Depends(require_admin)])
def export_logs(f: dict = Depends(_filters), limit: int = Query(default=10000, ge=1, le=100000)):
    rows = _query_logs(**f, before_id=None, after_id=None, limit=limit)

    def gen():
        buf = io.StringIO()
        w = csv.writer(buf)
        w.writerow(["id", "received_at", "region", "device_id", "level", "uptime_ms", "message"])
        yield buf.getvalue(); buf.seek(0); buf.truncate()
        for r in rows:
            w.writerow([r["id"], r["received_at"], r["region"], r["device_id"],
                        r["level"], r["uptime_ms"], r["message"]])
            yield buf.getvalue(); buf.seek(0); buf.truncate()

    return StreamingResponse(gen(), media_type="text/csv",
                             headers={"Content-Disposition": "attachment; filename=device_logs.csv"})
