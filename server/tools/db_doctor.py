"""
Диагностика базы: что в ней лежит и почему сервер этого не видит.

Нужен в одном конкретном случае, который легко получить и трудно опознать:
вы сменили `face.backend` в config.yaml. У insightface вектор 512-мерный,
у opencv — 128-мерный. Все, кто был зарегистрирован прежней моделью,
остаются в базе с прежней длиной вектора, и новая модель их узнать не может
в принципе — это не «плохо совпало», это несравнимые величины.

Раньше это ещё и роняло сервер при старте (np.stack на векторах разной
длины), теперь такие записи просто исключаются из поиска, а этот скрипт
показывает их и помогает разобраться.

Запуск (из папки server/):
    python -m tools.db_doctor              только показать
    python -m tools.db_doctor --purge      удалить несовместимые записи
    python -m tools.db_doctor --purge --yes    без вопроса
"""
from __future__ import annotations

import argparse
import sys
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from core.config import get_config      # noqa: E402
from core.db import Database            # noqa: E402

EXPECTED_DIM = {"insightface": 512, "opencv": 128}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--purge", action="store_true",
                    help="удалить сотрудников с несовместимой размерностью")
    ap.add_argument("--yes", action="store_true", help="не переспрашивать")
    args = ap.parse_args()

    cfg = get_config()
    if not cfg.db_file.exists():
        print(f"Базы нет: {cfg.db_file}")
        print("Это нормально для первого запуска — она создастся сама.")
        return 0

    db = Database(cfg.db_file)
    want = EXPECTED_DIM.get(cfg.backend)

    print(f"База      : {cfg.db_file}")
    print(f"Размер    : {cfg.db_file.stat().st_size/1024:.0f} КБ")
    print(f"Бэкенд    : {cfg.backend}" + (f" (ожидаемая размерность {want})" if want else ""))
    print(f"Порог     : {cfg.threshold}")
    print()

    rows = db._q("SELECT id, employee_no, full_name, active, dim, n_samples, "  # noqa: SLF001
                 "created_at FROM employees ORDER BY id")
    if not rows:
        print("Сотрудников нет. Зарегистрируйте первого на странице /enroll.")
        return 0

    dims = Counter(int(r["dim"]) for r in rows)
    print(f"Сотрудников: {len(rows)}   размерности векторов: "
          + ", ".join(f"{d} ({n} шт.)" for d, n in dims.most_common()))
    print()
    print(f"{'id':>4}  {'таб.№':<10} {'ФИО':<28} {'dim':>5} {'снимков':>8}  статус")
    print("-" * 78)

    bad = []
    for r in rows:
        dim = int(r["dim"])
        ok = (want is None) or (dim == want)
        if not ok:
            bad.append(r)
        status = "в поиске" if (ok and r["active"]) else \
                 ("заблокирован" if ok else "НЕСОВМЕСТИМ")
        print(f"{r['id']:>4}  {r['employee_no']:<10} {r['full_name'][:28]:<28} "
              f"{dim:>5} {r['n_samples']:>8}  {status}")

    print()
    stats = db.stats()
    print(f"Записей в журнале: {stats['scans']}   "
          f"попыток распознавания: {stats['verifies']}")

    if not bad:
        print("\nВсё в порядке: размерности совпадают с текущей моделью.")
        return 0

    print()
    print(f"НЕСОВМЕСТИМЫХ ЗАПИСЕЙ: {len(bad)}")
    print("Они исключены из поиска. Такие сотрудники не будут опознаваться,")
    print("сколько бы раз ни вставали перед камерой.")
    print()
    print("Два пути:")
    print(f"  1. Вернуть прежний бэкенд в config.yaml (было {dims.most_common()[0][0]}-мерным).")
    print("  2. Зарегистрировать этих людей заново текущей моделью,")
    print("     удалив старые записи: python -m tools.db_doctor --purge")
    print()
    print("Записи в журнале при удалении сотрудника СОХРАНЯЮТСЯ — журнал")
    print("append-only, ссылка на удалённого сотрудника в нём остаётся.")

    if not args.purge:
        return 1

    if not args.yes:
        ans = input(f"\nУдалить {len(bad)} записей? Введите 'да' для подтверждения: ")
        if ans.strip().lower() not in ("да", "yes", "y"):
            print("Отменено.")
            return 1

    for r in bad:
        db.delete_employee(int(r["id"]))
        print(f"удалён: {r['full_name']} (id={r['id']})")
    print(f"\nГотово. Зарегистрируйте этих сотрудников заново на /enroll.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
