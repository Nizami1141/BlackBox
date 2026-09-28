"""
Фейковый пост — весь сценарий без единого провода.

Нужен трижды:
  1. Показать концепт, когда плата лежит в другой сумке.
  2. Отлаживать сайт и журнал, не тратя заряд и не занимая Bluetooth.
  3. Прогонять тест сценария в CI, где железа нет в принципе.

Кадры берутся из папки (по умолчанию data/fake_faces) по кругу, штрихкод
«читается» через fake.scan_delay секунд после включения луча. Пакеты
собираются ровно теми же функциями protocol.py, что и на плате, и проходят
через тот же PhotoAssembler — то есть проверяется настоящий разбор,
а не заглушка.
"""
from __future__ import annotations

import asyncio
import itertools
import logging
import random
import time
from pathlib import Path
from typing import Optional

from .base import DeviceLink, LinkError
from .protocol import PhotoAssembler, build_barcode, split_photo

log = logging.getLogger(__name__)


class FakeLink(DeviceLink):
    kind = "fake"

    def __init__(self, device_id: str, faces_dir: Path,
                 scan_delay: float = 2.0, barcode: str = "",
                 ack_artifact: str = ""):
        super().__init__(device_id)
        self.faces_dir = Path(faces_dir)
        self.scan_delay = scan_delay
        self.fixed_barcode = barcode
        # Воспроизведение настоящей ошибки железа: сканер отвечает
        # подтверждением на команду START_DECODE, и байт контрольной суммы
        # после фильтра «только печатный ASCII» приезжает как «штрихкод»
        # из одного символа. Нужно, чтобы тест проверял отсев такого мусора
        # на реальном пути, а не на выдуманном.
        self.ack_artifact = ack_artifact
        self._asm = PhotoAssembler()
        self._files: list[Path] = []
        self._cycle: Optional[itertools.cycle] = None
        self._frame_id = 0
        self._barcode_seq = 0
        self._scan_task: Optional[asyncio.Task] = None

    async def start(self) -> None:
        self._files = sorted(
            p for p in self.faces_dir.glob("*")
            if p.suffix.lower() in (".jpg", ".jpeg", ".png")
        )
        if not self._files:
            log.warning("[%s] в %s нет картинок — фейковый пост будет отвечать "
                        "ошибкой NO_FACE", self.device_id, self.faces_dir)
        else:
            self._cycle = itertools.cycle(self._files)
        self._set_connected(True, "fake://local")
        self._deliver_status({"device_id": self.device_id, "mode": "fake",
                              "scan": False, "photo": False})

    async def stop(self) -> None:
        if self._scan_task:
            self._scan_task.cancel()
            self._scan_task = None
        self._set_connected(False)

    async def send_command(self, cmd: str) -> None:
        cmd = cmd.strip().upper()
        log.info("[%s] (fake) команда %s", self.device_id, cmd)

        if cmd in ("VERIFY", "SHOT"):
            asyncio.create_task(self._emit_photo())
        elif cmd == "SCAN ON":
            if self._scan_task and not self._scan_task.done():
                self._scan_task.cancel()
            self._scan_task = asyncio.create_task(self._emit_barcode())
            self._deliver_status({**self.status, "scan": True})
        elif cmd == "SCAN OFF":
            if self._scan_task:
                self._scan_task.cancel()
                self._scan_task = None
            self._deliver_status({**self.status, "scan": False})

    # ------------------------------------------------------------ внутреннее
    async def _emit_photo(self) -> None:
        # Небольшая задержка — иначе фейк отвечает быстрее, чем сайт успевает
        # показать «снимаю», и стадии сценария мелькают неразличимо.
        await asyncio.sleep(0.4)
        if not self._cycle:
            self._fail_waiters("NO_IMAGE",
                               f"Нет ни одной картинки в {self.faces_dir}")
            return
        path: Path = next(self._cycle)
        jpeg = path.read_bytes()
        self._frame_id += 1
        for packet in split_photo(jpeg, frame_id=self._frame_id):
            result = self._asm.feed(packet)
            if result is not None:
                self._deliver_photo(result)
        log.info("[%s] (fake) отдал кадр %s (%d байт)",
                 self.device_id, path.name, len(jpeg))

    async def _emit_barcode(self) -> None:
        if self.ack_artifact:
            await asyncio.sleep(0.05)
            self._push_code(self.ack_artifact)
        await asyncio.sleep(self.scan_delay)
        text = self.fixed_barcode or f"TRK{random.randint(100000, 999999)}AZ"
        self._push_code(text)

    def _push_code(self, text: str) -> None:
        packet = build_barcode(self._barcode_seq + 1, text)
        self._barcode_seq += 1
        result = self._asm.feed(packet)
        if result is not None:
            self._deliver_barcode(result)
            self._notify("barcode", {"seq": self._barcode_seq, "text": text})
        log.info("[%s] (fake) прочитал штрихкод %s", self.device_id, text)
