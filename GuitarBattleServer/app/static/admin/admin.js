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
  };
  const ROLE_ICONS = {
    midi: "bi-file-earmark-music",
    drums: "bi-disc",
    bass: "bi-music-note",
    guitar: "bi-music-note-beamed",
    others: "bi-soundwave",
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
    total: 0,
    search: "",
    published: "",
    currentSong: null,
    analysis: null,
    analysisMissing: false,
    pendingDelete: null,
    slugTouched: false,
    loadRequestId: 0,
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

  function handleUnauthorized() {
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
      el("songsBody").innerHTML = '<tr><td colspan="7" class="table-loading"><span class="spinner-border spinner-border-sm me-2"></span>Загрузка…</td></tr>';
      const table = el("songsBody").closest(".table-responsive");
      if (table) {
        table.hidden = false;
      }
      el("emptyState").hidden = true;
    }
  }

  function renderStats() {
    el("statTotal").textContent = String(state.total);
    el("statPublished").textContent = String(state.songs.filter((song) => song.is_published).length);
    el("statDrafts").textContent = String(state.songs.filter((song) => !song.is_published).length);
    el("statReady").textContent = String(state.songs.filter((song) => song.readiness?.publish_ready).length);
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

  function songRow(song) {
    const artist = song.artist ? escapeHtml(song.artist) : '<span class="text-secondary">Без исполнителя</span>';
    const initial = escapeHtml((song.title || "?").trim().charAt(0).toUpperCase() || "?");
    const publishAction = song.is_published ? "unpublish" : "publish";
    const publishIcon = song.is_published ? "bi-eye-slash" : "bi-cloud-arrow-up";
    const publishTitle = song.is_published ? "Снять с публикации" : "Опубликовать";
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
        <td class="d-none d-lg-table-cell"><span class="text-nowrap">${escapeHtml(song.bpm)}</span></td>
        <td class="d-none d-xl-table-cell"><span class="text-nowrap">${formatDuration(song.duration_seconds)}</span></td>
        <td class="d-none d-xl-table-cell"><span class="text-nowrap small text-secondary">${formatDate(song.updated_at)}</span></td>
        <td class="text-end">
          <div class="action-group">
            <button type="button" class="action-btn" data-action="files" title="Файлы" aria-label="Файлы">
              <i class="bi bi-folder2-open"></i>
            </button>
            <button type="button" class="action-btn" data-action="${publishAction}" title="${publishTitle}" aria-label="${publishTitle}">
              <i class="bi ${publishIcon}"></i>
            </button>
            <button type="button" class="action-btn" data-action="edit" title="Редактировать" aria-label="Редактировать">
              <i class="bi bi-pencil"></i>
            </button>
            <button type="button" class="action-btn action-danger" data-action="delete" title="Удалить" aria-label="Удалить">
              <i class="bi bi-trash3"></i>
            </button>
          </div>
        </td>
      </tr>
    `;
  }

  function renderSongs() {
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
      if (state.published !== "") {
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

  function showLogin(message = "") {
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
    clearSession();
    showLogin();
  }

  function openSongModal(song = null) {
    const form = el("songForm");
    form.reset();
    form.classList.remove("was-validated");
    showFormAlert(el("songFormAlert"), "");
    state.slugTouched = Boolean(song);

    el("songId").value = song?.id || "";
    el("songTitle").value = song?.title || "";
    el("songSlug").value = song?.slug || "";
    el("songArtist").value = song?.artist || "";
    el("songGenre").value = song?.genre || "Other";
    el("songBpm").value = song?.bpm ?? 120;
    el("songDifficulty").value = String(song?.difficulty ?? 1);
    el("songDuration").value = song?.duration_seconds ?? 180;
    el("songDailyWeekday").value =
      song?.daily_weekday === null || song?.daily_weekday === undefined
        ? ""
        : String(song.daily_weekday);
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
      difficulty: Number(el("songDifficulty").value),
      duration_seconds: Number(el("songDuration").value),
      description: el("songDescription").value.trim() || null,
      // Always sent so that an explicit "Не задано" clears an existing slot;
      // PATCH treats a present null as "remove the song from the schedule".
      daily_weekday: dailyWeekday === "" ? null : Number(dailyWeekday),
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
    el("deleteSongTitle").textContent = song.title;
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
    setDeleteBusy(true);
    try {
      await request(`/admin/songs/${song.id}`, { method: "DELETE" });
      hideModal("deleteModal");
      state.pendingDelete = null;
      if (state.currentSong?.id === song.id) {
        hideModal("filesModal");
        state.currentSong = null;
      }
      showToast("Песня удалена.");
      await loadSongs();
    } catch (error) {
      showToast(
        error.status === 409
          ? "Песню нельзя удалить: она используется в игровых сессиях."
          : friendlyErrorMessage(error),
        "danger",
      );
    } finally {
      setDeleteBusy(false);
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

  function fileSlotHtml(kind, role, versions) {
    const key = role ? `${kind}-${role}` : kind;
    const label = ROLE_LABELS[role || kind] || kind;
    const icon = ROLE_ICONS[role || kind] || "bi-file-earmark";
    const accept = kind === "midi" ? ".mid,.midi" : ".mp3,audio/mpeg";
    const files = versions || [];
    const uploadLabel = files.length ? "Заменить" : "Загрузить";
    const rows = files.length
      ? files.map((file) => fileVersionHtml(kind, file)).join("")
      : '<div class="file-meta">Файл не загружен</div>';
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
    el("filesMeta").textContent = `${detail.is_published ? "Опубликована" : "Черновик"} · ${detail.slug} · ${detail.bpm} BPM · ${formatDuration(detail.duration_seconds)}`;
    el("filesReadiness").innerHTML = renderReadiness(detail);
    el("filesBody").innerHTML = `
      <div class="file-list">
        ${fileSlotHtml("midi", "", byRole.midi)}
        ${AUDIO_ROLES.map((role) => fileSlotHtml("audio", role, byRole[role])).join("")}
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
        showToast("Для стема нужен MP3-файл.", "warning");
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
      if (!button) {
        return;
      }
      const row = button.closest("tr[data-id]");
      const song = state.songs.find((item) => item.id === row?.dataset.id);
      if (!song) {
        return;
      }
      const action = button.dataset.action;
      if (action === "files") {
        openFiles(song.id);
      } else if (action === "edit") {
        openSongModal(song);
      } else if (action === "delete") {
        openDeleteModal(song);
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
  };

  document.addEventListener("DOMContentLoaded", init);
})();
