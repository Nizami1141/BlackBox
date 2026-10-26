"""Транспортный слой: как сервер разговаривает с постами.

Сценарий проверки (core/flow.py) работает через DeviceLink и не знает,
что под ним — Bluetooth, Ethernet или заглушка. Смена транспорта на посту
не должна трогать ни бизнес-логику, ни сайт.
"""
from __future__ import annotations

import logging
from pathlib import Path

from .base import DeviceLink, LinkError, LinkRegistry
from .ble_link import BleLink
from .fake_link import FakeLink
from .protocol import Barcode, DeviceEvent, Photo
from .ws_link import WsLink

log = logging.getLogger(__name__)

__all__ = [
    "DeviceLink", "LinkError", "LinkRegistry", "BleLink", "FakeLink",
    "WsLink", "Barcode", "DeviceEvent", "Photo", "build_registry",
]


def build_link(device_id: str, spec: dict, base_dir: Path) -> DeviceLink | None:
    """Создать линк по секции `link:` из config.yaml."""
    kind = str(spec.get("type", "none")).lower()

    if kind == "ble":
        return BleLink(
            device_id,
            ble_name=spec.get("ble_name", "ESP32S3-CAM"),
            scan_timeout=float(spec.get("scan_timeout", 8.0)),
            retry_delay=float(spec.get("retry_delay", 3.0)),
        )
    if kind == "ws":
        # Линк создаётся заранее и ждёт, пока плата сама постучится
        # в /ws/device/<device_id>.
        return WsLink(device_id)
    if kind == "fake":
        d = Path(spec.get("faces_dir", "data/fake_faces"))
        return FakeLink(
            device_id,
            faces_dir=d if d.is_absolute() else base_dir / d,
            scan_delay=float(spec.get("scan_delay", 2.0)),
            barcode=str(spec.get("barcode", "")),
            ack_artifact=str(spec.get("ack_artifact", "")),
        )
    if kind in ("none", "", "http"):
        # "http" = старый режим, когда плата сама шлёт POST /api/v1/verify.
        # Сервером управлять таким постом нельзя, поэтому линка у него нет,
        # но устройство остаётся в базе и его запросы принимаются.
        return None

    log.warning("Неизвестный тип линка %r у устройства %s — пропускаю",
                kind, device_id)
    return None


def build_registry(devices: list, base_dir: Path) -> LinkRegistry:
    reg = LinkRegistry()
    for d in devices:
        link = build_link(d.device_id, d.link or {}, base_dir)
        if link is not None:
            reg.register(link)
    return reg
