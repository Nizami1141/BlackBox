"""
Рассылка событий в браузер по WebSocket.

Тонкость, из-за которой этот файл вообще существует: события приходят из
двух разных миров. Сценарий проверки живёт в цикле asyncio, а распознавание
лица (core.service) выполняется в рабочем потоке через asyncio.to_thread и
публикует свои события оттуда же. Трогать WebSocket из чужого потока нельзя.

Поэтому здесь запоминается ссылка на главный цикл, и всё, что приходит
из потоков, пробрасывается в него через call_soon_threadsafe.
"""
from __future__ import annotations

import asyncio
import json
import logging
from typing import Any, Optional

log = logging.getLogger(__name__)


class WsHub:
    def __init__(self, history: int = 50):
        self._clients: set[Any] = set()
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._history: list[dict] = []
        self._history_max = history

    def bind_loop(self, loop: asyncio.AbstractEventLoop) -> None:
        self._loop = loop

    async def connect(self, ws) -> None:
        await ws.accept()
        self._clients.add(ws)
        # Новому клиенту отдаём хвост истории: если оператор открыл вкладку
        # в середине проверки, он должен увидеть, что происходит, а не пустой
        # экран до следующего события.
        for ev in self._history[-10:]:
            try:
                await ws.send_text(json.dumps(ev, ensure_ascii=False))
            except Exception:  # noqa: BLE001
                break

    def disconnect(self, ws) -> None:
        self._clients.discard(ws)

    # ------------------------------------------------------------ отправка
    def publish(self, event: dict) -> None:
        """Можно звать из любого потока."""
        keep = {k: v for k, v in event.items() if k != "frame"}   # без картинок
        self._history.append(keep)
        if len(self._history) > self._history_max:
            self._history.pop(0)

        loop = self._loop
        if loop is None:
            return
        try:
            if loop.is_running():
                loop.call_soon_threadsafe(
                    lambda: asyncio.create_task(self._broadcast(event))
                )
        except RuntimeError:
            # Цикл уже закрывается — событие просто теряется, это не авария.
            pass

    async def _broadcast(self, event: dict) -> None:
        if not self._clients:
            return
        raw = json.dumps(event, ensure_ascii=False)
        dead = []
        for ws in list(self._clients):
            try:
                await ws.send_text(raw)
            except Exception:  # noqa: BLE001
                dead.append(ws)
        for ws in dead:
            self._clients.discard(ws)

    @property
    def clients(self) -> int:
        return len(self._clients)
