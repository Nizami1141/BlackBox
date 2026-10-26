"""PC endpoint for the ESP32-S3 all-hardware Ethernet PING/PONG check."""

from __future__ import annotations

import argparse
import socket


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="172.16.6.51", help="Ethernet IP of this laptop")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.host, args.port))
        server.listen(2)
        print(f"Waiting for ESP32 on {args.host}:{args.port} (Ctrl+C to stop)", flush=True)
        while True:
            connection, address = server.accept()
            with connection:
                connection.settimeout(3)
                request = connection.recv(32).strip()
                print(f"{address[0]}:{address[1]} -> {request!r}", flush=True)
                if request == b"PING":
                    connection.sendall(b"PONG\n")
                    print("[PASS] Ethernet PING/PONG", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("Stopped")
