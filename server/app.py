"""
Точка входа. Один процесс поднимает всё:

    сайт  +  API  +  мост до платы (BLE или WebSocket)

Раньше это были две программы: сервер отдельно и BLE-клиент на PyQt отдельно.
Так делать нельзя по простой причине: Bluetooth-адаптер Windows отдаёт
устройство только одному процессу. Пока открыт старый GUI из
esp32cam-ble-demo, сервер плату не увидит — и наоборот.

    python app.py              обычный запуск
    python app.py --open       и сразу открыть браузер
    python app.py --check      проверить конфигурацию и выйти
"""
from __future__ import annotations

import argparse
import logging
import sys
import threading
import webbrowser
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from core.config import Config, get_config      # noqa: E402
from core.face_engine import FaceError          # noqa: E402
from core.flow import CheckpointFlow            # noqa: E402
from core.service import CheckpointService      # noqa: E402
from link import build_registry                 # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--open", action="store_true",
                    help="открыть сайт в браузере после запуска")
    ap.add_argument("--check", action="store_true",
                    help="проверить конфигурацию и модели, ничего не поднимая")
    ap.add_argument("--config", default=None, help="путь к config.yaml")
    args = ap.parse_args()

    cfg = Config.load(args.config) if args.config else get_config()

    logging.basicConfig(
        level=getattr(logging, cfg.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )
    log = logging.getLogger("app")

    # --- модель и база -----------------------------------------------------
    try:
        service = CheckpointService(cfg)
    except FaceError as e:
        print(f"\n[ОШИБКА] {e.message}\n", file=sys.stderr)
        if e.code in ("MODEL_MISSING", "BACKEND_MISSING"):
            print("Подсказка: python -m tools.download_models\n", file=sys.stderr)
        return 2
    except ValueError as e:
        # Почти всегда это векторы разной длины в базе после смены
        # face.backend. Раньше сервер просто падал с непонятным traceback,
        # и выглядело это как «перезагрузил — и больше ничего не работает».
        print(f"\n[ОШИБКА] База не загрузилась: {e}\n", file=sys.stderr)
        print("Разбор и починка:  python -m tools.db_doctor\n", file=sys.stderr)
        return 3

    # Ключи устройств уезжают в заголовке HTTP, а заголовки переносят
    # только ASCII. Кириллица в api_key даст не отказ в доступе, а
    # невозможность отправить запрос вообще — это надо поймать сразу.
    for d in cfg.devices:
        try:
            d.api_key.encode("ascii")
            d.device_id.encode("ascii")
        except UnicodeEncodeError:
            log.error("У устройства %r в device_id или api_key есть символы вне "
                      "ASCII. Заголовки HTTP такое не переносят — пост не сможет "
                      "отправить запрос. Используйте латиницу, цифры и дефис.",
                      d.device_id)

    # --- транспорты до постов ---------------------------------------------
    registry = build_registry(cfg.devices, Path(__file__).resolve().parent)
    if not registry.all():
        log.warning("Ни одного управляемого поста в config.yaml. Кнопка "
                    "«Проверить» работать не будет. Добавьте устройству "
                    "секцию link: {type: ble} или {type: fake}.")

    flow = CheckpointFlow(
        service, registry,
        photo_timeout=cfg.photo_timeout,
        scan_timeout=cfg.scan_timeout,
        cooldown=cfg.flow_cooldown,
        min_barcode_len=cfg.min_barcode_len,
        lcd=cfg.lcd,
    )

    if args.check:
        # Посты бывают двух сортов, и показать надо оба. Управляемые — те,
        # у кого есть линк: сервер может сказать им «снимай». Автономные
        # (link.type: none) линка не имеют по замыслу — они приходят сами
        # по HTTP. Раньше здесь печатались только управляемые, и пост на
        # Ethernet выглядел как потерянный, хотя он в порядке.
        managed = {l.device_id for l in registry.all()}
        autonomous = [d.device_id for d in cfg.devices if d.device_id not in managed]

        print("Конфигурация в порядке.")
        print(f"  бэкенд лиц : {cfg.backend} (dim={service.engine.dim})")
        print(f"  порог      : {cfg.threshold}")
        print(f"  сотрудников: {service.index.size}")
        print(f"  управляемые: "
              f"{', '.join(f'{l.device_id}[{l.kind}]' for l in registry.all()) or 'нет'}")
        print(f"  автономные : "
              f"{', '.join(f'{d}[http]' for d in autonomous) or 'нет'}"
              f"{'   (забирают команду с сайта по Ethernet или ждут физическую кнопку)' if autonomous else ''}")
        if cfg.aks_api_key and cfg.aks_ip_devices:
            routes = ", ".join(f"{ip}→{did}" for ip, did in cfg.aks_ip_devices.items())
            print(f"  ESP32 API  : http://{cfg.aks_server_ip}:{cfg.port}/api/recognize")
            print(f"  IP whitelist: {routes}")
            unknown = sorted(set(cfg.aks_ip_devices.values()) -
                             {d.device_id for d in cfg.devices})
            if unknown:
                print(f"  ВНИМАНИЕ   : неизвестные device_id в aks_api: {', '.join(unknown)}")
        else:
            print("  ESP32 API  : НЕ НАСТРОЕН (секция aks_api)")
        if cfg.central_aks_enabled:
            print(f"  распознавание: ЦЕНТРАЛЬНЫЙ AKS {cfg.central_aks_base_url}")
            print("  liveness   : решение центрального AKS; локально не проверяется")
        else:
            print("  распознавание: локальная база ноутбука")
            print(f"  liveness   : {'ВКЛ' if cfg.liveness_enabled else 'выключен (см. docs)'}")
        return 0

    from api.server import create_app     # noqa: E402  (импорт после проверок)
    import uvicorn                        # noqa: E402

    app = create_app(service, flow, registry, cfg)

    url = f"http://127.0.0.1:{cfg.port}/"
    log.info("Сайт: %s", url)
    log.info("API:  %s api/docs", url)
    if args.open:
        threading.Timer(1.5, lambda: webbrowser.open(url)).start()

    uvicorn.run(app, host=cfg.host, port=cfg.port,
                log_level=cfg.log_level.lower(), access_log=False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
