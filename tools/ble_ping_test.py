"""Проверка BLE GATT прошивки s3_commscheck с ноутбука."""

from __future__ import annotations

import asyncio
import argparse
import sys

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError


DEVICE_NAME = "S3-COMM-TEST"
SERVICE_UUID = "7cf0a100-7f6b-4da8-a5f7-1c3db7400001"
RX_UUID = "7cf0a100-7f6b-4da8-a5f7-1c3db7400002"
TX_UUID = "7cf0a100-7f6b-4da8-a5f7-1c3db7400003"


async def main(use_notify: bool) -> int:
    print(f"[1/4] Ищу BLE-устройство {DEVICE_NAME!r} (до 15 секунд)...")
    device = await BleakScanner.find_device_by_filter(
        lambda dev, adv: (adv.local_name or dev.name or "") == DEVICE_NAME,
        timeout=15.0,
    )
    if device is None:
        print("[FAIL] Плата не найдена. Проверьте Bluetooth и лог ADVERTISING.")
        return 2

    print(f"[PASS] Найдена плата: {device.name} / {device.address}")
    notification = asyncio.Event()
    received = bytearray()

    def on_notify(_sender: object, data: bytearray) -> None:
        received[:] = data
        print(f"[PASS] Notification: {bytes(data).decode(errors='replace')!r}")
        notification.set()

    print("[2/4] Подключаюсь...")
    client_options = {"timeout": 15.0}
    if sys.platform == "win32":
        # После смены тестовой прошивки Windows способен вернуть старые
        # handles из GATT-кэша и тут же закрыть реальное соединение.
        client_options["winrt"] = {"use_cached_services": False}

    async with BleakClient(device, **client_options) as client:
        if not client.is_connected:
            print("[FAIL] BLE-соединение не установлено.")
            return 3
        print("[PASS] BLE-соединение установлено")

        services = client.services
        if services.get_service(SERVICE_UUID) is None:
            print("[FAIL] Тестовый GATT service не найден.")
            return 4
        print("[PASS] GATT service и характеристики обнаружены")

        print("[3/4] Отправляю PING...")
        notify_started = False
        if use_notify:
            try:
                await client.start_notify(TX_UUID, on_notify)
                notify_started = True
                print("[PASS] Подписка на TX notification включена")
            except BleakError as exc:
                print(f"[FAIL] Notification недоступен: {exc}")
                print("       Повторите без --notify для надёжного GATT write/read.")
                return 5
        elif sys.platform == "win32":
            print("[SKIP] Notification пропущен: Windows/WinRT возвращает Unreachable")
            print("       Проверяю двусторонний GATT write/read без повреждения сессии.")

        await client.write_gatt_char(RX_UUID, b"PING", response=True)
        print("[PASS] Write RX: 'PING'")

        if notify_started:
            try:
                await asyncio.wait_for(notification.wait(), timeout=10.0)
            except TimeoutError:
                print("[WARN] Notification не пришёл за 10 секунд; проверяю TX чтением.")

        print("[4/4] Читаю TX напрямую...")
        direct = b""
        for _ in range(10):
            direct = bytes(await client.read_gatt_char(TX_UUID))
            if direct.startswith(b"PONG"):
                break
            await asyncio.sleep(0.2)
        text = direct.decode(errors="replace")
        print(f"[PASS] Read TX: {text!r}")
        if notify_started:
            await client.stop_notify(TX_UUID)

    if not direct.startswith(b"PONG"):
        print("[FAIL] Получен ответ, но это не PONG.")
        return 6

    if notify_started and bytes(received).startswith(b"PONG"):
        print("\nИТОГ: BLE advertising, connect, GATT write/read и notify — PASS")
    else:
        print("\nИТОГ: BLE advertising, connect и двусторонний GATT write/read — PASS")
        print("      Notification отдельно не подтверждён из-за Windows/CCCD.")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--notify",
        action="store_true",
        help="дополнительно проверить notification (на Windows может дать Unreachable)",
    )
    arguments = parser.parse_args()
    try:
        raise SystemExit(asyncio.run(main(arguments.notify)))
    except KeyboardInterrupt:
        print("\nПроверка остановлена пользователем.")
        raise SystemExit(130)
    except Exception as exc:  # показываем понятную ошибку вместо длинного traceback
        print(f"[FAIL] {type(exc).__name__}: {exc}")
        raise SystemExit(1)
