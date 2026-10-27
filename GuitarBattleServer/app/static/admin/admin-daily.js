/* fretfight admin — Daily Challenge schedule.
 *
 * One song per weekday (UTC), 0 = Monday … 6 = Sunday. The schedule is the
 * single source for `/daily/today`: the server freezes the song into the day on
 * the first request, so a change here only affects days that have not started.
 *
 * Kept separate from admin.js (songs) and admin-site.js (landing/builds): it
 * only needs the administrator JWT from sessionStorage and the shared helpers
 * exposed by admin.js.
 *
 * Public surface: window.FFAdminDaily.show() — lazy first load, refresh on
 * every later visit; window.FFAdminDaily.refresh() — explicit reload.
 */
(() => {
  "use strict";

  const API_BASE = "/api/v1";
  const TOKEN_KEY = "guitarbattle.admin.token";
  const WEEKDAY_NAMES = [
    "Понедельник",
    "Вторник",
    "Среда",
    "Четверг",
    "Пятница",
    "Суббота",
    "Воскресенье",
  ];

  const state = {
    loaded: false,
    requestId: 0,
    slots: [],
    songs: [],
    busy: "",
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

  function detailOf(payload) {
    if (!payload) return "";
    if (typeof payload.detail === "string") return payload.detail;
    if (Array.isArray(payload.detail)) {
      return payload.detail
        .map((item) => (typeof item === "string" ? item : item.msg || item.detail || ""))
        .filter(Boolean)
        .join("; ");
    }
    return "";
  }

  function errorText(error) {
    if (error && error.status === 401) {
      return "Сессия истекла. Войдите в админку заново.";
    }
    if (error && error.status === 403) {
      return "Недостаточно прав. Нужен аккаунт администратора.";
    }
    if (error && error.status === 404) {
      return "Песня не найдена — возможно, её удалили.";
    }
    if (error && error.status === 409) {
      return "Слот не удалось обновить: конфликт расписания.";
    }
    return (error && error.message) || "Не удалось выполнить запрос.";
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

  function showAlert(message) {
    const box = el("dailyAlert");
    if (!box) return;
    box.textContent = message || "";
    box.hidden = !message;
  }

  async function api(path, options = {}) {
    const token = sessionStorage.getItem(TOKEN_KEY) || "";
    const headers = token ? { Authorization: "Bearer " + token } : {};
    const init = { method: options.method || "GET", headers, credentials: "omit" };
    if (options.body !== undefined) {
      headers["Content-Type"] = "application/json";
      init.body = JSON.stringify(options.body);
    }

    const response = await fetch(API_BASE + path, init);
    const text = await response.text();
    let data = null;
    if (text) {
      try {
        data = JSON.parse(text);
      } catch (error) {
        data = null;
      }
    }
    if (!response.ok) {
      if (response.status === 401 || response.status === 403) {
        handleUnauthorized();
      }
      const error = new Error(detailOf(data) || "Ошибка запроса (" + response.status + ")");
      error.status = response.status;
      throw error;
    }
    return data;
  }

  /* ---------- rendering ---------- */

  function songLabel(song) {
    const title = escapeHtml(song.title);
    const slug = escapeHtml(song.slug);
    const draft = song.is_published
      ? ""
      : ' <span class="badge-soft badge-draft"><i class="bi bi-pencil-square"></i>Черновик</span>';
    return `<div class="song-title text-truncate">${title}${draft}</div>
      <div class="song-slug text-truncate">${slug}</div>`;
  }

  function optionsHtml(selectedId) {
    const options = ['<option value="">— выберите песню —</option>'];
    state.songs.forEach((song) => {
      const suffix = song.is_published ? "" : " · черновик";
      const selected = song.id === selectedId ? " selected" : "";
      options.push(
        `<option value="${escapeHtml(song.id)}"${selected}>${escapeHtml(song.title)} — ${escapeHtml(song.slug)}${escapeHtml(suffix)}</option>`,
      );
    });
    return options.join("");
  }

  function slotRow(slot) {
    const busy = state.busy === String(slot.weekday);
    const song = slot.song;
    const songCell = song
      ? songLabel(song)
      : '<span class="text-secondary">Не назначена</span>';
    return `
      <tr data-weekday="${slot.weekday}">
        <td>
          <div class="fw-semibold">${escapeHtml(WEEKDAY_NAMES[slot.weekday] || slot.weekday_name)}</div>
          <div class="song-slug">${slot.weekday}</div>
        </td>
        <td>${songCell}</td>
        <td class="text-end">
          <div class="d-inline-flex flex-wrap gap-2 align-items-center justify-content-end">
            <select class="form-select form-select-sm" data-daily-select="${slot.weekday}"
                    aria-label="Песня для дня ${escapeHtml(WEEKDAY_NAMES[slot.weekday] || "")}"
                    ${busy ? "disabled" : ""} style="min-width: 15rem; max-width: 24rem">
              ${optionsHtml(song ? song.id : "")}
            </select>
            <button type="button" class="btn btn-sm btn-warning" data-daily-assign="${slot.weekday}"
                    ${busy ? "disabled" : ""}>Назначить</button>
            <button type="button" class="btn btn-sm btn-outline-danger" data-daily-clear="${slot.weekday}"
                    ${busy || !song ? "disabled" : ""}>Очистить</button>
          </div>
        </td>
      </tr>
    `;
  }

  function render() {
    const body = el("dailySlotsBody");
    if (!body) return;
    body.innerHTML = state.slots.map(slotRow).join("");
  }

  /* ---------- data ---------- */

  async function loadSongs() {
    // The picker needs every song, including ones already scheduled; the songs
    // table above is paginated, so it cannot be reused here. The admin endpoint
    // caps ``limit`` at 100, therefore page through it (bounded).
    const songs = [];
    let offset = 0;
    for (let page = 0; page < 20; page += 1) {
      // Archived songs cannot be scheduled (the API answers 409), so they are
      // not offered in the picker at all.
      const data = await api(`/admin/songs?limit=100&offset=${offset}&archived=false`);
      const items = (data && data.items) || [];
      songs.push(...items);
      offset += items.length;
      if (!items.length || offset >= ((data && data.total) || 0)) break;
    }
    state.songs = songs;
  }

  async function refresh() {
    const requestId = ++state.requestId;
    showAlert("");
    const body = el("dailySlotsBody");
    if (body && !state.loaded) {
      body.innerHTML =
        '<tr><td colspan="3" class="table-loading"><span class="spinner-border spinner-border-sm me-2"></span>Загрузка…</td></tr>';
    }
    try {
      const [schedule] = await Promise.all([api("/admin/daily"), loadSongs()]);
      if (requestId !== state.requestId) return;
      state.slots = (schedule && schedule.items) || [];
      state.loaded = true;
      render();
    } catch (error) {
      if (requestId !== state.requestId) return;
      showAlert(errorText(error));
    }
  }

  async function assign(weekday) {
    const select = document.querySelector(`[data-daily-select="${weekday}"]`);
    const songId = select ? select.value : "";
    if (!songId) {
      toast("Сначала выберите песню в списке.", "warning");
      return;
    }
    state.busy = String(weekday);
    render();
    try {
      await api("/admin/daily/" + weekday, { method: "PUT", body: { song_id: songId } });
      toast(`Слот «${WEEKDAY_NAMES[weekday]}» обновлён.`);
      await afterChange();
    } catch (error) {
      toast(errorText(error), "danger");
    } finally {
      state.busy = "";
      render();
    }
  }

  async function clear(weekday) {
    state.busy = String(weekday);
    render();
    try {
      await api("/admin/daily/" + weekday, { method: "PUT", body: { song_id: null } });
      toast(`Слот «${WEEKDAY_NAMES[weekday]}» освобождён.`);
      await afterChange();
    } catch (error) {
      toast(errorText(error), "danger");
    } finally {
      state.busy = "";
      render();
    }
  }

  async function afterChange() {
    await refresh();
    // The songs tab shows a Daily badge and hides these songs from the public
    // catalogue, so its cached rows must be reloaded as well.
    if (window.FFAdmin && typeof window.FFAdmin.reloadSongs === "function") {
      window.FFAdmin.reloadSongs();
    }
  }

  /* ---------- wiring ---------- */

  function init() {
    el("dailyReload")?.addEventListener("click", () => {
      refresh().catch((error) => showAlert(errorText(error)));
    });
    el("dailySlotsBody")?.addEventListener("click", (event) => {
      const assignButton = event.target.closest("[data-daily-assign]");
      if (assignButton) {
        assign(Number(assignButton.getAttribute("data-daily-assign")));
        return;
      }
      const clearButton = event.target.closest("[data-daily-clear]");
      if (clearButton) {
        clear(Number(clearButton.getAttribute("data-daily-clear")));
      }
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }

  window.FFAdminDaily = {
    show() {
      // Lazy first load, refresh on every later visit.
      refresh().catch((error) => showAlert(errorText(error)));
    },
    refresh() {
      return refresh();
    },
  };
})();
