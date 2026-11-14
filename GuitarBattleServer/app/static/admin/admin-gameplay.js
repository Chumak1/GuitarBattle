/* fretfight admin — gameplay visuals: UI LEVELS and STAGE LEVELS.
 *
 * UI LEVELS are characters: a unique display name plus a transparent PNG
 * spritesheet with eight poses in a 4x2 grid. STAGE LEVELS are standalone
 * background images with a scene name; they are not tied to a character, so
 * the form asks only for the name and the background upload.
 *
 * The songs form (admin.js) reads both collections through
 * `/admin/ui-levels` and `/admin/stage-levels`; after any change here it is
 * asked to reload its selectors through `window.FFAdmin.reloadVisualOptions`.
 *
 * Public surface: window.FFAdminGameplay.show() / .refresh().
 */
(() => {
  "use strict";

  const API_BASE = "/api/v1";
  const TOKEN_KEY = "guitarbattle.admin.token";
  const MAX_BYTES = 16 * 1024 * 1024;

  const state = {
    loaded: false,
    characters: [],
    stages: [],
    busy: false,
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

  function formatDate(value) {
    if (!value) return "—";
    const date = new Date(value);
    return Number.isNaN(date.getTime())
      ? "—"
      : date.toLocaleDateString("ru-RU", { day: "2-digit", month: "short", year: "numeric" });
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
    }
  }

  function showAlert(targetId, message) {
    const box = el(targetId);
    if (!box) return;
    box.textContent = message || "";
    box.hidden = !message;
  }

  async function api(path, { method = "GET", body = null } = {}) {
    const token = sessionStorage.getItem(TOKEN_KEY) || "";
    const headers = token ? { Authorization: `Bearer ${token}` } : {};
    const init = { method, headers, credentials: "omit" };
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

  function notifySongForm() {
    if (window.FFAdmin && typeof window.FFAdmin.reloadVisualOptions === "function") {
      window.FFAdmin.reloadVisualOptions();
    }
  }

  /* ---------- rendering ---------- */

  function characterRow(character) {
    return `
      <tr data-character-id="${escapeHtml(character.id)}">
        <td><img class="visual-thumb" src="${escapeHtml(character.sheet_url)}" alt="Спрайт-лист ${escapeHtml(character.name)}" loading="lazy"></td>
        <td>
          <div class="song-title text-truncate">${escapeHtml(character.name)}</div>
          <div class="song-slug text-truncate">${escapeHtml(character.sheet_url)}</div>
        </td>
        <td class="d-none d-md-table-cell">${formatBytes(character.size_bytes)}</td>
        <td class="d-none d-lg-table-cell small text-secondary">${formatDate(character.updated_at)}</td>
        <td class="text-end">
          <div class="action-group">
            <button type="button" class="action-btn" data-ui-replace="${escapeHtml(character.id)}" title="Заменить спрайт-лист" aria-label="Заменить спрайт-лист">
              <i class="bi bi-arrow-repeat"></i>
            </button>
            <button type="button" class="action-btn action-danger" data-ui-delete="${escapeHtml(character.id)}" title="Удалить персонажа" aria-label="Удалить персонажа">
              <i class="bi bi-trash3"></i>
            </button>
          </div>
        </td>
      </tr>
    `;
  }

  function stageRow(stage) {
    return `
      <tr data-stage-id="${escapeHtml(stage.id)}">
        <td><img class="visual-thumb" src="${escapeHtml(stage.background_url)}" alt="Фон сцены ${escapeHtml(stage.name)}" loading="lazy"></td>
        <td>
          <div class="song-title text-truncate">${escapeHtml(stage.name)}</div>
          <div class="song-slug text-truncate">${escapeHtml(stage.background_url)}</div>
        </td>
        <td class="d-none d-md-table-cell">${formatBytes(stage.size_bytes)}</td>
        <td class="text-end">
          <div class="action-group">
            <button type="button" class="action-btn" data-stage-replace="${escapeHtml(stage.id)}" title="Заменить фон" aria-label="Заменить фон">
              <i class="bi bi-arrow-repeat"></i>
            </button>
            <button type="button" class="action-btn action-danger" data-stage-delete="${escapeHtml(stage.id)}" title="Удалить сцену" aria-label="Удалить сцену">
              <i class="bi bi-trash3"></i>
            </button>
          </div>
        </td>
      </tr>
    `;
  }

  function renderCharacters() {
    const body = el("uiLevelsBody");
    if (!body) return;
    if (!state.characters.length) {
      body.innerHTML = '<tr><td colspan="5" class="text-secondary text-center py-4">Персонажей пока нет. Загрузите первый спрайт-лист.</td></tr>';
      return;
    }
    body.innerHTML = state.characters.map(characterRow).join("");
  }

  function renderStages() {
    const body = el("stageLevelsBody");
    if (!body) return;
    if (!state.stages.length) {
      body.innerHTML = '<tr><td colspan="4" class="text-secondary text-center py-4">Сцен пока нет. Загрузите фон сцены.</td></tr>';
      return;
    }
    body.innerHTML = state.stages.map(stageRow).join("");
  }

  function renderAll() {
    renderCharacters();
    renderStages();
  }

  async function loadAll() {
    const [characters, stages] = await Promise.all([
      api("/admin/ui-levels"),
      api("/admin/stage-levels"),
    ]);
    state.characters = characters?.items || [];
    state.stages = stages?.items || [];
    state.loaded = true;
    renderAll();
    notifySongForm();
  }

  async function reload({ silent = false } = {}) {
    if (state.busy) return;
    state.busy = true;
    setBusy(true);
    try {
      await loadAll();
      if (!silent) {
        showAlert("uiLevelsAlert", "");
        showAlert("stageLevelsAlert", "");
      }
    } catch (error) {
      if (!silent && error.status !== 401 && error.status !== 403) {
        toast(error.message || "Не удалось загрузить визуалы.", "danger");
      }
    } finally {
      state.busy = false;
      setBusy(false);
    }
  }

  function setBusy(busy) {
    ["uiLevelUpload", "stageLevelUpload"].forEach((id) => {
      const button = el(id);
      if (button) button.disabled = busy;
    });
  }

  function pickFile(accept, onFile) {
    const input = document.createElement("input");
    input.type = "file";
    input.accept = accept;
    input.hidden = true;
    input.addEventListener(
      "change",
      () => {
        const file = input.files && input.files[0];
        input.remove();
        if (file) onFile(file);
      },
      { once: true },
    );
    document.body.appendChild(input);
    input.click();
  }

  /* ---------- UI LEVELS actions ---------- */

  async function uploadCharacter(event) {
    event.preventDefault();
    const form = el("uiLevelForm");
    if (!form.checkValidity()) {
      form.classList.add("was-validated");
      return;
    }
    const file = el("uiLevelFile").files && el("uiLevelFile").files[0];
    if (!file) return;
    if (file.size > MAX_BYTES) {
      showAlert("uiLevelsAlert", "Файл больше 16 МБ.");
      return;
    }
    const data = new FormData();
    data.append("name", el("uiLevelName").value.trim());
    data.append("file", file);

    const button = el("uiLevelUpload");
    button.disabled = true;
    showAlert("uiLevelsAlert", "");
    try {
      await api("/admin/ui-levels", { method: "POST", body: data });
      form.reset();
      form.classList.remove("was-validated");
      toast("Персонаж загружен.");
      await reload({ silent: true });
    } catch (error) {
      showAlert("uiLevelsAlert", error.message || "Не удалось загрузить персонажа.");
    } finally {
      button.disabled = false;
    }
  }

  function replaceCharacter(character) {
    pickFile(".png,image/png", async (file) => {
      if (file.size > MAX_BYTES) {
        showAlert("uiLevelsAlert", "Файл больше 16 МБ.");
        return;
      }
      const data = new FormData();
      data.append("file", file);
      try {
        await api(`/admin/ui-levels/${character.id}/replace`, { method: "POST", body: data });
        toast(`Спрайт-лист «${character.name}» заменён.`);
        await reload({ silent: true });
      } catch (error) {
        showAlert("uiLevelsAlert", error.message || "Не удалось заменить спрайт-лист.");
      }
    });
  }

  async function deleteCharacter(character) {
    if (!window.confirm(`Удалить персонажа «${character.name}»?`)) return;
    try {
      await api(`/admin/ui-levels/${character.id}`, { method: "DELETE" });
      toast("Персонаж удалён.");
      await reload({ silent: true });
    } catch (error) {
      showAlert("uiLevelsAlert", error.message || "Не удалось удалить персонажа.");
    }
  }

  /* ---------- STAGE LEVELS actions ---------- */

  async function uploadStage(event) {
    event.preventDefault();
    const form = el("stageLevelForm");
    if (!form.checkValidity()) {
      form.classList.add("was-validated");
      return;
    }
    const file = el("stageFile").files && el("stageFile").files[0];
    if (!file) return;
    if (file.size > MAX_BYTES) {
      showAlert("stageLevelsAlert", "Файл больше 16 МБ.");
      return;
    }
    const data = new FormData();
    data.append("name", el("stageName").value.trim());
    data.append("file", file);

    const button = el("stageLevelUpload");
    button.disabled = true;
    showAlert("stageLevelsAlert", "");
    try {
      await api("/admin/stage-levels", { method: "POST", body: data });
      form.reset();
      form.classList.remove("was-validated");
      toast("Сцена загружена.");
      await reload({ silent: true });
    } catch (error) {
      showAlert("stageLevelsAlert", error.message || "Не удалось загрузить сцену.");
    } finally {
      button.disabled = false;
    }
  }

  function replaceStage(stage) {
    pickFile(".png,.jpg,.jpeg,image/png,image/jpeg", async (file) => {
      if (file.size > MAX_BYTES) {
        showAlert("stageLevelsAlert", "Файл больше 16 МБ.");
        return;
      }
      const data = new FormData();
      data.append("file", file);
      try {
        await api(`/admin/stage-levels/${stage.id}/replace`, { method: "POST", body: data });
        toast(`Фон сцены «${stage.name}» заменён.`);
        await reload({ silent: true });
      } catch (error) {
        showAlert("stageLevelsAlert", error.message || "Не удалось заменить фон.");
      }
    });
  }

  async function deleteStage(stage) {
    if (!window.confirm(`Удалить сцену «${stage.name}»?`)) return;
    try {
      await api(`/admin/stage-levels/${stage.id}`, { method: "DELETE" });
      toast("Сцена удалена.");
      await reload({ silent: true });
    } catch (error) {
      showAlert("stageLevelsAlert", error.message || "Не удалось удалить сцену.");
    }
  }

  /* ---------- wiring ---------- */

  function bind() {
    el("uiLevelForm")?.addEventListener("submit", uploadCharacter);
    el("stageLevelForm")?.addEventListener("submit", uploadStage);
    el("uiLevelsReload")?.addEventListener("click", () => reload());
    el("stageLevelsReload")?.addEventListener("click", () => reload());

    el("uiLevelsBody")?.addEventListener("click", (event) => {
      const replace = event.target.closest("[data-ui-replace]");
      if (replace) {
        const character = state.characters.find(
          (item) => item.id === replace.getAttribute("data-ui-replace"),
        );
        if (character) replaceCharacter(character);
        return;
      }
      const remove = event.target.closest("[data-ui-delete]");
      if (remove) {
        const character = state.characters.find(
          (item) => item.id === remove.getAttribute("data-ui-delete"),
        );
        if (character) deleteCharacter(character);
      }
    });

    el("stageLevelsBody")?.addEventListener("click", (event) => {
      const replace = event.target.closest("[data-stage-replace]");
      if (replace) {
        const stage = state.stages.find(
          (item) => item.id === replace.getAttribute("data-stage-replace"),
        );
        if (stage) replaceStage(stage);
        return;
      }
      const remove = event.target.closest("[data-stage-delete]");
      if (remove) {
        const stage = state.stages.find(
          (item) => item.id === remove.getAttribute("data-stage-delete"),
        );
        if (stage) deleteStage(stage);
      }
    });
  }

  window.FFAdminGameplay = {
    show() {
      // Reload on every visit: the songs tab can change nothing here, but a
      // second administrator may have uploaded or deleted a visual meanwhile.
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
