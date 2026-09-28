"""
Сценарий одной проверки: от нажатия кнопки на сайте до записи в журнале.

Весь порядок действий собран здесь, в одном месте, а не размазан по
обработчикам HTTP. Причина простая: этот порядок — и есть суть системы,
и любое его нарушение (например, «включить сканер до опознания») ломает
юридический смысл записи в журнале. Такое должно быть видно в одном файле.

Порядок:

    1. занять пост (на одном посту одновременно только одна проверка)
    2. выбросить старые штрихкоды из очереди
    3. VERIFY -> кадр с платы
    4. детекция лица + эмбеддинг + поиск по векторной базе  (в отдельном
       потоке: это единственная тяжёлая операция, ~40-150 мс, и блокировать
       ею цикл asyncio нельзя — сайт бы подвисал)
    5а. не узнали -> SCAN OFF, отказ, конец
    5б. узнали    -> сервер выдал session token, SCAN ON
    6. ждём штрихкод (scan_timeout)
    7. записываем scan_event по токену
    8. SCAN OFF, конец

На каждом шаге наружу летит событие: сайт показывает стадию, не дожидаясь
конца. Это важно, потому что весь проход занимает 5–50 секунд, и
«крутилка» без подписей выглядит как зависание.
"""
from __future__ import annotations

import asyncio
import base64
import logging
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

from link import DeviceLink, LinkError

from .service import CheckpointService, VerifyResult

log = logging.getLogger(__name__)

# Стадии. Строки уходят в браузер как есть — не переименовывайте их,
# не поправив web/assets/checkpoint.js.
ST_START = "started"
ST_CAPTURE = "capturing"
ST_PHOTO = "photo"
ST_RECOGNIZE = "recognizing"
ST_MATCHED = "matched"
ST_REJECTED = "rejected"
ST_AWAIT_SCAN = "awaiting_scan"
ST_SCANNED = "scanned"
ST_DONE = "done"
ST_ERROR = "error"


def _looks_like_barcode(text: str, min_len: int) -> bool:
    """Отличить настоящий код от обрывка служебного кадра сканера.

    По UART от GM67 в Host Mode ходят не только данные: на каждую команду
    он отвечает подтверждением 04 D0 04 00 FF 28. После фильтра «только
    печатный ASCII» от такого кадра остаются один-два байта контрольной
    суммы — знаки препинания вроде '(' или '$'. Записать в журнал груз
    с номером «$» гораздо хуже, чем подождать настоящий код.

    Проверок две, и ни одна не смотрит на время прихода — только на саму
    строку. Раньше и прошивка, и сервер полагались ещё и на «окно тишины»
    после включения луча, и именно оно съедало коды у тех, кто поднёс груз
    заранее. Разбор — в firmware/src/gm67.cpp у acceptable().

        1. Длина. Служебный кадр даёт максимум два печатных символа.
        2. Есть буква или цифра. В номере груза они есть всегда,
           в обрывке контрольной суммы — далеко не обязательно.
    """
    if len(text) < min_len:
        return False
    return any(ch.isalnum() for ch in text)


def _short_name(full_name: str) -> str:
    """ФИО для экрана 16 знакомест.

    «Алиев Рашад Немятович» целиком не влезет, а обрезка посередине слова
    выглядит как ошибка. Поэтому: фамилия и инициал имени.
    """
    parts = [p for p in full_name.replace(",", " ").split() if p]
    if not parts:
        return ""
    if len(parts) == 1:
        return parts[0][:16]
    short = f"{parts[0]} {parts[1][0]}."
    return short[:16] if len(short) <= 16 else parts[0][:16]


@dataclass
class FlowState:
    flow_id: str
    device_id: str
    stage: str = ST_START
    started_at: float = field(default_factory=time.time)
    finished_at: float = 0.0
    ok: bool = False
    reason: str = ""
    message: str = ""
    employee: Optional[dict] = None
    score: float = 0.0
    barcode: str = ""
    record_id: int = 0
    frame_jpeg: Optional[bytes] = None
    task: Optional["asyncio.Task"] = None      # чтобы «Отмена» реально прерывала

    def public(self) -> dict:
        """То, что видит браузер. Кадр отдельным полем, чтобы не гонять
        одну и ту же картинку в каждом событии."""
        return {
            "flow_id": self.flow_id,
            "device_id": self.device_id,
            "stage": self.stage,
            "ok": self.ok,
            "reason": self.reason,
            "message": self.message,
            "employee": self.employee,
            "score": round(self.score, 4),
            "barcode": self.barcode,
            "record_id": self.record_id,
            "elapsed_ms": int((time.time() - self.started_at) * 1000),
        }


class CheckpointFlow:
    """Оркестратор. По одному экземпляру на сервер."""

    def __init__(self, service: CheckpointService, registry,
                 photo_timeout: float = 15.0, scan_timeout: float = 45.0,
                 cooldown: float = 2.0, min_barcode_len: int = 3,
                 lcd: Optional[dict] = None):
        self.service = service
        self.registry = registry
        self.photo_timeout = photo_timeout
        self.scan_timeout = scan_timeout
        self.cooldown = cooldown
        self.min_barcode_len = min_barcode_len
        # Тексты для экрана поста. Живут в config.yaml, а не в прошивке:
        # поправить формулировку на азербайджанском можно, не перепрошивая
        # посты. Плата переводит UTF-8 в свои знакоместа сама.
        self.lcd = lcd or {}

        self._locks: dict[str, asyncio.Lock] = {}
        self._last_finish: dict[str, float] = {}
        self._active: dict[str, FlowState] = {}     # по device_id
        self._history: dict[str, FlowState] = {}    # по flow_id, последние N
        self._emit: list[Callable[[dict], Any]] = []

    # --------------------------------------------------------------- экран
    def _text(self, key: str, default: str = "") -> str:
        return str(self.lcd.get(key, default))

    async def _lcd(self, link, line1: str, line2: str = "", hold_ms: int = 0) -> None:
        """Показать сообщение на посту. Экран есть не у всех — ошибку глотаем."""
        try:
            await link.lcd(line1, line2, hold_ms)
        except Exception:  # noqa: BLE001
            log.debug("экран поста %s недоступен", link.device_id)

    # ------------------------------------------------------------- события
    def on_event(self, fn: Callable[[dict], Any]) -> None:
        self._emit.append(fn)

    def _publish(self, st: FlowState, extra: Optional[dict] = None) -> None:
        payload = {"type": "flow", **st.public()}
        if extra:
            payload.update(extra)
        for fn in list(self._emit):
            try:
                fn(payload)
            except Exception:  # noqa: BLE001
                log.exception("Подписчик сценария упал")

    def _hold_ms(self) -> int:
        return int(self.lcd.get("hold_ms", 4000))

    def _stage(self, st: FlowState, stage: str, extra: Optional[dict] = None) -> None:
        st.stage = stage
        log.info("[flow %s] %s", st.flow_id[:8], stage)
        self._publish(st, extra)

    # --------------------------------------------------------------- доступ
    def get(self, flow_id: str) -> Optional[FlowState]:
        return self._history.get(flow_id)

    def active(self, device_id: str) -> Optional[FlowState]:
        return self._active.get(device_id)

    def _lock(self, device_id: str) -> asyncio.Lock:
        lock = self._locks.get(device_id)
        if lock is None:
            lock = asyncio.Lock()
            self._locks[device_id] = lock
        return lock

    # ------------------------------------------------------------ основное
    def create(self, device_id: str) -> FlowState:
        """Завести проход и сразу вернуть его id.

        Отделено от run() специально: сайту нужен flow_id немедленно, ещё
        до того как начнётся ожидание кадра. Иначе браузеру пришлось бы
        угадывать, к какому проходу относятся приходящие события.
        """
        st = FlowState(flow_id=uuid.uuid4().hex, device_id=device_id)
        self._history[st.flow_id] = st
        # Держим не больше 200 проходов: кадры лежат в памяти, а не на диске.
        if len(self._history) > 200:
            oldest = min(self._history.values(), key=lambda s: s.started_at)
            self._history.pop(oldest.flow_id, None)
        return st

    async def run(self, st: FlowState) -> FlowState:
        device_id = st.device_id
        link: DeviceLink = self.registry.get(device_id)

        lock = self._lock(device_id)
        if lock.locked():
            st.stage, st.reason = ST_ERROR, "DEVICE_BUSY"
            st.message = "На этом посту уже идёт проверка"
            self._publish(st)
            return st

        async with lock:
            # Пауза между проходами. Без неё двойной клик по кнопке даёт
            # два кадра подряд, и второй прилетает, пока сотрудник ещё
            # отходит от камеры, — получается ложный отказ в журнале.
            since = time.time() - self._last_finish.get(device_id, 0.0)
            if since < self.cooldown:
                await asyncio.sleep(self.cooldown - since)

            self._active[device_id] = st
            st.task = asyncio.current_task()
            try:
                await self._run_locked(link, st)
            except asyncio.CancelledError:
                st.reason, st.message = "CANCELLED", "Проверка отменена оператором"
                self._stage(st, ST_ERROR)
                # Дальше не пробрасываем: отмена — это штатный исход,
                # а не авария сервера.
            except LinkError as e:
                st.reason, st.message = e.code, e.message
                self._stage(st, ST_ERROR)
            except Exception as e:  # noqa: BLE001
                log.exception("Сценарий упал")
                st.reason, st.message = "INTERNAL", str(e)
                self._stage(st, ST_ERROR)
            finally:
                # Луч гасим при любом исходе, в том числе при аварии.
                # Иначе после сбоя сканер останется включённым и груз можно
                # будет отметить без сотрудника.
                try:
                    await link.set_scanner(False)
                except LinkError:
                    pass
                st.finished_at = time.time()
                self._last_finish[device_id] = st.finished_at
                self._active.pop(device_id, None)
        return st

    async def _run_locked(self, link: DeviceLink, st: FlowState) -> None:
        self._stage(st, ST_START)

        # --- 1. кадр --------------------------------------------------------
        link.drain_barcodes()
        self._stage(st, ST_CAPTURE)
        await self._lcd(link, self._text("wait1", "Kameraya baxın"),
                        self._text("wait2", "Gözləyin..."))
        photo = await link.request_photo(self.photo_timeout)
        st.frame_jpeg = photo.data
        self._stage(st, ST_PHOTO, {
            "width": photo.width,
            "height": photo.height,
            "bytes": len(photo.data),
            # Кадр отдаём сразу вместе с событием: оператор должен видеть,
            # ЧТО именно ушло на распознавание, даже если лица там нет.
            "frame": "data:image/jpeg;base64," +
                     base64.b64encode(photo.data).decode("ascii"),
        })

        # --- 2. распознавание ----------------------------------------------
        self._stage(st, ST_RECOGNIZE)
        await self._lcd(link, self._text("search1", "Yoxlanılır..."),
                        self._text("search2", ""))
        res: VerifyResult = await asyncio.to_thread(
            self.service.verify_jpeg, photo.data, st.device_id
        )
        st.score = res.score
        st.reason = res.reason
        st.message = res.message

        if not res.matched:
            # На экране — только вердикт, без подробностей: сотрудник у поста
            # не должен видеть ни оценку схожести, ни кто на него похож.
            # Всё это уходит оператору на сайт и в журнал.
            await self._lcd(link,
                            self._text("deny1", "Giriş qadağandır"),
                            self._text("deny2", "Bazada yoxdur"),
                            self._hold_ms())
            self._stage(st, ST_REJECTED, {"candidates": res.candidates or []})
            return

        st.employee = {
            "id": res.employee_id,
            "employee_no": res.employee_no,
            "full_name": res.full_name,
            "position": res.position,
        }
        self._stage(st, ST_MATCHED, {
            "session_expires_in": res.expires_in,
            "candidates": res.candidates or [],
        })

        # --- 3. штрихкод ----------------------------------------------------
        # Сканер включаем ТОЛЬКО здесь — после того, как лицо опознано
        # и сервер выдал токен. Это техническая гарантия связки «лицо → груз».
        #
        # Экран показывает имя и просьбу отсканировать штрихкод — ровно то,
        # ради чего он и поставлен: на границе рядом не будет ни оператора,
        # ни монитора, и сотрудник должен понять, что делать дальше.
        await self._lcd(link, _short_name(res.full_name),
                        self._text("ok2", "Kodu skan edin"))
        await link.set_scanner(True)
        self._stage(st, ST_AWAIT_SCAN, {"timeout_s": self.scan_timeout})

        # Ждать дольше, чем живёт токен, бессмысленно: сервер всё равно
        # откажет с SESSION_EXPIRED.
        wait = min(self.scan_timeout, max(1.0, res.expires_in - 1))
        deadline = time.time() + wait

        # Цикл, а не одно ожидание: по UART от сканера приходят не только
        # данные, но и служебные кадры — подтверждения команд. Их обрывки
        # после фильтра «только печатный ASCII» выглядят как код из одного
        # символа вроде '(' или '$'. Прошивка такое отсекает сама, но сервер
        # не должен полагаться на то, что на посту стоит свежая прошивка:
        # записать в журнал груз с номером «$» гораздо хуже, чем подождать.
        bc = None
        while True:
            left = deadline - time.time()
            if left <= 0:
                st.reason = "SCAN_TIMEOUT"
                st.message = "Штрихкод не поднесли за отведённое время"
                await self._lcd(link, self._text("err1", "Xəta"),
                                self._text("timeout2", "Vaxt bitdi"),
                                self._hold_ms())
                self._stage(st, ST_ERROR)
                return
            try:
                cand = await link.wait_barcode(left)
            except LinkError as e:
                st.reason, st.message = e.code, e.message
                await self._lcd(link, self._text("err1", "Xəta"),
                                self._text("timeout2", "Vaxt bitdi"),
                                self._hold_ms())
                self._stage(st, ST_ERROR)
                return

            text = cand.text.strip()
            if not cand.truncated and not _looks_like_barcode(text,
                                                              self.min_barcode_len):
                log.warning("[flow %s] отброшен обрывок %r (%d симв.) — "
                            "это ответ сканера, а не штрихкод",
                            st.flow_id[:8], text, len(text))
                continue
            bc = cand
            break

        st.barcode = bc.text
        if bc.truncated:
            st.reason = "BARCODE_TRUNCATED"
            st.message = ("Штрихкод не влез в одно BLE-уведомление и пришёл "
                          "обрезанным — запись не сделана")
            self._stage(st, ST_ERROR)
            return

        # --- 4. запись в журнал --------------------------------------------
        ok, err, rec_id = await asyncio.to_thread(
            self.service.register_scan, res.session_token, bc.text, "", st.device_id
        )
        if not ok:
            st.reason = err
            st.message = f"Штрихкод не записан: {err}"
            self._stage(st, ST_ERROR)
            return

        await self._lcd(link, self._text("saved1", "Yük qeyd edildi"),
                        bc.text[:16], self._hold_ms())
        st.ok = True
        st.record_id = rec_id
        st.reason = ""
        st.message = (f"{res.full_name} проверил груз {bc.text}")
        self._stage(st, ST_SCANNED, {"record_id": rec_id})
        self._stage(st, ST_DONE)

    # ------------------------------------------------ события с самого поста
    def handle_device_event(self, device_id: str, name: str) -> Optional[FlowState]:
        """Кнопка на посту. Возвращает начатый проход или None.

        Здесь, а не в обработчике HTTP, специально: нажатие кнопки у камеры
        и нажатие кнопки на сайте обязаны запускать ОДИН И ТОТ ЖЕ сценарий.
        Если бы у них были разные пути в коде, они бы со временем разошлись,
        и «на посту работает не так, как на пульте» стало бы отдельным
        источником ошибок. На границе оператора рядом не будет — там
        единственный способ начать проверку это кнопка.
        """
        name = (name or "").strip().upper()

        if name == "CANCEL":
            asyncio.create_task(self.cancel(device_id))
            return None

        if name != "BUTTON":
            log.debug("[%s] неизвестное событие %r — пропускаю", device_id, name)
            return None

        if self.active(device_id) is not None:
            # Нажали второй раз, пока идёт проверка. Это не ошибка: человек
            # просто торопится. Молча игнорируем, не заводя лишний проход.
            log.info("[%s] кнопка нажата, но проверка уже идёт", device_id)
            return None

        try:
            self.registry.get(device_id)
        except LinkError:
            return None

        st = self.create(device_id)
        asyncio.create_task(self.run(st))
        log.info("[%s] проверка начата кнопкой на посту", device_id)
        return st

    # ------------------------------------------------------------- отмена
    async def cancel(self, device_id: str) -> bool:
        """Оператор нажал «Отмена».

        Гасим луч и снимаем задачу. Порядок именно такой: сначала луч,
        потому что снятие задачи может не успеть выполниться до того, как
        кто-то поднесёт штрихкод.
        """
        ok = False
        try:
            link = self.registry.get(device_id)
            await link.set_scanner(False)
            ok = True
        except LinkError:
            pass

        st = self._active.get(device_id)
        if st is not None and st.task is not None and not st.task.done():
            st.task.cancel()
            ok = True
        return ok
