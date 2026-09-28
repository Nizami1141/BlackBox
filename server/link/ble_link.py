"""
Транспорт Bluetooth LE (bleak).

Работает так же, как отлаженный BLE-клиент из демо, но без Qt: здесь всё
живёт в том же цикле asyncio, что и FastAPI, поэтому сценарий проверки
может просто `await link.request_photo()`.

Что важно знать при отладке:

* Bluetooth на Windows отдаёт устройство только одному процессу. Если
  параллельно открыт старый PyQt-GUI из esp32cam-ble-demo — сервер плату
  не найдёт. Закройте GUI.
* После перезагрузки платы Windows какое-то время держит старую пару.
  Реконнект здесь бесконечный с паузой, так что связь восстановится сама,
  просто не мгновенно.
* MTU согласуется уже ПОСЛЕ подключения. Размер чанка выбирает плата,
  серверу его знать не нужно — он приходит в заголовке кадра.
"""
from __future__ import annotations

import asyncio
import logging
from typing import Optional

from .base import DeviceLink, LinkError
from .protocol import (CHR_CTRL, CHR_DATA, CHR_STATUS, SVC_UUID, Barcode,
                       DeviceEvent, Photo, PhotoAssembler, parse_status)

log = logging.getLogger(__name__)


class BleLink(DeviceLink):
    kind = "ble"

    def __init__(self, device_id: str, ble_name: str = "ESP32S3-CAM",
                 scan_timeout: float = 8.0, retry_delay: float = 3.0):
        super().__init__(device_id)
        self.ble_name = ble_name
        self.scan_timeout = scan_timeout
        self.retry_delay = retry_delay
        self._task: Optional[asyncio.Task] = None
        self._client = None
        self._asm = PhotoAssembler()
        self._stop = False

    # ------------------------------------------------------ жизненный цикл
    async def start(self) -> None:
        if self._task and not self._task.done():
            return
        self._stop = False
        self._task = asyncio.create_task(self._loop(), name=f"ble:{self.device_id}")

    async def stop(self) -> None:
        self._stop = True
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):  # noqa: BLE001
                pass
            self._task = None
        self._set_connected(False)

    async def send_command(self, cmd: str) -> None:
        if not self._client or not self.connected:
            raise LinkError("LINK_DOWN", "Устройство не на связи")
        try:
            # response=False — «write without response». Плата подтверждения
            # не шлёт, а с ним каждая команда стоила бы лишний интервал
            # соединения (7.5 мс минимум).
            await self._client.write_gatt_char(CHR_CTRL, cmd.encode(), response=False)
            log.debug("[%s] -> %s", self.device_id, cmd)
        except Exception as e:  # noqa: BLE001
            raise LinkError("WRITE_FAILED", f"Не удалось послать команду: {e}") from e

    # ------------------------------------------------------------ колбэки
    def _on_data(self, _sender, data: bytearray) -> None:
        result = self._asm.feed(bytes(data))
        if result is None:
            return
        if isinstance(result, DeviceEvent):
            self._deliver_event(result)
        elif isinstance(result, Barcode):
            log.info("[%s] штрихкод #%d: %s", self.device_id, result.seq, result.text)
            self._deliver_barcode(result)
            self._notify("barcode", {"seq": result.seq, "text": result.text})
        elif isinstance(result, Photo):
            log.info("[%s] кадр #%d: %d байт, crc_ok=%s",
                     self.device_id, result.frame_id, len(result.data), result.crc_ok)
            self._deliver_photo(result)

    def _on_status(self, _sender, data: bytearray) -> None:
        st = parse_status(bytes(data))
        if st:
            self._deliver_status(st)

    # --------------------------------------------------------------- цикл
    async def _loop(self) -> None:
        from bleak import BleakClient, BleakScanner

        while not self._stop:
            log.info("[%s] ищу устройство «%s»…", self.device_id, self.ble_name)
            dev = None
            try:
                dev = await BleakScanner.find_device_by_filter(
                    lambda d, ad: (d.name or "") == self.ble_name
                    or SVC_UUID.lower() in [u.lower() for u in (ad.service_uuids or [])],
                    timeout=self.scan_timeout,
                )
            except Exception as e:  # noqa: BLE001
                log.warning("[%s] ошибка сканирования: %s", self.device_id, e)

            if dev is None:
                log.info("[%s] не найдено, повтор через %.0f с "
                         "(проверьте, что Bluetooth на ноутбуке включён)",
                         self.device_id, self.retry_delay)
                await asyncio.sleep(self.retry_delay)
                continue

            try:
                async with BleakClient(dev, timeout=15.0) as client:
                    self._client = client
                    self._asm = PhotoAssembler()      # новая сессия — новый сборщик
                    self._set_connected(True, str(dev.address))
                    log.info("[%s] подключено к %s", self.device_id, dev.address)

                    await client.start_notify(CHR_DATA, self._on_data)
                    await client.start_notify(CHR_STATUS, self._on_status)

                    # Стартовое состояние: сканер погашен, периодические фото
                    # выключены. Плата может быть перепрошита в демо-режиме,
                    # и тогда без этих двух команд сервер утонет в кадрах.
                    await self.send_command("PHOTO OFF")
                    await self.send_command("SCAN OFF")

                    while client.is_connected and not self._stop:
                        await asyncio.sleep(0.3)
            except asyncio.CancelledError:
                raise
            except Exception as e:  # noqa: BLE001
                log.warning("[%s] соединение потеряно: %s", self.device_id, e)
            finally:
                self._client = None
                self._set_connected(False)

            if not self._stop:
                await asyncio.sleep(2)
