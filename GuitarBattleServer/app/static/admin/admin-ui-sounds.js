/* fretfight admin — UI sounds.
 *
 * A fixed registry of interface sound events (seeded by migration 0032) with
 * one optional short clip per event. The panel uploads/replaces a file, plays
 * an authorized preview, deletes the file and edits the two playback knobs
 * (active, gain). The catalogue itself is not editable: the keys are compiled
 * into the client and the server seeds exactly the same list.
 *
 * Public surface: window.FFAdminUiSounds.show() / .refresh().
 */
(() => {
  "use strict";

  const API_BASE = "/api/v1";
  const TOKEN_KEY = "guitarbattle.admin.token";
  const MAX_BYTES = 2 * 1024 * 1024;
  const ACCEPTED = [".mp3", ".wav", ".ogg"];
  const CATEGORY_LABELS = {
    navigation: "Навигация",
    action: "Действия",
    system: "Система",
  };

  const state = {
    items: [],
    busy: false,
    previewKey: "",
    previewAudio: null,
    previewObjectUrl: "",
  };

  const el = (id) => document.getElementById(id);

  function escapeHtml(value) {
    return String(value ?? "")
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&#039;");
  }

  function formatBytes(bytes) {
    const value = Number(bytes) || 0;
    if (value < 1024) return `${value} Б`;
    if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} КБ`;
    return `${(value / (1024 * 1024)).toFixed(1)} МБ`;
  }

  function formatGain(gain) {
    return (Number(gain) || 0).toFixed(2);
  }

  function categoryLabel(category) {
    return CATEGORY_LABELS[category] || category;
  }

  function detailOf(payload) {
    if (!payload) return "";
    if (typeof payload.detail === "string") return payload.detail;
    if (Array.isArray(payload.detail)) {
      return payload.detail
        .map((item) => (typeof item === "string" ? item : item.msg || ""))
        .filter(Boolean)
        .join("; ");
    }
    return "";
  }

  function handleUnauthorized() {
    if (window.FFAdmin && typeof window.FFAdmin.handleUnauthorized === "function") {
      window.FFAdmin.handleUnauthorized();
      return;
    }
    sessionStorage.removeItem(TOKEN_KEY);
    window.location.reload();
  }

  function toast(message, variant) {
    if (window.FFAdmin && typeof window.FFAdmin.showToast === "function") {
      window.FFAdmin.showToast(message, variant || "success");
      return;
    }
    console.warn(message);
  }

  function showAlert(message) {
    const box = el("uiSoundsAlert");
    if (!box) return;
    box.textContent = message || "";
    box.hidden = !message;
  }

  function token() {
    return sessionStorage.getItem(TOKEN_KEY) || "";
  }

  async function api(path, { method = "GET", body = null } = {}) {
    const headers = {};
    const bearer = token();
    if (bearer) headers.Authorization = `Bearer ${bearer}`;
    const init = { method, headers };
    if (body instanceof FormData) {
      init.body = body;
    } else if (body !== null) {
      headers["Content-Type"] = "application/json";
      init.body = JSON.stringify(body);
    }

    let response;
    try {
      response = await fetch(`${API_BASE}${path}`, init);
    } catch {
      const error = new Error("Не удалось связаться с сервером.");
      error.status = 0;
      throw error;
    }
    const text = await response.text();
    let payload = null;
    if (text) {
      try {
        payload = JSON.parse(text);
      } catch {
        payload = null;
      }
    }
    if (!response.ok) {
      if (response.status === 401 || response.status === 403) {
        handleUnauthorized();
      }
      const error = new Error(detailOf(payload) || `Ошибка запроса (${response.status})`);
      error.status = response.status;
      throw error;
    }
    return payload;
  }

  /* ---------- preview player ---------- */

  function previewButton(key) {
    return Array.from(document.querySelectorAll("[data-ui-sound-play]")).find(
      (button) => button.getAttribute("data-ui-sound-play") === key,
    );
  }

  function stopPreview() {
    if (state.previewAudio) {
      state.previewAudio.pause();
      state.previewAudio.src = "";
      state.previewAudio = null;
    }
    if (state.previewObjectUrl) {
      URL.revokeObjectURL(state.previewObjectUrl);
      state.previewObjectUrl = "";
    }
    const previous = state.previewKey ? previewButton(state.previewKey) : null;
    if (previous) {
      previous.innerHTML = '<i class="bi bi-play-fill"></i>';
      previous.title = "Прослушать";
      previous.setAttribute("aria-label", "Прослушать");
    }
    state.previewKey = "";
  }

  async function togglePreview(item) {
    if (!item.has_file) return;
    if (state.previewKey === item.key) {
      stopPreview();
      return;
    }
    stopPreview();

    const button = previewButton(item.key);

    let response;
    try {
      // <audio> cannot send an Authorization header, so the file is fetched
      // here and handed to the player as a blob: URL (same pattern as the song
      // preview in admin.js).
      response = await fetch(
        `${API_BASE}/admin/ui-sounds/${encodeURIComponent(item.key)}/file?v=${encodeURIComponent(item.version)}`,
        { headers: bearerHeaders(), cache: "no-store" },
      );
    } catch {
      toast("Не удалось загрузить звук.", "danger");
      return;
    }
    if (response.status === 401 || response.status === 403) {
      handleUnauthorized();
      return;
    }
    if (!response.ok) {
      toast("Не удалось загрузить звук.", "danger");
      return;
    }

    const blob = await response.blob();
    state.previewObjectUrl = URL.createObjectURL(blob);
    const audio = new Audio(state.previewObjectUrl);
    state.previewAudio = audio;
    state.previewKey = item.key;
    if (button) {
      button.innerHTML = '<i class="bi bi-stop-fill"></i>';
      button.title = "Остановить";
      button.setAttribute("aria-label", "Остановить");
    }
    audio.addEventListener("ended", () => {
      if (state.previewKey === item.key) stopPreview();
    });
    audio.play().catch(() => {
      stopPreview();
      toast("Браузер не разрешил воспроизведение. Нажмите ещё раз.", "warning");
    });
  }

  function bearerHeaders() {
    const bearer = token();
    return bearer ? { Authorization: `Bearer ${bearer}` } : {};
  }

  /* ---------- rendering ---------- */

  function row(item) {
    const fileMeta = item.has_file
      ? `${formatBytes(item.size_bytes)} · ${escapeHtml(item.content_type || "audio")} · v${item.version}${item.original_name ? ` · ${escapeHtml(item.original_name)}` : ""}`
      : "Файл не загружен";
    return `
      <tr data-ui-sound-key="${escapeHtml(item.key)}">
        <td>
          <div class="song-title text-truncate">${escapeHtml(item.name)}</div>
          <div class="song-slug text-truncate">${escapeHtml(item.key)}</div>
        </td>
        <td><span class="badge-soft badge-empty">${escapeHtml(categoryLabel(item.category))}</span></td>
        <td>
          <div class="form-check form-switch mb-0">
            <input class="form-check-input" type="checkbox" role="switch"
                   data-ui-sound-active ${item.active ? "checked" : ""}
                   aria-label="Событие ${escapeHtml(item.key)} включено">
          </div>
        </td>
        <td>
          <div class="d-flex align-items-center gap-2">
            <input class="form-range ui-sound-gain" type="range" min="0" max="1" step="0.05"
                   value="${escapeHtml(item.gain)}" data-ui-sound-gain
                   aria-label="Громкость ${escapeHtml(item.key)}">
            <span class="file-meta" data-ui-sound-gain-value>${formatGain(item.gain)}</span>
          </div>
        </td>
        <td>
          <div class="file-meta mb-1">${fileMeta}</div>
          <input class="form-control form-control-sm ui-sound-file" type="file"
                 accept=".mp3,.wav,.ogg,audio/mpeg,audio/wav,audio/ogg"
                 data-ui-sound-file
                 aria-label="${item.has_file ? "Заменить" : "Загрузить"} звук ${escapeHtml(item.key)}">
        </td>
        <td class="text-end">
          <div class="action-group">
            <button type="button" class="action-btn" data-ui-sound-play="${escapeHtml(item.key)}"
                    ${item.has_file ? "" : "disabled"}
                    title="Прослушать" aria-label="Прослушать">
              <i class="bi bi-play-fill"></i>
            </button>
            <button type="button" class="action-btn action-danger" data-ui-sound-delete="${escapeHtml(item.key)}"
                    ${item.has_file ? "" : "disabled"}
                    title="Удалить файл" aria-label="Удалить файл">
              <i class="bi bi-trash3"></i>
            </button>
          </div>
        </td>
      </tr>
    `;
  }

  function render() {
    const body = el("uiSoundsBody");
    if (!body) return;
    if (!state.items.length) {
      body.innerHTML = '<tr><td colspan="6" class="text-secondary text-center py-4">Реестр звуков пуст. Проверьте миграции.</td></tr>';
      return;
    }
    body.innerHTML = state.items.map(row).join("");
  }

  function itemFor(key) {
    return state.items.find((item) => item.key === key);
  }

  function setBusy(busy) {
    state.busy = busy;
    const reload = el("uiSoundsReload");
    if (reload) reload.disabled = busy;
    document.querySelectorAll("[data-ui-sound-file]").forEach((input) => {
      input.disabled = busy;
    });
  }

  async function reload({ silent = false } = {}) {
    if (state.busy) return;
    stopPreview();
    setBusy(true);
    try {
      const data = await api("/admin/ui-sounds");
      state.items = Array.isArray(data?.items) ? data.items : [];
      render();
      if (!silent) showAlert("");
    } catch (error) {
      if (error.status !== 401 && error.status !== 403) {
        toast(error.message || "Не удалось загрузить звуки интерфейса.", "danger");
      }
    } finally {
      setBusy(false);
    }
  }

  /* ---------- mutations ---------- */

  function extensionOf(name) {
    const match = /\.[^.]+$/.exec(String(name || ""));
    return match ? match[0].toLowerCase() : "";
  }

  async function upload(item, input) {
    const file = input.files && input.files[0];
    if (!file) return;
    if (!ACCEPTED.includes(extensionOf(file.name))) {
      showAlert("Нужен файл MP3, WAV или OGG.");
      input.value = "";
      return;
    }
    if (file.size > MAX_BYTES) {
      showAlert("Файл больше 2 МБ.");
      input.value = "";
      return;
    }

    const data = new FormData();
    data.append("file", file, file.name);

    showAlert("");
    setBusy(true);
    try {
      await api(`/admin/ui-sounds/${encodeURIComponent(item.key)}/file`, {
        method: "POST",
        body: data,
      });
      toast(`Звук «${item.name}» ${item.has_file ? "заменён" : "загружен"}.`);
      setBusy(false);
      await reload({ silent: true });
    } catch (error) {
      showAlert(error.message || "Не удалось загрузить звук.");
      setBusy(false);
    } finally {
      input.value = "";
    }
  }

  async function removeFile(item) {
    if (!window.confirm(`Удалить файл звука «${item.name}»?`)) return;
    setBusy(true);
    try {
      await api(`/admin/ui-sounds/${encodeURIComponent(item.key)}/file`, {
        method: "DELETE",
      });
      toast("Файл удалён.");
      setBusy(false);
      await reload({ silent: true });
    } catch (error) {
      showAlert(error.message || "Не удалось удалить файл.");
      setBusy(false);
    }
  }

  async function patch(item, changes, failureMessage) {
    try {
      const updated = await api(`/admin/ui-sounds/${encodeURIComponent(item.key)}`, {
        method: "PATCH",
        body: changes,
      });
      const index = state.items.findIndex((entry) => entry.key === item.key);
      if (index >= 0 && updated) state.items[index] = updated;
      return true;
    } catch (error) {
      showAlert(error.message || failureMessage);
      await reload({ silent: true });
      return false;
    }
  }

  async function toggleActive(item, input) {
    const active = Boolean(input.checked);
    const ok = await patch(item, { active }, "Не удалось изменить событие.");
    if (!ok) return;
    item.active = active;
    toast(active ? "Событие включено." : "Событие выключено.");
  }

  async function changeGain(item, input, valueLabel) {
    const gain = Number(input.value);
    valueLabel.textContent = formatGain(gain);
    const ok = await patch(item, { gain }, "Не удалось изменить громкость.");
    if (!ok) return;
    item.gain = gain;
  }

  /* ---------- wiring ---------- */

  function bind() {
    el("uiSoundsReload")?.addEventListener("click", () => reload());

    el("uiSoundsBody")?.addEventListener("click", (event) => {
      const play = event.target.closest("[data-ui-sound-play]");
      if (play && !play.disabled) {
        const item = itemFor(play.getAttribute("data-ui-sound-play"));
        if (item) togglePreview(item);
        return;
      }
      const remove = event.target.closest("[data-ui-sound-delete]");
      if (remove && !remove.disabled) {
        const item = itemFor(remove.getAttribute("data-ui-sound-delete"));
        if (item) removeFile(item);
      }
    });

    el("uiSoundsBody")?.addEventListener("change", (event) => {
      const target = event.target;
      const cell = target.closest("tr[data-ui-sound-key]");
      if (!cell) return;
      const item = itemFor(cell.getAttribute("data-ui-sound-key"));
      if (!item) return;

      if (target.matches("[data-ui-sound-file]")) {
        upload(item, target);
        return;
      }
      if (target.matches("[data-ui-sound-active]")) {
        toggleActive(item, target);
      }
    });

    el("uiSoundsBody")?.addEventListener("input", (event) => {
      const target = event.target;
      if (!target.matches("[data-ui-sound-gain]")) return;
      const cell = target.closest("tr[data-ui-sound-key]");
      const item = cell ? itemFor(cell.getAttribute("data-ui-sound-key")) : null;
      const label = cell?.querySelector("[data-ui-sound-gain-value]");
      if (!item || !label) return;
      label.textContent = formatGain(target.value);
    });

    el("uiSoundsBody")?.addEventListener("change", (event) => {
      const target = event.target;
      if (!target.matches("[data-ui-sound-gain]")) return;
      const cell = target.closest("tr[data-ui-sound-key]");
      const item = cell ? itemFor(cell.getAttribute("data-ui-sound-key")) : null;
      const label = cell?.querySelector("[data-ui-sound-gain-value]");
      if (!item || !label) return;
      changeGain(item, target, label);
    });
  }

  window.FFAdminUiSounds = {
    show() {
      // Reload on every visit: another administrator may have replaced a file.
      reload({ silent: true });
    },
    refresh() {
      reload();
    },
  };

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", bind);
  } else {
    bind();
  }
})();
