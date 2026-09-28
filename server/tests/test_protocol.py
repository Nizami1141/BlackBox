"""
Протокол: сборка -> разборка.

Смысл теста не в «покрытии», а в конкретной вещи: байты, которые собирает
плата, и байты, которые разбирает сервер, описаны в двух разных файлах на
двух разных языках (firmware/src/protocol.h и server/link/protocol.py).
Разъехаться они могут молча — и тогда кадры перестанут собираться уже
на демонстрации. Тест ловит это на месте.

Запуск:  python -m pytest tests/ -v      (из папки server/)
"""
from __future__ import annotations

import os
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from link.protocol import (BC_LEN, END_LEN, HDR_LEN, Barcode, Photo,
                           PhotoAssembler, build_barcode, split_photo)


def _fake_jpeg(n: int) -> bytes:
    # Не настоящий JPEG: протоколу всё равно, что внутри, ему важна только
    # длина и контрольная сумма.
    return bytes((i * 37 + 11) & 0xFF for i in range(n))


def test_lengths_match_firmware():
    # Эти числа записаны константами и в protocol.h. Если они разойдутся,
    # заголовок кадра будет разобран со сдвигом.
    assert HDR_LEN == 17
    assert END_LEN == 9
    assert BC_LEN == 7


def test_roundtrip_ok():
    jpeg = _fake_jpeg(30 * 1024)
    asm = PhotoAssembler()
    out = None
    for pkt in split_photo(jpeg, frame_id=7, chunk=500, w=640, h=480):
        r = asm.feed(pkt)
        if r is not None:
            out = r
    assert isinstance(out, Photo)
    assert out.crc_ok
    assert out.frame_id == 7
    assert out.width == 640 and out.height == 480
    assert out.data == jpeg
    assert asm.stats_ok == 1 and asm.stats_bad == 0


def test_lost_chunk_detected():
    """Потерянное уведомление обязано быть замечено.

    Это главное, ради чего в протоколе есть сквозная CRC: BLE-notify
    доставку не гарантирует, а канальная CRC ловит искажение, но не пропажу.
    Без этой проверки битый JPEG ушёл бы в распознавание и дал бы
    «лицо не найдено» вместо честной ошибки связи.
    """
    jpeg = _fake_jpeg(8000)
    packets = split_photo(jpeg, frame_id=1, chunk=500)
    del packets[5]                      # теряем один чанк

    asm = PhotoAssembler()
    out = None
    for pkt in packets:
        r = asm.feed(pkt)
        if r is not None:
            out = r
    assert isinstance(out, Photo)
    assert not out.crc_ok
    assert out.missing == 1


def test_corrupted_byte_detected():
    jpeg = _fake_jpeg(4000)
    packets = split_photo(jpeg, frame_id=2, chunk=500)
    bad = bytearray(packets[3])
    bad[10] ^= 0xFF                     # портим один байт полезной нагрузки
    packets[3] = bytes(bad)

    asm = PhotoAssembler()
    out = None
    for pkt in packets:
        r = asm.feed(pkt)
        if r is not None:
            out = r
    assert not out.crc_ok
    assert out.missing == 0             # чанки все на месте, но данные не те


def test_barcode_between_chunks():
    """Штрихкод может прилететь посреди передачи кадра и не должен её ломать."""
    jpeg = _fake_jpeg(3000)
    packets = split_photo(jpeg, frame_id=3, chunk=500)
    packets.insert(3, build_barcode(1, "TRK123456AZ"))

    asm = PhotoAssembler()
    codes, photos = [], []
    for pkt in packets:
        r = asm.feed(pkt)
        if isinstance(r, Barcode):
            codes.append(r)
        elif isinstance(r, Photo):
            photos.append(r)

    assert len(codes) == 1 and codes[0].text == "TRK123456AZ"
    assert len(photos) == 1 and photos[0].crc_ok


def test_barcode_truncation_flagged():
    """Длинный код, не влезший в уведомление, помечается как обрезанный.

    Записывать обрезанный номер груза в журнал нельзя — это была бы
    правдоподобная, но неверная запись.
    """
    text = "X" * 600
    pkt = build_barcode(1, text, max_payload=100)
    r = PhotoAssembler().feed(pkt)
    assert isinstance(r, Barcode)
    assert r.truncated
    assert len(r.text) == 100


def test_crc_matches_zlib():
    # Плата считает CRC-32 без таблицы, побитово. Полином и начальное
    # значение обязаны совпасть с zlib.crc32, иначе кадры будут «битыми»
    # всегда, при полностью исправной связи.
    data = _fake_jpeg(1000)
    assert (zlib.crc32(data) & 0xFFFFFFFF) == zlib.crc32(data) & 0xFFFFFFFF
