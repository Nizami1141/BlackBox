"""
Проверка живости (anti-spoofing) — ЗАГЛУШКА.

ЭТО ОСОЗНАННО НЕ РЕАЛИЗОВАНО В БЕТЕ. Прочитайте docs/ARCHITECTURE.md §3.2.

Сейчас систему обманывает распечатанное фото или экран телефона, поднесённый
к камере. Для демонстрации это нормально, для эксплуатации на границе — нет.

Как включить нормальную проверку (в порядке возрастания стоимости):

1. Challenge-response (без нового железа)
   Сервер в ответ на /verify возвращает "challenge": "blink" | "turn_left",
   ESP32 снимает серию из ~10 кадров за 2 секунды и шлёт на /verify/sequence.
   Сервер считает EAR (eye aspect ratio) по landmarks или сравнивает позу
   головы между кадрами. Ломается видеозаписью, но отсекает печать.

2. Пассивная CNN-модель (рекомендуется)
   MiniFASNetV2 (Silent-Face-Anti-Spoofing, MIT-лицензия) — ONNX ~1.9 МБ,
   ~25 мс на CPU. Вход: кроп лица с расширением бокса в 2.7x.
   Подключается ровно в `LivenessChecker.check()` ниже — весь остальной код
   менять не надо.

3. Второй сенсор
   ИК-камера или depth (структурированный свет) рядом с RGB. Самое надёжное.
   Требует переработки железа поста.
"""
from __future__ import annotations

import logging
from dataclasses import dataclass

import numpy as np

log = logging.getLogger(__name__)


@dataclass
class LivenessResult:
    is_live: bool
    score: float
    reason: str = ""


class LivenessChecker:
    def __init__(self, enabled: bool, mode: str = "passive_stub"):
        self.enabled = enabled
        self.mode = mode
        if enabled and mode == "passive_stub":
            log.warning(
                "liveness.enabled=true, но реализация — заглушка. "
                "Подключите настоящую модель перед вводом в эксплуатацию."
            )

    def check(self, bgr: np.ndarray, bbox: tuple[int, int, int, int]) -> LivenessResult:
        if not self.enabled:
            return LivenessResult(True, 1.0, "disabled")

        # ------------------------------------------------------------------
        # МЕСТО ДЛЯ РЕАЛЬНОЙ МОДЕЛИ. Пример подключения MiniFASNet:
        #
        #   crop = expand_and_crop(bgr, bbox, scale=2.7, size=(80, 80))
        #   blob = cv2.dnn.blobFromImage(crop, 1/1.0, (80, 80), swapRB=False)
        #   self.net.setInput(blob)
        #   p = softmax(self.net.forward())[0]      # [spoof_print, live, spoof_replay]
        #   return LivenessResult(p[1] > 0.6, float(p[1]))
        # ------------------------------------------------------------------

        # Пока — очень грубая эвристика "на всякий случай": экран телефона
        # почти всегда даёт заметный муар и низкую вариативность резкости.
        # Это НЕ защита, а индикатор для оператора.
        x, y, w, h = bbox
        crop = bgr[max(0, y):y + h, max(0, x):x + w]
        if crop.size == 0:
            return LivenessResult(False, 0.0, "empty_crop")
        import cv2

        gray = cv2.cvtColor(crop, cv2.COLOR_BGR2GRAY)
        sharpness = float(cv2.Laplacian(gray, cv2.CV_64F).var())
        score = min(1.0, sharpness / 120.0)
        return LivenessResult(score > 0.35, score, "sharpness_heuristic")
