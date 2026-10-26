"""Run the dedicated ESP32-S3/GM67 check through CH343 without Serial Monitor."""

from __future__ import annotations

import argparse
import sys
import time

import serial


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="Check GM67 on ESP32-S3 GPIO1/GPIO2")
    parser.add_argument("command", choices=("TEST", "STATUS"), type=str.upper)
    parser.add_argument("--port", default="COM9", help="ESP32-S3 COM port (default COM9)")
    args = parser.parse_args()

    port = serial.Serial()
    port.port = args.port
    port.baudrate = 115200
    port.timeout = 0.5
    port.write_timeout = 3
    port.dtr = False
    port.rts = False

    try:
        with port:
            time.sleep(2.0)  # CH343 can reset the ESP32-S3 when the port opens.
            port.reset_input_buffer()
            port.write((args.command + "\n").encode("ascii"))
            port.flush()
            deadline = time.monotonic() + (43 if args.command == "TEST" else 7)
            printed = False
            result = None
            while time.monotonic() < deadline:
                line = port.readline()
                if not line:
                    continue
                printed = True
                message = line.decode("utf-8", errors="replace").rstrip()
                print(message)
                if message.startswith("RESULT: "):
                    result = message
                if args.command == "TEST" and message == "GM67_TEST_DONE":
                    return 0 if result == "RESULT: PASS — command response and barcode received" else 1
                if args.command == "STATUS" and message.startswith("[INFO] UART loopback"):
                    return 0
            if not printed:
                print("[FAIL] No response from the board. Check firmware, COM port and power.")
                return 2
            print("[WARN] Board responded, but test did not finish before timeout.")
            return 1
    except serial.SerialException as exc:
        print(f"[FAIL] Cannot open {args.port}: {exc}")
        print("Close Serial Monitor and any other program using this COM port.")
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
