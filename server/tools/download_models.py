"""
Скачать ONNX-модели для бэкенда "opencv" (YuNet + SFace) из OpenCV Zoo.

Запуск (из папки server/):
    python -m tools.download_models

ВАЖНО про источник: в репозитории opencv_zoo файлы .onnx хранятся через
Git LFS. Обычный raw.githubusercontent.com отдаёт не саму модель, а
текстовый указатель LFS на 130 байт — и OpenCV потом падает на загрузке.
Поэтому качаем с media.githubusercontent.com/media/... , а скачанный файл
дополнительно проверяем на размер и на сигнатуру указателя LFS.

Для бэкенда "insightface" этот скрипт не нужен — пакет качает модели сам.
"""
from __future__ import annotations

import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from core.config import get_config  # noqa: E402

LFS = "https://media.githubusercontent.com/media/opencv/opencv_zoo/main/models"
RAW = "https://raw.githubusercontent.com/opencv/opencv_zoo/main/models"

FILES = [
    (
        "face_detection_yunet_2023mar.onnx",
        "face_detection_yunet",
        "Детектор лиц YuNet",
        200_000,          # ожидаемый минимальный размер, байт
    ),
    (
        "face_recognition_sface_2021dec.onnx",
        "face_recognition_sface",
        "Эмбеддер SFace, 128-d",
        30_000_000,
    ),
]

LFS_POINTER_MAGIC = b"version https://git-lfs"


def _progress(blocks: int, bs: int, total: int) -> None:
    if total <= 0:
        return
    pct = min(100, blocks * bs * 100 // total)
    print(f"\r  {pct:3d}%", end="", flush=True)


def _looks_valid(path: Path, min_size: int) -> tuple[bool, str]:
    if not path.exists():
        return False, "файла нет"
    size = path.stat().st_size
    with open(path, "rb") as f:
        head = f.read(len(LFS_POINTER_MAGIC))
    if head == LFS_POINTER_MAGIC:
        return False, "скачался указатель Git LFS, а не сама модель"
    if size < min_size:
        return False, f"размер {size} байт меньше ожидаемых {min_size}"
    return True, ""


def fetch_insightface(models_dir: Path) -> int:
    """Прогреть buffalo_l: пакет качает модели сам, но делает это при первом
    же запросе на распознавание. Лучше пережить эти 300 МБ здесь и сейчас,
    чем на демонстрации, когда оператор нажмёт кнопку и ничего не произойдёт.
    """
    try:
        from insightface.app import FaceAnalysis
    except ImportError:
        print("[!] insightface не установлен.")
        print("    pip install insightface onnxruntime")
        print("    либо переключите face.backend на 'opencv' в config.yaml")
        return 1

    print(f"Готовлю buffalo_l (SCRFD + ArcFace, 512-d) в {models_dir}")
    print("Первый раз это ~300 МБ, дальше всё офлайн.\n")
    try:
        app = FaceAnalysis(name="buffalo_l", root=str(models_dir),
                           providers=["CPUExecutionProvider"])
        app.prepare(ctx_id=-1, det_size=(640, 640))
    except Exception as e:  # noqa: BLE001
        print(f"[!] не получилось: {e}")
        print("    Проверьте интернет или скачайте buffalo_l.zip вручную:")
        print("    https://github.com/deepinsight/insightface/releases")
        print(f"    и распакуйте в {models_dir / 'models' / 'buffalo_l'}")
        return 1
    print("\nГотово. Можно запускать:  python app.py")
    return 0


def main() -> int:
    cfg = get_config()
    cfg.models_dir.mkdir(parents=True, exist_ok=True)
    print(f"Папка моделей: {cfg.models_dir}\n")

    # Качаем то, что реально понадобится выбранному бэкенду.
    if cfg.backend == "insightface":
        return fetch_insightface(cfg.models_dir)

    failed = False
    for name, subdir, desc, min_size in FILES:
        dst = cfg.models_dir / name
        ok, _ = _looks_valid(dst, min_size)
        if ok:
            print(f"[есть]   {name}  ({dst.stat().st_size/1e6:.1f} МБ)")
            continue

        for base in (LFS, RAW):
            url = f"{base}/{subdir}/{name}"
            print(f"[качаю]  {name} — {desc}")
            try:
                urllib.request.urlretrieve(url, dst, _progress)
            except Exception as e:  # noqa: BLE001
                print(f"\r  ошибка сети: {e}")
                continue
            ok, why = _looks_valid(dst, min_size)
            if ok:
                print(f"\r  готово, {dst.stat().st_size/1e6:.1f} МБ")
                break
            print(f"\r  негодный файл: {why}")
            dst.unlink(missing_ok=True)
        else:
            failed = True
            print(f"  НЕ УДАЛОСЬ скачать {name}.")
            print(f"  Скачайте вручную по ссылке:")
            print(f"    {LFS}/{subdir}/{name}")
            print(f"  и положите файл в: {dst}\n")

    if failed:
        return 1
    print("\nГотово. Можно запускать:  python app.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
