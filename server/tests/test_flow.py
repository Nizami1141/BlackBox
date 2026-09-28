"""
Сквозной тест сценария проверки — без платы и без модели распознавания.

Проверяется не качество распознавания (для этого есть tools/calibrate_threshold),
а ПОРЯДОК действий, в котором и состоит смысл системы:

  * сканер не включается, пока лицо не опознано;
  * штрихкод без действующего токена в журнал не попадает;
  * токен, выданный одному посту, не принимается от другого;
  * токен нельзя использовать больше session.max_scans раз;
  * при аварии посреди прохода луч гасится.

Настоящая модель заменена заглушкой: эмбеддинг считается из среднего цвета
картинки. Этого достаточно, чтобы «разные люди» были разными векторами,
и тест не тянет за собой 300 МБ буффало.

Запуск:  python -m pytest tests/test_flow.py -v      (из папки server/)
"""
from __future__ import annotations

import asyncio
import os
import sys
import tempfile
import threading
from pathlib import Path

import cv2
import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import core.service as service_mod                      # noqa: E402
from core.config import Config, DeviceCfg               # noqa: E402
from core.face_engine import DetectedFace, FaceError    # noqa: E402
from core.flow import CheckpointFlow                    # noqa: E402
from core.service import CheckpointService              # noqa: E402
from link import LinkRegistry                           # noqa: E402
from link.fake_link import FakeLink                     # noqa: E402
from core.vector_db import VectorIndex                  # noqa: E402


# ---------------------------------------------------------------- заглушка
class StubEngine:
    """Вектор из среднего цвета картинки. Детерминированно и без моделей."""

    DIM = 8

    def __init__(self, *a, **kw):
        self.dim = self.DIM
        self.min_face_px = 80
        self.backend_name = "stub"
        self.last_ms = {"resize": 0, "detect": 0, "embed": 0}

    @staticmethod
    def decode_jpeg(data: bytes) -> np.ndarray:
        img = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)
        if img is None:
            raise FaceError("BAD_IMAGE", "Не удалось декодировать изображение")
        return img

    def extract(self, bgr: np.ndarray) -> DetectedFace:
        mean = bgr.reshape(-1, 3).mean(axis=0) / 255.0
        if mean.sum() < 0.05:
            # Чёрный кадр = «лица нет». Нужен, чтобы проверить путь отказа.
            raise FaceError("NO_FACE", "Лицо не найдено в кадре")
        v = np.array([mean[0], mean[1], mean[2], mean[0] * mean[1],
                      mean[1] * mean[2], mean[0] * mean[2],
                      mean.sum() / 3, 0.1], dtype=np.float32)
        v /= np.linalg.norm(v)
        h, w = bgr.shape[:2]
        return DetectedFace(bbox=(0, 0, w, h), det_score=0.99, embedding=v)

    def extract_from_jpeg(self, data: bytes):
        img = self.decode_jpeg(data)
        return self.extract(img), img

    def detect_boxes(self, bgr: np.ndarray) -> list[dict]:
        h, w = bgr.shape[:2]
        if bgr.reshape(-1, 3).mean() < 12:
            return []
        return [{"bbox": [0, 0, w, h], "det_score": 0.99, "too_small": False}]


def _face(color, size=(480, 640)) -> np.ndarray:
    img = np.zeros((size[0], size[1], 3), np.uint8)
    img[:, :] = color
    return img


def _jpeg(img: np.ndarray) -> bytes:
    ok, buf = cv2.imencode(".jpg", img)
    assert ok
    return buf.tobytes()


# ------------------------------------------------------------------ стенд
@pytest.fixture()
def stand(monkeypatch):
    monkeypatch.setattr(service_mod, "FaceEngine", StubEngine)

    tmp = Path(tempfile.mkdtemp())
    faces = tmp / "faces"
    faces.mkdir()

    cfg = Config()
    cfg.data_dir = tmp
    cfg.db_file = tmp / "test.db"
    cfg.models_dir = tmp
    cfg.threshold = 0.95            # заглушка даёт очень близкие вектора
    cfg.session_ttl = 30
    cfg.session_max_scans = 1
    cfg.keep_enroll_thumbnail = False
    cfg.devices = [
        DeviceCfg("GATE-01", "k1", "тест"),
        DeviceCfg("GATE-02", "k2", "второй пост"),
    ]

    service = CheckpointService(cfg)

    # Регистрируем «сотрудника» — синий кадр.
    blue = _face((200, 60, 40))
    emp_id, _ = service.enroll("100", "Тестов Тест Тестович", [blue])

    # Тот же цвет лежит в папке фейкового поста: значит пост «покажет»
    # именно этого человека.
    cv2.imwrite(str(faces / "known.jpg"), blue)

    reg = LinkRegistry()
    link = FakeLink("GATE-01", faces, scan_delay=0.1, barcode="TRK-0001")
    reg.register(link)

    flow = CheckpointFlow(service, reg, photo_timeout=5, scan_timeout=3, cooldown=0)
    return {"cfg": cfg, "service": service, "reg": reg, "link": link,
            "flow": flow, "emp_id": emp_id, "faces": faces}


# ------------------------------------------------------------------ тесты
@pytest.mark.asyncio
async def test_happy_path(stand):
    """Полный проход: кадр -> опознание -> сканер -> запись в журнал."""
    await stand["link"].start()
    flow = stand["flow"]

    stages = []
    flow.on_event(lambda ev: stages.append(ev["stage"]))

    st = await flow.run(flow.create("GATE-01"))

    assert st.ok, f"{st.reason}: {st.message}"
    assert st.employee["id"] == stand["emp_id"]
    assert st.barcode == "TRK-0001"
    assert st.record_id > 0

    # Порядок стадий и есть предмет проверки: сканер (awaiting_scan)
    # обязан идти ПОСЛЕ опознания (matched), а не до.
    assert stages.index("matched") < stages.index("awaiting_scan")
    assert "rejected" not in stages

    rows = stand["service"].db.list_scan_events()
    assert len(rows) == 1
    assert rows[0]["barcode"] == "TRK-0001"
    assert rows[0]["employee_id"] == stand["emp_id"]


@pytest.mark.asyncio
async def test_unknown_face_never_enables_scanner(stand):
    """Незнакомое лицо: сканер не должен включиться ни на мгновение."""
    # Подменяем содержимое папки поста на «другого человека».
    for p in stand["faces"].glob("*"):
        p.unlink()
    cv2.imwrite(str(stand["faces"] / "stranger.jpg"), _face((40, 200, 90)))

    await stand["link"].start()
    flow = stand["flow"]

    commands = []
    orig = stand["link"].send_command

    async def spy(cmd):
        commands.append(cmd)
        await orig(cmd)

    stand["link"].send_command = spy

    st = await flow.run(flow.create("GATE-01"))

    assert not st.ok
    assert st.reason == "NOT_FOUND"
    assert "SCAN ON" not in commands, "сканер включился без опознания!"
    assert not stand["service"].db.list_scan_events()


@pytest.mark.asyncio
async def test_no_face_in_frame(stand):
    for p in stand["faces"].glob("*"):
        p.unlink()
    cv2.imwrite(str(stand["faces"] / "dark.jpg"), _face((0, 0, 0)))

    await stand["link"].start()
    flow = stand["flow"]
    st = await flow.run(flow.create("GATE-01"))

    assert not st.ok
    assert st.reason == "NO_FACE"


def test_token_bound_to_device(stand):
    """Токен, выданный посту GATE-01, не принимается от GATE-02.

    Без этой проверки штрихкод, отсканированный на соседней полосе, можно
    было бы приписать сотруднику, который прошёл опознание здесь.
    """
    service = stand["service"]
    jpeg = _jpeg(_face((200, 60, 40)))

    r = service.verify_jpeg(jpeg, "GATE-01")
    assert r.matched

    ok, err, _ = service.register_scan(r.session_token, "TRK-9", "", "GATE-02")
    assert not ok and err == "SESSION_DEVICE_MISMATCH"

    ok, err, rec = service.register_scan(r.session_token, "TRK-9", "", "GATE-01")
    assert ok and rec > 0


def test_token_single_use(stand):
    """session.max_scans = 1, значит второй груз по тому же токену не пройдёт."""
    service = stand["service"]
    r = service.verify_jpeg(_jpeg(_face((200, 60, 40))), "GATE-01")
    assert r.matched

    ok, _, _ = service.register_scan(r.session_token, "TRK-1", "", "GATE-01")
    assert ok
    ok, err, _ = service.register_scan(r.session_token, "TRK-2", "", "GATE-01")
    assert not ok and err == "SESSION_EXHAUSTED"


def test_unknown_token_rejected(stand):
    ok, err, _ = stand["service"].register_scan("0" * 32, "TRK-X", "", "GATE-01")
    assert not ok and err == "SESSION_UNKNOWN"


def test_inactive_employee_rejected(stand):
    service = stand["service"]
    service.set_active(stand["emp_id"], False)
    r = service.verify_jpeg(_jpeg(_face((200, 60, 40))), "GATE-01")
    assert not r.matched
    # Заблокированный сотрудник исчезает из индекса целиком,
    # поэтому сервер честно говорит «не найден».
    assert r.reason in ("INACTIVE", "NOT_FOUND")


@pytest.mark.asyncio
async def test_scan_timeout_turns_beam_off(stand):
    """Сотрудник не поднёс груз: луч обязан погаснуть сам."""
    await stand["link"].start()
    stand["link"].scan_delay = 99          # штрихкода не будет
    flow = stand["flow"]

    commands = []
    orig = stand["link"].send_command

    async def spy(cmd):
        commands.append(cmd)
        await orig(cmd)

    stand["link"].send_command = spy

    st = await flow.run(flow.create("GATE-01"))

    assert not st.ok
    assert st.reason == "SCAN_TIMEOUT"
    assert commands.count("SCAN OFF") >= 1
    assert commands.index("SCAN ON") < commands.index("SCAN OFF")


@pytest.mark.asyncio
async def test_busy_device_rejects_second_run(stand):
    await stand["link"].start()
    stand["link"].scan_delay = 2
    flow = stand["flow"]

    first = asyncio.create_task(flow.run(flow.create("GATE-01")))
    await asyncio.sleep(0.2)
    second = await flow.run(flow.create("GATE-01"))

    assert second.reason == "DEVICE_BUSY"
    await first


@pytest.mark.asyncio
async def test_scanner_ack_is_not_recorded_as_barcode(stand):
    """Подтверждение команды от сканера не должно попасть в журнал.

    Настоящая авария: в Host Mode сканер отвечает кадром 04 D0 04 00 FF 28
    на START_DECODE. Фильтр «оставляем только печатный ASCII» выбрасывал
    непечатные байты и оставлял 0x28 — символ '('. Через паузу он уезжал
    наружу как прочитанный штрихкод, и в журнал писалась строка «сотрудник
    проверил груз с номером (». У других команд контрольная сумма другая,
    поэтому мусор бывал и '$'.

    Здесь фейковый пост воспроизводит это поведение: сразу после SCAN ON
    отдаёт одиночный символ, а настоящий код — позже.
    """
    await stand["link"].start()
    stand["link"].ack_artifact = "$"
    stand["link"].scan_delay = 0.4
    flow = stand["flow"]

    codes = []
    flow.on_event(lambda ev: codes.append(ev.get("barcode")) if ev["stage"] == "scanned" else None)

    st = await flow.run(flow.create("GATE-01"))

    assert st.ok, f"{st.reason}: {st.message}"
    assert st.barcode == "TRK-0001", f"в журнал уехал мусор: {st.barcode!r}"

    rows = stand["service"].db.list_scan_events()
    assert len(rows) == 1
    assert rows[0]["barcode"] == "TRK-0001"


@pytest.mark.asyncio
async def test_concurrent_engine_calls_do_not_corrupt(stand):
    """Параллельные обращения к модели не должны портить друг друга.

    Страница регистрации проверяет каждый снимок сразу после съёмки, и
    оператор снимает их подряд, не дожидаясь ответа. Без замка внутри
    FaceEngine это ломало состояние детектора: у OpenCV — размер входа,
    у InsightFace — общий список найденных лиц. Симптом был «при добавлении
    сотрудника интерфейс зависает».
    """
    service = stand["service"]
    blue = _jpeg(_face((200, 60, 40)))
    green = _jpeg(_face((40, 200, 90)))

    results = []
    errors = []

    def worker(data, n):
        for _ in range(n):
            try:
                results.append(service.preview_faces(data))
            except Exception as e:  # noqa: BLE001
                errors.append(e)

    threads = [threading.Thread(target=worker, args=(blue if i % 2 else green, 8))
               for i in range(6)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=30)

    assert not any(t.is_alive() for t in threads), "обращения к модели зависли"
    assert not errors, f"ошибки при параллельных вызовах: {errors[:3]}"
    assert len(results) == 48


def test_mixed_dims_do_not_break_startup(stand):
    """Векторы разной длины в базе не должны ронять сервер.

    Это причина «перезагрузил — и больше ничего не работает»: после смены
    face.backend в базе оказываются вектора 512 и 128, np.stack на них
    падал ValueError прямо в конструкторе сервиса, и сервер не стартовал.
    Теперь чужая размерность просто исключается из поиска.
    """
    service = stand["service"]
    db = service.db

    # Подкладываем «сотрудника от прежней модели»: вектор другой длины.
    alien = np.ones(64, dtype=np.float32)
    alien /= np.linalg.norm(alien)
    db.add_employee("999", "Старая Модель", alien, [alien])

    idx = VectorIndex(db, expected_dim=service.engine.dim)
    assert idx.size == 1, "нормальный сотрудник должен остаться в поиске"
    assert len(idx.stale) == 1, "несовместимая запись должна быть отмечена"

    # И поиск по-прежнему работает.
    hits = idx.search(np.array(
        StubEngine().extract(_face((200, 60, 40))).embedding))
    assert hits and hits[0].employee_id == stand["emp_id"]


@pytest.mark.asyncio
async def test_button_on_post_starts_the_same_flow(stand):
    """Кнопка у камеры запускает ровно тот же сценарий, что кнопка на сайте.

    На границе рядом с постом не будет ни оператора, ни компьютера, и
    единственный способ начать проверку — нажать кнопку. Важно, чтобы это
    был тот же путь в коде: два разных со временем разойдутся, и «на посту
    работает не так, как на пульте» станет отдельным источником ошибок.
    """
    await stand["link"].start()
    flow = stand["flow"]

    st = flow.handle_device_event("GATE-01", "BUTTON")
    assert st is not None, "нажатие кнопки не начало проверку"
    assert st.task is None or True
    await asyncio.sleep(0)
    result = await flow._history[st.flow_id].task   # noqa: SLF001

    assert result.ok, f"{result.reason}: {result.message}"
    assert result.barcode == "TRK-0001"


@pytest.mark.asyncio
async def test_second_button_press_is_ignored(stand):
    """Второе нажатие во время проверки не заводит второй проход."""
    await stand["link"].start()
    stand["link"].scan_delay = 1.0
    flow = stand["flow"]

    first = flow.handle_device_event("GATE-01", "BUTTON")
    await asyncio.sleep(0.3)
    second = flow.handle_device_event("GATE-01", "BUTTON")

    assert first is not None
    assert second is None, "второе нажатие завело лишнюю проверку"
    await first.task


@pytest.mark.asyncio
async def test_lcd_messages_are_sent_to_the_post(stand):
    """На экран поста уходят строки на азербайджанском из config.yaml."""
    await stand["link"].start()
    flow = stand["flow"]
    flow.lcd = {
        "wait1": "Kameraya baxın", "wait2": "Gözləyin...",
        "ok2": "Kodu skan edin", "saved1": "Yük qeyd edildi",
        "deny1": "Giriş qadağandır", "hold_ms": 1000,
    }

    sent = []
    orig = stand["link"].send_command

    async def spy(cmd):
        sent.append(cmd)
        await orig(cmd)

    stand["link"].send_command = spy

    st = await flow.run(flow.create("GATE-01"))
    assert st.ok

    joined = "\n".join(sent)
    assert "LCD1 Kameraya baxın" in joined, "экран не получил «смотрите в камеру»"
    assert "LCD2 Kodu skan edin" in joined, "экран не получил просьбу отсканировать код"
    assert "LCD1 Yük qeyd edildi" in joined, "экран не получил итог"

    # Просьба отсканировать обязана прийти ПОСЛЕ опознания, а не раньше:
    # иначе экран просил бы код у человека, которого система не узнала.
    assert joined.index("Kodu skan edin") < joined.index("Yük qeyd edildi")


@pytest.mark.asyncio
async def test_rejected_shows_denial_and_no_scan_prompt(stand):
    """Незнакомому человеку экран не предлагает сканировать код."""
    for p in stand["faces"].glob("*"):
        p.unlink()
    cv2.imwrite(str(stand["faces"] / "stranger.jpg"), _face((40, 200, 90)))

    await stand["link"].start()
    flow = stand["flow"]
    flow.lcd = {"deny1": "Giriş qadağandır", "ok2": "Kodu skan edin", "hold_ms": 1000}

    sent = []
    orig = stand["link"].send_command

    async def spy(cmd):
        sent.append(cmd)
        await orig(cmd)

    stand["link"].send_command = spy
    st = await flow.run(flow.create("GATE-01"))

    joined = "\n".join(sent)
    assert not st.ok
    assert "Giriş qadağandır" in joined
    assert "Kodu skan edin" not in joined, "экран просил код у неопознанного!"


@pytest.mark.asyncio
async def test_short_real_barcode_is_recorded(stand):
    """Короткий, но настоящий номер груза должен попасть в журнал.

    Обратная сторона предыдущего теста и причина реальной жалобы: «сканер
    сканирует, а в базу не отправляет». Отбор обрывков был построен на одной
    длине, порог стоял 4, и трёхсимвольный код молча выбрасывался — снаружи
    это выглядело как «штрихкод не поднесли».

    Теперь решающая проверка другая: в коде должна быть буква или цифра.
    Длина осталась страховкой от служебного кадра, который даёт максимум
    два печатных символа.
    """
    await stand["link"].start()
    stand["link"].ack_artifact = "("       # обрывок по-прежнему должен отсеяться
    stand["link"].fixed_barcode = "A17"    # а вот это — настоящий код
    stand["link"].scan_delay = 0.4

    st = await stand["flow"].run(stand["flow"].create("GATE-01"))

    assert st.ok, f"{st.reason}: {st.message}"
    assert st.barcode == "A17"

    rows = stand["service"].db.list_scan_events()
    assert len(rows) == 1
    assert rows[0]["barcode"] == "A17"


def test_barcode_filter_rules():
    """Правила отбора по отдельности, без сценария и без железа."""
    from core.flow import _looks_like_barcode as ok

    # Обрывки служебных кадров: одиночные знаки препинания.
    assert not ok("(", 3)
    assert not ok("$", 3)
    assert not ok("$(", 3)
    # Знаки препинания любой длины кодом не являются.
    assert not ok("-----", 3)
    # Настоящие коды — в том числе короткие.
    assert ok("A17", 3)
    assert ok("TRK-0001", 3)
    assert ok("1234567890128", 3)
