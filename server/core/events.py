"""Простейшая внутрипроцессная шина событий: API-поток -> GUI-поток.

GUI подписывается и получает события распознавания и сканирования в реальном
времени, без опроса базы. Подписчики вызываются в потоке публикации, поэтому
в GUI из обработчика нужно только эмитить Qt-сигнал (это потокобезопасно).
"""
from __future__ import annotations

import logging
import threading
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

log = logging.getLogger(__name__)

EV_VERIFY = "verify"      # завершилась попытка распознавания
EV_SCAN = "scan"          # записан штрихкод
EV_DEVICE = "device"      # устройство вышло на связь
EV_DB_CHANGED = "db"      # изменилась база сотрудников


@dataclass
class Event:
    kind: str
    ts: float
    data: dict[str, Any] = field(default_factory=dict)
    image: Optional[bytes] = None   # jpeg кадра, если он есть (не пишется на диск)


class EventBus:
    def __init__(self, history: int = 200):
        self._subs: list[Callable[[Event], None]] = []
        self._lock = threading.RLock()
        self.history: deque[Event] = deque(maxlen=history)

    def subscribe(self, fn: Callable[[Event], None]) -> None:
        with self._lock:
            self._subs.append(fn)

    def unsubscribe(self, fn: Callable[[Event], None]) -> None:
        with self._lock:
            if fn in self._subs:
                self._subs.remove(fn)

    def publish(self, ev: Event) -> None:
        with self._lock:
            subs = list(self._subs)
            # В историю кладём событие без картинки, чтобы не раздувать память
            self.history.append(Event(ev.kind, ev.ts, dict(ev.data), None))
        for fn in subs:
            try:
                fn(ev)
            except Exception:  # noqa: BLE001
                log.exception("Подписчик шины событий упал")


BUS = EventBus()
