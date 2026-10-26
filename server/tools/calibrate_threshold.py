"""
Подбор порога сходства по вашей собственной базе.

Зачем это нужно. Порог 0.45 в config.yaml — не истина, а разумная отправная
точка из литературы. Реальное значение зависит от вашей камеры, освещения
на посту и от того, насколько похожи люди в вашей базе. Пока порог не
измерен, вы не знаете ни сколько «чужих» система пропустит (FAR), ни как
часто она будет отказывать своим (FRR). Для пограничного поста это не
абстракция: FAR — это пропущенный посторонний, FRR — это очередь и
раздражённые сотрудники.

Что делает скрипт: берёт все снимки из базы (таблица face_samples),
считает две выборки косинусных расстояний —

    свои-со-своими   (пары снимков одного человека)
    свои-с-чужими    (пары снимков разных людей)

— и печатает, какая доля ошибок получается при каждом пороге.

Запуск (из папки server/):
    python -m tools.calibrate_threshold
    python -m tools.calibrate_threshold --csv out.csv

Осмысленный результат получается, начиная примерно с 10 сотрудников
и 3 снимков на каждого. На двух людях считать нечего.
"""
from __future__ import annotations

import argparse
import itertools
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from core.config import get_config      # noqa: E402
from core.db import Database            # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="", help="сохранить таблицу порогов в файл")
    ap.add_argument("--lo", type=float, default=0.20)
    ap.add_argument("--hi", type=float, default=0.80)
    ap.add_argument("--step", type=float, default=0.01)
    args = ap.parse_args()

    cfg = get_config()
    db = Database(cfg.db_file)

    by_emp: dict[int, list[np.ndarray]] = defaultdict(list)
    for emp_id, vec in db.load_samples():
        by_emp[emp_id].append(np.asarray(vec, dtype=np.float32))

    if len(by_emp) < 2:
        print("В базе меньше двух сотрудников — калибровать нечего.")
        return 1

    genuine: list[float] = []      # пары снимков ОДНОГО человека
    impostor: list[float] = []     # пары снимков РАЗНЫХ людей

    for vecs in by_emp.values():
        for a, b in itertools.combinations(vecs, 2):
            genuine.append(float(np.dot(a, b)))

    ids = list(by_emp)
    for i, j in itertools.combinations(ids, 2):
        for a in by_emp[i]:
            for b in by_emp[j]:
                impostor.append(float(np.dot(a, b)))

    if not genuine:
        print("У каждого сотрудника по одному снимку — пар «свой со своим» нет.")
        print("Добавьте хотя бы по 2-3 снимка на человека и повторите.")
        return 1

    g = np.array(genuine)
    im = np.array(impostor)

    print(f"Сотрудников: {len(by_emp)}, снимков: {sum(len(v) for v in by_emp.values())}")
    print(f"Пар свой-свой:   {len(g):6d}   среднее {g.mean():.3f}  минимум {g.min():.3f}")
    print(f"Пар свой-чужой:  {len(im):6d}   среднее {im.mean():.3f}  максимум {im.max():.3f}")
    print()
    print(" порог     FRR (отказ своему)   FAR (пропуск чужого)")
    print(" -------   ------------------   --------------------")

    rows = []
    best = None
    thr = args.lo
    while thr <= args.hi + 1e-9:
        frr = float((g < thr).mean())            # своих отвергли
        far = float((im >= thr).mean())          # чужих пропустили
        rows.append((thr, frr, far))
        # EER — точка, где обе ошибки равны. Не «лучший» порог сам по себе,
        # но удобная точка отсчёта, от которой сдвигаются в нужную сторону.
        if best is None or abs(frr - far) < abs(best[1] - best[2]):
            best = (thr, frr, far)
        mark = ""
        if abs(thr - cfg.threshold) < args.step / 2:
            mark = "  <- сейчас в config.yaml"
        print(f"  {thr:.2f}      {frr*100:7.2f} %          {far*100:7.3f} %{mark}")
        thr += args.step

    print()
    print(f"Точка равных ошибок (EER): порог {best[0]:.2f}, "
          f"FRR={best[1]*100:.2f} %, FAR={best[2]*100:.3f} %")
    print()
    print("Как выбирать. На КПП цена ошибок разная: пропустить постороннего")
    print("хуже, чем лишний раз переснять своего. Поэтому берут порог ПРАВЕЕ")
    print("точки EER — такой, где FAR уже близок к нулю, и мирятся с тем,")
    print("что часть сотрудников иногда просят встать ровнее.")

    if args.csv:
        with open(args.csv, "w", encoding="utf-8") as f:
            f.write("threshold;frr;far\n")
            for t, frr, far in rows:
                f.write(f"{t:.3f};{frr:.6f};{far:.6f}\n")
        print(f"\nТаблица сохранена: {args.csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
