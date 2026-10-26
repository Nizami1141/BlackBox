"""Загрузка конфигурации из config.yaml."""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

# Корень server/ — на уровень выше папки core/
BASE_DIR = Path(__file__).resolve().parent.parent


def _abs(p: str | os.PathLike) -> Path:
    p = Path(p)
    return p if p.is_absolute() else (BASE_DIR / p)


@dataclass
class DeviceCfg:
    device_id: str
    api_key: str
    location: str = ""
    # Как сервер дотягивается до этого поста: {"type": "ble"|"ws"|"fake"|"none", ...}
    # Разбирается в link/__init__.py:build_link().
    link: dict[str, Any] = field(default_factory=dict)


@dataclass
class Config:
    raw: dict[str, Any] = field(default_factory=dict)

    # --- server ---
    host: str = "0.0.0.0"
    port: int = 8000

    # --- storage ---
    data_dir: Path = BASE_DIR / "data"
    db_file: Path = BASE_DIR / "data/checkpoint.db"
    keep_probe_images: bool = False
    probe_dir: Path = BASE_DIR / "data/probes"
    keep_enroll_thumbnail: bool = True

    # --- face ---
    backend: str = "opencv"
    models_dir: Path = BASE_DIR / "models"
    threshold: float = 0.42
    det_score_min: float = 0.75
    min_face_px: int = 80
    allow_multiple_faces: bool = False
    # Размер, до которого детектор масштабирует кадр. 320 вместо 640
    # ускоряет детекцию примерно вчетверо; на КПП лицо крупное, хватает.
    det_size: int = 320
    # До скольких пикселей по большей стороне ужимать кадр ПЕРЕД моделью.
    # Веб-камера отдаёт 1280x960, гонять это через сеть незачем.
    max_side: int = 800
    # Ограничение потоков onnxruntime. 0 = не трогать.
    threads: int = 0

    # --- liveness ---
    liveness_enabled: bool = False
    liveness_mode: str = "passive_stub"

    # --- session ---
    session_ttl: int = 90
    session_max_scans: int = 5

    # --- flow: тайминги одного прохода проверки ---
    photo_timeout: float = 15.0        # сколько ждём кадр от платы
    scan_timeout: float = 45.0         # сколько ждём, что поднесут штрихкод
    flow_cooldown: float = 2.0         # пауза между проверками на одном посту
    # Короче этого штрихкодом не считаем: обрывки служебных кадров сканера
    # выглядят как код из одного символа. См. core/flow.py.
    min_barcode_len: int = 3

    # --- тексты для экрана поста (азербайджанский) ---
    lcd: dict[str, Any] = field(default_factory=dict)

    # --- web ---
    web_dir: Path = BASE_DIR / "web"
    # Кто имеет право открывать сайт и дёргать /api/web/*.
    # Пусто = только с этого же компьютера (по умолчанию и правильно:
    # сайт — пульт оператора, а не публичная страница).
    web_allow_lan: bool = False

    # --- devices ---
    devices: list[DeviceCfg] = field(default_factory=list)

    # --- AKS API compatibility (/api/recognize, /api/log_cargo) ---
    # Contract used by the Ethernet firmware and described in esp32.html.
    aks_api_key: str = ""
    aks_server_ip: str = ""
    aks_ip_devices: dict[str, str] = field(default_factory=dict)
    aks_allow_local_test: bool = True

    # --- real central AKS (the laptop forwards ESP32 requests) ---
    central_aks_enabled: bool = False
    central_aks_base_url: str = ""
    central_aks_timeout_seconds: float = 30.0

    log_level: str = "INFO"

    @classmethod
    def load(cls, path: str | os.PathLike | None = None) -> "Config":
        path = Path(path) if path else (BASE_DIR / "config.yaml")
        with open(path, "r", encoding="utf-8") as f:
            raw = yaml.safe_load(f) or {}

        srv = raw.get("server", {})
        st = raw.get("storage", {})
        fc = raw.get("face", {})
        lv = raw.get("liveness", {})
        se = raw.get("session", {})
        fl = raw.get("flow", {})
        wb = raw.get("web", {})
        aks = raw.get("aks_api", {})
        central = raw.get("central_aks", {})
        lg = raw.get("logging", {})

        cfg = cls(
            raw=raw,
            host=srv.get("host", "0.0.0.0"),
            port=int(srv.get("port", 8000)),
            data_dir=_abs(st.get("data_dir", "data")),
            db_file=_abs(st.get("db_file", "data/checkpoint.db")),
            keep_probe_images=bool(st.get("keep_probe_images", False)),
            probe_dir=_abs(st.get("probe_dir", "data/probes")),
            keep_enroll_thumbnail=bool(st.get("keep_enroll_thumbnail", True)),
            backend=fc.get("backend", "opencv"),
            models_dir=_abs(fc.get("models_dir", "models")),
            threshold=float(fc.get("threshold", 0.42)),
            det_score_min=float(fc.get("det_score_min", 0.75)),
            min_face_px=int(fc.get("min_face_px", 80)),
            allow_multiple_faces=bool(fc.get("allow_multiple_faces", False)),
            det_size=int(fc.get("det_size", 320)),
            max_side=int(fc.get("max_side", 800)),
            threads=int(fc.get("threads", 0)),
            liveness_enabled=bool(lv.get("enabled", False)),
            liveness_mode=lv.get("mode", "passive_stub"),
            session_ttl=int(se.get("ttl_seconds", 90)),
            session_max_scans=int(se.get("max_scans", 5)),
            photo_timeout=float(fl.get("photo_timeout", 15.0)),
            scan_timeout=float(fl.get("scan_timeout", 45.0)),
            flow_cooldown=float(fl.get("cooldown", 2.0)),
            min_barcode_len=int(fl.get("min_barcode_len", 3)),
            lcd=raw.get("lcd", {}) or {},
            web_dir=_abs(wb.get("dir", "web")),
            web_allow_lan=bool(wb.get("allow_lan", False)),
            devices=[DeviceCfg(**d) for d in raw.get("devices", [])],
            aks_api_key=str(aks.get("api_key", "")),
            aks_server_ip=str(aks.get("server_ip", "")),
            aks_ip_devices={str(ip): str(device_id)
                            for ip, device_id in (aks.get("ip_devices", {}) or {}).items()},
            aks_allow_local_test=bool(aks.get("allow_local_test", True)),
            central_aks_enabled=bool(central.get("enabled", False)),
            central_aks_base_url=str(central.get("base_url", "")).rstrip("/"),
            central_aks_timeout_seconds=float(central.get("timeout_seconds", 30.0)),
            log_level=lg.get("level", "INFO"),
        )

        cfg.data_dir.mkdir(parents=True, exist_ok=True)
        cfg.models_dir.mkdir(parents=True, exist_ok=True)
        if cfg.keep_probe_images:
            cfg.probe_dir.mkdir(parents=True, exist_ok=True)
        return cfg


_cfg: Config | None = None


def get_config() -> Config:
    global _cfg
    if _cfg is None:
        _cfg = Config.load()
    return _cfg
