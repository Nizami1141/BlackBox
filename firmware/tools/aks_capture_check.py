"""Export the exact JPEG sent by the production firmware to AKS via COM.

Run on the laptop physically connected to ESP32. The ESP still sends the
recognition request directly to AKS. This script starts no local API server.
"""
from __future__ import annotations

import argparse
from datetime import datetime
from pathlib import Path
import time

import serial


def read_exact(port, length: int, timeout: float = 90) -> bytes:
    deadline = time.monotonic() + timeout
    image = bytearray()
    while len(image) < length and time.monotonic() < deadline:
        chunk = port.read(min(4096, length - len(image)))
        image.extend(chunk)
    if len(image) != length:
        raise RuntimeError(f"Incomplete JPEG: {len(image)} of {length} bytes")
    return bytes(image)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="ESP32 COM port, e.g. COM9")
    parser.add_argument("--output", type=Path, default=Path("aks_debug_photos"))
    parser.add_argument("--tls-only", action="store_true",
                        help="Test five TLS/read-only GET round trips without a photo or API key")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    logs = []
    photo = None
    print("Close Serial Monitor first." +
          (" Testing TLS with read-only GET; no photo or API key." if args.tls_only else
           " Stand in front of the camera."))
    # Set modem lines before opening to avoid an unintended ESP32 reset.
    with serial.Serial(port=None, baudrate=115200, timeout=1, write_timeout=5) as port:
        port.dtr = False
        port.rts = False
        port.port = args.port
        port.open()
        time.sleep(0.4)
        port.reset_input_buffer()
        port.write(b"DEBUG_TLS\n" if args.tls_only else b"DEBUG_CHECK\n")
        port.flush()
        deadline = time.monotonic() + (240 if args.tls_only else 120)
        try:
            while time.monotonic() < deadline:
                line = port.readline().strip()
                if not line:
                    continue
                text = line.decode("utf-8", errors="replace")
                print(text)
                logs.append(text)
                if args.tls_only and "[cp] TLS_END " in text:
                    return 0 if "passed=5 total=5" in text else 1
                if line.startswith(b"JPEG_BEGIN "):
                    fields = line.split()
                    if len(fields) != 4:
                        raise RuntimeError(f"Invalid JPEG header: {text}")
                    length, width, height = map(int, fields[1:])
                    if not 0 < length <= 4 * 1024 * 1024:
                        raise RuntimeError(f"Invalid JPEG size: {length}")
                    image = read_exact(port, length)
                    if not image.startswith(b"\xff\xd8") or not image.endswith(b"\xff\xd9"):
                        raise RuntimeError("Image has invalid JPEG boundaries")
                    photo = args.output / f"aks_{stamp}_{width}x{height}.jpg"
                    photo.write_bytes(image)
                    print(f"Saved exact AKS frame: {photo.resolve()}")
                if ("[eth] recognize:" in text or "[cp] проверка не завершена" in text):
                    if photo is None:
                        raise RuntimeError("No JPEG was exported; update the ESP32 firmware")
                    return 0
                if "[cp] не удалось получить кадр" in text:
                    raise RuntimeError("Camera failed to capture a frame")
            raise TimeoutError("No recognition result; check firmware, COM port and network")
        finally:
            destination = args.output / f"aks_{stamp}.txt"
            destination.write_text("\n".join(logs) + "\n", encoding="utf-8")
            print(f"Saved diagnostic log: {destination.resolve()}")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, TimeoutError, serial.SerialException) as exc:
        print(f"[FAIL] {exc}")
        raise SystemExit(1)
