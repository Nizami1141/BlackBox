"""SQLite-хранилище: сотрудники, шаблоны лиц, устройства, сессии, журнал."""
from __future__ import annotations

import sqlite3
import threading
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Optional

import numpy as np

SCHEMA = """
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;

CREATE TABLE IF NOT EXISTS employees (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    employee_no   TEXT    NOT NULL UNIQUE,     -- табельный номер
    full_name     TEXT    NOT NULL,
    position      TEXT    DEFAULT '',
    department    TEXT    DEFAULT '',
    active        INTEGER NOT NULL DEFAULT 1,
    dim           INTEGER NOT NULL,
    template      BLOB    NOT NULL,            -- усреднённый L2-нормализованный вектор, float32
    n_samples     INTEGER NOT NULL DEFAULT 1,
    thumbnail     BLOB,                        -- маленький jpeg только для GUI
    created_at    REAL    NOT NULL,
    updated_at    REAL    NOT NULL
);

-- Отдельные снимки: нужны, чтобы пересчитать эталон при добавлении фото
-- и чтобы калибровать порог (tools/calibrate_threshold.py).
CREATE TABLE IF NOT EXISTS face_samples (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    employee_id   INTEGER NOT NULL REFERENCES employees(id) ON DELETE CASCADE,
    dim           INTEGER NOT NULL,
    embedding     BLOB    NOT NULL,
    source        TEXT    DEFAULT '',
    created_at    REAL    NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_samples_emp ON face_samples(employee_id);

CREATE TABLE IF NOT EXISTS devices (
    device_id     TEXT PRIMARY KEY,
    api_key       TEXT NOT NULL,
    location      TEXT DEFAULT '',
    last_seen     REAL,
    last_ip       TEXT
);

CREATE TABLE IF NOT EXISTS sessions (
    token         TEXT PRIMARY KEY,
    employee_id   INTEGER NOT NULL REFERENCES employees(id) ON DELETE CASCADE,
    device_id     TEXT    NOT NULL,
    score         REAL    NOT NULL,
    created_at    REAL    NOT NULL,
    expires_at    REAL    NOT NULL,
    scans_used    INTEGER NOT NULL DEFAULT 0,
    revoked       INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_sessions_exp ON sessions(expires_at);

-- Главный журнал: кто какой груз проверил. Только добавление, без UPDATE.
CREATE TABLE IF NOT EXISTS scan_events (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    ts            REAL    NOT NULL,
    employee_id   INTEGER NOT NULL REFERENCES employees(id),
    device_id     TEXT    NOT NULL,
    barcode       TEXT    NOT NULL,
    symbology     TEXT    DEFAULT '',
    session_token TEXT    NOT NULL,
    match_score   REAL,
    note          TEXT    DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_scan_ts  ON scan_events(ts);
CREATE INDEX IF NOT EXISTS idx_scan_emp ON scan_events(employee_id);
CREATE INDEX IF NOT EXISTS idx_scan_bc  ON scan_events(barcode);

-- Аудит КАЖДОЙ попытки распознавания, включая неудачные.
CREATE TABLE IF NOT EXISTS verify_log (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    ts            REAL    NOT NULL,
    device_id     TEXT    NOT NULL,
    matched       INTEGER NOT NULL,
    employee_id   INTEGER,
    score         REAL,
    reason        TEXT DEFAULT '',
    latency_ms    INTEGER
);
CREATE INDEX IF NOT EXISTS idx_verify_ts ON verify_log(ts);

-- Compatibility layer for the AKS contract.  The public API uses a numeric
-- log_id, while the internal security boundary uses an unguessable token.
-- Keeping the mapping in SQLite makes an in-flight check survive a server
-- restart and prevents the browser/firmware from ever seeing the token.
CREATE TABLE IF NOT EXISTS aks_logs (
    log_id        INTEGER PRIMARY KEY AUTOINCREMENT,
    ts            REAL    NOT NULL,
    device_id     TEXT    NOT NULL,
    session_token TEXT,
    matched       INTEGER NOT NULL,
    cargo_barcode TEXT,
    record_id     INTEGER
);
CREATE INDEX IF NOT EXISTS idx_aks_logs_device ON aks_logs(device_id, ts);
"""


def emb_to_blob(v: np.ndarray) -> bytes:
    return np.asarray(v, dtype=np.float32).tobytes()


def blob_to_emb(b: bytes) -> np.ndarray:
    return np.frombuffer(b, dtype=np.float32)


@dataclass
class Employee:
    id: int
    employee_no: str
    full_name: str
    position: str
    department: str
    active: bool
    n_samples: int
    created_at: float

    @staticmethod
    def from_row(r: sqlite3.Row) -> "Employee":
        return Employee(
            id=r["id"],
            employee_no=r["employee_no"],
            full_name=r["full_name"],
            position=r["position"] or "",
            department=r["department"] or "",
            active=bool(r["active"]),
            n_samples=r["n_samples"],
            created_at=r["created_at"],
        )


class Database:
    """Потокобезопасная обёртка над SQLite (API-поток + GUI-поток)."""

    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.RLock()
        self.conn = sqlite3.connect(str(path), check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        with self._lock:
            self.conn.executescript(SCHEMA)
            self.conn.commit()

    # ---------------------------------------------------------------- utils
    def _ex(self, sql: str, args: Iterable[Any] = ()) -> sqlite3.Cursor:
        with self._lock:
            cur = self.conn.execute(sql, tuple(args))
            self.conn.commit()
            return cur

    def _q(self, sql: str, args: Iterable[Any] = ()) -> list[sqlite3.Row]:
        with self._lock:
            return self.conn.execute(sql, tuple(args)).fetchall()

    # ------------------------------------------------------------ devices
    def sync_devices(self, devices: list) -> None:
        """Привести таблицу devices в соответствие с config.yaml."""
        for d in devices:
            self._ex(
                "INSERT INTO devices(device_id, api_key, location) VALUES(?,?,?) "
                "ON CONFLICT(device_id) DO UPDATE SET api_key=excluded.api_key, "
                "location=excluded.location",
                (d.device_id, d.api_key, d.location),
            )

    def check_device(self, device_id: str, api_key: str) -> bool:
        rows = self._q("SELECT api_key FROM devices WHERE device_id=?", (device_id,))
        if not rows:
            return False
        # Сравнение постоянного времени — защита от timing-атак на ключ.
        # Сравниваем именно байты: compare_digest на str падает, если в ключе
        # есть не-ASCII символы (а пользователь вполне может вписать кириллицу).
        import hmac

        return hmac.compare_digest(
            str(rows[0]["api_key"]).encode("utf-8"),
            str(api_key).encode("utf-8"),
        )

    def touch_device(self, device_id: str, ip: str = "") -> None:
        self._ex("UPDATE devices SET last_seen=?, last_ip=? WHERE device_id=?",
                 (time.time(), ip, device_id))

    def list_devices(self) -> list[sqlite3.Row]:
        return self._q("SELECT * FROM devices ORDER BY device_id")

    # ---------------------------------------------------------- employees
    def add_employee(
        self,
        employee_no: str,
        full_name: str,
        template: np.ndarray,
        samples: list[np.ndarray],
        position: str = "",
        department: str = "",
        thumbnail: Optional[bytes] = None,
    ) -> int:
        now = time.time()
        dim = int(len(template))
        with self._lock:
            cur = self.conn.execute(
                "INSERT INTO employees(employee_no, full_name, position, department, "
                "active, dim, template, n_samples, thumbnail, created_at, updated_at) "
                "VALUES(?,?,?,?,1,?,?,?,?,?,?)",
                (employee_no, full_name, position, department, dim,
                 emb_to_blob(template), len(samples), thumbnail, now, now),
            )
            emp_id = int(cur.lastrowid)
            for s in samples:
                self.conn.execute(
                    "INSERT INTO face_samples(employee_id, dim, embedding, source, created_at) "
                    "VALUES(?,?,?,?,?)",
                    (emp_id, dim, emb_to_blob(s), "enroll", now),
                )
            self.conn.commit()
        return emp_id

    def add_samples(self, employee_id: int, samples: list[np.ndarray],
                    new_template: np.ndarray) -> None:
        now = time.time()
        with self._lock:
            for s in samples:
                self.conn.execute(
                    "INSERT INTO face_samples(employee_id, dim, embedding, source, created_at) "
                    "VALUES(?,?,?,?,?)",
                    (employee_id, int(len(s)), emb_to_blob(s), "append", now),
                )
            n = self.conn.execute(
                "SELECT COUNT(*) c FROM face_samples WHERE employee_id=?", (employee_id,)
            ).fetchone()["c"]
            self.conn.execute(
                "UPDATE employees SET template=?, n_samples=?, updated_at=? WHERE id=?",
                (emb_to_blob(new_template), n, now, employee_id),
            )
            self.conn.commit()

    def set_active(self, employee_id: int, active: bool) -> None:
        self._ex("UPDATE employees SET active=?, updated_at=? WHERE id=?",
                 (1 if active else 0, time.time(), employee_id))

    def delete_employee(self, employee_id: int) -> None:
        self._ex("DELETE FROM employees WHERE id=?", (employee_id,))

    def get_employee(self, employee_id: int) -> Optional[sqlite3.Row]:
        rows = self._q("SELECT * FROM employees WHERE id=?", (employee_id,))
        return rows[0] if rows else None

    def list_employees(self, only_active: bool = False) -> list[Employee]:
        sql = ("SELECT id, employee_no, full_name, position, department, active, "
               "n_samples, created_at FROM employees")
        if only_active:
            sql += " WHERE active=1"
        sql += " ORDER BY full_name"
        return [Employee.from_row(r) for r in self._q(sql)]

    def get_thumbnail(self, employee_id: int) -> Optional[bytes]:
        rows = self._q("SELECT thumbnail FROM employees WHERE id=?", (employee_id,))
        return rows[0]["thumbnail"] if rows else None

    def load_templates(self, expected_dim: Optional[int] = None
                       ) -> tuple[list[int], np.ndarray, list[sqlite3.Row]]:
        """Все активные эталоны одной матрицей (N x dim) для векторного поиска.

        Возвращает (ids, матрица, отбракованные_строки).

        Про третий элемент. Векторы разной длины в одну матрицу не
        складываются, и раньше `np.stack` здесь падал с ValueError. Падение
        происходило внутри конструктора сервиса, то есть **сервер просто
        не запускался** — и выглядело это как «добавил человека, перезапустил,
        и больше ничего не работает».

        Разъехаться размерности могут по одной причине: сменили face.backend.
        У insightface вектор 512-мерный, у opencv — 128-мерный, и старые
        записи остаются в базе с прежней длиной. Теперь такие записи не
        роняют сервер, а откладываются в сторону, и о них громко пишется
        в лог: их надо перерегистрировать.
        """
        rows = self._q("SELECT id, dim, template FROM employees WHERE active=1 ORDER BY id")
        if not rows:
            return [], np.zeros((0, 0), dtype=np.float32), []

        dims = [int(r["dim"]) for r in rows]
        if expected_dim is None:
            # Ориентируемся на самую многочисленную размерность.
            expected_dim = max(set(dims), key=dims.count)

        good = [r for r in rows if int(r["dim"]) == expected_dim]
        bad = [r for r in rows if int(r["dim"]) != expected_dim]

        if not good:
            return [], np.zeros((0, expected_dim), dtype=np.float32), bad

        ids = [r["id"] for r in good]
        mat = np.stack([blob_to_emb(r["template"]) for r in good]).astype(np.float32)
        return ids, mat, bad

    def load_samples(self) -> list[tuple[int, np.ndarray]]:
        rows = self._q("SELECT employee_id, embedding FROM face_samples")
        return [(r["employee_id"], blob_to_emb(r["embedding"])) for r in rows]

    # ----------------------------------------------------------- sessions
    def create_session(self, employee_id: int, device_id: str, score: float,
                       ttl: int) -> tuple[str, float]:
        token = uuid.uuid4().hex
        now = time.time()
        exp = now + ttl
        self._ex(
            "INSERT INTO sessions(token, employee_id, device_id, score, created_at, expires_at) "
            "VALUES(?,?,?,?,?,?)",
            (token, employee_id, device_id, score, now, exp),
        )
        return token, exp

    def consume_session(self, token: str, device_id: str,
                        max_scans: int) -> tuple[Optional[sqlite3.Row], str]:
        """Проверить и «потратить» одно использование токена.

        Возвращает (row, "") при успехе или (None, код_ошибки).
        Вся проверка идёт в одной транзакции, чтобы не было гонки
        при двух одновременных запросах с одним токеном.
        """
        now = time.time()
        with self._lock:
            row = self.conn.execute(
                "SELECT * FROM sessions WHERE token=?", (token,)
            ).fetchone()
            if row is None:
                return None, "SESSION_UNKNOWN"
            if row["revoked"]:
                return None, "SESSION_REVOKED"
            if row["expires_at"] < now:
                return None, "SESSION_EXPIRED"
            if row["device_id"] != device_id:
                # Токен пытается использовать другое устройство — это атака.
                return None, "SESSION_DEVICE_MISMATCH"
            if row["scans_used"] >= max_scans:
                return None, "SESSION_EXHAUSTED"
            self.conn.execute(
                "UPDATE sessions SET scans_used=scans_used+1 WHERE token=?", (token,)
            )
            self.conn.commit()
            return row, ""

    def purge_sessions(self) -> int:
        cur = self._ex("DELETE FROM sessions WHERE expires_at < ?", (time.time() - 3600,))
        return cur.rowcount

    # ------------------------------------------------------- AKS API log_id
    def create_aks_log(self, device_id: str, session_token: Optional[str],
                       matched: bool) -> int:
        cur = self._ex(
            "INSERT INTO aks_logs(ts, device_id, session_token, matched) VALUES(?,?,?,?)",
            (time.time(), device_id, session_token, 1 if matched else 0),
        )
        return int(cur.lastrowid)

    def get_aks_log(self, log_id: int) -> Optional[sqlite3.Row]:
        rows = self._q("SELECT * FROM aks_logs WHERE log_id=?", (int(log_id),))
        return rows[0] if rows else None

    def finish_aks_log(self, log_id: int, barcode: str, record_id: int) -> None:
        self._ex(
            "UPDATE aks_logs SET cargo_barcode=?, record_id=? WHERE log_id=?",
            (barcode, int(record_id), int(log_id)),
        )

    # -------------------------------------------------------------- журнал
    def add_scan_event(self, employee_id: int, device_id: str, barcode: str,
                       symbology: str, token: str, score: float,
                       note: str = "") -> int:
        cur = self._ex(
            "INSERT INTO scan_events(ts, employee_id, device_id, barcode, symbology, "
            "session_token, match_score, note) VALUES(?,?,?,?,?,?,?,?)",
            (time.time(), employee_id, device_id, barcode, symbology, token, score, note),
        )
        return int(cur.lastrowid)

    def list_scan_events(self, limit: int = 500, employee_id: Optional[int] = None,
                         barcode: str = "", since: Optional[float] = None,
                         until: Optional[float] = None) -> list[sqlite3.Row]:
        """Журнал с фильтрами для страницы «Журнал» на сайте.

        Фильтры собираются параметрами, а не склейкой строк: журнал —
        главная таблица системы, и SQL-инъекция через поле поиска здесь
        стоила бы слишком дорого.
        """
        sql = ("SELECT e.*, emp.full_name, emp.employee_no FROM scan_events e "
               "LEFT JOIN employees emp ON emp.id = e.employee_id WHERE 1=1")
        args: list[Any] = []
        if employee_id:
            sql += " AND e.employee_id = ?"
            args.append(employee_id)
        if barcode:
            sql += " AND e.barcode LIKE ?"
            args.append(f"%{barcode}%")
        if since:
            sql += " AND e.ts >= ?"
            args.append(since)
        if until:
            sql += " AND e.ts <= ?"
            args.append(until)
        sql += " ORDER BY e.ts DESC LIMIT ?"
        args.append(int(limit))
        return self._q(sql, args)

    def list_verify_log(self, limit: int = 200) -> list[sqlite3.Row]:
        """Все попытки распознавания, включая неудачные.

        Нужен, чтобы разбирать жалобы «меня не узнаёт»: там видно score
        и код причины отказа по каждой попытке.
        """
        return self._q(
            "SELECT v.*, emp.full_name FROM verify_log v "
            "LEFT JOIN employees emp ON emp.id = v.employee_id "
            "ORDER BY v.ts DESC LIMIT ?", (limit,)
        )

    def add_verify_log(self, device_id: str, matched: bool, employee_id: Optional[int],
                       score: Optional[float], reason: str, latency_ms: int) -> None:
        self._ex(
            "INSERT INTO verify_log(ts, device_id, matched, employee_id, score, reason, "
            "latency_ms) VALUES(?,?,?,?,?,?,?)",
            (time.time(), device_id, 1 if matched else 0, employee_id, score,
             reason, latency_ms),
        )

    def stats(self) -> dict[str, int]:
        def one(sql: str) -> int:
            return int(self._q(sql)[0][0])

        return {
            "employees": one("SELECT COUNT(*) FROM employees WHERE active=1"),
            "samples": one("SELECT COUNT(*) FROM face_samples"),
            "scans": one("SELECT COUNT(*) FROM scan_events"),
            "verifies": one("SELECT COUNT(*) FROM verify_log"),
        }
