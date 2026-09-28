"""
Сервисный слой: вся бизнес-логика в одном месте.

И HTTP API, и PyQt6 GUI работают через этот класс. Благодаря этому GUI
регистрирует сотрудников ровно тем же пайплайном, каким сервер потом их
распознаёт, — рассинхронизации быть не может.
"""
from __future__ import annotations

import io
import logging
import time
from dataclasses import dataclass
from typing import Optional

import cv2
import numpy as np

from .config import Config
from .db import Database
from .events import BUS, EV_DB_CHANGED, EV_SCAN, EV_VERIFY, Event
from .face_engine import FaceEngine, FaceError, average_embeddings
from .liveness import LivenessChecker
from .vector_db import VectorIndex

log = logging.getLogger(__name__)


@dataclass
class VerifyResult:
    matched: bool
    reason: str = ""                     # код причины отказа
    message: str = ""                    # текст для LCD/оператора
    employee_id: Optional[int] = None
    employee_no: str = ""
    full_name: str = ""
    position: str = ""
    score: float = 0.0
    session_token: str = ""
    expires_in: int = 0
    bbox: Optional[tuple[int, int, int, int]] = None
    latency_ms: int = 0
    candidates: list = None              # топ-кандидаты для отладки в GUI


class CheckpointService:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.db = Database(cfg.db_file)
        self.db.sync_devices(cfg.devices)
        self.engine = FaceEngine(
            backend=cfg.backend,
            models_dir=cfg.models_dir,
            det_score_min=cfg.det_score_min,
            min_face_px=cfg.min_face_px,
            allow_multiple_faces=cfg.allow_multiple_faces,
            det_size=cfg.det_size,
            max_side=cfg.max_side,
            threads=cfg.threads,
        )
        # Индексу сообщаем размерность модели: записи с другой длиной
        # вектора (остались от прежнего face.backend) он отложит в сторону,
        # а не уронит сервер при старте.
        self.index = VectorIndex(self.db, expected_dim=self.engine.dim)
        self.liveness = LivenessChecker(cfg.liveness_enabled, cfg.liveness_mode)
        log.info("Сервис поднят: %d сотрудников в базе", self.index.size)

    # ================================================================= verify
    def verify_jpeg(self, jpeg: bytes, device_id: str) -> VerifyResult:
        t0 = time.perf_counter()

        def finish(res: VerifyResult, image: Optional[bytes]) -> VerifyResult:
            res.latency_ms = int((time.perf_counter() - t0) * 1000)
            self.db.add_verify_log(device_id, res.matched, res.employee_id,
                                   res.score, res.reason, res.latency_ms)
            BUS.publish(Event(EV_VERIFY, time.time(), {
                "device_id": device_id,
                "matched": res.matched,
                "reason": res.reason,
                "message": res.message,
                "employee_id": res.employee_id,
                "full_name": res.full_name,
                "employee_no": res.employee_no,
                "score": res.score,
                "bbox": res.bbox,
                "latency_ms": res.latency_ms,
                "candidates": res.candidates or [],
            }, image=image))
            return res

        # 1. Декодирование + детекция + эмбеддинг
        try:
            face, img = self.engine.extract_from_jpeg(jpeg)
        except FaceError as e:
            return finish(VerifyResult(False, e.code, e.message), jpeg)

        # 2. Проверка живости (сейчас заглушка, см. core/liveness.py)
        lv = self.liveness.check(img, face.bbox)
        if not lv.is_live:
            return finish(
                VerifyResult(False, "SPOOF_SUSPECTED",
                             f"Подозрение на подделку (score={lv.score:.2f})",
                             bbox=face.bbox),
                jpeg,
            )

        # 3. Поиск по векторной базе
        try:
            hit, hits = self.index.identify(face.embedding, self.cfg.threshold)
        except ValueError as e:
            return finish(VerifyResult(False, "INDEX_MISMATCH", str(e), bbox=face.bbox), jpeg)

        cands = []
        for h in hits:
            row = self.db.get_employee(h.employee_id)
            cands.append({
                "employee_id": h.employee_id,
                "full_name": row["full_name"] if row else "?",
                "score": round(h.score, 4),
            })

        if hit is None:
            best = hits[0].score if hits else 0.0
            return finish(
                VerifyResult(False, "NOT_FOUND",
                             "Сотрудник не найден в базе",
                             score=best, bbox=face.bbox, candidates=cands),
                jpeg,
            )

        row = self.db.get_employee(hit.employee_id)
        if row is None or not row["active"]:
            return finish(
                VerifyResult(False, "INACTIVE", "Сотрудник заблокирован",
                             employee_id=hit.employee_id, score=hit.score,
                             bbox=face.bbox, candidates=cands),
                jpeg,
            )

        # 4. Выдача одноразового токена — связка «лицо ↔ штрихкод»
        token, exp = self.db.create_session(hit.employee_id, device_id,
                                            hit.score, self.cfg.session_ttl)

        if self.cfg.keep_probe_images:
            self._save_probe(jpeg, hit.employee_id, device_id)

        return finish(
            VerifyResult(
                matched=True,
                message=f"Здравствуйте, {row['full_name']}",
                employee_id=hit.employee_id,
                employee_no=row["employee_no"],
                full_name=row["full_name"],
                position=row["position"] or "",
                score=hit.score,
                session_token=token,
                expires_in=int(exp - time.time()),
                bbox=face.bbox,
                candidates=cands,
            ),
            jpeg,
        )

    # =================================================================== scan
    def register_scan(self, token: str, barcode: str, symbology: str,
                      device_id: str) -> tuple[bool, str, int]:
        """Записать штрихкод. Возвращает (ok, код_ошибки, record_id)."""
        barcode = (barcode or "").strip()
        if not barcode:
            return False, "EMPTY_BARCODE", 0

        row, err = self.db.consume_session(token, device_id, self.cfg.session_max_scans)
        if row is None:
            BUS.publish(Event(EV_SCAN, time.time(), {
                "device_id": device_id, "ok": False, "reason": err,
                "barcode": barcode, "symbology": symbology,
            }))
            return False, err, 0

        emp = self.db.get_employee(row["employee_id"])
        rec_id = self.db.add_scan_event(
            employee_id=row["employee_id"],
            device_id=device_id,
            barcode=barcode,
            symbology=symbology,
            token=token,
            score=row["score"],
        )
        BUS.publish(Event(EV_SCAN, time.time(), {
            "device_id": device_id,
            "ok": True,
            "record_id": rec_id,
            "barcode": barcode,
            "symbology": symbology,
            "employee_id": row["employee_id"],
            "full_name": emp["full_name"] if emp else "",
            "employee_no": emp["employee_no"] if emp else "",
            "scans_used": row["scans_used"] + 1,
            "max_scans": self.cfg.session_max_scans,
        }))
        return True, "", rec_id

    # ============================================================== enrolment
    def enroll(self, employee_no: str, full_name: str, images: list[np.ndarray],
               position: str = "", department: str = "") -> tuple[int, list[str]]:
        """Зарегистрировать сотрудника по нескольким снимкам.

        Возвращает (employee_id, список предупреждений по отдельным кадрам).
        """
        embs, warns = self._embed_batch(images)
        if not embs:
            raise FaceError("NO_VALID_SAMPLES",
                            "Ни на одном снимке не удалось получить пригодное лицо")

        # Внутренняя проверка качества: все снимки должны быть похожи друг на друга.
        # Если нет — оператор перепутал людей или снимки очень разного качества.
        if len(embs) > 1:
            sims = [float(np.dot(embs[0], e)) for e in embs[1:]]
            if min(sims) < 0.35:
                warns.append(
                    f"Снимки плохо согласуются между собой (min cos={min(sims):.2f}). "
                    f"Проверьте, что это один и тот же человек."
                )

        template = average_embeddings(embs)
        thumb = self._thumbnail(images[0]) if self.cfg.keep_enroll_thumbnail else None
        emp_id = self.db.add_employee(employee_no, full_name, template, embs,
                                      position, department, thumb)
        self.index.reload()
        BUS.publish(Event(EV_DB_CHANGED, time.time(), {"action": "add", "id": emp_id}))
        return emp_id, warns

    def enroll_jpegs(self, employee_no: str, full_name: str, blobs: list[bytes],
                     position: str = "", department: str = "") -> tuple[int, list[str]]:
        """То же, что enroll(), но на вход — байты файлов из браузера.

        Веб-страница регистрации присылает и снимки с веб-камеры (JPEG из
        canvas), и загруженные файлы. Декодирование одно и то же, поэтому
        отдельного пути для них нет.
        """
        images = [self.engine.decode_jpeg(b) for b in blobs]
        return self.enroll(employee_no, full_name, images, position, department)

    def append_samples_jpegs(self, employee_id: int, blobs: list[bytes]) -> list[str]:
        return self.append_samples(employee_id, [self.engine.decode_jpeg(b) for b in blobs])

    def preview_faces(self, jpeg: bytes) -> list[dict]:
        """Что видит детектор на этом снимке — для подсказки при регистрации.

        Оператор должен понимать, почему кадр отклонён, ДО того как нажмёт
        «Сохранить»: лица нет, лицо мелкое, лиц два.
        """
        return self.engine.detect_boxes(self.engine.decode_jpeg(jpeg))

    def append_samples(self, employee_id: int, images: list[np.ndarray]) -> list[str]:
        embs, warns = self._embed_batch(images)
        if not embs:
            raise FaceError("NO_VALID_SAMPLES", "Пригодных лиц не найдено")
        old = [e for _, e in self.db.load_samples() if _ == employee_id]
        template = average_embeddings(old + embs)
        self.db.add_samples(employee_id, embs, template)
        self.index.reload()
        BUS.publish(Event(EV_DB_CHANGED, time.time(),
                          {"action": "update", "id": employee_id}))
        return warns

    def delete_employee(self, employee_id: int) -> None:
        self.db.delete_employee(employee_id)
        self.index.reload()
        BUS.publish(Event(EV_DB_CHANGED, time.time(),
                          {"action": "delete", "id": employee_id}))

    def set_active(self, employee_id: int, active: bool) -> None:
        self.db.set_active(employee_id, active)
        self.index.reload()
        BUS.publish(Event(EV_DB_CHANGED, time.time(),
                          {"action": "active", "id": employee_id}))

    # ================================================================ helpers
    def _embed_batch(self, images: list[np.ndarray]) -> tuple[list[np.ndarray], list[str]]:
        embs, warns = [], []
        for i, img in enumerate(images, 1):
            try:
                embs.append(self.engine.extract(img).embedding)
            except FaceError as e:
                warns.append(f"Снимок {i}: {e.message}")
        return embs, warns

    @staticmethod
    def _thumbnail(bgr: np.ndarray, size: int = 160) -> bytes:
        h, w = bgr.shape[:2]
        s = size / max(h, w)
        small = cv2.resize(bgr, (max(1, int(w * s)), max(1, int(h * s))))
        ok, buf = cv2.imencode(".jpg", small, [cv2.IMWRITE_JPEG_QUALITY, 80])
        return buf.tobytes() if ok else b""

    def _save_probe(self, jpeg: bytes, emp_id: int, device_id: str) -> None:
        try:
            self.cfg.probe_dir.mkdir(parents=True, exist_ok=True)
            name = f"{int(time.time()*1000)}_{device_id}_{emp_id}.jpg"
            (self.cfg.probe_dir / name).write_bytes(jpeg)
        except Exception:  # noqa: BLE001
            log.exception("Не удалось сохранить probe-кадр")
