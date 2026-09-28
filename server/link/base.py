"""
Транспорт до устройства — общий интерфейс.

Зачем этот слой вообще нужен. Сегодня плата разговаривает с ноутбуком по
Bluetooth LE, потому что университетская сеть блокирует всё остальное.
Завтра на посту будет Ethernet, а BLE исчезнет. Если бы сценарий проверки
(core/flow.py) дёргал bleak напрямую, при переходе на Ethernet пришлось бы
переписывать всю бизнес-логику. Поэтому сценарий знает только четыре
глагола, а как именно они долетают до железа — дело реализации:

    request_photo()   попросить кадр и дождаться его
    set_scanner()     включить/погасить луч сканера
    send_command()    послать произвольную ASCII-команду
    wait_barcode()    дождаться прочитанного штрихкода

Реализации:
    ble_link.BleLink     работает сейчас (bleak)
    ws_link.WsLink       для Wi-Fi/Ethernet: плата сама открывает WebSocket
    fake_link.FakeLink   тесты и демонстрация без железа
"""
from __future__ import annotations

import abc
import asyncio
import logging
import time
from typing import Callable, Optional

from .protocol import (CMD_LCD1, CMD_LCD2, CMD_LCD_HOLD, CMD_SCAN_OFF,
                       CMD_SCAN_ON, CMD_VERIFY, Barcode, DeviceEvent, Photo)

log = logging.getLogger(__name__)


class LinkError(Exception):
    """Ошибка транспорта, которую можно показать оператору дословно."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


class DeviceLink(abc.ABC):
    """Один физический пост (одна плата)."""

    kind = "abstract"

    def __init__(self, device_id: str):
        self.device_id = device_id
        self.connected = False
        self.status: dict = {}
        self.last_seen: float = 0.0
        self.address: str = ""

        # Ожидание кадра: сценарий кладёт сюда future и ждёт его.
        # Один запрос за раз — параллельно проверять два лица на одном посту
        # физически невозможно (камера одна).
        self._photo_waiter: Optional[asyncio.Future] = None
        # Штрихкоды складываем в очередь: код может прийти чуть раньше, чем
        # сценарий успеет позвать wait_barcode(), и терять его нельзя.
        self._barcodes: asyncio.Queue[Barcode] = asyncio.Queue(maxsize=8)
        # Сколько сценариев прямо сейчас ждут штрихкод. Нужен только для
        # осмысленного лога: код, пришедший «мимо» проверки, теряется молча,
        # и без этого счётчика причину не отличить от неисправного сканера.
        self._waiting = 0

        # Наблюдатели (веб-интерфейс): вызываются при смене статуса.
        self._observers: list[Callable[[str, dict], None]] = []

    # ------------------------------------------------------------ наблюдатели
    def add_observer(self, fn: Callable[[str, dict], None]) -> None:
        self._observers.append(fn)

    def _notify(self, kind: str, data: dict) -> None:
        for fn in list(self._observers):
            try:
                fn(kind, {"device_id": self.device_id, **data})
            except Exception:  # noqa: BLE001
                log.exception("Наблюдатель линка упал")

    # ------------------------------------------------- вызывают реализации
    def _set_connected(self, on: bool, address: str = "") -> None:
        if on == self.connected and address == self.address:
            return
        self.connected = on
        if address:
            self.address = address
        if not on:
            # Разрыв связи должен немедленно разбудить всех ждущих,
            # иначе запрос кадра будет висеть до таймаута впустую.
            self._fail_waiters("LINK_DOWN", "Связь с устройством потеряна")
        self._notify("device", {"connected": on, "address": self.address,
                                "kind": self.kind})

    def _deliver_photo(self, photo: Photo) -> None:
        self.last_seen = time.time()
        w = self._photo_waiter
        if w is None or w.done():
            # Кадр, который никто не просил (демо-режим платы или
            # запоздавший ответ). Логируем и выбрасываем.
            log.debug("[%s] незапрошенный кадр #%s", self.device_id, photo.frame_id)
            return
        self._photo_waiter = None
        w.set_result(photo)

    def _deliver_barcode(self, bc: Barcode) -> None:
        self.last_seen = time.time()
        # Логируем КАЖДЫЙ пришедший код, даже если его никто не ждёт.
        # Без этой строки «сканер пищит, а в журнале пусто» неотличимо от
        # «сканер вообще ничего не прислал», а это совершенно разные поломки
        # и чинятся они в разных местах.
        if self._waiting:
            log.info("[%s] штрихкод %r принят, проверка его ждёт",
                     self.device_id, bc.text)
        else:
            log.warning(
                "[%s] штрихкод %r пришёл, но проверка сейчас НЕ идёт — "
                "он положен в очередь и будет выброшен в начале следующей "
                "проверки. Так бывает, если сканировали до опознания лица "
                "или уже после того, как проверка закончилась по таймауту.",
                self.device_id, bc.text)
        try:
            self._barcodes.put_nowait(bc)
        except asyncio.QueueFull:
            log.warning("[%s] очередь штрихкодов переполнена", self.device_id)

    def _deliver_event(self, ev: DeviceEvent) -> None:
        """Событие, начатое на самом посту (нажали кнопку).

        Просто пробрасывается наблюдателям: решает, что с ним делать,
        api/server.py — он же запускает сценарий проверки.
        """
        self.last_seen = time.time()
        log.info("[%s] событие с поста: %s", self.device_id, ev.name)
        self._notify("event", {"name": ev.name})

    def _deliver_status(self, st: dict) -> None:
        self.last_seen = time.time()
        self.status = st
        self._notify("status", {"status": st})

    def _fail_waiters(self, code: str, message: str) -> None:
        w = self._photo_waiter
        self._photo_waiter = None
        if w is not None and not w.done():
            w.set_exception(LinkError(code, message))

    # -------------------------------------------------------- жизненный цикл
    @abc.abstractmethod
    async def start(self) -> None:
        """Начать поддерживать связь (обычно — фоновая задача)."""

    @abc.abstractmethod
    async def stop(self) -> None:
        ...

    @abc.abstractmethod
    async def send_command(self, cmd: str) -> None:
        ...

    # ------------------------------------------------------------- действия
    async def request_photo(self, timeout: float = 15.0) -> Photo:
        """Попросить кадр и дождаться его целиком.

        Таймаут по умолчанию 15 с: VGA-кадр по BLE уезжает за 0.5–1.5 с,
        но при плохом эфире и мелком MTU бывает и 8 с. Меньше 10 ставить
        не стоит — будут ложные «устройство не ответило».
        """
        if not self.connected:
            raise LinkError("LINK_DOWN", "Устройство не на связи")
        if self._photo_waiter is not None and not self._photo_waiter.done():
            raise LinkError("BUSY", "Кадр уже запрошен, дождитесь ответа")

        loop = asyncio.get_running_loop()
        self._photo_waiter = loop.create_future()
        await self.send_command(CMD_VERIFY)
        try:
            photo: Photo = await asyncio.wait_for(self._photo_waiter, timeout)
        except asyncio.TimeoutError as e:
            self._photo_waiter = None
            raise LinkError("PHOTO_TIMEOUT",
                            "Устройство не прислало кадр за отведённое время") from e

        if not photo.crc_ok:
            raise LinkError(
                "PHOTO_CORRUPT",
                f"Кадр пришёл повреждённым (потеряно чанков: {photo.missing}). "
                f"Увеличьте BLE_CHUNK_DELAY_MS в config.h до 3–5.",
            )
        return photo

    async def set_scanner(self, on: bool) -> None:
        await self.send_command(CMD_SCAN_ON if on else CMD_SCAN_OFF)

    async def lcd(self, line1: str = "", line2: str = "", hold_ms: int = 0) -> None:
        """Показать две строки на экране поста.

        Текст уходит как есть, в UTF-8: перевод в знакоместа HD44780 делает
        сама плата (firmware/src/lcd.cpp). Поэтому сервер ничего не знает про
        таблицу символов экрана, а формулировки правятся в config.yaml,
        без перепрошивки постов.

        Экран есть не на каждом посту, и это не повод ронять проверку:
        ошибка отправки только логируется.
        """
        try:
            if hold_ms:
                await self.send_command(f"{CMD_LCD_HOLD} {int(hold_ms)}")
            await self.send_command(f"{CMD_LCD1} {line1}")
            await self.send_command(f"{CMD_LCD2} {line2}")
        except LinkError as e:
            log.debug("[%s] экран недоступен: %s", self.device_id, e.message)

    def drain_barcodes(self) -> None:
        """Выбросить всё, что скопилось до начала новой проверки.

        Без этого «хвост» прошлого скана записался бы на нового сотрудника.
        """
        while not self._barcodes.empty():
            try:
                self._barcodes.get_nowait()
            except asyncio.QueueEmpty:
                break

    async def wait_barcode(self, timeout: float) -> Barcode:
        self._waiting += 1
        try:
            return await asyncio.wait_for(self._barcodes.get(), timeout)
        except asyncio.TimeoutError as e:
            raise LinkError("SCAN_TIMEOUT",
                            "Штрихкод не поднесли за отведённое время") from e
        finally:
            self._waiting -= 1

    # ---------------------------------------------------------------- сводка
    def info(self) -> dict:
        return {
            "device_id": self.device_id,
            "kind": self.kind,
            "connected": self.connected,
            "address": self.address,
            "last_seen": self.last_seen,
            "status": self.status,
        }


class LinkRegistry:
    """Все посты, подключённые к этому серверу."""

    def __init__(self) -> None:
        self._links: dict[str, DeviceLink] = {}
        self._observers: list[Callable[[str, dict], None]] = []

    def add_observer(self, fn: Callable[[str, dict], None]) -> None:
        self._observers.append(fn)
        for link in self._links.values():
            link.add_observer(fn)

    def register(self, link: DeviceLink) -> DeviceLink:
        for fn in self._observers:
            link.add_observer(fn)
        self._links[link.device_id] = link
        log.info("Линк зарегистрирован: %s (%s)", link.device_id, link.kind)
        return link

    def get(self, device_id: str) -> DeviceLink:
        link = self._links.get(device_id)
        if link is None:
            raise LinkError("NO_DEVICE", f"Устройство {device_id!r} не настроено")
        return link

    def first_connected(self) -> Optional[DeviceLink]:
        for link in self._links.values():
            if link.connected:
                return link
        return None

    def all(self) -> list[DeviceLink]:
        return list(self._links.values())

    def info(self) -> list[dict]:
        return [l.info() for l in self._links.values()]

    async def start_all(self) -> None:
        for link in self._links.values():
            await link.start()

    async def stop_all(self) -> None:
        for link in self._links.values():
            try:
                await link.stop()
            except Exception:  # noqa: BLE001
                log.exception("Ошибка остановки линка %s", link.device_id)
