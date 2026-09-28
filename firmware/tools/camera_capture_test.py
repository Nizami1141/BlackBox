"""Получает JPEG от s3_cameracheck через COM-порт и сохраняет на диск."""

from __future__ import annotations

import argparse
import datetime as dt
from pathlib import Path
import time

import serial


def wait_for_line(port: serial.Serial, prefix: bytes, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline().strip()
        if line:
            print("[ESP]", line.decode(errors="replace"))
        if line.startswith(prefix):
            return line
        if line.startswith(b"CAMERA_FAIL") or line.startswith(b"CAPTURE_FAIL"):
            raise RuntimeError(line.decode(errors="replace"))
    raise TimeoutError(f"Не получен ответ {prefix.decode()} за {timeout:.0f} секунд")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM9", help="COM-порт платы")
    parser.add_argument("--output", default="camera_test_photos", help="Папка снимков")
    args = parser.parse_args()

    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"[1/3] Открываю {args.port}...")
    with serial.Serial(args.port, 115200, timeout=1, write_timeout=3) as port:
        port.dtr = False
        port.rts = False
        time.sleep(0.4)
        port.reset_input_buffer()
        print("[2/3] Запрашиваю настоящий JPEG-кадр...")
        port.write(b"CAPTURE\n")
        port.flush()
        header = wait_for_line(port, b"JPEG_BEGIN ", 12)
        fields = header.split()
        if len(fields) != 4:
            raise RuntimeError(f"Некорректный заголовок: {header!r}")
        length, width, height = map(int, fields[1:])
        image = port.read(length)
        if len(image) != length:
            raise RuntimeError(f"Получено {len(image)} из {length} байт")
        if not image.startswith(b"\xff\xd8") or not image.endswith(b"\xff\xd9"):
            raise RuntimeError("Кадр не имеет корректных границ JPEG")

    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    destination = output_dir / f"ov5640_{stamp}_{width}x{height}.jpg"
    destination.write_bytes(image)
    print(f"[PASS] JPEG корректен: {width}x{height}, {length} байт")
    print(f"[3/3] Фотография сохранена: {destination.resolve()}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"[FAIL] {type(exc).__name__}: {exc}")
        raise SystemExit(1)
