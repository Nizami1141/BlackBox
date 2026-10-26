"""Отправить команду s3_protocol_loopback через COM-порт без Serial Monitor."""

from __future__ import annotations

import argparse
import sys
import time

import serial


COMMANDS = ("CLEAR", "CAM", "PINS", "UART", "SPI", "I2C", "GPIO", "STATUS")


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(
        description="Команда проверки интерфейсов ESP32-S3 через COM-порт"
    )
    parser.add_argument("command", type=str.upper, choices=COMMANDS)
    parser.add_argument("--port", default="COM9", help="COM-порт платы (по умолчанию COM9)")
    args = parser.parse_args()

    port = serial.Serial()
    port.port = args.port
    port.baudrate = 115200
    port.timeout = 0.5
    port.write_timeout = 3
    port.dtr = False
    port.rts = False

    print(f"Открываю {args.port} и отправляю {args.command}...")
    try:
        with port:
            # У CH343 открытие порта иногда перезапускает ESP32-S3.
            # Дождёмся конца setup(), затем уберём старые строки из буфера.
            time.sleep(2.0)
            port.reset_input_buffer()
            port.write((args.command + "\n").encode("ascii"))
            port.flush()

            deadline = time.monotonic() + (25 if args.command == "CAM" else 12)
            printed = False
            while time.monotonic() < deadline:
                line = port.readline()
                if not line:
                    continue
                printed = True
                message = line.decode("utf-8", errors="replace").rstrip()
                print(message)
                if message == "-----------------------------------------------":
                    return 0
            if not printed:
                print("[FAIL] Нет ответа. Проверьте прошивку, COM-порт и питание платы.")
                return 2
            print("[WARN] Ответ получен, но итоговая сводка не завершилась до таймаута.")
            return 1
    except serial.SerialException as exc:
        print(f"[FAIL] COM-порт недоступен: {exc}")
        print("Закройте Serial Monitor (Ctrl+C) и другие программы, использующие этот порт.")
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
