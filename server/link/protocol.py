"""
Разбор пакетов, которыми плата разговаривает с сервером.

Это точная копия описания из firmware/src/protocol.h. Если правите там —
правьте и здесь, иначе кадры перестанут собираться. Тест
`tests/test_protocol.py` проверяет обе стороны на одних и тех же байтах.

Формат (little-endian, как '<' в struct):
    Заголовок (17 байт): [0x01][frame_id u32][total u32][chunk u16][n u16][w u16][h u16]
    Данные:              [0x02][seq u16][payload...]
    Конец (9 байт):      [0x03][frame_id u32][crc32 u32]
    Штрихкод:            [0x04][seq u32][len u16][ASCII...]

Зачем сквозной CRC, если у BLE есть своя проверка: канальная CRC ловит
искажение пакета, но не ловит ПОТЕРЮ уведомления, а notify доставку не
гарантирует. Без сквозной суммы битый JPEG молча уехал бы в распознавание
и дал бы «лицо не найдено» вместо честной ошибки связи.
"""
from __future__ import annotations

import json
import struct
import zlib
from dataclasses import dataclass, field
from typing import Optional, Union

# --- UUID сервиса и характеристик (должны совпадать с protocol.h) ----------
SVC_UUID = "9a1f0001-4b2c-4d8e-9f10-2a3b4c5d6e7f"
CHR_CTRL = "9a1f0002-4b2c-4d8e-9f10-2a3b4c5d6e7f"    # write  — команды плате
CHR_DATA = "9a1f0003-4b2c-4d8e-9f10-2a3b4c5d6e7f"    # notify — фото и штрихкоды
CHR_STATUS = "9a1f0004-4b2c-4d8e-9f10-2a3b4c5d6e7f"  # notify — статус (JSON)

PKT_HEADER = 0x01
PKT_CHUNK = 0x02
PKT_END = 0x03
PKT_BARCODE = 0x04
# Событие от платы к серверу. Пока одно: "BUTTON" — сотрудник нажал кнопку
# у камеры. Нужно, чтобы проверку запускал он сам, а не только оператор
# мышью на сайте: на границе рядом с постом компьютера не будет.
PKT_EVENT = 0x05

HDR_FMT = "<IIHHHH"                          # frame_id, total, chunk, n, w, h
HDR_LEN = 1 + struct.calcsize(HDR_FMT)       # 17
END_FMT = "<II"                              # frame_id, crc32
END_LEN = 1 + struct.calcsize(END_FMT)       # 9
BC_FMT = "<IH"                               # seq, len
BC_LEN = 1 + struct.calcsize(BC_FMT)         # 7
EV_FMT = "<H"                                # len
EV_LEN = 1 + struct.calcsize(EV_FMT)         # 3

# --- команды, которые сервер пишет в CHR_CTRL (обычный ASCII) -------------
CMD_VERIFY = "VERIFY"        # снять кадр на опознание и прислать
CMD_SHOT = "SHOT"            # то же самое, «ручной» снимок
CMD_SCAN_ON = "SCAN ON"      # включить луч сканера
CMD_SCAN_OFF = "SCAN OFF"    # погасить луч
CMD_PHOTO_ON = "PHOTO ON"    # периодические фото (демо-режим)
CMD_PHOTO_OFF = "PHOTO OFF"
CMD_LCD1 = "LCD1"            # первая строка экрана, дальше UTF-8 текст
CMD_LCD2 = "LCD2"            # вторая строка
CMD_LCD_HOLD = "LCDHOLD"     # сколько мс держать сообщение до возврата в покой


@dataclass
class DeviceEvent:
    """Нажатие кнопки и прочие события, инициированные на самом посту."""
    name: str


@dataclass
class Barcode:
    seq: int
    text: str
    truncated: bool = False       # строка не влезла в одно уведомление


@dataclass
class Photo:
    frame_id: int
    data: bytes
    width: int
    height: int
    crc_ok: bool
    missing: int = 0              # сколько чанков не дошло


@dataclass
class _Partial:
    frame_id: int
    total: int
    chunk: int
    n_chunks: int
    width: int
    height: int
    parts: dict = field(default_factory=dict)


class PhotoAssembler:
    """Собирает JPEG из потока уведомлений.

    Чанки складываются в словарь по номеру, а не дописываются в буфер:
    порядок доставки уведомлений формально не гарантирован, и пропуск
    одного пакета не должен сдвигать все последующие байты.
    """

    def __init__(self) -> None:
        self._cur: Optional[_Partial] = None
        self.stats_ok = 0
        self.stats_bad = 0
        self.stats_codes = 0
        self.stats_events = 0

    def feed(self, packet: bytes) -> Union[Photo, Barcode, DeviceEvent, None]:
        """Разобрать одно уведомление.

        Возвращает Photo, Barcode или None (промежуточный пакет).
        Штрихкод может прилететь прямо посреди передачи фотографии —
        это нормально и сборку кадра не ломает.
        """
        if not packet:
            return None
        kind = packet[0]

        if kind == PKT_BARCODE:
            if len(packet) < BC_LEN:
                return None
            seq, full = struct.unpack(BC_FMT, packet[1:BC_LEN])
            raw = packet[BC_LEN:]
            self.stats_codes += 1
            return Barcode(seq, raw.decode("utf-8", "replace"),
                           truncated=len(raw) < full)

        if kind == PKT_EVENT:
            if len(packet) < EV_LEN:
                return None
            (full,) = struct.unpack(EV_FMT, packet[1:EV_LEN])
            name = packet[EV_LEN:EV_LEN + full].decode("ascii", "replace")
            self.stats_events += 1
            return DeviceEvent(name.strip())

        if kind == PKT_HEADER:
            if len(packet) < HDR_LEN:
                return None
            fid, total, chunk, n, w, h = struct.unpack(HDR_FMT, packet[1:HDR_LEN])
            self._cur = _Partial(fid, total, chunk, n, w, h)
            return None

        if kind == PKT_CHUNK:
            if self._cur is None or len(packet) < 3:
                return None
            (seq,) = struct.unpack("<H", packet[1:3])
            self._cur.parts[seq] = packet[3:]
            return None

        if kind == PKT_END:
            if self._cur is None or len(packet) < END_LEN:
                return None
            fid, crc = struct.unpack(END_FMT, packet[1:END_LEN])
            cur, self._cur = self._cur, None

            if fid != cur.frame_id:
                self.stats_bad += 1
                return Photo(fid, b"", cur.width, cur.height, False, cur.n_chunks)

            missing = [i for i in range(cur.n_chunks) if i not in cur.parts]
            data = b"".join(cur.parts.get(i, b"") for i in range(cur.n_chunks))
            ok = (not missing) and len(data) == cur.total and \
                 (zlib.crc32(data) & 0xFFFFFFFF) == crc

            if ok:
                self.stats_ok += 1
            else:
                self.stats_bad += 1
            return Photo(fid, data, cur.width, cur.height, ok, len(missing))

        return None


# ---------------------------------------------------------------------------
#  Сборка пакетов. Нужна фейковому линку и тестам: так один и тот же разборщик
#  проверяется на байтах, которые он сам же и произвёл.
# ---------------------------------------------------------------------------
def build_header(frame_id: int, total: int, chunk: int, n: int,
                 w: int, h: int) -> bytes:
    return bytes([PKT_HEADER]) + struct.pack(HDR_FMT, frame_id, total, chunk, n, w, h)


def build_chunk(seq: int, payload: bytes) -> bytes:
    return bytes([PKT_CHUNK]) + struct.pack("<H", seq) + payload


def build_end(frame_id: int, data: bytes) -> bytes:
    return bytes([PKT_END]) + struct.pack(END_FMT, frame_id,
                                          zlib.crc32(data) & 0xFFFFFFFF)


def build_barcode(seq: int, text: str, max_payload: int = 500) -> bytes:
    raw = text.encode("utf-8")
    put = raw[:max_payload]
    return bytes([PKT_BARCODE]) + struct.pack(BC_FMT, seq, len(raw)) + put


def split_photo(jpeg: bytes, frame_id: int = 1, chunk: int = 500,
                w: int = 640, h: int = 480) -> list[bytes]:
    """Нарезать JPEG ровно так же, как это делает плата."""
    n = (len(jpeg) + chunk - 1) // chunk
    out = [build_header(frame_id, len(jpeg), chunk, n, w, h)]
    for seq in range(n):
        out.append(build_chunk(seq, jpeg[seq * chunk:(seq + 1) * chunk]))
    out.append(build_end(frame_id, jpeg))
    return out


def build_event(name: str, max_payload: int = 500) -> bytes:
    raw = name.encode("ascii", "replace")
    return bytes([PKT_EVENT]) + struct.pack(EV_FMT, len(raw)) + raw[:max_payload]


def parse_status(raw: bytes) -> dict:
    try:
        return json.loads(raw.decode("utf-8", "ignore"))
    except Exception:  # noqa: BLE001
        return {}
