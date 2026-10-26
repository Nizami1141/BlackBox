/* ==========================================================================
   Страница проверки серверной части.

   Делает ровно то же, что прошивка поста в Ethernet-режиме: multipart POST
   кадра на /api/recognize с X-API-Key, затем multipart POST штрихкода на
   /api/log_cargo с полученным log_id. Никаких особых «тестовых» путей на
   сервере нет и не должно быть — проверять надо ту дорогу, по которой
   ходит плата, а не соседнюю.

   Отсюда и ключ устройства вводится руками: он берётся из прошивки, и
   расхождение с config.yaml — одна из реальных причин, по которым пост
   молчит. Пусть она ловится здесь, а не в логе платы.
   ========================================================================== */

let picked = null;      // {blob, name, url} — выбранный кадр
let stream = null;      // поток веб-камеры, если включена

/* ------------------------------------------------------------------ шаги */
function step(name, state, text) {
    const li = document.querySelector(`.steps li[data-step="${name}"]`);
    if (!li) return;
    li.classList.remove("idle", "ok", "bad");
    if (state) li.classList.add(state);
    li.querySelector(".res").textContent = text || "—";
}

function stepsReset() {
    ["pick", "auth", "face", "match", "token", "scan"]
        .forEach((s) => step(s, "idle", "—"));
}

function logLine(text) {
    const box = el("log");
    const t = new Date().toLocaleTimeString("ru-RU");
    box.textContent += `${t}  ${text}\n`;
    box.scrollTop = box.scrollHeight;
}

/* ---------------------------------------------------------------- вердикт */
function verdict(kind, who, meta) {
    const v = el("verdict");
    v.className = "verdict" + (kind ? " " + kind : "");
    v.innerHTML = `<div class="who">${esc(who)}</div>` +
                  `<div class="meta">${esc(meta || "")}</div>`;
}

function showLcd(l1, l2) {
    // Экран поста — 16 знакомест в строке, длиннее обрезается. Показываем
    // с тем же обрезанием, иначе страница врала бы про то, что увидит человек.
    const cut = (s) => String(s || "").slice(0, 16);
    el("lcd").textContent = cut(l1) + "\n" + cut(l2);
}

/* ------------------------------------------------------------- выбор кадра */
function setPicked(blob, name) {
    if (picked && picked.url) URL.revokeObjectURL(picked.url);
    const url = URL.createObjectURL(blob);
    picked = { blob, name, url };
    el("frame").innerHTML = `<img src="${url}" alt="выбранный кадр">`;
    el("btnRun").disabled = false;
    step("pick", "ok", `${(blob.size / 1024).toFixed(1)} КБ`);
    logLine(`кадр выбран: ${name}, ${blob.size} байт`);
}

async function fromFile(f) {
    if (!f) return;
    // Проверяем сигнатуру, а не расширение: расширение врёт чаще.
    const head = new Uint8Array(await f.slice(0, 2).arrayBuffer());
    if (head[0] !== 0xff || head[1] !== 0xd8) {
        step("pick", "bad", "не JPEG");
        toast("Это не JPEG: первые два байта должны быть FF D8.", "err");
        return;
    }
    setPicked(f, f.name);
}

/* ------------------------------------------------------------- веб-камера */
async function camStart() {
    try {
        stream = await navigator.mediaDevices.getUserMedia({
            video: { width: 1280, height: 960 },
        });
        el("video").srcObject = stream;
        el("btnSnap").disabled = false;
        el("btnCamStart").disabled = true;
        logLine("веб-камера включена");
    } catch (e) {
        toast("Камера недоступна: " + e.message, "err");
    }
}

function camSnap() {
    const v = el("video");
    if (!v.videoWidth) return;
    const c = document.createElement("canvas");
    c.width = v.videoWidth;
    c.height = v.videoHeight;
    c.getContext("2d").drawImage(v, 0, 0);
    c.toBlob((b) => setPicked(b, "webcam.jpg"), "image/jpeg", 0.92);
}

function camStop() {
    if (!stream) return;
    stream.getTracks().forEach((t) => t.stop());
    stream = null;
    el("btnSnap").disabled = true;
    el("btnCamStart").disabled = false;
}

/* --------------------------------------------------- последний кадр с поста */
async function pullFromPost() {
    const dev = el("device").value;
    if (!dev) return;
    const res = await fetch(`/api/web/last-frame/${encodeURIComponent(dev)}.jpg`);
    if (res.status === 404) {
        toast(`От поста ${dev} ещё не приходило кадров. Нажмите кнопку на плате.`, "err");
        return;
    }
    if (!res.ok) {
        toast(`Не удалось забрать кадр: HTTP ${res.status}`, "err");
        return;
    }
    const blob = await res.blob();
    setPicked(blob, `последний кадр ${dev}`);

    // Подпись к кадру — что сервер о нём тогда решил.
    try {
        const info = await API.get(`/api/web/last-frame/${encodeURIComponent(dev)}`);
        logLine(`кадр с поста: ${info.bytes} байт, ${Math.round(info.age_s)} с назад, ` +
                `тогда ответили: ${info.reason || (info.matched ? "опознан" : "—")}`);
    } catch (e) { /* подпись необязательна */ }
}

/* ------------------------------------------------------- основной сценарий */
async function run() {
    if (!picked) return;
    const dev = el("device").value;
    const key = el("key").value.trim();
    const barcode = el("barcode").value.trim();

    if (!key) {
        toast("Введите ключ устройства — тот же, что в прошивке.", "err");
        el("key").focus();
        return;
    }

    el("btnRun").disabled = true;
    stepsReset();
    step("pick", "ok", `${(picked.blob.size / 1024).toFixed(1)} КБ`);
    verdict("wait", "Отправляю…", `${picked.blob.size} байт на /api/recognize`);
    showLcd("Yoxlanilir...", "");

    let r;
    const t0 = performance.now();
    try {
        const form = new FormData();
        form.append("file", picked.blob, "face.jpg");
        const res = await fetch("/api/recognize", {
            method: "POST",
            headers: { "X-API-Key": key },
            body: form,
        });
        const payload = await res.json().catch(() => ({}));

        if (res.status === 401) {
            step("auth", "bad", "401");
            verdict("bad", "X-API-Key не принят",
                    "Ключ отсутствует или не совпадает с aks_api.api_key в config.yaml.");
            logLine("401: X-API-Key отсутствует или неверен");
            el("btnRun").disabled = false;
            return;
        }
        if (res.status === 403) {
            step("auth", "bad", "403");
            verdict("bad", "IP не разрешён",
                    "Источник запроса отсутствует в aks_api.ip_devices.");
            logLine("403: IP не найден в whitelist");
            el("btnRun").disabled = false;
            return;
        }
        if (!res.ok) {
            step("auth", "bad", `HTTP ${res.status}`);
            const detail = payload.detail || payload;
            verdict("bad", `Ответ HTTP ${res.status}`, detail.message || detail.reason || "");
            el("btnRun").disabled = false;
            return;
        }
        r = payload.faces && payload.faces[0] ? payload.faces[0] : {};
        r.matched = Boolean(r.match);
        r.message = r.message || r.reason || "";
        if (r.user) {
            r.name = `${r.user.first_name || ""} ${r.user.last_name || ""}`.trim();
            r.employee_no = r.user.qr_id || "";
        }
    } catch (e) {
        step("auth", "bad", "нет связи");
        verdict("bad", "Сервер не ответил", e.message);
        el("btnRun").disabled = false;
        return;
    }

    const ms = Math.round(performance.now() - t0);
    step("auth", "ok", "200");
    logLine(`verify: HTTP 200 за ${ms} мс, matched=${r.matched}, reason=${r.reason || "—"}`);
    showLcd(r.matched ? r.name : "Giriş qadağandır",
            r.matched ? "Kodu skan edin" : r.message);
    renderCands([]);

    // --- шаг 3: нашлось ли лицо в кадре ---------------------------------
    if (r.reason === "NO_FACE") {
        step("face", "bad", "нет лица");
        verdict("bad", "Лицо в кадре не найдено",
                "Детектор не увидел лица В КАРТИНКЕ — дело в кадре, а не в базе. " +
                "Тёмный, размытый, или лицо занимает слишком мало места.");
        el("btnRun").disabled = false;
        return;
    }
    if (r.reason === "MULTIPLE_FACES") {
        step("face", "bad", "лиц больше одного");
        verdict("bad", "В кадре несколько лиц",
                "Пост такой кадр отвергает намеренно: непонятно, кого пропускать.");
        el("btnRun").disabled = false;
        return;
    }
    if (r.reason === "INDEX_MISMATCH") {
        step("face", "ok", "лицо есть");
        step("match", "bad", "база несовместима");
        verdict("bad", "Векторы разной размерности",
                "В базе записи от прежнего face.backend. Запустите tools.db_doctor");
        el("btnRun").disabled = false;
        return;
    }
    step("face", "ok", "лицо есть");

    // --- шаг 4: опознан ли ------------------------------------------------
    if (!r.matched) {
        step("match", "bad", r.score != null ? `score ${r.score}` : r.reason);
        verdict("bad", "Не опознан",
                r.reason === "NOT_FOUND"
                    ? "Лицо найдено, но такого человека в базе нет. Пайплайн работает — " +
                      "не хватает регистрации на /enroll."
                    : (r.message || r.reason));
        el("btnRun").disabled = false;
        return;
    }
    step("match", "ok", `score ${r.score}`);
    step("token", "ok", `log_id ${r.log_id}`);
    verdict("ok", r.name, `таб. № ${r.employee_no} · схожесть ${r.score} · ${ms} мс`);
    logLine(`опознан: ${r.name}, log_id=${r.log_id}, TTL ${r.expires_in || "—"} с`);

    // --- шаги 5-6: штрихкод ----------------------------------------------
    if (!barcode) {
        step("scan", "idle", "не задан");
        toast("Опознание прошло. Впишите штрихкод, чтобы проверить запись в журнал.", "ok");
        el("btnRun").disabled = false;
        return;
    }

    try {
        const cargo = new FormData();
        cargo.append("log_id", String(r.log_id));
        cargo.append("cargo_barcode", barcode);
        const res = await fetch("/api/log_cargo", {
            method: "POST",
            headers: { "X-API-Key": key },
            body: cargo,
        });
        const s = await res.json().catch(() => ({}));
        if (!res.ok || s.status !== "success") {
            const detail = s.detail || s;
            const err = new Error(detail.message || detail.reason || `HTTP ${res.status}`);
            err.reason = detail.reason;
            throw err;
        }
        step("scan", "ok", `запись №${s.record_id}`);
        showLcd("Yük qeyd edildi", barcode);
        verdict("ok", "Груз записан",
                `${r.name} · ${barcode} · запись №${s.record_id}`);
        logLine(`scan: записано, запись №${s.record_id}`);
        toast("Строка появилась в журнале.", "ok");
    } catch (e) {
        step("scan", "bad", e.reason || "отказ");
        verdict("bad", "Штрихкод не записан",
                e.message + ". Токен одноразовый и привязан к посту — повторный " +
                "скан тем же токеном отвергается, и это правильно.");
        logLine(`scan: отказ — ${e.reason || e.message}`);
    }
    el("btnRun").disabled = false;
}

function renderCands(list) {
    const tb = el("cands");
    if (!list || !list.length) {
        tb.innerHTML = "<tr><td>—</td></tr>";
        return;
    }
    tb.innerHTML = list.map((c) =>
        `<tr><td>${esc(c.full_name)}</td>` +
        `<td class="mono num">${c.score}</td></tr>`).join("");
}

/* ------------------------------------------------------------- состояние */
async function loadState() {
    let st;
    try { st = await API.get("/api/web/state"); }
    catch (e) { el("health").innerHTML = "<dt>сервер</dt><dd>не отвечает</dd>"; return; }

    el("health").innerHTML =
        `<dt>бэкенд</dt><dd>${esc(st.backend)} (${st.dim}-мерный вектор)</dd>` +
        `<dt>порог</dt><dd>${st.threshold}</dd>` +
        `<dt>сотрудников</dt><dd>${st.employees}</dd>` +
        (st.stale_employees
            ? `<dt>непригодных</dt><dd>${st.stale_employees} — векторы другой длины</dd>`
            : "") +
        `<dt>liveness</dt><dd>${st.liveness ? "включён" : "выключен"}</dd>`;

    const aks = st.aks_api || {};
    const devices = aks.ip_devices || {};
    el("network").innerHTML = aks.enabled
        ? `<b>Тестовая сеть</b><br>` +
          `ноутбук: <code>${esc(aks.server_ip)}:${aks.port}</code><br>` +
          Object.entries(devices).map(([ip, id]) =>
              `ESP32/W5500: <code>${esc(ip)}</code> → ${esc(id)}`).join("<br>")
        : "AKS API не настроен в config.yaml";

    el("emptyBase").style.display = st.employees ? "none" : "block";

    // Список постов. Автономный пост тоже в списке: именно его кадр
    // интереснее всего забрать кнопкой «С поста».
    const sel = el("device");
    if (!sel.options.length && st.devices && st.devices.length) {
        sel.innerHTML = st.devices.map((d) =>
            `<option value="${esc(d.device_id)}">${esc(d.device_id)}` +
            ` · ${esc(d.kind)}</option>`).join("");
    }
    renderDevices(st.devices || [], "dot", "devtext");
}

/* ----------------------------------------------------------------- запуск */
document.addEventListener("DOMContentLoaded", () => {
    showLcd("", "");
    stepsReset();

    // переключение источника кадра
    document.querySelectorAll(".src").forEach((s) => {
        s.addEventListener("click", () => {
            document.querySelectorAll(".src").forEach((x) => x.classList.remove("on"));
            s.classList.add("on");
            const which = s.dataset.src;
            el("paneFile").style.display = which === "file" ? "" : "none";
            el("paneCam").style.display = which === "cam" ? "" : "none";
            el("panePost").style.display = which === "post" ? "" : "none";
            if (which !== "cam") camStop();
        });
    });
    el("panePost").style.display = "none";
    el("paneCam").style.display = "none";

    // файл: клик и перетаскивание
    el("pick").addEventListener("click", (e) => { e.preventDefault(); el("file").click(); });
    el("file").addEventListener("change", (e) => fromFile(e.target.files[0]));

    const drop = el("drop");
    ["dragenter", "dragover"].forEach((ev) => drop.addEventListener(ev, (e) => {
        e.preventDefault(); drop.classList.add("over");
    }));
    ["dragleave", "drop"].forEach((ev) => drop.addEventListener(ev, (e) => {
        e.preventDefault(); drop.classList.remove("over");
    }));
    drop.addEventListener("drop", (e) => fromFile(e.dataTransfer.files[0]));

    el("btnCamStart").addEventListener("click", camStart);
    el("btnSnap").addEventListener("click", camSnap);
    el("btnPull").addEventListener("click", pullFromPost);
    el("btnRun").addEventListener("click", run);

    // Enter в поле штрихкода запускает проверку — руки уже на клавиатуре.
    el("barcode").addEventListener("keydown", (e) => {
        if (e.key === "Enter" && !el("btnRun").disabled) run();
    });

    loadState();
    setInterval(loadState, 5000);
    logLine("страница готова");
});
