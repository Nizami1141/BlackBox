/* ==========================================================================
   Страница поста: кнопка «Проверить» и показ хода проверки.

   Ключевая мысль этой страницы: HTTP-запрос на /api/web/check возвращается
   мгновенно, он только запускает сценарий. Всё остальное — стадии, кадр,
   вердикт, штрихкод — приходит событиями по WebSocket. Проход занимает
   от 5 до 50 секунд, и держать на нём открытый запрос было бы неверно:
   при разрыве браузер потерял бы результат уже сделанной работы.
   ========================================================================== */

const STAGES = ["capturing", "recognizing", "awaiting_scan", "done"];

let currentFlow = null;
let devices = [];
let busy = false;
let autonomousTimeout = null;
let manualDeviceChoice = false;

/* ------------------------------------------------------------- состояние */
async function refreshState() {
    try {
        const st = await API.get("/api/web/state");
        devices = st.devices || [];
        renderDevices(devices, "dot", "devtext");
        renderDeviceSelect();
        warnStale(st.stale_employees);

        // Порог и бэкенд полезно видеть на самой странице: когда «не узнаёт»,
        // первый вопрос всегда про них.
        const v = el("verdict");
        if (!currentFlow && v && !busy) {
            v.querySelector(".meta").textContent = st.recognition_source === "central_aks"
                ? "Распознавание и решение о живости — центральный AKS; локальная база не используется"
                : `${st.employees} сотрудников в базе · порог ${st.threshold} · ` +
                  `модель ${st.backend}` +
                  (st.liveness ? "" : " · проверка живости выключена");
        }
    } catch (e) {
        renderDevices([], "dot", "devtext");
    }
}

function renderDeviceSelect() {
    const sel = el("device");
    const prev = sel.value;
    sel.innerHTML = "";
    if (!devices.length) {
        sel.innerHTML = '<option value="">постов нет</option>';
        return;
    }
    for (const d of devices) {
        const o = document.createElement("option");
        o.value = d.device_id;
        if (d.autonomous) {
            // Пост сам опрашивает сервер и забирает команду с сайта.
            o.textContent = `${d.device_id} · Ethernet` +
                            (d.connected ? "" : " (давно не выходил на связь)");
            o.disabled = !d.triggerable;
        } else {
            o.textContent = `${d.device_id} · ${d.kind}` +
                            (d.connected ? "" : " (нет связи)");
            o.disabled = !d.connected;
        }
        sel.appendChild(o);
    }
    // По умолчанию предпочитаем настоящий пост заглушке. Ручной выбор
    // оператора сохраняем даже на время проверки, когда пост перестаёт
    // опрашивать сервер и временно становится untriggerable.
    const usable = (d) => d.connected && (!d.autonomous || d.triggerable);
    const previousConnected = devices.find((d) => d.device_id === prev && d.connected);
    const realPost = devices.find((d) => d.autonomous && usable(d));
    const selected = ((manualDeviceChoice || busy) && previousConnected)
        ? previousConnected : (realPost || devices.find(usable));
    sel.value = selected?.device_id || "";

    const btn = el("btnCheck");
    if (btn) {
        btn.disabled = busy || !selected || !usable(selected);
        btn.title = selected && usable(selected) ? "" :
            "Выбранный пост сейчас не опрашивает сервер";
    }
}

// Предупреждение о записях, оставшихся от прежней модели. Показывается
// один раз за сессию вкладки: повторять на каждом опросе состояния незачем.
let staleWarned = false;
function warnStale(ids) {
    if (!ids || !ids.length || staleWarned) return;
    staleWarned = true;
    toast(`${ids.length} сотрудников исключены из поиска: их эталоны сняты ` +
          `другой моделью распознавания. Опознаваться они не будут. ` +
          `Разбор: python -m tools.db_doctor`, "err");
}

/* ----------------------------------------------------------------- стадии */
function resetStages() {
    document.querySelectorAll("#stages li").forEach((li) => {
        li.className = "";
    });
}

function markStage(stage, failed) {
    const idx = STAGES.indexOf(stage);
    document.querySelectorAll("#stages li").forEach((li, i) => {
        li.className = "";
        if (i < idx) li.classList.add("done");
        else if (i === idx) li.classList.add(failed ? "fail" : "active");
    });
    if (stage === "done") {
        document.querySelectorAll("#stages li").forEach((li) => {
            li.className = "done";
        });
    }
}

function markFailAt(stage) {
    const idx = STAGES.indexOf(stage);
    document.querySelectorAll("#stages li").forEach((li, i) => {
        li.className = i < idx ? "done" : (i === idx ? "fail" : "");
    });
}

/* ---------------------------------------------------------------- вердикт */
function verdict(cls, who, meta, code) {
    const v = el("verdict");
    v.className = "verdict" + (cls ? " " + cls : "");
    v.innerHTML =
        `<div class="who">${esc(who)}</div>` +
        `<div class="meta">${esc(meta || "")}</div>` +
        (code ? `<div class="code">${esc(code)}</div>` : "");
}

function logLine(text) {
    const box = el("log");
    const t = new Date();
    const p = (n) => String(n).padStart(2, "0");
    box.textContent = `${p(t.getHours())}:${p(t.getMinutes())}:${p(t.getSeconds())}  ` +
                      text + "\n" + box.textContent;
    // Не даём логу расти бесконечно: вкладку держат открытой сутками.
    if (box.textContent.length > 20000) {
        box.textContent = box.textContent.slice(0, 20000);
    }
}

/* --------------------------------------------------------------- события */
/* ------------------------------------------------- события автономного поста
   У автономного поста инициатива своя: человек жмёт кнопку у камеры, плата
   сама ходит на /api/recognize и /api/log_cargo. Сценария (flow) на сервере
   при этом нет — но сервис публикует события verify и scan, и страница
   обязана их показывать. Иначе пост работает, журнал пополняется, а сайт
   стоит немой, будто ничего не происходит, — ровно так и было.        */

function showPostFrame(deviceId) {
    // Кадр берём из последнего снимка поста. Задержка не для красоты:
    // событие verify публикуется из рабочего потока ДО того, как endpoint
    // сохранит кадр, и мгновенный запрос может получить 404.
    setTimeout(() => {
        const img = new Image();
        img.alt = "кадр с поста";
        img.onload = () => { el("frame").innerHTML = ""; el("frame").appendChild(img); };
        img.src = `/api/web/last-frame/${encodeURIComponent(deviceId)}.jpg?t=${Date.now()}`;
    }, 300);
}

function onAutonomousVerify(ev) {
    // Оператор в этот момент гоняет управляемый пост со страницы — не мешаем.
    if (currentFlow) return;

    resetStages();
    showPostFrame(ev.device_id);
    logLine(`пост ${ev.device_id}: кадр обработан за ${ev.latency_ms} мс`);
    const scoreText = ev.score == null ? "" :
        ` · схожесть ${Number(ev.score).toFixed(3)}`;
    const sourceText = ev.source === "central" ? " · центральный AKS" : "";

    if (ev.matched) {
        markStage("awaiting_scan");
        verdict("ok", ev.full_name,
                `таб. № ${ev.employee_no}${scoreText}` +
                ` · пост ${ev.device_id}${sourceText} · поднесите штрихкод`);
        logLine(`опознан: ${ev.full_name}${scoreText}${sourceText}`);
    } else {
        markFailAt("recognizing");
        const why = ev.reason === "NO_FACE" ? "лицо в кадре не найдено"
                  : ev.reason === "NOT_FOUND" ? "сотрудник не найден в базе"
                  : (ev.message || ev.reason);
        verdict("bad", "Доступ запрещён",
                `${why}${scoreText} · пост ${ev.device_id}${sourceText}`);
        logLine(`отказ: ${ev.reason}${scoreText}${sourceText}`);
        if (busy) finish();
    }
}

function onAutonomousScan(ev) {
    if (currentFlow) return;

    if (ev.ok) {
        markStage("done");
        verdict("ok", ev.full_name || "Записано",
                `груз записан ${ev.source === "central" ? "в центральный AKS" : "в журнал"} · пост ${ev.device_id}`, ev.barcode);
        logLine(`штрихкод ${ev.barcode} → ${ev.source === "central" ? "центральный log_id" : "запись №"} ${ev.record_id}`);
        if (ev.source !== "central") loadRecent();
    } else {
        verdict("bad", "Штрихкод не записан",
                `${ev.reason} · пост ${ev.device_id}`, ev.barcode);
        logLine(`скан отвергнут: ${ev.reason} (${ev.barcode})`);
    }
    if (busy) finish();
}

function onEvent(ev) {
    if (ev.type === "device") {
        refreshState();
        logLine(`пост ${ev.device_id}: ${ev.connected ? "на связи" : "связь потеряна"}`);
        return;
    }
    if (ev.type === "status") return;          // телеметрия платы, не шумим
    if (ev.type === "verify") { onAutonomousVerify(ev); return; }
    if (ev.type === "scan")   { onAutonomousScan(ev);   return; }
    if (ev.type !== "flow") return;

    // События чужих проходов игнорируем: постов может быть несколько,
    // а вкладка показывает один.
    if (currentFlow && ev.flow_id !== currentFlow) return;

    switch (ev.stage) {
    case "started":
        resetStages();
        verdict("wait", "Проверка началась", `пост ${ev.device_id}`);
        break;

    case "capturing":
        markStage("capturing");
        verdict("wait", "Снимаю…", "смотрите в камеру, не двигайтесь");
        break;

    case "photo":
        el("frame").innerHTML = `<img src="${ev.frame}" alt="кадр">`;
        logLine(`кадр ${ev.width}×${ev.height}, ${(ev.bytes / 1024).toFixed(1)} КБ`);
        break;

    case "recognizing":
        markStage("recognizing");
        verdict("wait", "Ищу в базе…", "");
        break;

    case "matched":
        markStage("awaiting_scan");
        verdict("ok", ev.employee.full_name,
                `таб. № ${ev.employee.employee_no}` +
                (ev.employee.position ? ` · ${ev.employee.position}` : "") +
                ` · схожесть ${ev.score}`);
        logLine(`опознан: ${ev.employee.full_name} (${ev.score})`);
        break;

    case "rejected":
        markFailAt("recognizing");
        verdict("bad", "Доступ запрещён",
                `${ev.message || "сотрудник не найден"} · лучшая схожесть ${ev.score}`);
        logLine(`отказ: ${ev.reason} (${Number(ev.score).toFixed(3)})`);
        finish();
        break;

    case "awaiting_scan": {
        markStage("awaiting_scan");
        const v = el("verdict");
        const meta = v.querySelector(".meta");
        if (meta) meta.textContent += " · поднесите штрихкод груза";
        logLine(`сканер включён на ${ev.timeout_s} с`);
        break;
    }

    case "scanned":
        verdict("ok", ev.employee ? ev.employee.full_name : "Записано",
                "груз записан в журнал", ev.barcode);
        logLine(`штрихкод ${ev.barcode} → запись №${ev.record_id}`);
        break;

    case "done":
        markStage("done");
        loadRecent();
        finish();
        break;

    case "error":
        markFailAt(el("stages").querySelector("li.active")?.dataset.stage || "capturing");
        verdict("bad", "Проверка не завершена", ev.message || ev.reason);
        logLine(`ошибка: ${ev.reason} — ${ev.message || ""}`);
        toast(ev.message || ev.reason, "err");
        finish();
        break;
    }
}

function finish() {
    if (autonomousTimeout) clearTimeout(autonomousTimeout);
    autonomousTimeout = null;
    busy = false;
    currentFlow = null;
    renderDeviceSelect();
    el("btnCancel").disabled = true;
}

/* --------------------------------------------------------------- действия */
async function check() {
    if (busy) return;
    const deviceId = el("device").value;
    const chosen = devices.find((d) => d.device_id === deviceId);
    if (!chosen) return;
    busy = true;
    el("btnCheck").disabled = true;
    el("btnCancel").disabled = chosen.autonomous;
    el("frame").innerHTML = "кадр появится здесь";
    resetStages();
    try {
        const r = await API.post(chosen.autonomous ? "/api/web/trigger" : "/api/web/check",
                                 { device_id: deviceId });
        if (chosen.autonomous) {
            verdict("wait", "Команда отправлена", "жду кадр с поста");
            autonomousTimeout = setTimeout(() => {
                if (busy && !currentFlow) {
                    verdict("bad", "Нет ответа от поста", "проверьте Ethernet и питание ESP32");
                    finish();
                }
            }, 120000);
        } else {
            currentFlow = r.flow_id;
        }
        logLine(`запущена проверка на посту ${r.device_id}`);
    } catch (e) {
        verdict("bad", "Не удалось начать", e.message);
        toast(e.message, "err");
        finish();
    }
}

async function cancel() {
    try {
        await API.post("/api/web/cancel", { device_id: el("device").value });
        logLine("проверка отменена оператором");
    } catch (e) {
        toast(e.message, "err");
    }
}

async function loadRecent() {
    try {
        const r = await API.get("/api/web/journal?limit=8");
        const tb = el("recent");
        if (!r.items.length) {
            tb.innerHTML = '<tr><td colspan="4">записей пока нет</td></tr>';
            return;
        }
        tb.innerHTML = r.items.map((it) =>
            `<tr><td>${fmtTime(it.ts)}</td>` +
            `<td>${esc(it.full_name || "?")}</td>` +
            `<td class="mono">${esc(it.barcode)}</td>` +
            `<td>${esc(it.device_id)}</td></tr>`).join("");
    } catch (e) { /* журнал не критичен для работы поста */ }
}

/* --------------------------------------------------- связь по Ethernet ----
   Пост опрашивает сервер за командой, затем сам присылает кадр и штрихкод.
   Опрос показывает доступность поста, а успешная обработка кадра — всю
   цепочку от команды до модели.                                           */
function fmtAgo(sec) {
    if (sec == null) return "ни разу";
    if (sec < 60) return `${Math.round(sec)} с назад`;
    if (sec < 3600) return `${Math.round(sec / 60)} мин назад`;
    return `${Math.round(sec / 3600)} ч назад`;
}

async function refreshLink() {
    const box = el("linkStatus");
    if (!box) return;
    let r;
    try { r = await API.get("/api/web/link-status"); }
    catch (e) { box.innerHTML = '<span class="tag bad">сервер не отвечает</span>'; return; }

    if (!r.devices.length) {
        box.innerHTML = '<span class="tag">автономных постов в config.yaml нет</span>';
        return;
    }

    box.innerHTML = r.devices.map((d) => {
        const ok = d.online;
        const rows = [
            ["адрес поста", d.last_ip || "—"],
            ["последний контакт", fmtAgo(d.seconds_ago)],
            ["проверок принято", d.checks_total || 0],
            ["из них опознано", d.checks_matched || 0],
        ];
        if (d.avg_latency_ms != null) rows.push(["средняя обработка", `${d.avg_latency_ms} мс`]);
        if (d.last_frame_bytes != null) {
            rows.push(["последний кадр",
                `${(d.last_frame_bytes / 1024).toFixed(1)} КБ · ` +
                `<a href="/api/web/last-frame/${encodeURIComponent(d.device_id)}.jpg" ` +
                `target="_blank">открыть</a>`]);
        }
        return `<div style="margin-bottom:8px">` +
            `<b>${esc(d.device_id)}</b> ` +
            `<span class="tag ${ok ? "ok" : "bad"}">` +
            `${ok ? "на связи" : "молчит"}</span></div>` +
            `<table>` + rows.map(([k, v]) =>
                `<tr><td style="color:var(--muted)">${k}</td>` +
                `<td class="mono">${v}</td></tr>`).join("") +
            `</table>`;
    }).join("<hr style='border:0;border-top:1px solid var(--border);margin:14px 0'>");
}

/* ------------------------------------------------------------------ старт */
el("btnCheck").addEventListener("click", check);
el("btnCancel").addEventListener("click", cancel);
el("device").addEventListener("change", () => { manualDeviceChoice = true; });

// Пробел как горячая клавиша: оператору удобнее, чем целиться мышью,
// когда перед камерой очередь.
document.addEventListener("keydown", (e) => {
    if (e.code === "Space" && e.target === document.body) {
        e.preventDefault();
        check();
    }
});

new Live(onEvent);
refreshState();
loadRecent();
refreshLink();
setInterval(refreshState, 5000);
// Состояние связи обновляем отдельно от основного состояния страницы.
setInterval(refreshLink, 10000);
