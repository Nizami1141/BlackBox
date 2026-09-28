"""
Проверка серверной части без платы.

Скрипт притворяется постом: делает те же два HTTP-запроса, с теми же
заголовками и тем же ключом устройства, что и прошивка в Ethernet-режиме.
Разница только в том, откуда берётся картинка — из файла, а не с OV5640.

Зачем это нужно. Когда пост отвечает NO_FACE, виноватых двое: сервер и
кадр. Различить их можно только одним способом — подсунуть серверу
заведомо хороший снимок. Опознал — сервер исправен, и дальше надо
разбираться с камерой. Не опознал — дело в базе, пороге или модели,
и камера ни при чём.

    python -m tools.check_post face.jpg
    python -m tools.check_post face.jpg --barcode TRK4417AZ
    python -m tools.check_post --pull frame.jpg      забрать кадр от платы
    python -m tools.check_post --health

Зависимостей сверх стандартной библиотеки нет: requests в проекте не
используется, а urllib для двух запросов более чем достаточно.
"""
from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from core.config import get_config      # noqa: E402


# --------------------------------------------------------------------- вывод
def head(title: str) -> None:
    print(f"\n=== {title} ===")


def kv(name: str, value) -> None:
    print(f"  {name:<16} {value}")


def fail(msg: str, hint: str = "") -> None:
    print(f"\n[ОШИБКА] {msg}")
    if hint:
        print(f"          {hint}")


# ------------------------------------------------------------------ запросы
def request(url: str, *, data: bytes = None, headers: dict = None,
            method: str = None) -> tuple[int, bytes]:
    """Вернуть (код, тело). Ошибки HTTP не бросают исключений: код 403 —
    это такой же ответ сервера, как и 200, и разбирать его надо так же."""
    req = urllib.request.Request(url, data=data, headers=headers or {},
                                 method=method)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except urllib.error.URLError as e:
        raise SystemExit(
            f"\n[ОШИБКА] Сервер не отвечает: {e.reason}\n"
            f"          Проверьте, что запущен app.py и что адрес верный."
        )


def as_json(body: bytes) -> dict:
    try:
        return json.loads(body.decode("utf-8"))
    except Exception:
        return {"_raw": body[:200].decode("utf-8", "replace")}


# --------------------------------------------------------------------- шаги
def do_health(base: str) -> None:
    head("Здоровье сервера")
    code, body = request(f"{base}/api/v1/health")
    d = as_json(body)
    if code != 200:
        fail(f"HTTP {code}", str(d))
        return
    kv("бэкенд", d.get("backend"))
    kv("размерность", d.get("dim"))
    kv("порог", d.get("threshold"))
    kv("сотрудников", d.get("employees"))
    stale = d.get("stale_employees") or 0
    if stale:
        kv("НЕПРИГОДНЫХ", f"{stale} — векторы другой размерности, "
                          f"исключены из поиска (python -m tools.db_doctor)")
    if not d.get("employees"):
        print("\n  База пуста. Любой кадр получит отказ, и это не поломка —")
        print("  сначала заведите сотрудника на странице /enroll.")


def do_verify(base: str, hdr: dict, jpeg: bytes) -> dict:
    head("Опознание — POST /api/v1/verify")
    kv("размер кадра", f"{len(jpeg)} байт")

    code, body = request(f"{base}/api/v1/verify", data=jpeg,
                         headers={**hdr, "Content-Type": "image/jpeg"})
    d = as_json(body)
    kv("HTTP", code)

    if code == 401:
        fail("Сервер не увидел заголовков устройства.",
             "Так отвечают, когда X-Device-Id или X-Device-Key пустые.")
        return {}
    if code == 403:
        fail("Ключ устройства не подошёл.",
             "device_id и api_key должны совпадать с секцией devices "
             "в config.yaml.")
        return {}
    if code != 200:
        fail(f"Неожиданный ответ HTTP {code}", str(d))
        return {}

    kv("matched", d.get("matched"))
    kv("reason", d.get("reason") or "—")
    kv("score", d.get("score"))
    kv("время модели", f"{d.get('latency_ms')} мс")
    kv("на экране", f"{d.get('lcd1')} | {d.get('lcd2')}")

    if d.get("matched"):
        kv("сотрудник", f"{d.get('name')} (таб. № {d.get('employee_no')})")
        kv("токен живёт", f"{d.get('expires_in')} с")
    else:
        why = {
            "NO_FACE": "детектор не нашёл лица В КАРТИНКЕ. Дело в кадре, "
                       "а не в базе: тёмный, размытый, или лицо мелкое.",
            "NOT_FOUND": "лицо найдено, но такого человека в базе нет. "
                         "Пайплайн работает — не хватает регистрации.",
            "MULTIPLE_FACES": "в кадре больше одного лица, и пост такой "
                              "кадр отвергает намеренно.",
            "INDEX_MISMATCH": "в базе векторы другой размерности. "
                              "Запустите python -m tools.db_doctor",
            "SPOOF_SUSPECTED": "сработала проверка живости.",
        }.get(d.get("reason"), "")
        if why:
            print(f"\n  {why}")
    return d


def do_scan(base: str, hdr: dict, token: str, barcode: str) -> None:
    head("Штрихкод — POST /api/v1/scan")
    payload = json.dumps({"session_token": token, "barcode": barcode,
                          "symbology": ""}).encode("utf-8")
    code, body = request(f"{base}/api/v1/scan", data=payload,
                         headers={**hdr, "Content-Type": "application/json"})
    d = as_json(body)
    kv("HTTP", code)
    kv("ok", d.get("ok"))
    if d.get("ok"):
        kv("запись №", d.get("record_id"))
        kv("на экране", f"{d.get('lcd1')} | {d.get('lcd2')}")
        print("\n  Строка записана в журнал. Проверьте /journal.")
    else:
        fail(f"Не записано: {d.get('reason')}",
             "Токен одноразовый и привязан к посту: повторный скан тем же "
             "токеном сервер отвергнет, и это правильно.")


def do_pull(base: str, device: str, dest: Path) -> None:
    head(f"Последний кадр от поста {device}")
    code, body = request(f"{base}/api/web/last-frame/{device}.jpg")
    if code == 404:
        fail("Кадров от этого поста ещё не приходило.",
             "Нажмите кнопку на плате и повторите.")
        return
    if code == 403:
        fail("Доступ только с самого сервера.",
             "Запускайте скрипт на том же компьютере, где работает app.py.")
        return
    if code != 200:
        fail(f"HTTP {code}", body[:200].decode("utf-8", "replace"))
        return
    dest.write_bytes(body)
    kv("сохранено", f"{dest} ({len(body)} байт)")
    print("\n  Откройте файл и посмотрите, что на самом деле снимает камера.")
    print("  Тёмный, размытый или пустой кадр объясняет NO_FACE без гаданий.")


# --------------------------------------------------------------------- main
def main() -> int:
    cfg = get_config()
    dev = cfg.devices[0] if cfg.devices else None

    ap = argparse.ArgumentParser(
        description="Притвориться постом и проверить серверную часть.")
    ap.add_argument("image", nargs="?", help="JPEG с лицом")
    ap.add_argument("--barcode", help="после опознания отправить этот штрихкод")
    ap.add_argument("--device", default=dev.device_id if dev else "",
                    help="device_id (по умолчанию первый из config.yaml)")
    ap.add_argument("--key", default=dev.api_key if dev else "",
                    help="api_key (по умолчанию из config.yaml)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=cfg.port)
    ap.add_argument("--pull", metavar="FILE",
                    help="забрать последний кадр от поста и сохранить в файл")
    ap.add_argument("--health", action="store_true",
                    help="только опросить здоровье сервера")
    args = ap.parse_args()

    base = f"http://{args.host}:{args.port}"
    hdr = {"X-Device-Id": args.device, "X-Device-Key": args.key}

    print(f"Сервер:    {base}")
    print(f"Устройство: {args.device}")

    do_health(base)

    if args.pull:
        do_pull(base, args.device, Path(args.pull))
        return 0
    if args.health:
        return 0
    if not args.image:
        print("\nКадр не задан. Укажите файл JPEG, --pull или --health.")
        return 0

    path = Path(args.image)
    if not path.is_file():
        fail(f"Файл не найден: {path}")
        return 2
    jpeg = path.read_bytes()
    if jpeg[:2] != b"\xff\xd8":
        fail(f"{path} не похож на JPEG.",
             "Первые два байта должны быть FF D8. Пост шлёт именно JPEG, "
             "и подсовывать PNG смысла нет — сервер его отвергнет так же.")
        return 2

    r = do_verify(base, hdr, jpeg)
    if not r:
        return 1
    if args.barcode:
        if not r.get("matched"):
            print("\n  Штрихкод не отправляю: без опознания токена нет, "
                  "и сервер такой скан отвергнет. Это и есть та гарантия, "
                  "ради которой всё построено.")
            return 1
        do_scan(base, hdr, r["session_token"], args.barcode)
    elif r.get("matched"):
        print("\n  Опознание прошло. Чтобы проверить запись в журнал, "
              "повторите с --barcode TRK4417AZ")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
