/* ==========================================================================
   Общий слой работы с сервером: запросы, WebSocket, мелкие утилиты.
   Подключается на всех трёх страницах.

   Никаких фреймворков. Причина не в идеологии: пульт должен открываться на
   посту без интернета, а любая CDN — это внешняя зависимость, которая
   однажды не ответит. Страниц три, состояние простое, ванильного JS хватает.
   ========================================================================== */

const API = {
    async get(url) { return API._json(await fetch(url)); },

    async post(url, body) {
        return API._json(await fetch(url, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(body || {}),
        }));
    },

    async del(url) { return API._json(await fetch(url, { method: "DELETE" })); },

    async form(url, formData) {
        return API._json(await fetch(url, { method: "POST", body: formData }));
    },

    async _json(res) {
        let data = null;
        try { data = await res.json(); } catch (e) { /* пустой ответ */ }
        if (!res.ok) {
            // Сервер всегда кладёт в ошибку reason и обычно message —
            // показываем именно message, он написан для человека.
            const err = new Error((data && (data.message || data.reason ||
                                  data.detail?.message || data.detail?.reason)) ||
                                  `HTTP ${res.status}`);
            err.reason = data && data.reason;
            throw err;
        }
        return data;
    },
};

/* --------------------------------------------------------------- WebSocket
   Живые события: стадии проверки, появление и пропажа постов, изменения базы.
   Переподключение обязательно: сервер перезапускают, а вкладку оператор
   держит открытой всю смену.                                              */
class Live {
    constructor(onEvent) {
        this.onEvent = onEvent;
        this.ws = null;
        this.delay = 1000;
        this.connect();
    }

    connect() {
        const proto = location.protocol === "https:" ? "wss" : "ws";
        this.ws = new WebSocket(`${proto}://${location.host}/ws/ui`);

        this.ws.onopen = () => { this.delay = 1000; };
        this.ws.onmessage = (e) => {
            try { this.onEvent(JSON.parse(e.data)); }
            catch (err) { console.warn("плохое событие", e.data); }
        };
        this.ws.onclose = () => {
            // Пауза растёт до 10 с: если сервер лежит, не надо долбить его
            // сотней попыток в минуту.
            setTimeout(() => this.connect(), this.delay);
            this.delay = Math.min(this.delay * 1.6, 10000);
        };
        this.ws.onerror = () => this.ws.close();
    }
}

/* ------------------------------------------------------------- утилиты */
function el(id) { return document.getElementById(id); }

function fmtTime(ts) {
    const d = new Date(ts * 1000);
    const p = (n) => String(n).padStart(2, "0");
    return `${p(d.getDate())}.${p(d.getMonth() + 1)}.${d.getFullYear()} ` +
           `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

function esc(s) {
    return String(s == null ? "" : s).replace(/[&<>"']/g, (c) => ({
        "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
    }[c]));
}

function toast(text, kind) {
    const box = el("toasts");
    if (!box) return;
    const t = document.createElement("div");
    t.className = "toast" + (kind ? " " + kind : "");
    t.textContent = text;
    box.appendChild(t);
    setTimeout(() => t.remove(), 6000);
}

/* Индикатор связи в шапке. Один пост или несколько — показываем сводку. */
function renderDevices(devices, dotId, textId) {
    const dot = el(dotId), txt = el(textId);
    if (!dot || !txt) return;
    const online = devices.filter((d) => d.connected);
    dot.classList.toggle("on", online.length > 0);
    if (online.length === 0) {
        txt.textContent = devices.length
            ? "постов на связи нет"
            : "посты не настроены";
    } else {
        txt.textContent = online.map((d) => `${d.device_id} (${d.kind})`).join(", ");
    }
}
