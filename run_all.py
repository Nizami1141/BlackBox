"""Start the checkpoint API (8000) and Device Registry (8001) together."""
from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parent


def registry_key() -> str:
    explicit = os.getenv("DEVICE_API_KEY")
    if explicit:
        return explicit
    config = yaml.safe_load((ROOT / "server" / "config.yaml").read_text(encoding="utf-8"))
    key = config.get("aks_api", {}).get("api_key", "")
    if key:
        return key
    raise RuntimeError("aks_api.api_key missing from server/config.yaml")


def main() -> int:
    environment = os.environ.copy()
    environment["DEVICE_API_KEY"] = registry_key()
    registry = subprocess.Popen(
        [sys.executable, "-m", "uvicorn", "server:app", "--host", "0.0.0.0",
         "--port", "8001"],
        cwd=ROOT / "registry", env=environment,
    )
    checkpoint = subprocess.Popen(
        [sys.executable, "app.py"],
        cwd=ROOT / "server", env=environment,
    )
    try:
        while True:
            if registry.poll() is not None:
                return registry.returncode or 1
            if checkpoint.poll() is not None:
                return checkpoint.returncode or 1
            time.sleep(0.25)
    except KeyboardInterrupt:
        return 0
    finally:
        for process in (checkpoint, registry):
            if process.poll() is None:
                process.terminate()
        for process in (checkpoint, registry):
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    raise SystemExit(main())
