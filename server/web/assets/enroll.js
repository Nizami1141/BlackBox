/* ==========================================================================
   Страница регистрации сотрудников.

   Снимки берутся с веб-камеры браузера или загружаются файлами. Каждый
   кадр сразу проверяется сервером (/api/web/preview): сколько лиц видит
   детектор и не слишком ли они мелкие. Это сделано специально — оператор
   должен узнать о негодном снимке в момент съёмки, а не через минуту
   при сохранении, когда переснимать уже некого.

   Эталон считается тем же самым пайплайном, каким потом идёт опознание,
   поэтому «зарегистрировали одним, ищем другим» здесь невозможно.
   ========================================================================== */

let shots = [];          // [{blob, url, ok, note}]
let stream = null;

/* --------------------------------------------------------------- камера */
async function listCameras() {
    try {
        const devs = await navigator.mediaDevices.enumerateDevices();
        const cams = devs.filter((d) => d.kind === "videoinput");
        const sel = el("camList");
        sel.innerHTML = "";
        cams.forEach((c, i) => {
            const o = document.createElement("option");
            o.value = c.deviceId;
            // Пока не выдано разрешение, браузер не отдаёт названия камер —
            // подставляем номер, чтобы список не выглядел пустым.
            o.textContent = c.label || `камера ${i + 1}`;
            sel.appendChild(o);
        });
    } catch (e) { /* до разрешения список может быть недоступен */ }
}

async function startCam() {
    try {
        if (stream) stream.getTracks().forEach((t) => t.stop());
        const id = el("camList").value;
        stream = await navigator.mediaDevices.getUserMedia({
            video: id ? { deviceId: { exact: id }, width: 1280, height: 960 }
                      : { width: 1280, height: 960 },
            audio: false,
        });
        el("cam").srcObject = stream;
        el("btnShot").disabled = false;
        el("btnCam").textContent = "Переключить камеру";
        await listCameras();
    } catch (e) {
        // Самая частая причина — страница открыта не по http://localhost.
        // Браузеры дают доступ к камере только в защищённом контексте.
        toast("Камера недоступна: " + e.message + ". Откройте пульт по адресу " +
              "http://127.0.0.1:8000/ — по другому адресу браузер камеру не даст.", "err");
    }
}

function grab() {
    const v = el("cam");
    if (!v.videoWidth) { toast("камера ещё не запустилась", "err"); return; }
    const c = document.createElement("canvas");
    c.width = v.videoWidth;
    c.height = v.videoHeight;
    c.getContext("2d").drawImage(v, 0, 0);
    // 0.92 — качество JPEG. Ниже начинают появляться артефакты сжатия,
    // а они портят именно мелкие детали лица, по которым и строится вектор.
    c.toBlob((blob) => addShot(blob), "image/jpeg", 0.92);
}

/* ---------------------------------------------------------------- снимки */
async function addShot(blob) {
    if (shots.length >= 12) { toast("больше 12 снимков за раз нельзя", "err"); return; }
    const shot = { blob, url: URL.createObjectURL(blob), ok: null, note: "проверяю…" };
    shots.push(shot);
    renderShots();

    try {
        const fd = new FormData();
        fd.append("image", blob, "shot.jpg");
        const r = await API.form("/api/web/preview", fd);
        const good = r.faces.filter((f) => !f.too_small);
        if (good.length === 1) {
            shot.ok = true;
            shot.note = `лицо найдено (${r.faces[0].det_score})`;
        } else if (r.faces.length === 0) {
            shot.ok = false;
            shot.note = "лицо не найдено";
        } else if (good.length === 0) {
            shot.ok = false;
            shot.note = `лицо мельче ${r.min_face_px} px — подойдите ближе`;
        } else {
            shot.ok = false;
            shot.note = `в кадре ${good.length} лица`;
        }
    } catch (e) {
        shot.ok = false;
        shot.note = e.message;
    }
    renderShots();
}

function renderShots() {
    el("shots").innerHTML = shots.map((s, i) =>
        `<div class="shot${s.ok === false ? " bad" : ""}" title="${esc(s.note)}">` +
        `<img src="${s.url}" alt=""><button data-i="${i}">×</button></div>`).join("");

    el("shots").querySelectorAll("button").forEach((b) => {
        b.onclick = () => {
            const i = Number(b.dataset.i);
            URL.revokeObjectURL(shots[i].url);
            shots.splice(i, 1);
            renderShots();
        };
    });
}

/* -------------------------------------------------------------- сохранить */
async function save() {
    const no = el("employee_no").value.trim();
    const name = el("full_name").value.trim();
    if (!no || !name) { toast("нужны табельный номер и ФИО", "err"); return; }

    const good = shots.filter((s) => s.ok !== false);
    if (!good.length) { toast("нет ни одного пригодного снимка", "err"); return; }
    if (good.length < 3 &&
        !confirm(`Пригодных снимков всего ${good.length}. ` +
                 `Эталон по одному-двум кадрам заметно хуже узнаёт человека ` +
                 `при другом освещении. Продолжить?`)) {
        return;
    }

    const fd = new FormData();
    fd.append("employee_no", no);
    fd.append("full_name", name);
    fd.append("position", el("position").value.trim());
    fd.append("department", el("department").value.trim());
    good.forEach((s, i) => fd.append("images", s.blob, `shot${i}.jpg`));

    el("btnSave").disabled = true;
    try {
        const r = await API.form("/api/web/enroll", fd);
        toast(`${name} зарегистрирован (снимков принято: ${r.samples})`, "ok");
        (r.warnings || []).forEach((w) => toast(w));
        shots.forEach((s) => URL.revokeObjectURL(s.url));
        shots = [];
        renderShots();
        ["employee_no", "full_name", "position"].forEach((id) => { el(id).value = ""; });
        loadEmployees();
    } catch (e) {
        toast(e.message, "err");
    } finally {
        el("btnSave").disabled = false;
    }
}

/* ------------------------------------------------------------ список базы */
let addSamplesFor = null;

async function loadEmployees() {
    try {
        const r = await API.get("/api/web/employees");
        const tb = el("employees");
        if (!r.items.length) {
            tb.innerHTML = '<tr><td colspan="6">база пуста — зарегистрируйте первого сотрудника</td></tr>';
            return;
        }
        tb.innerHTML = r.items.map((e) =>
            `<tr>` +
            `<td><img src="/api/web/employees/${e.id}/thumb" alt="" ` +
            `style="width:36px;height:36px;border-radius:50%;object-fit:cover" ` +
            `onerror="this.style.visibility='hidden'"></td>` +
            `<td class="mono">${esc(e.employee_no)}</td>` +
            `<td>${esc(e.full_name)}` +
            (e.active ? "" : ' <span class="tag bad">заблокирован</span>') + `</td>` +
            `<td>${esc(e.position)}</td>` +
            `<td class="num">${e.n_samples}</td>` +
            `<td style="white-space:nowrap">` +
            `<button data-add="${e.id}">Добавить снимки</button> ` +
            `<button data-act="${e.id}" data-on="${e.active ? 0 : 1}">` +
            `${e.active ? "Заблокировать" : "Разблокировать"}</button> ` +
            `<button class="danger" data-del="${e.id}" data-name="${esc(e.full_name)}">Удалить</button>` +
            `</td></tr>`).join("");

        tb.querySelectorAll("[data-add]").forEach((b) => b.onclick = () => {
            addSamplesFor = b.dataset.add;
            el("addFiles").click();
        });
        tb.querySelectorAll("[data-act]").forEach((b) => b.onclick = async () => {
            await API.post(`/api/web/employees/${b.dataset.act}/active?active=${b.dataset.on === "1"}`);
            loadEmployees();
        });
        tb.querySelectorAll("[data-del]").forEach((b) => b.onclick = async () => {
            // Удаление сотрудника стирает и его эталон, и все снимки.
            // Записи в журнале остаются — журнал append-only.
            if (!confirm(`Удалить ${b.dataset.name} из базы? ` +
                         `Эталон и снимки будут стёрты, записи журнала останутся.`)) return;
            await API.del(`/api/web/employees/${b.dataset.del}`);
            loadEmployees();
        });
    } catch (e) {
        el("employees").innerHTML = `<tr><td colspan="6">${esc(e.message)}</td></tr>`;
    }
}

/* --------------------------------------------------------------- события */
el("btnCam").addEventListener("click", startCam);
el("btnShot").addEventListener("click", grab);
el("camList").addEventListener("change", startCam);
el("btnSave").addEventListener("click", save);

el("files").addEventListener("change", (e) => {
    [...e.target.files].forEach((f) => addShot(f));
    e.target.value = "";
});

el("addFiles").addEventListener("change", async (e) => {
    const files = [...e.target.files];
    e.target.value = "";
    if (!files.length || !addSamplesFor) return;
    const fd = new FormData();
    files.forEach((f, i) => fd.append("images", f, f.name || `s${i}.jpg`));
    try {
        const r = await API.form(`/api/web/employees/${addSamplesFor}/samples`, fd);
        toast("снимки добавлены, эталон пересчитан", "ok");
        (r.warnings || []).forEach((w) => toast(w));
        loadEmployees();
    } catch (err) {
        toast(err.message, "err");
    }
});

async function refreshHeader() {
    try {
        const st = await API.get("/api/web/state");
        renderDevices(st.devices || [], "dot", "devtext");
    } catch (e) { renderDevices([], "dot", "devtext"); }
}

listCameras();
loadEmployees();
refreshHeader();
setInterval(refreshHeader, 8000);
