/* ==========================================================================
   Журнал: главная таблица системы.

   Каждая строка здесь — это утверждение «такого-то числа такой-то сотрудник
   проверил такой-то груз», подтверждённое одноразовым токеном, который
   сервер выдал после опознания лица. Записи только добавляются: изменить
   или удалить строку через интерфейс нельзя, и это осознанно.

   Таблица обновляется сама при появлении новой записи — оператору не нужно
   жать «Обновить», чтобы увидеть только что сделанную проверку.
   ========================================================================== */

const REASONS = {
    NO_FACE: "лицо не найдено",
    FACE_TOO_SMALL: "лицо слишком мелкое",
    MULTIPLE_FACES: "в кадре несколько лиц",
    NOT_FOUND: "нет в базе",
    INACTIVE: "сотрудник заблокирован",
    BAD_IMAGE: "кадр не декодировался",
    SPOOF_SUSPECTED: "подозрение на подделку",
    INDEX_MISMATCH: "размерность вектора не совпала с базой",
};

async function loadEmployeesFilter() {
    try {
        const r = await API.get("/api/web/employees");
        const sel = el("fEmployee");
        r.items.forEach((e) => {
            const o = document.createElement("option");
            o.value = e.id;
            o.textContent = `${e.full_name} (${e.employee_no})`;
            sel.appendChild(o);
        });
    } catch (e) { /* фильтр необязателен */ }
}

async function load() {
    const p = new URLSearchParams();
    p.set("limit", el("fLimit").value);
    if (el("fBarcode").value.trim()) p.set("barcode", el("fBarcode").value.trim());
    if (el("fEmployee").value) p.set("employee_id", el("fEmployee").value);

    try {
        const r = await API.get("/api/web/journal?" + p.toString());
        const tb = el("rows");
        if (!r.items.length) {
            tb.innerHTML = '<tr><td colspan="7">записей нет</td></tr>';
            el("count").textContent = "";
            return;
        }
        tb.innerHTML = r.items.map((it) =>
            `<tr>` +
            `<td class="mono">${it.id}</td>` +
            `<td>${fmtTime(it.ts)}</td>` +
            `<td class="mono">${esc(it.employee_no || "")}</td>` +
            `<td>${esc(it.full_name || "—")}</td>` +
            `<td class="mono">${esc(it.barcode)}</td>` +
            `<td>${esc(it.device_id)}</td>` +
            `<td class="num">${it.match_score != null ? it.match_score.toFixed(3) : ""}</td>` +
            `</tr>`).join("");
        el("count").textContent = `показано записей: ${r.items.length}`;
    } catch (e) {
        el("rows").innerHTML = `<tr><td colspan="7">${esc(e.message)}</td></tr>`;
    }
}

async function loadVerify() {
    try {
        const r = await API.get("/api/web/verify-log?limit=100");
        const tb = el("vrows");
        if (!r.items.length) { tb.innerHTML = '<tr><td colspan="7">—</td></tr>'; return; }
        tb.innerHTML = r.items.map((v) =>
            `<tr>` +
            `<td>${fmtTime(v.ts)}</td>` +
            `<td>${esc(v.device_id)}</td>` +
            `<td><span class="tag ${v.matched ? "ok" : "bad"}">` +
            `${v.matched ? "опознан" : "отказ"}</span></td>` +
            `<td>${esc(v.full_name || "—")}</td>` +
            `<td class="num">${v.score != null ? v.score.toFixed(3) : ""}</td>` +
            `<td>${esc(REASONS[v.reason] || v.reason || "")}</td>` +
            `<td class="num">${v.latency_ms ?? ""}</td>` +
            `</tr>`).join("");
    } catch (e) { /* не критично */ }
}

function reloadAll() { load(); loadVerify(); }

el("btnReload").addEventListener("click", reloadAll);
el("fLimit").addEventListener("change", load);
el("fEmployee").addEventListener("change", load);

// Поиск по штрихкоду с задержкой: без неё каждый символ уходил бы
// отдельным запросом в SQLite.
let t = null;
el("fBarcode").addEventListener("input", () => {
    clearTimeout(t);
    t = setTimeout(load, 300);
});

new Live((ev) => {
    if (ev.type === "device") {
        API.get("/api/web/state")
           .then((st) => renderDevices(st.devices || [], "dot", "devtext"))
           .catch(() => {});
    }
    // Новая запись в журнале или завершённая попытка — перечитываем таблицы.
    if (ev.type === "scan" || (ev.type === "flow" && ev.stage === "done")) reloadAll();
    if (ev.type === "verify") loadVerify();
});

API.get("/api/web/state")
   .then((st) => renderDevices(st.devices || [], "dot", "devtext"))
   .catch(() => renderDevices([], "dot", "devtext"));

loadEmployeesFilter();
reloadAll();
