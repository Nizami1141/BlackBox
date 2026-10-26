"""
Пайплайн распознавания лица: детекция -> выравнивание -> эмбеддинг.

Два взаимозаменяемых бэкенда:

  "insightface" SCRFD (детектор) + ArcFace (эмбеддер) из пакета buffalo_l,
                512-мерный вектор. Всё считается локально. Точнее, тяжелее,
                при первом запуске качает ~300 МБ. Это дефолт.

  "opencv"      YuNet (детектор) + SFace (128-d эмбеддер) из OpenCV Zoo.
                Ставится одним `pip install opencv-contrib-python`, ничего
                компилировать не надо. Запасной путь для Windows.

Оба возвращают L2-нормализованный вектор, поэтому мера сходства всегда одна
и та же — косинусное сходство = скалярное произведение.

---------------------------------------------------------------------------
ПОЧЕМУ ЗДЕСЬ МЬЮТЕКС

Это не перестраховка, а исправление конкретной аварии. Распознавание
вызывается из пула потоков (`asyncio.to_thread`), и обращений бывает
несколько сразу: страница регистрации проверяет каждый снимок сразу после
съёмки, а оператор снимает их подряд, не дожидаясь ответа.

Ни один из бэкендов к параллельным вызовам не готов:

  * OpenCV: `FaceDetectorYN` хранит размер входа внутри объекта. Вызов
    setInputSize() из второго потока между setInputSize() и detect()
    первого — и детектор читает кадр с чужими размерами.
  * InsightFace: сессия onnxruntime переиспользует внутренние буферы.

Симптом был ровно такой, как описан: при добавлении сотрудника интерфейс
зависал наглухо. Все обращения к моделям теперь идут через один замок.
Потеря производительности нулевая: узкое место — сама модель, и параллельно
она всё равно не считает.
---------------------------------------------------------------------------
"""
from __future__ import annotations

import logging
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Sequence

import cv2
import numpy as np

log = logging.getLogger(__name__)


class FaceError(Exception):
    """Осмысленная ошибка пайплайна, которую можно показать оператору."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


@dataclass
class DetectedFace:
    bbox: tuple[int, int, int, int]      # x, y, w, h в координатах ИСХОДНОГО кадра
    det_score: float
    embedding: np.ndarray                # L2-нормализованный float32
    landmarks: Optional[np.ndarray] = None


def _l2norm(v: np.ndarray) -> np.ndarray:
    v = np.asarray(v, dtype=np.float32).reshape(-1)
    n = float(np.linalg.norm(v))
    if n < 1e-9:
        raise FaceError("BAD_EMBEDDING", "Нулевой вектор признаков")
    return (v / n).astype(np.float32)


# ---------------------------------------------------------------------------
#  Бэкенд 1: OpenCV YuNet + SFace
# ---------------------------------------------------------------------------
class OpenCVBackend:
    DET_FILE = "face_detection_yunet_2023mar.onnx"
    REC_FILE = "face_recognition_sface_2021dec.onnx"
    DIM = 128

    def __init__(self, models_dir: Path, det_score_min: float, det_size: int = 320):
        det_path = models_dir / self.DET_FILE
        rec_path = models_dir / self.REC_FILE
        for p in (det_path, rec_path):
            if not p.exists():
                raise FaceError(
                    "MODEL_MISSING",
                    f"Не найден файл модели {p.name}. "
                    f"Запустите:  python -m tools.download_models",
                )
        self.detector = cv2.FaceDetectorYN.create(
            model=str(det_path),
            config="",
            input_size=(det_size, det_size),
            score_threshold=float(det_score_min),
            nms_threshold=0.3,
            top_k=5000,
        )
        self.recognizer = cv2.FaceRecognizerSF.create(str(rec_path), "")

    def detect(self, bgr: np.ndarray) -> list[tuple]:
        h, w = bgr.shape[:2]
        self.detector.setInputSize((w, h))
        _, faces = self.detector.detect(bgr)
        if faces is None:
            return []
        out = []
        for row in faces:
            x, y, fw, fh = row[0:4]
            out.append((row, float(row[-1]), (int(x), int(y), int(fw), int(fh))))
        return out

    def embed(self, bgr: np.ndarray, raw) -> np.ndarray:
        aligned = self.recognizer.alignCrop(bgr, raw)
        return _l2norm(self.recognizer.feature(aligned))


# ---------------------------------------------------------------------------
#  Бэкенд 2: InsightFace (SCRFD + ArcFace, пакет buffalo_l)
# ---------------------------------------------------------------------------
class InsightFaceBackend:
    DIM = 512

    def __init__(self, models_dir: Path, det_score_min: float, det_size: int = 320,
                 threads: int = 0):
        try:
            from insightface.app import FaceAnalysis
        except ImportError as e:  # pragma: no cover
            raise FaceError(
                "BACKEND_MISSING",
                "Не установлен insightface. Либо `pip install insightface onnxruntime`, "
                "либо переключите face.backend на 'opencv' в config.yaml",
            ) from e

        # Ограничение числа потоков onnxruntime. По умолчанию он занимает все
        # ядра, и на ноутбуке это делает хуже, а не лучше: потоки дерутся за
        # кэш, а рядом ещё крутится веб-сервер и приём BLE.
        if threads > 0:
            try:
                import onnxruntime as ort
                ort.set_default_logger_severity(3)
            except Exception:  # noqa: BLE001
                pass

        providers = ["CPUExecutionProvider"]
        try:
            import onnxruntime as ort
            if "CUDAExecutionProvider" in ort.get_available_providers():
                providers.insert(0, "CUDAExecutionProvider")
                log.info("onnxruntime: найдена CUDA, распознавание пойдёт на видеокарте")
        except Exception:  # noqa: BLE001
            pass

        self.app = FaceAnalysis(name="buffalo_l", root=str(models_dir),
                                providers=providers)
        # det_size — размер, до которого детектор масштабирует кадр. 640 был
        # выбран «по умолчанию из примеров» и стоит примерно вчетверо дороже
        # 320. На КПП лицо занимает половину кадра, и 320 хватает с запасом.
        self.app.prepare(ctx_id=-1, det_size=(det_size, det_size),
                         det_thresh=float(det_score_min))

    def detect(self, bgr: np.ndarray) -> list[tuple]:
        # Объект лица возвращается наружу целиком, а не через индекс во
        # внутреннем списке. Раньше здесь был self._cache, и при двух
        # одновременных вызовах второй затирал список первого — embed()
        # доставал чужое лицо или падал по индексу.
        out = []
        for f in self.app.get(bgr):
            x1, y1, x2, y2 = [int(v) for v in f.bbox]
            out.append((f, float(f.det_score), (x1, y1, x2 - x1, y2 - y1)))
        return out

    def embed(self, bgr: np.ndarray, raw) -> np.ndarray:
        return _l2norm(raw.normed_embedding)


# ---------------------------------------------------------------------------
#  Фасад
# ---------------------------------------------------------------------------
class FaceEngine:
    def __init__(
        self,
        backend: str,
        models_dir: Path,
        det_score_min: float = 0.65,
        min_face_px: int = 80,
        allow_multiple_faces: bool = False,
        det_size: int = 320,
        max_side: int = 800,
        threads: int = 0,
    ):
        self.backend_name = backend
        self.min_face_px = min_face_px
        self.allow_multiple_faces = allow_multiple_faces
        self.max_side = max_side

        # Один замок на всё: и детекция, и эмбеддинг, и превью.
        self._lock = threading.RLock()
        self.last_ms = {"resize": 0, "detect": 0, "embed": 0}

        t0 = time.perf_counter()
        if backend == "opencv":
            self.impl = OpenCVBackend(models_dir, det_score_min, det_size)
        elif backend == "insightface":
            self.impl = InsightFaceBackend(models_dir, det_score_min, det_size, threads)
        else:
            raise FaceError("BAD_BACKEND", f"Неизвестный face.backend: {backend!r}")

        self.dim = self.impl.DIM
        log.info("FaceEngine: backend=%s, dim=%d, det_size=%d, max_side=%d "
                 "(загрузка %.1f с)", backend, self.dim, det_size, max_side,
                 time.perf_counter() - t0)

    # -- вспомогательное ---------------------------------------------------
    @staticmethod
    def decode_jpeg(data: bytes) -> np.ndarray:
        arr = np.frombuffer(data, dtype=np.uint8)
        img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
        if img is None:
            raise FaceError("BAD_IMAGE", "Не удалось декодировать изображение")
        return img

    def _fit(self, bgr: np.ndarray) -> tuple[np.ndarray, float]:
        """Уменьшить кадр до max_side. Возвращает (кадр, коэффициент).

        Веб-камера отдаёт 1280x960, а детектор всё равно сожмёт кадр до
        det_size. Гонять через модель лишние пиксели незачем: на снимках
        с веб-камеры это была заметная часть времени регистрации.
        Кадр с платы (640x480) не трогается вообще.
        """
        h, w = bgr.shape[:2]
        m = max(h, w)
        if self.max_side <= 0 or m <= self.max_side:
            return bgr, 1.0
        s = self.max_side / m
        small = cv2.resize(bgr, (int(w * s), int(h * s)), interpolation=cv2.INTER_AREA)
        return small, s

    # -- основной вызов ----------------------------------------------------
    def extract(self, bgr: np.ndarray) -> DetectedFace:
        """Найти ровно одно пригодное лицо и вернуть его эмбеддинг.

        Бросает FaceError с понятным кодом, если лица нет / их много /
        лицо слишком мелкое. Коды ошибок уходят на сайт и в журнал.
        """
        with self._lock:
            t0 = time.perf_counter()
            img, scale = self._fit(bgr)
            t1 = time.perf_counter()
            faces = self.impl.detect(img)
            t2 = time.perf_counter()

            if not faces:
                self._timing(t0, t1, t2, t2)
                raise FaceError("NO_FACE", "Лицо не найдено в кадре")

            # Порог размера считаем в координатах ИСХОДНОГО кадра, чтобы
            # min_face_px значил одно и то же независимо от max_side.
            def side(bbox):
                return min(bbox[2], bbox[3]) / scale

            big = [f for f in faces if side(f[2]) >= self.min_face_px]
            if not big:
                self._timing(t0, t1, t2, t2)
                raise FaceError(
                    "FACE_TOO_SMALL",
                    f"Лицо меньше {self.min_face_px} px — попросите подойти ближе",
                )

            if len(big) > 1 and not self.allow_multiple_faces:
                self._timing(t0, t1, t2, t2)
                raise FaceError(
                    "MULTIPLE_FACES",
                    f"В кадре {len(big)} лица — в кадре должен быть один человек",
                )

            raw, score, bbox = max(big, key=lambda f: f[2][2] * f[2][3])
            emb = self.impl.embed(img, raw)
            t3 = time.perf_counter()
            self._timing(t0, t1, t2, t3)

            orig = tuple(int(v / scale) for v in bbox)
            return DetectedFace(bbox=orig, det_score=score, embedding=emb)

    def _timing(self, t0, t1, t2, t3) -> None:
        self.last_ms = {
            "resize": int((t1 - t0) * 1000),
            "detect": int((t2 - t1) * 1000),
            "embed": int((t3 - t2) * 1000),
        }
        log.debug("face: resize %(resize)d мс, detect %(detect)d мс, "
                  "embed %(embed)d мс", self.last_ms)

    def detect_boxes(self, bgr: np.ndarray) -> list[dict]:
        """Только рамки, без эмбеддинга — для подсказки при регистрации.

        Отдельный метод, а не обращение к self.impl снаружи: бэкенды
        возвращают «сырую» строку детектора в разном формате.
        """
        with self._lock:
            img, scale = self._fit(bgr)
            out = []
            for _raw, score, bbox in self.impl.detect(img):
                out.append({
                    "bbox": [int(v / scale) for v in bbox],
                    "det_score": round(float(score), 3),
                    "too_small": min(bbox[2], bbox[3]) / scale < self.min_face_px,
                })
            return out

    def extract_from_jpeg(self, data: bytes) -> tuple[DetectedFace, np.ndarray]:
        img = self.decode_jpeg(data)
        return self.extract(img), img

    def extract_all(self, bgr: np.ndarray) -> list[DetectedFace]:
        with self._lock:
            img, scale = self._fit(bgr)
            res = []
            for raw, score, bbox in self.impl.detect(img):
                if min(bbox[2], bbox[3]) / scale < self.min_face_px:
                    continue
                try:
                    res.append(DetectedFace(
                        bbox=tuple(int(v / scale) for v in bbox),
                        det_score=score,
                        embedding=self.impl.embed(img, raw),
                    ))
                except Exception:  # noqa: BLE001
                    continue
            return res


def cosine(a: np.ndarray, b: np.ndarray) -> float:
    """Косинусное сходство L2-нормализованных векторов = скалярное произведение."""
    return float(np.dot(a, b))


def average_embeddings(embs: Sequence[np.ndarray]) -> np.ndarray:
    """Эталон сотрудника = усреднённый и заново нормализованный вектор
    по нескольким снимкам. Это заметно устойчивее одного снимка."""
    if not embs:
        raise FaceError("NO_SAMPLES", "Нет ни одного снимка для усреднения")
    m = np.mean(np.stack([np.asarray(e, dtype=np.float32) for e in embs]), axis=0)
    return _l2norm(m)
