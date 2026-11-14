/* fretfight admin — campaigns ("Tower riffs").
 *
 * A campaign is a name, a default bot difficulty, a description, a cover image
 * and an ordered list of songs. Every stage carries its own bot difficulty,
 * pre-filled from the campaign default when the song is added and saved per
 * stage with the ordered list. The cover is uploaded separately: every upload
 * stores a new uuid file name, so the returned URL changes and the preview
 * (and the client cache) picks up the new image. The number of stages equals
 * the number of added songs; the administrator can create as many campaigns as
 * needed.
 *
 * Kept separate from admin.js (songs) and admin-site.js (landing/builds): it
 * only needs the administrator JWT from sessionStorage and the shared helpers
 * exposed by admin.js.
 *
 * Public surface: window.FFAdminCampaigns.show() — lazy first load, refresh on
 * every later visit; window.FFAdminCampaigns.refresh() — explicit reload.
 */
(() => {
  "use strict";

  const API_BASE = "/api/v1";
  const TOKEN_KEY = "guitarbattle.admin.token";
  const BOT_LEVELS = [
    { value: 0, label: "EASY" },
    { value: 1, label: "NORMAL" },
    { value: 2, label: "HARD" },
  ];
  const SONG_PAGE_LIMIT = 200;
  // Mirrors MAX_CAMPAIGN_COVER_BYTES on the server (2 MiB). Checked here first
  // so an oversized file is rejected without uploading it.
  const MAX_COVER_BYTES = 2 * 1024 * 1024;

  const state = {
    loaded: false,
    requestId: 0,
    campaigns: [],
    songs: [],
    characters: [],
    selectedId: null,
    draftSongs: [],
    original: null,
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
      return "Кампания или песня не найдена — возможно, её удалили.";
    }
    if (error && error.status === 409) {
      return "Кампания с таким названием уже существует.";
    }
    if (error && error.status === 413) {
      return "Файл больше 2 МБ.";
    }
    if (error && error.status === 415) {
      return "Нужен файл PNG, JPEG или WebP.";
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

  function showAlert(id, message, variant) {
    const box = el(id);
    if (!box) return;
    box.className = "alert alert-" + (variant || "danger");
    box.textContent = message || "";
    box.hidden = !message;
  }

  async function api(path, options = {}) {
    const token = sessionStorage.getItem(TOKEN_KEY) || "";
    const headers = token ? { Authorization: "Bearer " + token } : {};
    const init = { method: options.method || "GET", headers, credentials: "omit" };
    if (options.body instanceof FormData) {
      // Let the browser set the multipart boundary; a manual JSON header
      // would break the upload.
      init.body = options.body;
    } else if (options.body !== undefined) {
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
      if (response.status === 401) handleUnauthorized();
      const error = new Error(detailOf(data) || "Ошибка запроса (" + response.status + ")");
      error.status = response.status;
      throw error;
    }
    return data;
  }

  /* ---------- rendering ---------- */

  function botLabel(level) {
    const found = BOT_LEVELS.find((item) => item.value === Number(level));
    return found ? found.label : "—";
  }

  function botBadge(level) {
    return '<span class="badge-soft">' + escapeHtml(botLabel(level)) + "</span>";
  }

  function clampBotLevel(value, fallback) {
    const parsed = Number(value);
    return parsed === 0 || parsed === 1 || parsed === 2 ? parsed : Number(fallback);
  }

  function levelOptions(selected) {
    return BOT_LEVELS.map(
      (item) =>
        `<option value="${item.value}"${item.value === Number(selected) ? " selected" : ""}>${item.label}</option>`,
    ).join("");
  }

  function rewardCharacterOptions() {
    // The empty option is always first: it means "no reward".
    const options = ['<option value="">— без награды —</option>'];
    state.characters.forEach((character) => {
      options.push(
        `<option value="${escapeHtml(character.id)}">${escapeHtml(character.name)}</option>`,
      );
    });
    return options.join("");
  }

  function populateRewardSelect(select, fallback) {
    if (!select) return;
    // Keep the administrator's current choice across refreshes; fall back to
    // the saved value (or the empty option) when it is no longer available.
    const previous = select.value;
    select.innerHTML = rewardCharacterOptions();
    const saved = fallback ? String(fallback) : "";
    const candidate = previous || saved;
    select.value =
      candidate && [...select.options].some((option) => option.value === candidate)
        ? candidate
        : "";
  }

  function formatDate(value) {
    if (!value) return "—";
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return "—";
    return new Intl.DateTimeFormat("ru-RU", {
      day: "2-digit",
      month: "2-digit",
      year: "2-digit",
      hour: "2-digit",
      minute: "2-digit",
    }).format(date);
  }

  function findCampaign(id) {
    return state.campaigns.find((campaign) => String(campaign.id) === String(id)) || null;
  }

  function renderCover() {
    const holder = el("campaignCoverPreview");
    if (!holder) return;
    holder.textContent = "";
    const campaign = findCampaign(state.selectedId);
    if (!campaign || !campaign.cover_image_url) {
      const empty = document.createElement("span");
      empty.className = "text-secondary small";
      empty.textContent = "Обложка не задана";
      holder.appendChild(empty);
      return;
    }
    const image = document.createElement("img");
    image.alt = "Обложка кампании «" + campaign.name + "»";
    // Every upload stores a new uuid file name, so the URL itself changes and
    // no cache-busting query is needed: the browser sees a new resource.
    image.src = campaign.cover_image_url;
    holder.appendChild(image);
    const note = document.createElement("div");
    note.className = "campaign-cover-url";
    note.textContent = campaign.cover_image_url;
    holder.appendChild(note);
  }

  function campaignRow(campaign) {
    const id = escapeHtml(campaign.id);
    const reward = campaign.reward_character_name
      ? `<div class="song-slug text-truncate"><i class="bi bi-unlock me-1"></i>Награда: ${escapeHtml(campaign.reward_character_name)}</div>`
      : "";
    return `
      <tr data-campaign-id="${id}">
        <td>
          <div class="fw-semibold text-truncate">${escapeHtml(campaign.name)}</div>
          <div class="song-slug text-truncate">${id}</div>
          ${reward}
        </td>
        <td>${botBadge(campaign.bot_level)}</td>
        <td>${Number(campaign.song_count) || 0}</td>
        <td class="d-none d-lg-table-cell text-secondary">${formatDate(campaign.updated_at)}</td>
        <td class="text-end">
          <div class="d-inline-flex flex-wrap gap-2 justify-content-end">
            <button type="button" class="btn btn-sm btn-outline-light" data-campaign-edit="${id}">
              <i class="bi bi-pencil me-1"></i>Редактировать
            </button>
            <button type="button" class="btn btn-sm btn-outline-danger" data-campaign-delete="${id}">
              <i class="bi bi-trash me-1"></i>Удалить
            </button>
          </div>
        </td>
      </tr>
    `;
  }

  function renderList() {
    const body = el("campaignsBody");
    const empty = el("campaignsEmpty");
    if (!body) return;
    body.innerHTML = state.campaigns.map(campaignRow).join("");
    if (empty) empty.hidden = state.campaigns.length > 0;
  }

  function draftSongOptions() {
    const added = new Set(state.draftSongs.map((song) => String(song.song_id)));
    const options = ['<option value="">— выберите песню —</option>'];
    state.songs.forEach((song) => {
      if (added.has(String(song.id))) return;
      const draft = song.is_published ? "" : " · черновик";
      options.push(
        `<option value="${escapeHtml(song.id)}">${escapeHtml(song.title)} — ${escapeHtml(song.slug)} · D${escapeHtml(song.difficulty)}${escapeHtml(draft)}</option>`,
      );
    });
    return options.join("");
  }

  function stageRow(song, index, total) {
    const level = clampBotLevel(song.bot_level, 1);
    const draft = song.is_published
      ? ""
      : ' <span class="badge-soft badge-draft"><i class="bi bi-pencil-square"></i>Черновик</span>';
    return `
      <li class="d-flex flex-wrap align-items-center justify-content-between gap-2 border rounded px-3 py-2"
          data-stage-index="${index}">
        <div class="d-flex align-items-center gap-3 min-w-0">
          <span class="badge-soft">${index + 1}</span>
          <div class="min-w-0">
            <div class="fw-semibold text-truncate">${escapeHtml(song.title)}${draft}</div>
            <div class="song-slug text-truncate">${escapeHtml(song.slug)} · D${escapeHtml(song.difficulty)}</div>
          </div>
        </div>
        <div class="d-inline-flex flex-wrap align-items-center gap-2">
          <select class="form-select form-select-sm" data-stage-level="${index}"
                  style="width: 118px" aria-label="Сложность бота на этапе ${index + 1}"
                  title="Сложность бота на этом этапе">
            ${levelOptions(level)}
          </select>
          <button type="button" class="btn btn-sm btn-outline-light" data-stage-up="${index}"
                  title="Выше" ${index === 0 ? "disabled" : ""}>
            <i class="bi bi-arrow-up"></i>
          </button>
          <button type="button" class="btn btn-sm btn-outline-light" data-stage-down="${index}"
                  title="Ниже" ${index === total - 1 ? "disabled" : ""}>
            <i class="bi bi-arrow-down"></i>
          </button>
          <button type="button" class="btn btn-sm btn-outline-danger" data-stage-remove="${index}"
                  title="Убрать">
            <i class="bi bi-x-lg"></i>
          </button>
        </div>
      </li>
    `;
  }

  function renderEditor() {
    const editor = el("campaignEditor");
    if (!editor) return;
    const campaign = findCampaign(state.selectedId);
    editor.hidden = !campaign;
    if (!campaign) return;

    const title = el("campaignEditorTitle");
    if (title) title.textContent = campaign.name;

    const rewardSelect = el("campaignRewardCharacter");
    if (rewardSelect) populateRewardSelect(rewardSelect, campaign.reward_character_id);

    const conditionInput = el("campaignRewardCondition");
    if (conditionInput) conditionInput.placeholder = "Complete " + campaign.name;

    renderCover();

    const list = el("campaignSongsList");
    if (list) {
      list.innerHTML = state.draftSongs
        .map((song, index) => stageRow(song, index, state.draftSongs.length))
        .join("");
    }
    const empty = el("campaignSongsEmpty");
    if (empty) empty.hidden = state.draftSongs.length > 0;

    const select = el("campaignSongSelect");
    if (select) {
      const previous = select.value;
      select.innerHTML = draftSongOptions();
      if (previous && [...select.options].some((option) => option.value === previous)) {
        select.value = previous;
      }
    }
  }

  function openEditor(id) {
    const campaign = findCampaign(id);
    if (!campaign) return;
    state.selectedId = campaign.id;
    const defaultLevel = clampBotLevel(campaign.bot_level, 1);
    state.original = {
      name: campaign.name,
      bot_level: defaultLevel,
      reward_character_id: campaign.reward_character_id || null,
      reward_condition: campaign.reward_condition || null,
      description: campaign.description || null,
      cover_image_url: campaign.cover_image_url || null,
    };
    state.draftSongs = (campaign.songs || [])
      .slice()
      .sort((a, b) => a.position - b.position)
      .map((song) => ({
        song_id: song.song_id,
        slug: song.slug,
        title: song.title,
        difficulty: song.difficulty,
        is_published: song.is_published,
        // Effective per-stage level sent by the API; older drafts fall back to
        // the campaign default.
        bot_level: clampBotLevel(song.bot_level, defaultLevel),
      }));

    const nameInput = el("campaignEditName");
    if (nameInput) nameInput.value = campaign.name;
    const levelSelect = el("campaignEditBotLevel");
    if (levelSelect) levelSelect.value = String(defaultLevel);
    const rewardSelect = el("campaignRewardCharacter");
    if (rewardSelect) rewardSelect.value = campaign.reward_character_id
      ? String(campaign.reward_character_id)
      : "";
    const conditionInput = el("campaignRewardCondition");
    if (conditionInput) conditionInput.value = campaign.reward_condition || "";
    const descriptionInput = el("campaignEditDescription");
    if (descriptionInput) descriptionInput.value = campaign.description || "";
    const coverInput = el("campaignCoverFile");
    if (coverInput) coverInput.value = "";
    showAlert("campaignEditorAlert", "");
    renderEditor();
    el("campaignEditor")?.scrollIntoView({ behavior: "smooth", block: "start" });
  }

  function closeEditor() {
    state.selectedId = null;
    state.original = null;
    state.draftSongs = [];
    renderEditor();
  }

  /* ---------- data ---------- */

  async function loadSongs() {
    // The campaign picker needs every active song; archived rows cannot be
    // published, so they are not offered. The admin endpoint is capped at 200.
    const data = await api(`/admin/songs?limit=${SONG_PAGE_LIMIT}&offset=0&archived=false`);
    state.songs = (data && data.items) || [];
  }

  async function loadCharacters() {
    // Reward picker: the internal UUID is the admin identifier; the public
    // integer id is what the client sees in the player catalogue.
    const data = await api("/admin/ui-levels");
    state.characters = ((data && data.items) || []).slice().sort((a, b) =>
      String(a.name).localeCompare(String(b.name), "ru"),
    );
  }

  async function refresh() {
    const requestId = ++state.requestId;
    showAlert("campaignsAlert", "");
    const body = el("campaignsBody");
    if (body && !state.loaded) {
      body.innerHTML =
        '<tr><td colspan="5" class="table-loading"><span class="spinner-border spinner-border-sm me-2"></span>Загрузка…</td></tr>';
    }
    try {
      const [data] = await Promise.all([api("/admin/campaigns"), loadSongs(), loadCharacters()]);
      if (requestId !== state.requestId) return;
      state.campaigns = (data && data.items) || [];
      state.loaded = true;
      populateRewardSelect(el("campaignCreateRewardCharacter"), null);
      renderList();
      if (state.selectedId) {
        // Keep the editor in sync after a save/delete of another tab.
        if (findCampaign(state.selectedId)) renderEditor();
        else closeEditor();
      }
    } catch (error) {
      if (requestId !== state.requestId) return;
      showAlert("campaignsAlert", errorText(error));
    }
  }

  async function createCampaign(event) {
    event.preventDefault();
    const nameInput = el("campaignName");
    const name = nameInput ? nameInput.value.trim() : "";
    if (!name) {
      nameInput?.classList.add("is-invalid");
      nameInput?.focus();
      return;
    }
    nameInput.classList.remove("is-invalid");

    const button = el("campaignCreate");
    const spinner = el("campaignCreateSpinner");
    if (button) button.disabled = true;
    if (spinner) spinner.hidden = false;
    showAlert("campaignsAlert", "");

    const level = Number(el("campaignBotLevel").value);
    const createRewardSelect = el("campaignCreateRewardCharacter");
    const rewardCharacterId = createRewardSelect ? createRewardSelect.value || null : null;
    const conditionInput = el("campaignCreateRewardCondition");
    const rewardCondition = conditionInput ? conditionInput.value.trim() || null : null;
    try {
      const campaign = await api("/admin/campaigns", {
        method: "POST",
        body: {
          name,
          bot_level: level,
          reward_character_id: rewardCharacterId,
          reward_condition: rewardCondition,
        },
      });
      const form = el("campaignCreateForm");
      if (form) form.reset();
      // form.reset() restores the markup default; force the empty "no reward"
      // option explicitly so the select is never left on a stale value.
      if (createRewardSelect) createRewardSelect.value = "";
      if (conditionInput) conditionInput.placeholder = "Complete Tower riffs";
      await refresh();
      openEditor(campaign.id);
      toast("Кампания создана. Добавьте песни.");
    } catch (error) {
      showAlert("campaignsAlert", errorText(error));
    } finally {
      if (button) button.disabled = false;
      if (spinner) spinner.hidden = true;
    }
  }

  async function saveCampaign() {
    if (!state.selectedId) return;
    const nameInput = el("campaignEditName");
    const name = nameInput ? nameInput.value.trim() : "";
    if (!name) {
      showAlert("campaignEditorAlert", "Введите название кампании.");
      return;
    }

    const button = el("campaignSave");
    const spinner = el("campaignSaveSpinner");
    if (button) button.disabled = true;
    if (spinner) spinner.hidden = false;
    showAlert("campaignEditorAlert", "");

    const id = state.selectedId;
    const level = Number(el("campaignEditBotLevel").value);
    const rewardSelect = el("campaignRewardCharacter");
    const rewardCharacterId = rewardSelect ? rewardSelect.value || null : null;
    const conditionInput = el("campaignRewardCondition");
    const rewardCondition = conditionInput ? conditionInput.value.trim() || null : null;
    const descriptionInput = el("campaignEditDescription");
    const description = descriptionInput ? descriptionInput.value.trim() || null : null;
    try {
      const original = state.original || {};
      const metadataChanged =
        name !== original.name ||
        level !== Number(original.bot_level) ||
        String(rewardCharacterId || "") !== String(original.reward_character_id || "") ||
        (rewardCondition || "") !== (original.reward_condition || "") ||
        (description || "") !== (original.description || "");
      if (metadataChanged) {
        await api("/admin/campaigns/" + encodeURIComponent(id), {
          method: "PATCH",
          body: {
            name,
            bot_level: level,
            reward_character_id: rewardCharacterId,
            reward_condition: rewardCondition,
            description,
          },
        });
      }
      const saved = await api("/admin/campaigns/" + encodeURIComponent(id) + "/songs", {
        method: "PUT",
        body: {
          songs: state.draftSongs.map((song) => ({
            song_id: song.song_id,
            bot_level: clampBotLevel(song.bot_level, level),
          })),
        },
      });
      toast("Кампания сохранена.");
      await refresh();
      openEditor(saved.id);
    } catch (error) {
      showAlert("campaignEditorAlert", errorText(error));
    } finally {
      if (button) button.disabled = false;
      if (spinner) spinner.hidden = true;
    }
  }

  function applySavedCampaign(saved) {
    if (!saved) return;
    const index = state.campaigns.findIndex(
      (campaign) => String(campaign.id) === String(saved.id),
    );
    if (index >= 0) state.campaigns[index] = saved;
    if (String(state.selectedId) === String(saved.id)) {
      // Keep the unsaved stage draft and text inputs; only the server-owned
      // cover URL and the saved description baseline change.
      state.original = {
        ...(state.original || {}),
        description: saved.description || null,
        cover_image_url: saved.cover_image_url || null,
      };
      renderEditor();
    }
    renderList();
  }

  async function uploadCover() {
    if (!state.selectedId) return;
    const input = el("campaignCoverFile");
    const file = input && input.files && input.files[0];
    if (!file) {
      showAlert("campaignEditorAlert", "Выберите файл обложки.");
      return;
    }
    if (file.size > MAX_COVER_BYTES) {
      showAlert("campaignEditorAlert", "Файл больше 2 МБ.");
      return;
    }

    const button = el("campaignCoverUpload");
    const spinner = el("campaignCoverSpinner");
    if (button) button.disabled = true;
    if (spinner) spinner.hidden = false;
    showAlert("campaignEditorAlert", "");

    try {
      const form = new FormData();
      form.append("file", file);
      const saved = await api(
        "/admin/campaigns/" + encodeURIComponent(state.selectedId) + "/cover",
        { method: "POST", body: form },
      );
      if (input) input.value = "";
      applySavedCampaign(saved);
      toast("Обложка обновлена.");
    } catch (error) {
      showAlert("campaignEditorAlert", errorText(error));
    } finally {
      if (button) button.disabled = false;
      if (spinner) spinner.hidden = true;
    }
  }

  async function removeCover() {
    if (!state.selectedId) return;
    if (!window.confirm("Удалить обложку кампании?")) return;

    const button = el("campaignCoverRemove");
    if (button) button.disabled = true;
    showAlert("campaignEditorAlert", "");
    try {
      const saved = await api(
        "/admin/campaigns/" + encodeURIComponent(state.selectedId) + "/cover",
        { method: "DELETE" },
      );
      applySavedCampaign(saved);
      toast("Обложка удалена.");
    } catch (error) {
      showAlert("campaignEditorAlert", errorText(error));
    } finally {
      if (button) button.disabled = false;
    }
  }

  async function deleteCampaign(id) {
    const campaign = findCampaign(id);
    const name = campaign ? campaign.name : "кампанию";
    if (!window.confirm(`Удалить кампанию «${name}»? Список этапов будет удалён.`)) return;
    try {
      await api("/admin/campaigns/" + encodeURIComponent(id), { method: "DELETE" });
      if (String(state.selectedId) === String(id)) closeEditor();
      await refresh();
      toast("Кампания удалена.");
    } catch (error) {
      showAlert("campaignsAlert", errorText(error));
    }
  }

  function addStage() {
    const select = el("campaignSongSelect");
    const songId = select ? select.value : "";
    if (!songId) {
      toast("Сначала выберите песню в списке.", "warning");
      return;
    }
    if (state.draftSongs.some((song) => String(song.song_id) === String(songId))) {
      toast("Эта песня уже добавлена в кампанию.", "warning");
      return;
    }
    const song = state.songs.find((item) => String(item.id) === String(songId));
    if (!song) return;
    // A newly added stage starts with the campaign default level (as currently
    // picked in the editor, even before it is saved).
    const defaultLevel = clampBotLevel(
      el("campaignEditBotLevel")?.value,
      state.original?.bot_level ?? 1,
    );
    state.draftSongs.push({
      song_id: song.id,
      slug: song.slug,
      title: song.title,
      difficulty: song.difficulty,
      is_published: song.is_published,
      bot_level: defaultLevel,
    });
    if (select) select.value = "";
    renderEditor();
  }

  function moveStage(index, delta) {
    const target = index + delta;
    if (target < 0 || target >= state.draftSongs.length) return;
    const [song] = state.draftSongs.splice(index, 1);
    state.draftSongs.splice(target, 0, song);
    renderEditor();
  }

  function removeStage(index) {
    state.draftSongs.splice(index, 1);
    renderEditor();
  }

  /* ---------- wiring ---------- */

  function init() {
    el("campaignsReload")?.addEventListener("click", () => {
      refresh().catch((error) => showAlert("campaignsAlert", errorText(error)));
    });
    el("campaignCreateForm")?.addEventListener("submit", createCampaign);
    // The unlock text defaults to "Complete <name>"; mirror the name as it is
    // typed so the administrator sees the effective default.
    el("campaignName")?.addEventListener("input", (event) => {
      const condition = el("campaignCreateRewardCondition");
      if (!condition) return;
      const typed = event.target.value.trim();
      condition.placeholder = typed ? "Complete " + typed : "Complete Tower riffs";
    });
    el("campaignSave")?.addEventListener("click", saveCampaign);
    el("campaignDelete")?.addEventListener("click", () => {
      if (state.selectedId) deleteCampaign(state.selectedId);
    });
    el("campaignEditorClose")?.addEventListener("click", closeEditor);
    el("campaignCoverUpload")?.addEventListener("click", uploadCover);
    el("campaignCoverRemove")?.addEventListener("click", removeCover);
    el("campaignSongAdd")?.addEventListener("click", addStage);
    el("campaignsBody")?.addEventListener("click", (event) => {
      const edit = event.target.closest("[data-campaign-edit]");
      if (edit) {
        openEditor(edit.getAttribute("data-campaign-edit"));
        return;
      }
      const remove = event.target.closest("[data-campaign-delete]");
      if (remove) deleteCampaign(remove.getAttribute("data-campaign-delete"));
    });
    el("campaignSongsList")?.addEventListener("click", (event) => {
      const up = event.target.closest("[data-stage-up]");
      if (up) {
        moveStage(Number(up.getAttribute("data-stage-up")), -1);
        return;
      }
      const down = event.target.closest("[data-stage-down]");
      if (down) {
        moveStage(Number(down.getAttribute("data-stage-down")), 1);
        return;
      }
      const remove = event.target.closest("[data-stage-remove]");
      if (remove) removeStage(Number(remove.getAttribute("data-stage-remove")));
    });
    // Per-stage bot level lives in the draft state, so it survives reordering,
    // adding and removing other stages until SAVE.
    el("campaignSongsList")?.addEventListener("change", (event) => {
      const select = event.target.closest("[data-stage-level]");
      if (!select) return;
      const index = Number(select.getAttribute("data-stage-level"));
      if (!Number.isInteger(index) || index < 0 || index >= state.draftSongs.length) return;
      state.draftSongs[index].bot_level = clampBotLevel(select.value, 1);
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }

  window.FFAdminCampaigns = {
    show() {
      // Lazy first load, refresh on every later visit.
      refresh().catch((error) => showAlert("campaignsAlert", errorText(error)));
    },
    refresh() {
      return refresh();
    },
  };
})();
