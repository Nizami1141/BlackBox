"""
Транспорт WebSocket — путь на Wi-Fi и Ethernet.

Сегодня не используется (плата ходит по BLE), но написан сейчас осознанно:
когда на посту появится Ethernet, менять придётся только прошивку, а весь
серверный код — сценарий проверки, сайт, журнал — останется прежним.

Почему WebSocket, а не «плата сама шлёт POST /verify», как было в первой
версии проекта. В сценарии с сайтом инициатива у сервера: оператор нажал
кнопку — сервер должен СКАЗАТЬ плате «снимай». По обычному HTTP сервер
не может позвать устройство: у платы нет адреса, доступного снаружи, и
она за NAT. Пришлось бы либо опрашивать сервер каждые полсекунды (задержка
и лишний трафик), либо держать long-poll — то есть тот же WebSocket, только
хуже. Поэтому плата сама открывает одно постоянное соединение к серверу,
и дальше связь двусторонняя.

Формат внутри WebSocket ровно тот же, что по BLE:
    binary-кадры  = пакеты protocol.py (0x01 заголовок / 0x02 данные /
                    0x03 конец / 0x04 штрихкод)
    text-кадры    = JSON статуса
    команды от сервера — text-кадры с ASCII ("VERIFY", "SCAN ON", ...)

Один разборщик пакетов на оба транспорта — значит, поймав ошибку на BLE,
вы чините её и для Ethernet.

Прошивка (когда дойдут руки до Ethernet):
    #include <ArduinoWebsockets.h>
    client.connect("ws://192.168.1.10:8000/ws/device/GATE-01?key=<api_key>");
    client.onMessage(...)         -> те же команды, что сейчас в CtrlCB
    client.sendBinary(hdr, len)   -> вместо g_chrData->notify()
Функции buildHeader/buildChunk/buildEnd/buildBarcode менять не нужно
вообще: они уже отдают готовые байты.
"""
from __future__ import annotations

import asyncio
import logging
from typing import Any, Optional

from .base import DeviceLink, LinkError
from .protocol import (Barcode, DeviceEvent, Photo, PhotoAssembler,
                       parse_status)

log = logging.getLogger(__name__)


class WsLink(DeviceLink):
    kind = "ws"

    def __init__(self, device_id: str):
        super().__init__(device_id)
        self._ws: Optional[Any] = None       # starlette.websockets.WebSocket
        self._asm = PhotoAssembler()
        self._lock = asyncio.Lock()

    # Линк живёт, пока устройство держит соединение; поднимать нечего.
    async def start(self) -> None:
        return

    async def stop(self) -> None:
        ws, self._ws = self._ws, None
        self._set_connected(False)
        if ws is not None:
            try:
                await ws.close()
            except Exception:  # noqa: BLE001
                pass

    async def send_command(self, cmd: str) -> None:
        ws = self._ws
        if ws is None or not self.connected:
            raise LinkError("LINK_DOWN", "Устройство не на связи")
        async with self._lock:
            try:
                await ws.send_text(cmd)
                log.debug("[%s] -> %s", self.device_id, cmd)
            except Exception as e:  # noqa: BLE001
                raise LinkError("WRITE_FAILED", f"Не удалось послать команду: {e}") from e

    # ------------------------------------------- вызывается из api/server.py
    async def attach(self, ws: Any, address: str = "") -> None:
        """Принять новое соединение от устройства.

        Если пост переподключился, не дождавшись, пока сервер заметит обрыв,
        старое соединение надо закрыть — иначе команды будут уходить в него.
        """
        old, self._ws = self._ws, ws
        if old is not None:
            try:
                await old.close()
            except Exception:  # noqa: BLE001
                pass
        self._asm = PhotoAssembler()
        self._set_connected(True, address)

    async def detach(self, ws: Any) -> None:
        if self._ws is ws:
            self._ws = None
            self._set_connected(False)

    def feed_binary(self, data: bytes) -> None:
        result = self._asm.feed(data)
        if isinstance(result, DeviceEvent):
            self._deliver_event(result)
        elif isinstance(result, Barcode):
            self._deliver_barcode(result)
            self._notify("barcode", {"seq": result.seq, "text": result.text})
        elif isinstance(result, Photo):
            self._deliver_photo(result)

    def feed_text(self, text: str) -> None:
        st = parse_status(text.encode("utf-8"))
        if st:
            self._deliver_status(st)
