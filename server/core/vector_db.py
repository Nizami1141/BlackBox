"""
Векторная база лиц.

Реализация намеренно простая: все эталоны держатся в одной матрице NumPy
(N x dim, float32, L2-нормализованные). Поиск = одно матричное умножение.

Почему не FAISS/Qdrant: для 512-мерных векторов и N до ~50 000 полный перебор
на CPU занимает единицы миллисекунд — это в сотни раз быстрее, чем сеть и
эмбеддинг. Внешний векторный движок здесь добавил бы зависимость без выигрыша.
Если база вырастет за 100 000 — интерфейс `search()` менять не придётся,
меняется только внутренность (см. TODO внизу).
"""
from __future__ import annotations

import logging
import threading
from dataclasses import dataclass
from typing import Optional

import numpy as np

from .db import Database


@dataclass
class SearchHit:
    employee_id: int
    score: float          # косинусное сходство, [-1..1]
    rank: int


log = logging.getLogger(__name__)


class VectorIndex:
    def __init__(self, db: Database, expected_dim: Optional[int] = None):
        self.db = db
        self.expected_dim = expected_dim
        self._lock = threading.RLock()
        self._ids: list[int] = []
        self._mat: np.ndarray = np.zeros((0, 0), dtype=np.float32)
        self.stale: list[int] = []      # id сотрудников с чужой размерностью
        self.reload()

    def reload(self) -> None:
        with self._lock:
            self._ids, self._mat, bad = self.db.load_templates(self.expected_dim)
            self.stale = [int(r["id"]) for r in bad]
            if bad:
                # Громко, потому что молча это выглядит как «система забыла
                # половину сотрудников», а причина совсем в другом.
                log.warning(
                    "В базе %d эталонов чужой размерности — они ИСКЛЮЧЕНЫ из поиска. "
                    "Так бывает после смены face.backend: у insightface вектор 512-мерный, "
                    "у opencv 128-мерный. Этих сотрудников надо зарегистрировать заново. "
                    "Разбор: python -m tools.db_doctor", len(bad))

    @property
    def size(self) -> int:
        return len(self._ids)

    def search(self, query: np.ndarray, top_k: int = 5) -> list[SearchHit]:
        with self._lock:
            if self.size == 0:
                return []
            q = np.asarray(query, dtype=np.float32).reshape(-1)
            if q.shape[0] != self._mat.shape[1]:
                raise ValueError(
                    f"Размерность вектора {q.shape[0]} не совпадает с базой "
                    f"{self._mat.shape[1]}. Скорее всего сменили face.backend — "
                    f"базу надо перерегистрировать."
                )
            sims = self._mat @ q                      # (N,)
            k = min(top_k, sims.shape[0])
            idx = np.argpartition(-sims, k - 1)[:k]
            idx = idx[np.argsort(-sims[idx])]
            return [SearchHit(self._ids[int(i)], float(sims[int(i)]), r)
                    for r, i in enumerate(idx)]

    def identify(self, query: np.ndarray, threshold: float,
                 margin: float = 0.0) -> tuple[Optional[SearchHit], list[SearchHit]]:
        """Вернуть (совпадение | None, топ-кандидаты).

        `margin` — необязательный запас между первым и вторым кандидатом.
        Если два человека в базе похожи и разница меньше margin, безопаснее
        отказать, чем пропустить не того. Для КПП это правильное поведение.
        """
        hits = self.search(query, top_k=5)
        if not hits:
            return None, []
        best = hits[0]
        if best.score < threshold:
            return None, hits
        if margin > 0 and len(hits) > 1 and (best.score - hits[1].score) < margin:
            return None, hits
        return best, hits


# TODO (при N > 100k): заменить внутренности на hnswlib —
#   index = hnswlib.Index(space='cosine', dim=dim); index.add_items(mat, ids)
#   Публичный интерфейс search()/identify() остаётся тем же.
