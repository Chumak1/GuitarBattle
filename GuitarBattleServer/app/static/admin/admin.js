(() => {
  "use strict";

  const API_BASE = "/api/v1";
  const TOKEN_KEY = "guitarbattle.admin.token";
  const AUDIO_ROLES = ["drums", "bass", "guitar", "others"];
  const ALL_ROLES = ["midi", ...AUDIO_ROLES];
  const ROLE_LABELS = {
    midi: "MIDI",
    drums: "Барабаны",
    bass: "Бас",
    guitar: "Гитара",
    others: "Другое",
    preview: "Preview Song (полный микс)",
  };
  const ROLE_ICONS = {
    midi: "bi-file-earmark-music",
    drums: "bi-disc",
    bass: "bi-music-note",
    guitar: "bi-music-note-beamed",
    others: "bi-soundwave",
    preview: "bi-play-circle",
  };
  const READINESS = {
    empty: { label: "Нет файлов", className: "badge-empty" },
    midi_only: { label: "Только MIDI", className: "badge-midi" },
    partial: { label: "Частично", className: "badge-partial" },
    complete: { label: "Готова", className: "badge-complete" },
  };
  const MAX_MIDI_BYTES = 20 * 1024 * 1024;
  const MAX_AUDIO_BYTES = 100 * 1024 * 1024;
  const WEEKDAY_SHORT = ["Пн", "Вт", "Ср", "Чт", "Пт", "Сб", "Вс"];

  const state = {
    token: sessionStorage.getItem(TOKEN_KEY) || "",
    user: null,
    songs: [],
    // Gameplay visuals for the song form: UI-level characters and stage
    // backgrounds, loaded from the admin endpoints. Empty until the first
    // successful load; the form still works (both selectors are optional).
    characters: [],
    stages: [],
    // Selection of the currently open song form. Kept in state so a slow
    // `/admin/ui-levels` response cannot wipe the choice the modal was opened
    // with: the selects are re-rendered when the options arrive.
    formCharacterId: "",
    formStageId: "",
    visualsLoaded: false,
    total: 0,
    search: "",
    published: "",
    currentSong: null,
    analysis: null,
    analysisMissing: false,
    pendingDelete: null,
    slugTouched: false,
    loadRequestId: 0,
    // Preview Song player: the id of the song currently loaded, the blob URL
    // handed to <audio>, the in-flight download and the request id that cancels
    // superseded downloads.
    previewSongId: null,
    previewObjectUrl: "",
    previewRequestId: 0,
    previewLoading: false,
    previewXhr: null,
  };

  class ApiError extends Error {
    constructor(message, status = 0) {
      super(message);
      this.name = "ApiError";
      this.status = status;
    }
  }

  const el = (id) => document.getElementById(id);

  function escapeHtml(value) {
    return String(value ?? "")
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&#039;");
  }

  function getModal(id) {
    if (!window.bootstrap?.Modal) {
      return null;
    }
    return window.bootstrap.Modal.getOrCreateInstance(document.getElementById(id));
  }

  function showModal(id) {
    getModal(id)?.show();
  }

  function hideModal(id) {
    const instance = window.bootstrap?.Modal?.getInstance(document.getElementById(id));
    instance?.hide();
  }

  function waitForModalHidden(id) {
    return new Promise((resolve) => {
      const modalElement = document.getElementById(id);
      if (!modalElement?.classList.contains("show")) {
        resolve();
        return;
      }
      let settled = false;
      const finish = () => {
        if (settled) {
          return;
        }
        settled = true;
        modalElement.removeEventListener("hidden.bs.modal", finish);
        resolve();
      };
      modalElement.addEventListener("hidden.bs.modal", finish, { once: true });
      window.setTimeout(finish, 700);
    });
  }

  function closeAllModals() {
    ["songModal", "filesModal", "deleteModal", "passwordModal"].forEach(hideModal);
  }

  function showToast(message, variant = "success") {
    const container = el("toastContainer");
    if (!container || !window.bootstrap?.Toast) {
      console.warn(message);
      return;
    }

    const wrapper = document.createElement("div");
    wrapper.innerHTML = `
      <div class="toast align-items-center border-0 text-bg-${escapeHtml(variant)}" role="alert" aria-live="assertive" aria-atomic="true">
        <div class="d-flex">
          <div class="toast-body"></div>
          <button type="button" class="btn-close btn-close-white me-2 m-auto" data-bs-dismiss="toast" aria-label="Закрыть"></button>
        </div>
      </div>
    `;
    const toastElement = wrapper.firstElementChild;
    toastElement.querySelector(".toast-body").textContent = message;
    container.appendChild(toastElement);
    const toast = new window.bootstrap.Toast(toastElement, { delay: 5200 });
    toastElement.addEventListener("hidden.bs.toast", () => toastElement.remove(), { once: true });
    toast.show();
  }

  function showAppAlert(message, variant = "danger") {
    const alert = el("appAlert");
    alert.className = `alert alert-${variant}`;
    alert.textContent = message;
    alert.hidden = !message;
  }

  function showLoginAlert(message) {
    const alert = el("loginAlert");
    alert.textContent = message;
    alert.hidden = !message;
  }

  function showFormAlert(element, message) {
    element.textContent = message;
    element.hidden = !message;
  }

  function extractDetail(payload, fallback) {
    if (!payload) {
      return fallback;
    }
    const detail = payload.detail;
    if (typeof detail === "string") {
      return detail;
    }
    if (Array.isArray(detail)) {
      const messages = detail
        .map((item) => (typeof item === "string" ? item : item?.msg || item?.detail || ""))
        .filter(Boolean);
      return messages.length ? messages.join("; ") : fallback;
    }
    if (detail) {
      return String(detail);
    }
    return fallback;
  }

  function friendlyErrorMessage(error) {
    if (error instanceof ApiError) {
      if (error.status === 0) {
        return error.message;
      }
      if (error.status === 401) {
        return "Требуется авторизация. Войдите снова.";
      }
      if (error.status === 403) {
        return "Недостаточно прав. Нужен аккаунт администратора.";
      }
      if (error.status === 413) {
        return "Файл слишком большой для загрузки.";
      }
      return error.message;
    }
    return error?.message || "Неизвестная ошибка.";
  }

  function clearSession() {
    state.token = "";
    state.user = null;
    sessionStorage.removeItem(TOKEN_KEY);
  }

  // /auth/login also sets the persistent `ff_refresh` cookie. Clearing only the
  // in-memory admin token would leave a valid 30-day session on the machine, so
  // sign-out (and an invalid token) revokes it server-side as well.
  function revokePersistentSession() {
    request("/auth/logout", { method: "POST", auth: false }).catch(() => {});
  }

  function handleUnauthorized() {
    revokePersistentSession();
    clearSession();
    showLogin("Сессия истекла или недействительна. Войдите снова.");
  }

  async function request(path, { method = "GET", body = null, auth = true } = {}) {
    const headers = {};
    if (body !== null && !(body instanceof FormData)) {
      headers["Content-Type"] = "application/json";
    }
    if (auth && state.token) {
      headers.Authorization = `Bearer ${state.token}`;
    }

    let response;
    try {
      response = await fetch(`${API_BASE}${path}`, { method, headers, body });
    } catch {
      throw new ApiError("Не удалось связаться с сервером. Проверьте подключение.", 0);
    }

    const responseText = await response.text();
    let payload = null;
    if (responseText) {
      try {
        payload = JSON.parse(responseText);
      } catch {
        payload = null;
      }
    }

    if (!response.ok) {
      if (response.status === 401 && auth) {
        handleUnauthorized();
      }
      throw new ApiError(
        extractDetail(payload, `Ошибка запроса (${response.status})`),
        response.status,
      );
    }

    return payload;
  }

  function uploadWithProgress(path, formData, onProgress) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open("POST", `${API_BASE}${path}`);
      if (state.token) {
        xhr.setRequestHeader("Authorization", `Bearer ${state.token}`);
      }
      xhr.upload.addEventListener("progress", (event) => {
        if (event.lengthComputable) {
          onProgress(Math.min(100, Math.round((event.loaded / event.total) * 100)));
        }
      });
      xhr.addEventListener("load", () => {
        let payload = null;
        if (xhr.responseText) {
          try {
            payload = JSON.parse(xhr.responseText);
          } catch {
            payload = null;
          }
        }
        if (xhr.status >= 200 && xhr.status < 300) {
          resolve(payload);
          return;
        }
        if (xhr.status === 401) {
          handleUnauthorized();
        }
        reject(new ApiError(
          extractDetail(payload, `Ошибка загрузки (${xhr.status})`),
          xhr.status,
        ));
      });
      xhr.addEventListener("error", () => {
        reject(new ApiError("Не удалось загрузить файл. Проверьте подключение.", 0));
      });
      xhr.addEventListener("abort", () => {
        reject(new ApiError("Загрузка отменена.", 0));
      });
      xhr.send(formData);
    });
  }

  function slugify(value) {
    const transliteration = {
      а: "a", б: "b", в: "v", г: "g", д: "d", е: "e", ё: "e", ж: "zh",
      з: "z", и: "i", й: "y", к: "k", л: "l", м: "m", н: "n", о: "o",
      п: "p", р: "r", с: "s", т: "t", у: "u", ф: "f", х: "h", ц: "ts",
      ч: "ch", ш: "sh", щ: "sch", ъ: "", ы: "y", ь: "", э: "e",
      ю: "yu", я: "ya",
    };
    return String(value ?? "")
      .toLowerCase()
      .split("")
      .map((character) => transliteration[character] ?? character)
      .join("")
      .replace(/[^a-z0-9]+/g, "-")
      .replace(/^-+|-+$/g, "")
      .slice(0, 120);
  }

  function formatBytes(bytes) {
    const value = Number(bytes) || 0;
    if (value < 1024) {
      return `${value} Б`;
    }
    if (value < 1024 * 1024) {
      return `${(value / 1024).toFixed(1)} КБ`;
    }
    if (value < 1024 * 1024 * 1024) {
      return `${(value / (1024 * 1024)).toFixed(1)} МБ`;
    }
    return `${(value / (1024 * 1024 * 1024)).toFixed(2)} ГБ`;
  }

  function formatDuration(totalSeconds) {
    const seconds = Math.max(0, Math.round(Number(totalSeconds) || 0));
    const hours = Math.floor(seconds / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    const rest = seconds % 60;
    if (hours > 0) {
      return `${hours}:${String(minutes).padStart(2, "0")}:${String(rest).padStart(2, "0")}`;
    }
    return `${minutes}:${String(rest).padStart(2, "0")}`;
  }

  function formatDate(value) {
    if (!value) {
      return "—";
    }
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) {
      return "—";
    }
    return new Intl.DateTimeFormat("ru-RU", {
      day: "2-digit",
      month: "short",
      year: "numeric",
      hour: "2-digit",
      minute: "2-digit",
    }).format(date);
  }

  function readinessInfo(song) {
    return READINESS[song?.readiness?.state] || READINESS.empty;
  }

  function readinessBadge(song) {
    const info = readinessInfo(song);
    const missing = (song?.readiness?.missing_roles || []).map((role) => ROLE_LABELS[role] || role);
    const title = missing.length ? `Не хватает: ${missing.join(", ")}` : "Все файлы загружены";
    return `<span class="badge-soft ${info.className}" title="${escapeHtml(title)}">
      <i class="bi ${song?.readiness?.publish_ready ? "bi-check2-circle" : "bi-circle"}"></i>${info.label}
    </span>`;
  }

  function publishedBadge(song) {
    if (song.archived_at) {
      return '<span class="badge-soft badge-draft" title="Скрыта из каталога, результаты сохранены"><i class="bi bi-archive"></i>В архиве</span>';
    }
    return song.is_published
      ? '<span class="badge-soft badge-published"><i class="bi bi-broadcast"></i>Опубликована</span>'
      : '<span class="badge-soft badge-draft"><i class="bi bi-pencil-square"></i>Черновик</span>';
  }

  // A song with a daily slot is not in the ordinary catalogue; the badge makes
  // that visible right in the list instead of only on the Daily Challenge tab.
  function dailyBadge(song) {
    const weekday = song?.daily_weekday;
    if (weekday === null || weekday === undefined) {
      return "";
    }
    const label = WEEKDAY_SHORT[weekday] || String(weekday);
    return `<span class="badge-soft badge-midi" title="Песня дня: ${escapeHtml(label)}">
      <i class="bi bi-calendar-check"></i>Daily · ${escapeHtml(label)}
    </span>`;
  }

  function setSongsLoading(loading) {
    ["refreshButton", "refreshButtonTop", "refreshButtonBottom"].forEach((id) => {
      const button = el(id);
      if (button) {
        button.disabled = loading;
      }
    });
    if (loading) {
      el("songsBody").innerHTML = '<tr><td colspan="9" class="table-loading"><span class="spinner-border spinner-border-sm me-2"></span>Загрузка…</td></tr>';
      const table = el("songsBody").closest(".table-responsive");
      if (table) {
        table.hidden = false;
      }
      el("emptyState").hidden = true;
    }
  }

  function renderStats() {
    el("statTotal").textContent = String(state.total);
    el("statPublished").textContent = String(
      state.songs.filter((song) => song.is_published && !song.archived_at).length,
    );
    el("statDrafts").textContent = String(
      state.songs.filter((song) => !song.is_published && !song.archived_at).length,
    );
    el("statReady").textContent = String(
      state.songs.filter((song) => !song.archived_at && song.readiness?.publish_ready).length,
    );
    el("statScope").textContent = state.total > state.songs.length ? "на странице" : "по текущей выборке";
  }

  function renderEmptyState(hasRows) {
    const empty = el("emptyState");
    const table = el("songsBody").closest(".table-responsive");
    const hasFilter = Boolean(state.search) || state.published !== "";
    if (table) {
      table.hidden = !hasRows;
    }
    empty.hidden = hasRows;
    if (hasRows) {
      return;
    }
    const title = empty.querySelector("h2");
    const text = empty.querySelector("p");
    const createButton = el("emptyNewSongButton");
    if (hasFilter) {
      title.textContent = "Ничего не найдено";
      text.textContent = "Попробуйте изменить запрос или сбросить фильтр статуса.";
      createButton.hidden = true;
    } else {
      title.textContent = "Песен пока нет";
      text.textContent = "Создайте первую песню, загрузите MIDI и стемы — после этого её можно будет опубликовать.";
      createButton.hidden = false;
    }
  }

  // ``difficulty`` is the LVL shown to players: always clamp it to 1..10 so a
  // malformed value can never break the row markup.
  function songLevel(song) {
    const parsed = Number.parseInt(song?.difficulty, 10);
    if (!Number.isFinite(parsed)) {
      return 1;
    }
    return Math.min(10, Math.max(1, parsed));
  }

  // ---------------------------------------------------------------------
  // Gameplay visuals (UI LEVELS characters + STAGE LEVELS backgrounds)
  // ---------------------------------------------------------------------
  async function loadVisualOptions() {
    try {
      const [characters, stages] = await Promise.all([
        request("/admin/ui-levels"),
        request("/admin/stage-levels"),
      ]);
      state.characters = characters?.items || [];
      state.stages = stages?.items || [];
    } catch (error) {
      if (error.status === 401 || error.status === 403) {
        return;
      }
      // The selectors are optional metadata: the songs tab must keep working
      // when a visual lookup fails.
      state.characters = [];
      state.stages = [];
    }
    state.visualsLoaded = true;
    // Keep an open form in sync without losing the current selection.
    if (el("songCharacter") && el("songStage")) {
      renderSongVisualSelects(state.formCharacterId, state.formStageId);
    }
  }

  async function ensureVisualOptions() {
    if (state.visualsLoaded) {
      return;
    }
    await loadVisualOptions();
  }

  function stageOptionsHtml(selectedStageId) {
    // Stages are standalone backgrounds (migration 0025): every stage can be
    // paired with any character, so the character selection never filters them.
    const options = ['<option value="">Не задано</option>'];
    state.stages.forEach((stage) => {
      const selected = stage.id === selectedStageId ? " selected" : "";
      options.push(
        `<option value="${escapeHtml(stage.id)}"${selected}>${escapeHtml(stage.name)}</option>`,
      );
    });
    return options.join("");
  }

  function renderSongVisualSelects(characterId, stageId) {
    const characterSelect = el("songCharacter");
    const stageSelect = el("songStage");
    if (!characterSelect || !stageSelect) {
      return;
    }
    state.formCharacterId = characterId || "";
    state.formStageId = stageId || "";
    const characterOptions = ['<option value="">Не задано</option>'];
    state.characters.forEach((character) => {
      const selected = character.id === characterId ? " selected" : "";
      characterOptions.push(
        `<option value="${escapeHtml(character.id)}"${selected}>${escapeHtml(character.name)}</option>`,
      );
    });
    characterSelect.innerHTML = characterOptions.join("");
    characterSelect.value = characterId || "";
    stageSelect.innerHTML = stageOptionsHtml(stageId || "");
    stageSelect.value = stageId || "";
  }

  function visualCell(song) {
    const character = song.character_name
      ? `<span class="badge-soft badge-visual" title="UI LEVEL">${escapeHtml(song.character_name)}</span>`
      : "";
    const stage = song.stage_name
      ? `<span class="badge-soft badge-visual" title="STAGE LEVEL">${escapeHtml(song.stage_name)}</span>`
      : "";
    if (!character && !stage) {
      return '<span class="text-secondary small">—</span>';
    }
    return `<div class="d-flex flex-column gap-1 align-items-start">${character}${stage}</div>`;
  }

  function songRow(song) {
    const artist = song.artist ? escapeHtml(song.artist) : '<span class="text-secondary">Без исполнителя</span>';
    const initial = escapeHtml((song.title || "?").trim().charAt(0).toUpperCase() || "?");
    const archived = Boolean(song.archived_at);
    const publishAction = song.is_published ? "unpublish" : "publish";
    const publishIcon = song.is_published ? "bi-eye-slash" : "bi-cloud-arrow-up";
    const publishTitle = song.is_published ? "Снять с публикации" : "Опубликовать";
    const level = songLevel(song);
    // Meter is additive: payloads from before migration 0020 (or a cached
    // response) fall back to 4/4.
    const timeSignature = `${song?.time_signature_numerator ?? 4}/${song?.time_signature_denominator ?? 4}`;
    // The preview download needs the admin token, so the button is only a
    // trigger for the blob player; without an uploaded preview it is disabled.
    const hasPreview = Boolean(song.preview_url);
    const previewTitle = hasPreview
      ? "Послушать Preview Song"
      : "Preview Song не загружен — загрузите его в „Файлах“";
    // An archived song cannot be published or unpublished; it is restored first.
    const publishButton = archived
      ? `<button type="button" class="action-btn" data-action="restore" title="Вернуть из архива" aria-label="Вернуть из архива">
              <i class="bi bi-arrow-counterclockwise"></i>
            </button>`
      : `<button type="button" class="action-btn" data-action="${publishAction}" title="${publishTitle}" aria-label="${publishTitle}">
              <i class="bi ${publishIcon}"></i>
            </button>`;
    const deleteTitle = archived ? "Удалить навсегда" : "Скрыть из каталога";
    return `
      <tr data-id="${escapeHtml(song.id)}">
        <td>
          <div class="song-cell">
            <div class="cover-thumb">${initial}</div>
            <div class="min-w-0">
              <div class="song-title text-truncate">${escapeHtml(song.title)}</div>
              <div class="song-byline text-truncate">${artist}</div>
              <div class="song-slug text-truncate">${escapeHtml(song.slug)}</div>
            </div>
          </div>
        </td>
        <td><div class="d-flex flex-wrap gap-1">${publishedBadge(song)}${dailyBadge(song)}</div></td>
        <td>${readinessBadge(song)}</td>
        <td><span class="badge-soft badge-level" title="Уровень сложности">LVL ${level}</span></td>
        <td class="d-none d-lg-table-cell"><span class="text-nowrap">${escapeHtml(song.bpm)}</span></td>
        <td class="d-none d-lg-table-cell"><span class="text-nowrap">${escapeHtml(timeSignature)}</span></td>
        <td class="d-none d-xl-table-cell"><span class="text-nowrap">${formatDuration(song.duration_seconds)}</span></td>
        <td class="d-none d-xl-table-cell"><span class="text-nowrap small text-secondary">${formatDate(song.updated_at)}</span></td>
        <td class="d-none d-md-table-cell">${visualCell(song)}</td>
        <td class="text-end">
          <div class="action-group">
            <button type="button" class="action-btn" data-action="preview" title="${previewTitle}" aria-label="Послушать Preview Song" ${hasPreview ? "" : "disabled"}>
              <i class="bi bi-play-fill"></i>
            </button>
            <button type="button" class="action-btn" data-action="files" title="Файлы" aria-label="Файлы">
              <i class="bi bi-folder2-open"></i>
            </button>
            ${publishButton}
            <button type="button" class="action-btn" data-action="edit" title="Редактировать" aria-label="Редактировать">
              <i class="bi bi-pencil"></i>
            </button>
            <button type="button" class="action-btn action-danger" data-action="delete" title="${deleteTitle}" aria-label="${deleteTitle}">
              <i class="bi ${archived ? "bi-trash3" : "bi-archive"}"></i>
            </button>
          </div>
        </td>
      </tr>
    `;
  }

  function renderSongs() {
    // Re-rendering the table replaces the rows the player was started from, so
    // any reload/search/filter stops playback and releases the blob URL.
    closePreviewPlayer();
    const body = el("songsBody");
    body.innerHTML = state.songs.map(songRow).join("");
    renderEmptyState(state.songs.length > 0);

    if (state.total > state.songs.length) {
      el("pageNote").textContent = `Показаны ${state.songs.length} из ${state.total}. Уточните поиск, чтобы найти остальные песни.`;
      el("pageNote").hidden = false;
    } else {
      el("pageNote").hidden = true;
    }
    renderStats();
  }

  async function loadSongs() {
    const requestId = ++state.loadRequestId;
    setSongsLoading(true);
    try {
      const params = new URLSearchParams({ limit: "100", offset: "0" });
      if (state.search) {
        params.set("search", state.search);
      }
      if (state.published === "archived") {
        params.set("archived", "true");
      } else if (state.published !== "") {
        params.set("published", state.published);
      }
      const data = await request(`/admin/songs?${params.toString()}`);
      if (requestId !== state.loadRequestId) {
        return;
      }
      state.songs = data.items || [];
      state.total = data.total || 0;
      renderSongs();
      showAppAlert("");
    } catch (error) {
      if (requestId !== state.loadRequestId) {
        return;
      }
      if (error.status === 401 || error.status === 403) {
        return;
      }
      showAppAlert(friendlyErrorMessage(error));
      showToast(friendlyErrorMessage(error), "danger");
    } finally {
      if (requestId === state.loadRequestId) {
        setSongsLoading(false);
      }
    }
  }

  function renderBackgroundMusic(data) {
    const hasFile = Boolean(data?.url);
    const status = el("bgMusicStatus");
    status.textContent = hasFile ? "Загружено" : "Не загружено";
    status.className = `badge-soft ${hasFile ? "badge-complete" : "badge-empty"}`;
    el("bgMusicMeta").textContent = hasFile
      ? `${formatBytes(data.size_bytes)} · ${formatDate(data.uploaded_at)}${data.original_name ? ` · ${data.original_name}` : ""}`
      : "MP3 до 100 МБ";
    const link = el("bgMusicLink");
    link.hidden = !hasFile;
    if (hasFile) {
      link.href = data.url;
    }
    el("bgMusicDelete").hidden = !hasFile;
    el("bgMusicUpload").textContent = hasFile ? "Заменить" : "Загрузить";
  }

  async function loadBackgroundMusic() {
    try {
      const data = await request("/admin/app/background-music");
      renderBackgroundMusic(data);
    } catch (error) {
      // A failed status read must not break the songs screen; 401/403 are
      // already handled by the shared request helper.
      if (error.status === 401 || error.status === 403) {
        return;
      }
      showToast(friendlyErrorMessage(error), "danger");
    }
  }

  function setBackgroundMusicBusy(busy) {
    el("bgMusicFile").disabled = busy;
    el("bgMusicUpload").disabled = busy;
    el("bgMusicDelete").disabled = busy;
  }

  async function uploadBackgroundMusic() {
    const input = el("bgMusicFile");
    const file = input?.files?.[0];
    if (!file) {
      showToast("Сначала выберите MP3-файл.", "warning");
      return;
    }
    if (!/\.mp3$/i.test(file.name)) {
      showToast("Для Background Music нужен MP3-файл.", "warning");
      return;
    }
    if (file.size > MAX_AUDIO_BYTES) {
      showToast("MP3 больше 100 МБ. Уменьшите файл или выберите другой.", "warning");
      return;
    }

    const formData = new FormData();
    formData.append("file", file, file.name);
    setBackgroundMusicBusy(true);
    el("bgMusicProgressWrap").hidden = false;
    el("bgMusicProgress").style.width = "0%";
    try {
      await uploadWithProgress("/admin/app/background-music", formData, (percent) => {
        el("bgMusicProgress").style.width = `${percent}%`;
      });
      input.value = "";
      showToast("Background Music сохранена.");
      await loadBackgroundMusic();
    } catch (error) {
      showToast(friendlyErrorMessage(error), "danger");
    } finally {
      setBackgroundMusicBusy(false);
      el("bgMusicProgressWrap").hidden = true;
      el("bgMusicProgress").style.width = "0%";
    }
  }

  async function deleteBackgroundMusic() {
    setBackgroundMusicBusy(true);
    try {
      await request("/admin/app/background-music", { method: "DELETE" });
      el("bgMusicFile").value = "";
      showToast("Background Music удалена.");
      await loadBackgroundMusic();
    } catch (error) {
      showToast(friendlyErrorMessage(error), "danger");
    } finally {
      setBackgroundMusicBusy(false);
    }
  }

  function showLogin(message = "") {
    // A preview must not keep playing after sign-out or an expired session.
    closePreviewPlayer();
    closeAllModals();
    el("appView").hidden = true;
    el("loginView").hidden = false;
    el("loginPassword").value = "";
    showLoginAlert(message);
    window.setTimeout(() => el("loginIdentifier").focus(), 80);
  }

  function showApp() {
    el("loginView").hidden = true;
    el("appView").hidden = false;
    const user = state.user;
    const displayName = user?.display_name || user?.username || "Администратор";
    el("userName").textContent = displayName;
    el("userAvatar").textContent = displayName.trim().charAt(0).toUpperCase() || "A";
    el("userEmail").textContent = user?.email || "—";
    showLoginAlert("");
    loadBackgroundMusic();
    // Selectors of the song form; a failure is non-fatal (see loadVisualOptions).
    loadVisualOptions();
  }

  function setLoginBusy(busy) {
    el("loginSubmit").disabled = busy;
    el("loginSpinner").hidden = !busy;
  }

  async function handleLogin(event) {
    event.preventDefault();
    const form = el("loginForm");
    if (!form.checkValidity()) {
      form.classList.add("was-validated");
      return;
    }

    showLoginAlert("");
    setLoginBusy(true);
    try {
      const credentials = {
        identifier: el("loginIdentifier").value.trim(),
        password: el("loginPassword").value,
      };
      const tokenResponse = await request("/auth/login", {
        method: "POST",
        body: JSON.stringify(credentials),
        auth: false,
      });
      state.token = tokenResponse.access_token;
      sessionStorage.setItem(TOKEN_KEY, state.token);

      const [me] = await Promise.all([
        request("/auth/me"),
        request("/admin/songs?limit=1"),
      ]);
      state.user = me;
      await loadSongs();
      form.reset();
      form.classList.remove("was-validated");
      showApp();
      showToast(`Добро пожаловать, ${state.user.display_name || state.user.username}.`);
    } catch (error) {
      clearSession();
      if (error.status === 403) {
        showLoginAlert("У этого аккаунта нет прав администратора.");
      } else if (error.status === 401) {
        showLoginAlert("Неверный логин или пароль.");
      } else {
        showLoginAlert(friendlyErrorMessage(error));
      }
    } finally {
      setLoginBusy(false);
    }
  }

  async function restoreSession() {
    if (!state.token) {
      showLogin();
      return;
    }
    try {
      const [me] = await Promise.all([
        request("/auth/me"),
        request("/admin/songs?limit=1"),
      ]);
      state.user = me;
      await loadSongs();
      showApp();
    } catch (error) {
      if (error.status === 403) {
        clearSession();
        showLogin("У этого аккаунта нет прав администратора.");
      } else if (error.status === 401) {
        clearSession();
        showLogin("Сессия истекла. Войдите снова.");
      } else {
        clearSession();
        showLogin("Не удалось проверить сессию. Войдите снова.");
      }
    }
  }

  function logout() {
    revokePersistentSession();
    clearSession();
    showLogin();
  }

  async function openSongModal(song = null) {
    const form = el("songForm");
    form.reset();
    form.classList.remove("was-validated");
    showFormAlert(el("songFormAlert"), "");
    state.slugTouched = Boolean(song);
    // Load the character/stage options before rendering their selects, so an
    // edit always shows the stored selection even on a slow connection.
    await ensureVisualOptions();

    el("songId").value = song?.id || "";
    el("songTitle").value = song?.title || "";
    el("songSlug").value = song?.slug || "";
    el("songArtist").value = song?.artist || "";
    el("songGenre").value = song?.genre || "Other";
    el("songBpm").value = song?.bpm ?? 120;
    el("songTimeSignatureNumerator").value = song?.time_signature_numerator ?? 4;
    el("songTimeSignatureDenominator").value = String(song?.time_signature_denominator ?? 4);
    el("songDifficulty").value = String(song?.difficulty ?? 1);
    el("songDuration").value = song?.duration_seconds ?? 180;
    el("songDailyWeekday").value =
      song?.daily_weekday === null || song?.daily_weekday === undefined
        ? ""
        : String(song.daily_weekday);
    renderSongVisualSelects(song?.character_id || "", song?.stage_id || "");
    el("songDescription").value = song?.description || "";
    el("songModalTitle").textContent = song ? "Редактировать песню" : "Новая песня";
    el("songModalSubtitle").textContent = song
      ? "Измените метаданные. Файлы и публикация остаются без изменений."
      : "Заполните метаданные, файлы загрузите на следующем шаге.";
    el("songSaveLabel").textContent = song ? "Сохранить изменения" : "Создать песню";
    updateDurationPreview();
    showModal("songModal");
  }

  function setSongSaving(saving) {
    el("songSaveButton").disabled = saving;
    el("songSaveSpinner").hidden = !saving;
  }

  function updateDurationPreview() {
    const value = Number(el("songDuration").value);
    el("durationPreview").textContent = Number.isFinite(value) && value > 0
      ? formatDuration(value)
      : "—";
  }

  async function saveSong(event) {
    event.preventDefault();
    const form = el("songForm");
    if (!form.checkValidity()) {
      form.classList.add("was-validated");
      return;
    }

    const id = el("songId").value;
    const dailyWeekday = el("songDailyWeekday").value;
    const payload = {
      slug: el("songSlug").value.trim(),
      title: el("songTitle").value.trim(),
      artist: el("songArtist").value.trim() || null,
      genre: el("songGenre").value.trim() || "Other",
      bpm: Number(el("songBpm").value),
      // Meter of the song: the client places the metronome and bar lines from
      // it (7/8 → seven eighth-note clicks per bar).
      time_signature_numerator: Number(el("songTimeSignatureNumerator").value),
      time_signature_denominator: Number(el("songTimeSignatureDenominator").value),
      difficulty: Number(el("songDifficulty").value),
      duration_seconds: Number(el("songDuration").value),
      description: el("songDescription").value.trim() || null,
      // Always sent so that an explicit "Не задано" clears an existing slot;
      // PATCH treats a present null as "remove the song from the schedule".
      daily_weekday: dailyWeekday === "" ? null : Number(dailyWeekday),
      // Optional gameplay visuals; null clears the selection.
      character_id: el("songCharacter").value || null,
      stage_id: el("songStage").value || null,
    };

    showFormAlert(el("songFormAlert"), "");
    setSongSaving(true);
    try {
      const saved = id
        ? await request(`/admin/songs/${id}`, {
          method: "PATCH",
          body: JSON.stringify(payload),
        })
        : await request("/admin/songs", {
          method: "POST",
          body: JSON.stringify(payload),
        });

      hideModal("songModal");
      showToast(id ? "Изменения сохранены." : "Песня создана. Загрузите MIDI и стемы.");
      await loadSongs();
      if (!id) {
        await waitForModalHidden("songModal");
        await openFiles(saved.id);
      }
    } catch (error) {
      showFormAlert(
        el("songFormAlert"),
        error.status === 409 ? "Этот slug уже занят другой песней." : friendlyErrorMessage(error),
      );
    } finally {
      setSongSaving(false);
    }
  }

  function openDeleteModal(song) {
    state.pendingDelete = song;
    const archived = Boolean(song.archived_at);
    el("deleteSongTitle").textContent = song.title;
    el("deleteModalTitle").textContent = archived ? "Удалить песню навсегда?" : "Скрыть песню из каталога?";
    el("deleteModalText").textContent = archived
      ? "будет удалена из базы вместе с записями о файлах."
      : "исчезнет из каталога и из новых игровых сессий, а её идентификатор сохранится.";
    el("deleteModalHint").textContent = archived
      ? "Удаление возможно только если у песни нет результатов игроков. Если результаты есть, сервер откажет — оставьте песню в архиве."
      : "Все результаты игроков, личные рекорды, мастерство и Daily Challenge останутся на месте.";
    showModal("deleteModal");
  }

  function setDeleteBusy(busy) {
    el("confirmDeleteButton").disabled = busy;
    el("deleteSpinner").hidden = !busy;
  }

  async function confirmDelete() {
    const song = state.pendingDelete;
    if (!song) {
      return;
    }
    const archived = Boolean(song.archived_at);
    setDeleteBusy(true);
    try {
      const query = archived ? "?permanent=true" : "";
      await request(`/admin/songs/${song.id}${query}`, { method: "DELETE" });
      hideModal("deleteModal");
      state.pendingDelete = null;
      if (state.currentSong?.id === song.id) {
        hideModal("filesModal");
        state.currentSong = null;
      }
      showToast(archived ? "Песня удалена навсегда." : "Песня скрыта из каталога, результаты сохранены.");
      await loadSongs();
    } catch (error) {
      showToast(
        error.status === 409
          ? "Песню нельзя удалить: у неё есть результаты игроков. Оставьте её в архиве."
          : friendlyErrorMessage(error),
        "danger",
      );
    } finally {
      setDeleteBusy(false);
    }
  }

  async function restoreSong(song) {
    try {
      await request(`/admin/songs/${song.id}/restore`, { method: "POST" });
      showToast("Песня возвращена из архива как черновик.");
      await loadSongs();
      if (state.currentSong?.id === song.id) {
        await refreshCurrentSong();
      }
    } catch (error) {
      showToast(friendlyErrorMessage(error), "danger");
    }
  }

  async function togglePublish(song) {
    if (!song.is_published && !song.readiness?.publish_ready) {
      const proceed = window.confirm(
        "Песня ещё не готова к публикации: нужен MIDI и все четыре стема.\n\nОпубликовать всё равно?",
      );
      if (!proceed) {
        return;
      }
    }
    const action = song.is_published ? "unpublish" : "publish";
    try {
      await request(`/admin/songs/${song.id}/${action}`, { method: "POST" });
      showToast(song.is_published ? "Песня снята с публикации." : "Песня опубликована.");
      await loadSongs();
      if (state.currentSong?.id === song.id) {
        await refreshCurrentSong();
      }
    } catch (error) {
      showToast(friendlyErrorMessage(error), "danger");
    }
  }

  function downloadUrlForFile(file) {
    return file?.download_url || "";
  }

  function findCurrentFile(fileId) {
    if (!state.currentSong) {
      return null;
    }
    const versions = state.currentSong.files || [];
    const stored = versions.find((file) => file?.id === fileId);
    if (stored) {
      return stored;
    }
    const midi = state.currentSong.assets?.midi;
    if (midi?.id === fileId) {
      return midi;
    }
    const stems = state.currentSong.assets?.stems || {};
    return AUDIO_ROLES.map((role) => stems[role]).find((file) => file?.id === fileId) || null;
  }

  async function downloadFile(fileId) {
    const file = findCurrentFile(fileId);
    if (!file) {
      showToast("Файл не найден.", "danger");
      return;
    }
    const url = downloadUrlForFile(file);
    if (!url) {
      showToast("Для этого файла нет ссылки на скачивание.", "danger");
      return;
    }
    try {
      const response = await fetch(url, {
        headers: state.token ? { Authorization: `Bearer ${state.token}` } : {},
      });
      if (response.status === 401) {
        handleUnauthorized();
        throw new ApiError("Требуется авторизация.", 401);
      }
      if (!response.ok) {
        throw new ApiError(`Не удалось скачать файл (${response.status}).`, response.status);
      }
      const blob = await response.blob();
      const objectUrl = URL.createObjectURL(blob);
      const link = document.createElement("a");
      link.href = objectUrl;
      link.download = `${state.currentSong.slug}-${file.role}.${file.role === "midi" ? "mid" : "mp3"}`;
      document.body.appendChild(link);
      link.click();
      link.remove();
      window.setTimeout(() => URL.revokeObjectURL(objectUrl), 1500);
    } catch (error) {
      showToast(friendlyErrorMessage(error), "danger");
    }
  }

  function syncPreviewIcons() {
    const audio = el("songPreviewAudio");
    const playing = Boolean(audio) && !audio.paused && Boolean(state.previewSongId);
    el("songsBody").querySelectorAll('button[data-action="preview"]').forEach((button) => {
      const row = button.closest("tr[data-id]");
      const active = playing && row?.dataset.id === state.previewSongId;
      const icon = button.querySelector("i");
      if (icon) {
        icon.className = `bi ${active ? "bi-pause-fill" : "bi-play-fill"}`;
      }
    });
  }

  function stopPreviewPlayback() {
    const audio = el("songPreviewAudio");
    if (audio) {
      audio.pause();
      audio.removeAttribute("src");
      audio.load();
    }
    if (state.previewObjectUrl) {
      URL.revokeObjectURL(state.previewObjectUrl);
      state.previewObjectUrl = "";
    }
    state.previewSongId = null;
    syncPreviewIcons();
  }

  function resetPreviewProgress() {
    const bar = el("songPreviewProgressBar");
    const track = el("songPreviewProgressTrack");
    const text = el("songPreviewProgressText");
    if (bar) {
      bar.style.width = "0%";
      bar.classList.remove("progress-bar-striped", "progress-bar-animated");
    }
    if (track) {
      track.setAttribute("aria-valuenow", "0");
    }
    if (text) {
      text.textContent = "";
    }
  }

  function hidePreviewProgress() {
    const wrap = el("songPreviewProgress");
    if (wrap) {
      wrap.hidden = true;
    }
    resetPreviewProgress();
  }

  // ``total === 0`` means the server did not send Content-Length: show an
  // indeterminate striped bar instead of a fake percentage.
  function updatePreviewProgress(loaded, total) {
    const wrap = el("songPreviewProgress");
    const bar = el("songPreviewProgressBar");
    const track = el("songPreviewProgressTrack");
    const text = el("songPreviewProgressText");
    if (!wrap || !bar || !text) {
      return;
    }
    wrap.hidden = false;
    const received = formatBytes(loaded);
    if (total > 0) {
      const percent = Math.max(0, Math.min(100, Math.round((loaded / total) * 100)));
      bar.style.width = `${percent}%`;
      bar.classList.remove("progress-bar-striped", "progress-bar-animated");
      if (track) {
        track.setAttribute("aria-valuenow", String(percent));
      }
      text.textContent = `Загрузка ${percent}% · ${received} из ${formatBytes(total)}`;
      return;
    }
    // Unknown length: animate a full-width striped bar, drop aria-valuenow so
    // assistive technology does not announce a bogus percentage.
    bar.style.width = "100%";
    bar.classList.add("progress-bar-striped", "progress-bar-animated");
    if (track) {
      track.removeAttribute("aria-valuenow");
    }
    text.textContent = loaded > 0 ? `Загрузка… ${received}` : "Загрузка…";
  }

  function abortPreviewDownload() {
    const xhr = state.previewXhr;
    state.previewXhr = null;
    if (xhr) {
      xhr.abort();
    }
  }

  // The download route requires the admin Bearer token, so <audio> cannot point
  // at it directly. XHR is used instead of fetch because it reports download
  // progress and can be aborted when the administrator switches songs or closes
  // the player.
  function downloadPreviewBlob(url, requestId) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open("GET", url);
      xhr.responseType = "blob";
      if (state.token) {
        xhr.setRequestHeader("Authorization", `Bearer ${state.token}`);
      }
      xhr.addEventListener("progress", (event) => {
        if (requestId !== state.previewRequestId) {
          return;
        }
        updatePreviewProgress(event.loaded, event.lengthComputable ? event.total : 0);
      });
      xhr.addEventListener("load", () => {
        if (state.previewXhr === xhr) {
          state.previewXhr = null;
        }
        if (xhr.status >= 200 && xhr.status < 300) {
          resolve(xhr.response);
          return;
        }
        if (xhr.status === 401 || xhr.status === 403) {
          handleUnauthorized();
          reject(new ApiError("Требуется авторизация администратора.", xhr.status));
          return;
        }
        if (xhr.status === 404) {
          reject(new ApiError("Preview Song не найден. Загрузите его заново в «Файлах».", 404));
          return;
        }
        reject(new ApiError(`Не удалось загрузить Preview Song (${xhr.status}).`, xhr.status));
      });
      xhr.addEventListener("error", () => {
        if (state.previewXhr === xhr) {
          state.previewXhr = null;
        }
        reject(new ApiError("Не удалось загрузить Preview Song. Проверьте подключение.", 0));
      });
      xhr.addEventListener("abort", () => {
        if (state.previewXhr === xhr) {
          state.previewXhr = null;
        }
        reject(new ApiError("Загрузка отменена.", 0));
      });
      state.previewXhr = xhr;
      xhr.send();
    });
  }

  // Stops playback, frees the blob URL, cancels an in-flight download and hides
  // the player. Bumping the request id makes any late callback a no-op.
  function closePreviewPlayer() {
    state.previewRequestId += 1;
    state.previewLoading = false;
    abortPreviewDownload();
    stopPreviewPlayback();
    hidePreviewProgress();
    const player = el("songPreviewPlayer");
    if (player) {
      player.hidden = true;
    }
    const title = el("songPreviewTitle");
    if (title) {
      title.textContent = "";
    }
  }

  async function playPreview(song) {
    const audio = el("songPreviewAudio");
    const player = el("songPreviewPlayer");
    if (!audio || !player || !song) {
      return;
    }

    // Repeated click on the loaded song: toggle playback, no second download.
    if (state.previewSongId === song.id && state.previewObjectUrl) {
      if (audio.paused) {
        try {
          await audio.play();
        } catch {
          // A failed resume is visible in the native controls; nothing to do.
        }
      } else {
        audio.pause();
      }
      return;
    }
    if (state.previewSongId === song.id && state.previewLoading) {
      return;
    }

    if (!song.preview_url) {
      showToast("Preview Song не загружен — загрузите его в «Файлах».", "danger");
      return;
    }

    const requestId = ++state.previewRequestId;
    // Switching songs cancels the previous download, stops playback and frees
    // its object URL before the new one starts.
    abortPreviewDownload();
    stopPreviewPlayback();
    state.previewLoading = true;
    state.previewSongId = song.id;
    el("songPreviewTitle").textContent = song.artist
      ? `${song.title} — ${song.artist}`
      : song.title;
    player.hidden = false;
    resetPreviewProgress();
    updatePreviewProgress(0, 0);

    try {
      const blob = await downloadPreviewBlob(song.preview_url, requestId);
      if (requestId !== state.previewRequestId) {
        return;
      }
      hidePreviewProgress();
      const objectUrl = URL.createObjectURL(blob);
      state.previewObjectUrl = objectUrl;
      audio.src = objectUrl;
      try {
        await audio.play();
      } catch {
        // Browsers may block autoplay; the native controls stay usable.
      }
    } catch (error) {
      if (requestId !== state.previewRequestId) {
        return;
      }
      closePreviewPlayer();
      showToast(friendlyErrorMessage(error), "danger");
    } finally {
      if (requestId === state.previewRequestId) {
        state.previewLoading = false;
        hidePreviewProgress();
      }
    }
  }

  function fileVersionHtml(kind, file) {
    const active = Boolean(file.is_active);
    const badge = active
      ? '<span class="badge-soft badge-published">Активен</span>'
      : '<span class="badge-soft badge-empty">Архив</span>';
    const analyzeButton = active && kind === "midi"
      ? `<button type="button" class="btn btn-sm btn-outline-light" data-analyze title="Разобрать MIDI и сохранить ноты">
           <i class="bi bi-activity me-1"></i>Разобрать
         </button>`
      : "";
    const activateButton = active
      ? ""
      : `<button type="button" class="btn btn-sm btn-outline-warning" data-activate="${escapeHtml(file.id)}" title="Сделать эту версию активной">
           Сделать активной
         </button>`;
    return `
      <div class="file-version ${active ? "is-active" : "is-archive"}">
        <div class="file-version-info">
          ${badge}
          <span class="file-meta">${formatBytes(file.size_bytes)} · ${formatDate(file.created_at)}</span>
        </div>
        <div class="file-slot-actions">
          ${analyzeButton}
          <button type="button" class="btn btn-sm btn-outline-light" data-download="${escapeHtml(file.id)}" title="Скачать файл">
            <i class="bi bi-download"></i>
          </button>
          ${activateButton}
        </div>
      </div>
    `;
  }

  function fileSlotHtml(kind, role, versions, options = {}) {
    const key = role ? `${kind}-${role}` : kind;
    const label = ROLE_LABELS[role || kind] || kind;
    const icon = ROLE_ICONS[role || kind] || "bi-file-earmark";
    const accept = kind === "midi" ? ".mid,.midi" : ".mp3,audio/mpeg";
    const files = versions || [];
    const uploadLabel = files.length ? "Заменить" : "Загрузить";
    const rows = files.length
      ? files.map((file) => fileVersionHtml(kind, file)).join("")
      : '<div class="file-meta">Файл не загружен</div>';
    const deleteButton = options.deleteButton
      ? `<button type="button" class="btn btn-sm btn-outline-danger" data-delete-preview
                 title="Удалить Preview Song" aria-label="Удалить Preview Song" ${files.length ? "" : "hidden"}>
           <i class="bi bi-trash3"></i>
         </button>`
      : "";
    return `
      <div class="file-slot" data-key="${escapeHtml(key)}">
        <div class="file-slot-head">
          <div class="file-slot-title">
            <span class="slot-icon"><i class="bi ${icon}"></i></span>
            <div>
              <strong>${escapeHtml(label)}</strong>
              <div class="file-meta">${files.length > 1 ? `Версий: ${files.length}` : ""}</div>
            </div>
          </div>
          ${deleteButton ? `<div class="file-slot-actions">${deleteButton}</div>` : ""}
        </div>
        <div class="file-versions">${rows}</div>
        <div class="file-slot-upload">
          <input type="file" class="form-control form-control-sm" accept="${accept}"
                 data-file-kind="${kind}" data-file-role="${escapeHtml(role || "")}"
                 aria-label="Выбрать файл ${escapeHtml(label)}">
          <button type="button" class="btn btn-sm btn-warning" data-upload="${kind}" data-role="${escapeHtml(role || "")}">
            ${uploadLabel}
          </button>
        </div>
        <div class="progress upload-progress" hidden>
          <div class="progress-bar" data-progress-for="${escapeHtml(key)}" role="progressbar" style="width: 0%"></div>
        </div>
      </div>
    `;
  }

  function renderReadiness(detail) {
    const readiness = detail.readiness || {};
    const available = new Set(readiness.available_roles || []);
    const info = READINESS[readiness.state] || READINESS.empty;
    const percent = Math.round((available.size / ALL_ROLES.length) * 100);
    const roleChips = ALL_ROLES.map((role) => `
      <span class="role-chip ${available.has(role) ? "is-ready" : ""}">
        <i class="bi ${available.has(role) ? "bi-check2" : "bi-dash"}"></i>${ROLE_LABELS[role]}
      </span>
    `).join("");
    const missing = (readiness.missing_roles || []).map((role) => ROLE_LABELS[role] || role);
    return `
      <div class="readiness-head">
        <div>
          <div class="readiness-title">
            <span class="badge-soft ${info.className} me-1">${info.label}</span>
            ${readiness.publish_ready ? "Можно публиковать" : "Публикация пока недоступна"}
          </div>
          <div class="readiness-sub">${available.size} из ${ALL_ROLES.length} файлов загружено${missing.length ? ` · не хватает: ${escapeHtml(missing.join(", "))}` : ""}</div>
        </div>
        <div class="text-secondary small">${readiness.is_playable ? "MIDI готов" : "Нужен MIDI"}</div>
      </div>
      <div class="readiness-progress">
        <div class="progress-bar" role="progressbar" style="width: ${percent}%" aria-valuenow="${percent}" aria-valuemin="0" aria-valuemax="100"></div>
      </div>
      <div class="role-chips">${roleChips}</div>
    `;
  }

  function renderAnalysis() {
    const panel = el("analysisPanel");
    if (!state.analysis) {
      // A song whose MIDI exists but was never parsed is a normal state, not an
      // error: offer the analyze action instead of showing a failure toast.
      panel.innerHTML = state.analysisMissing
        ? `
          <div class="analysis-card">
            <div class="d-flex justify-content-between align-items-start gap-2">
              <div>
                <div class="fw-semibold"><i class="bi bi-activity me-1 text-warning"></i>Анализ MIDI</div>
                <div class="small text-secondary">Анализ не найден — ноты ещё не сохранены</div>
              </div>
              <button type="button" class="btn btn-sm btn-outline-light" data-analyze>
                <i class="bi bi-activity me-1"></i>Разобрать
              </button>
            </div>
          </div>
        `
        : "";
      bindAnalysisActions();
      return;
    }
    const analysis = state.analysis;
    const tempo = (analysis.tempo_changes || [])
      .slice(0, 4)
      .map((item) => `${item.bpm.toFixed ? item.bpm.toFixed(1) : item.bpm} BPM @ ${item.tick}`)
      .join(" · ");
    panel.innerHTML = `
      <div class="analysis-card">
        <div class="d-flex justify-content-between align-items-start gap-2">
          <div>
            <div class="fw-semibold"><i class="bi bi-activity me-1 text-warning"></i>Анализ MIDI</div>
            <div class="small text-secondary">Сохранённые ноты и темповая карта</div>
          </div>
          <span class="small text-secondary">${formatDate(analysis.parsed_at)}</span>
        </div>
        <div class="analysis-grid">
          <div class="analysis-stat"><div class="label">Нот</div><div class="value">${analysis.note_count}</div></div>
          <div class="analysis-stat"><div class="label">BPM</div><div class="value">${Number(analysis.bpm).toFixed(1)}</div></div>
          <div class="analysis-stat"><div class="label">Треков</div><div class="value">${analysis.track_count}</div></div>
          <div class="analysis-stat"><div class="label">Длительность</div><div class="value">${formatDuration(analysis.duration_seconds)}</div></div>
        </div>
        ${tempo ? `<div class="analysis-notes">Темп: ${escapeHtml(tempo)}</div>` : ""}
        ${analysis.notes_truncated ? '<div class="analysis-notes">В ответе показаны первые 2000 нот.</div>' : ""}
      </div>
    `;
  }

  function bindAnalysisActions() {
    el("analysisPanel").querySelectorAll("[data-analyze]").forEach((button) => {
      button.addEventListener("click", () => analyzeMidi(button));
    });
  }

  function bindFileActions() {
    const body = el("filesBody");
    body.querySelectorAll("[data-upload]").forEach((button) => {
      button.addEventListener("click", () => uploadFromSlot(button));
    });
    body.querySelectorAll("[data-download]").forEach((button) => {
      button.addEventListener("click", () => downloadFile(button.dataset.download));
    });
    body.querySelectorAll("[data-analyze]").forEach((button) => {
      button.addEventListener("click", () => analyzeMidi(button));
    });
    body.querySelectorAll("[data-activate]").forEach((button) => {
      button.addEventListener("click", () => activateFile(button));
    });
    body.querySelectorAll("[data-delete-preview]").forEach((button) => {
      button.addEventListener("click", () => deletePreview(button));
    });
  }

  function versionsByRole(detail) {
    const groups = {};
    (detail.files || []).forEach((file) => {
      if (!file || !file.role) {
        return;
      }
      (groups[file.role] = groups[file.role] || []).push(file);
    });
    Object.keys(groups).forEach((role) => {
      groups[role].sort((left, right) => {
        if (Boolean(left.is_active) !== Boolean(right.is_active)) {
          return left.is_active ? -1 : 1;
        }
        return new Date(right.created_at) - new Date(left.created_at);
      });
    });
    return groups;
  }

  function renderFiles(detail) {
    const byRole = versionsByRole(detail);
    el("filesTitle").textContent = detail.title;
    el("filesMeta").textContent = `${detail.archived_at ? "В архиве" : detail.is_published ? "Опубликована" : "Черновик"} · ${detail.slug} · ${detail.bpm} BPM · ${detail.time_signature_numerator ?? 4}/${detail.time_signature_denominator ?? 4} · LVL ${songLevel(detail)} · ${formatDuration(detail.duration_seconds)}`;
    el("filesReadiness").innerHTML = renderReadiness(detail);
    el("filesBody").innerHTML = `
      <div class="file-list">
        ${fileSlotHtml("midi", "", byRole.midi)}
        ${AUDIO_ROLES.map((role) => fileSlotHtml("audio", role, byRole[role])).join("")}
      </div>
      <div class="mt-4">
        <div class="file-list">
          ${fileSlotHtml("preview", "preview", byRole.preview, { deleteButton: true })}
        </div>
        <p class="form-hint mb-0 mt-2">
          Preview Song — полный микс песни для неигровых экранов. Не влияет на готовность,
          публикацию и игровые стемы.
        </p>
      </div>
    `;
    bindFileActions();
    renderAnalysis();
  }

  async function loadAnalysis(detail) {
    state.analysis = null;
    state.analysisMissing = false;
    if (!detail.assets?.midi) {
      return;
    }
    try {
      state.analysis = await request(
        `/admin/songs/${detail.id}/files/${detail.assets.midi.id}/analysis`,
      );
    } catch (error) {
      if (error.status === 404) {
        // 404 means "not analyzed yet" — the UI offers the analyze action.
        state.analysisMissing = true;
      } else {
        // Other failures are reported by the action that triggered the refresh,
        // not while opening the file manager.
        console.warn("Could not load MIDI analysis", error);
      }
    }
  }

  async function openFiles(songId) {
    el("filesTitle").textContent = "Файлы песни";
    el("filesMeta").textContent = "Загрузка…";
    el("filesReadiness").innerHTML = "";
    el("filesBody").innerHTML = '<div class="table-loading"><span class="spinner-border spinner-border-sm me-2"></span>Загрузка файлов…</div>';
    el("analysisPanel").innerHTML = "";
    showModal("filesModal");

    try {
      const detail = await request(`/admin/songs/${songId}`);
      state.currentSong = detail;
      await loadAnalysis(detail);
      renderFiles(detail);
    } catch (error) {
      hideModal("filesModal");
      showToast(friendlyErrorMessage(error), "danger");
    }
  }

  async function refreshCurrentSong() {
    if (!state.currentSong) {
      return;
    }
    const detail = await request(`/admin/songs/${state.currentSong.id}`);
    state.currentSong = detail;
    await loadAnalysis(detail);
    renderFiles(detail);
    await loadSongs();
  }

  function setSlotBusy(button, busy) {
    const slot = button.closest(".file-slot");
    if (!slot) {
      return;
    }
    slot.querySelectorAll("input, button").forEach((control) => {
      control.disabled = busy;
    });
  }

  function setUploadProgress(kind, role, percent) {
    const key = role ? `${kind}-${role}` : kind;
    const bar = el("filesBody").querySelector(`[data-progress-for="${key}"]`);
    if (!bar) {
      return;
    }
    const progress = bar.closest(".upload-progress");
    if (progress) {
      progress.hidden = false;
    }
    bar.style.width = `${percent}%`;
  }

  async function uploadFromSlot(button) {
    const song = state.currentSong;
    if (!song) {
      return;
    }
    const kind = button.dataset.upload;
    const role = button.dataset.role || "";
    const input = el("filesBody").querySelector(
      `input[data-file-kind="${kind}"][data-file-role="${role}"]`,
    );
    const file = input?.files?.[0];
    if (!file) {
      showToast("Сначала выберите файл.", "warning");
      return;
    }
    if (kind === "midi") {
      if (!/\.(mid|midi)$/i.test(file.name)) {
        showToast("Для MIDI нужен файл .mid или .midi.", "warning");
        return;
      }
      if (file.size > MAX_MIDI_BYTES) {
        showToast("MIDI больше 20 МБ. Уменьшите файл или выберите другой.", "warning");
        return;
      }
    } else {
      if (!/\.mp3$/i.test(file.name)) {
        showToast(
          kind === "preview" ? "Для Preview Song нужен MP3-файл." : "Для стема нужен MP3-файл.",
          "warning",
        );
        return;
      }
      if (file.size > MAX_AUDIO_BYTES) {
        showToast("MP3 больше 100 МБ. Уменьшите файл или выберите другой.", "warning");
        return;
      }
    }

    const formData = new FormData();
    formData.append("file", file, file.name);
    const path = kind === "midi"
      ? `/admin/songs/${song.id}/files/midi`
      : `/admin/songs/${song.id}/files/audio/${role}`;

    setSlotBusy(button, true);
    setUploadProgress(kind, role, 0);
    try {
      await uploadWithProgress(path, formData, (percent) => setUploadProgress(kind, role, percent));
      showToast(`Файл «${ROLE_LABELS[role || "midi"]}» загружен.`);
      state.analysis = null;
      await refreshCurrentSong();
    } catch (error) {
      showToast(friendlyErrorMessage(error), "danger");
    } finally {
      setSlotBusy(button, false);
    }
  }

  async function activateFile(button) {
    const song = state.currentSong;
    if (!song) {
      return;
    }
    const fileId = button.dataset.activate;
    if (!fileId) {
      return;
    }
    setSlotBusy(button, true);
    try {
      await request(`/admin/songs/${song.id}/files/${fileId}/activate`, {
        method: "POST",
      });
      showToast("Версия файла активирована.");
      state.analysis = null;
      await refreshCurrentSong();
    } catch (error) {
      showToast(friendlyErrorMessage(error), "danger");
      setSlotBusy(button, false);
    }
  }

  async function deletePreview(button) {
    const song = state.currentSong;
    if (!song) {
      return;
    }
    setSlotBusy(button, true);
    try {
      await request(`/admin/songs/${song.id}/files/preview`, { method: "DELETE" });
      showToast("Preview Song удалён.");
      await refreshCurrentSong();
    } catch (error) {
      showToast(friendlyErrorMessage(error), "danger");
      setSlotBusy(button, false);
    }
  }

  async function analyzeMidi(button) {
    const song = state.currentSong;
    const midi = song?.assets?.midi;
    if (!song || !midi) {
      return;
    }
    const original = button.innerHTML;
    button.disabled = true;
    button.innerHTML = '<span class="spinner-border spinner-border-sm" aria-hidden="true"></span>';
    try {
      state.analysis = await request(
        `/admin/songs/${song.id}/files/${midi.id}/analyze`,
        { method: "POST" },
      );
      state.analysisMissing = false;
      renderAnalysis();
      showToast("MIDI проанализирован.");
    } catch (error) {
      showToast(friendlyErrorMessage(error), "danger");
      button.disabled = false;
      button.innerHTML = original;
    }
  }

  function openPasswordModal() {
    const form = el("passwordForm");
    form.reset();
    form.classList.remove("was-validated");
    showFormAlert(el("passwordFormAlert"), "");
    showModal("passwordModal");
  }

  function setPasswordSaving(saving) {
    el("passwordSaveButton").disabled = saving;
    el("passwordSpinner").hidden = !saving;
  }

  async function savePassword(event) {
    event.preventDefault();
    const form = el("passwordForm");
    const newPassword = el("newPassword").value;
    const confirmPassword = el("confirmPassword").value;
    if (!form.checkValidity()) {
      form.classList.add("was-validated");
      return;
    }
    if (newPassword !== confirmPassword) {
      showFormAlert(el("passwordFormAlert"), "Новые пароли не совпадают.");
      return;
    }

    showFormAlert(el("passwordFormAlert"), "");
    setPasswordSaving(true);
    try {
      await request("/auth/change-password", {
        method: "POST",
        body: JSON.stringify({
          current_password: el("currentPassword").value,
          new_password: newPassword,
        }),
      });
      hideModal("passwordModal");
      showToast("Пароль изменён.");
    } catch (error) {
      showFormAlert(
        el("passwordFormAlert"),
        error.status === 400 ? "Текущий пароль неверен." : friendlyErrorMessage(error),
      );
    } finally {
      setPasswordSaving(false);
    }
  }

  function bindEvents() {
    el("loginForm").addEventListener("submit", handleLogin);
    el("toggleLoginPassword").addEventListener("click", () => {
      const input = el("loginPassword");
      const icon = el("toggleLoginPassword").querySelector("i");
      const isPassword = input.type === "password";
      input.type = isPassword ? "text" : "password";
      icon.className = `bi ${isPassword ? "bi-eye-slash" : "bi-eye"}`;
      el("toggleLoginPassword").title = isPassword ? "Скрыть пароль" : "Показать пароль";
    });

    el("logoutButton").addEventListener("click", logout);
    el("newSongButton").addEventListener("click", () => openSongModal());
    el("emptyNewSongButton").addEventListener("click", () => openSongModal());
    el("refreshButton").addEventListener("click", loadSongs);
    el("refreshButtonTop").addEventListener("click", loadSongs);
    el("refreshButtonBottom").addEventListener("click", loadSongs);
    el("changePasswordButton").addEventListener("click", openPasswordModal);
    el("filesRefreshButton").addEventListener("click", () => {
      refreshCurrentSong().catch((error) => showToast(friendlyErrorMessage(error), "danger"));
    });
    el("bgMusicUpload").addEventListener("click", uploadBackgroundMusic);
    el("bgMusicDelete").addEventListener("click", deleteBackgroundMusic);
    el("songPreviewClose").addEventListener("click", closePreviewPlayer);
    const previewAudio = el("songPreviewAudio");
    previewAudio.addEventListener("play", syncPreviewIcons);
    previewAudio.addEventListener("pause", syncPreviewIcons);
    previewAudio.addEventListener("ended", syncPreviewIcons);

    let searchTimer = null;
    el("searchInput").addEventListener("input", (event) => {
      window.clearTimeout(searchTimer);
      searchTimer = window.setTimeout(() => {
        state.search = event.target.value.trim();
        loadSongs();
      }, 320);
    });
    el("publishedFilter").addEventListener("change", (event) => {
      state.published = event.target.value;
      loadSongs();
    });

    el("songsBody").addEventListener("click", (event) => {
      const button = event.target.closest("button[data-action]");
      if (!button || button.disabled) {
        return;
      }
      const row = button.closest("tr[data-id]");
      const song = state.songs.find((item) => item.id === row?.dataset.id);
      if (!song) {
        return;
      }
      const action = button.dataset.action;
      if (action === "preview") {
        playPreview(song);
      } else if (action === "files") {
        openFiles(song.id);
      } else if (action === "edit") {
        openSongModal(song);
      } else if (action === "delete") {
        openDeleteModal(song);
      } else if (action === "restore") {
        restoreSong(song);
      } else if (action === "publish" || action === "unpublish") {
        togglePublish(song);
      }
    });

    el("songTitle").addEventListener("input", () => {
      if (!state.slugTouched) {
        el("songSlug").value = slugify(el("songTitle").value);
      }
    });
    el("songSlug").addEventListener("input", () => {
      state.slugTouched = true;
    });
    el("songDuration").addEventListener("input", updateDurationPreview);
    el("songCharacter").addEventListener("change", () => {
      state.formCharacterId = el("songCharacter").value;
    });
    el("songStage").addEventListener("change", () => {
      state.formStageId = el("songStage").value;
    });
    el("songForm").addEventListener("submit", saveSong);

    el("confirmDeleteButton").addEventListener("click", confirmDelete);
    el("passwordForm").addEventListener("submit", savePassword);
  }

  function init() {
    bindEvents();
    if (state.token) {
      restoreSession();
    } else {
      showLogin();
    }
  }

  // Shared with the other admin modules (statistics): an expired token must
  // always fall back to the single login view of this SPA instead of every
  // module growing its own login screen, and destructive actions reuse the same
  // toast surface and knowledge of the signed-in administrator.
  window.FFAdmin = {
    handleUnauthorized,
    clearSession,
    showLogin,
    showToast,
    getUser: () => state.user,
    // The Daily Challenge tab changes song slots; the songs table shows the
    // resulting badge and catalogue state, so it exposes a reload hook.
    reloadSongs: loadSongs,
    // The gameplay visuals tabs change the character/stage selectors.
    reloadVisualOptions: loadVisualOptions,
  };

  document.addEventListener("DOMContentLoaded", init);
})();
