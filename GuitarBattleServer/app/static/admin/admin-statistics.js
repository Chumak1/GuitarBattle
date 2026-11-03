/* fretfight admin — statistics tab.
 *
 * Product analytics: app time, sessions, launches and scores per account plus
 * aggregates per game. Kept separate from admin.js (songs) and admin-site.js
 * (landing CMS, builds); it only needs the administrator JWT from sessionStorage
 * and the shared helpers from admin.js (login fallback, toasts, current admin).
 *
 * Public surface: window.FFAdminStatistics.show() — lazy first load, refresh on
 * every later visit; window.FFAdminStatistics.refresh() — explicit reload.
 */
(() => {
  "use strict";

  const API_BASE = "/api/v1";
  const TOKEN_KEY = "guitarbattle.admin.token";
  const PAGE_SIZE = 50;
  const ONLINE_WINDOW_MS = 5 * 60 * 1000;

  // Columns the API can sort by; text columns start ascending, numbers and
  // dates descending. "Последний запуск" is display-only because the API enum
  // has no last_launch sort key.
  const TEXT_SORTS = ["username", "display_name", "email"];
  const SORT_KEYS = [
    "last_seen",
    "username",
    "display_name",
    "email",
    "created_at",
    "app_seconds",
    "sessions_count",
    "launches_count",
    "scores_count",
    "level",
  ];

  const state = {
    loaded: false,
    days: 30,
    search: "",
    country: "",
    sort: "last_seen",
    order: "desc",
    offset: 0,
    total: 0,
    users: [],
    games: [],
    usersRequestId: 0,
    gamesRequestId: 0,
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
    return (error && error.message) || "Не удалось выполнить запрос.";
  }

  /* Expired token or a non-administrator token: hand over to the admin shell,
   * which clears the session and shows its own login view. */
  function handleUnauthorized() {
    if (window.FFAdmin && typeof window.FFAdmin.handleUnauthorized === "function") {
      window.FFAdmin.handleUnauthorized();
      return;
    }
    sessionStorage.removeItem(TOKEN_KEY);
    window.location.reload();
  }

  async function api(path, options = {}) {
    const token = sessionStorage.getItem(TOKEN_KEY) || "";
    const headers = token ? { Authorization: "Bearer " + token } : {};

    const response = await fetch(API_BASE + path, {
      method: options.method || "GET",
      headers,
      credentials: "omit",
    });
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

  function showAlert(message) {
    const box = el("statsAlert");
    if (!box) return;
    box.textContent = message || "";
    box.hidden = !message;
  }

  function notify(message, variant = "success") {
    if (window.FFAdmin && typeof window.FFAdmin.showToast === "function") {
      window.FFAdmin.showToast(message, variant);
      return;
    }
    if (variant === "danger") showAlert(message);
  }

  function currentAdminId() {
    const me =
      window.FFAdmin && typeof window.FFAdmin.getUser === "function"
        ? window.FFAdmin.getUser()
        : null;
    return me && me.id ? String(me.id) : "";
  }

  /* ---------- formatting ---------- */

  function formatAppTime(seconds) {
    const total = Math.max(0, Math.round(Number(seconds) || 0));
    if (total < 60) return total + " сек";
    const hours = Math.floor(total / 3600);
    const minutes = Math.floor((total % 3600) / 60);
    if (hours > 0) return minutes > 0 ? hours + " ч " + minutes + " мин" : hours + " ч";
    return minutes + " мин";
  }

  function formatNumber(value) {
    return new Intl.NumberFormat("ru-RU").format(Number(value) || 0);
  }

  function formatDateTime(value) {
    if (!value) return "—";
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return "—";
    return new Intl.DateTimeFormat("ru-RU", {
      day: "2-digit",
      month: "short",
      year: "numeric",
      hour: "2-digit",
      minute: "2-digit",
    }).format(date);
  }

  function relativeTime(value) {
    if (!value) return "нет данных";
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return "нет данных";
    const diffMinutes = Math.round((Date.now() - date.getTime()) / 60000);
    if (diffMinutes < 1) return "только что";
    if (diffMinutes < 60) return diffMinutes + " мин назад";
    const diffHours = Math.round(diffMinutes / 60);
    if (diffHours < 24) return diffHours + " ч назад";
    const diffDays = Math.round(diffHours / 24);
    if (diffDays < 30) return diffDays + " дн назад";
    return formatDateTime(value);
  }

  function lastActivity(user) {
    const candidates = [user.last_seen_at, user.last_launch_at]
      .filter(Boolean)
      .map((value) => new Date(value).getTime())
      .filter((value) => !Number.isNaN(value));
    if (!candidates.length) return null;
    return new Date(Math.max(...candidates)).toISOString();
  }

  /* ---------- platforms ---------- */

  const PLATFORM_BADGE_LIMIT = 2;
  const PLATFORM_MODIFIERS = {
    Windows: "stats-platform-windows",
    macOS: "stats-platform-macos",
    Linux: "stats-platform-linux",
  };

  function plural(count, one, few, many) {
    const value = Math.abs(Number(count) || 0);
    const mod10 = value % 10;
    const mod100 = value % 100;
    if (mod10 === 1 && mod100 !== 11) return one;
    if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return few;
    return many;
  }

  function sessionsLabel(count) {
    return formatNumber(count) + " " + plural(count, "сессия", "сессии", "сессий");
  }

  /* The API returns one entry per raw OS name ("Windows 11", "Windows 10"),
   * newest first. The table shows one badge per family, so equal families are
   * merged while the raw names stay available in the tooltip. */
  function platformGroups(user) {
    const groups = new Map();
    (user.platforms || []).forEach((platform) => {
      const family = platform.family || "Unknown";
      const existing = groups.get(family);
      if (existing) {
        existing.push(platform);
        return;
      }
      groups.set(family, [platform]);
    });
    return Array.from(groups, ([family, names]) => ({ family, names }));
  }

  function platformTooltip(group) {
    return group.names
      .map(
        (platform) =>
          platform.name +
          " — " +
          sessionsLabel(platform.sessions) +
          ", последний визит " +
          formatDateTime(platform.last_seen_at),
      )
      .join("\n");
  }

  function platformBadge(group) {
    const modifier = PLATFORM_MODIFIERS[group.family] || "stats-platform-unknown";
    return (
      '<span class="stats-platform ' + modifier + '" title="' +
      escapeHtml(platformTooltip(group)) + '">' +
      escapeHtml(group.family) + "</span>"
    );
  }

  function platformsCell(user) {
    const groups = platformGroups(user);
    if (!groups.length) {
      return '<span class="stats-platforms-empty">—</span>';
    }
    const shown = groups.slice(0, PLATFORM_BADGE_LIMIT);
    const rest = groups.slice(PLATFORM_BADGE_LIMIT);
    const badges = shown.map(platformBadge);
    if (rest.length) {
      badges.push(
        '<span class="stats-platform stats-platform-more" title="' +
          escapeHtml(rest.map(platformTooltip).join("\n")) + '">+' + rest.length + "</span>",
      );
    }
    return '<div class="stats-platforms">' + badges.join("") + "</div>";
  }

  /* ---------- country ---------- */

  /* The server resolves the country from the registration IP (a free geo-IP
   * service, best effort) and also sends the emoji flag, so every admin client
   * renders the same value. Accounts created before migration 0019 have no
   * country and show a dash. Both IP addresses live in the tooltip only. */
  function countryTooltip(user) {
    return (
      "IP регистрации: " + (user.registration_ip || "нет данных") + "\n" +
      "IP последнего входа: " + (user.last_login_ip || "нет данных")
    );
  }

  function countryCell(user) {
    const tooltip = escapeHtml(countryTooltip(user));
    if (!user.country_code) {
      return '<span class="stats-country-empty" title="' + tooltip + '">—</span>';
    }
    const name = user.country_name || user.country_code;
    const flag = user.country_flag
      ? '<span class="stats-country-flag">' + escapeHtml(user.country_flag) + "</span>"
      : "";
    return (
      '<span class="stats-country" title="' + tooltip + '">' +
      flag +
      '<span class="stats-country-name">' + escapeHtml(name) + "</span>" +
      "</span>"
    );
  }

  /* The filter list comes from overview.countries (top ten by players). A
   * selected country that dropped out of that list keeps its own option so
   * the filter never silently resets. */
  function renderCountryOptions(countries) {
    const select = el("statsCountry");
    if (!select) return;
    const selected = state.country;
    const options = ['<option value="">Все страны</option>'];
    (countries || []).forEach((item) => {
      if (!item || !item.code) return;
      const label = (item.name || item.code) + " (" + item.code + ")";
      options.push(
        '<option value="' + escapeHtml(item.code) + '">' + escapeHtml(label) + "</option>"
      );
    });
    select.innerHTML = options.join("");
    select.value = selected;
    if (selected && select.value !== selected) {
      const extra = document.createElement("option");
      extra.value = selected;
      extra.textContent = selected;
      select.appendChild(extra);
      select.value = selected;
    }
  }

  /* ---------- overview ---------- */

  function renderOverview(overview) {
    el("statsUsersTotal").textContent = formatNumber(overview.users_total);
    el("statsUsers24h").textContent = formatNumber(overview.users_active_24h);
    el("statsUsers7d").textContent = formatNumber(overview.users_active_7d);
    el("statsAppTime").textContent = formatAppTime(overview.app_seconds_total);
    el("statsAppTimeSub").textContent = "за " + state.days + " дн";
    el("statsAvgSession").textContent = formatAppTime(overview.avg_session_seconds);
    el("statsSessions").textContent = "сессий: " + formatNumber(overview.sessions_total);
    el("statsLaunches").textContent = formatNumber(overview.launches_total);
    el("statsLaunches24h").textContent = "за 24 ч: " + formatNumber(overview.launches_24h);
    el("statsGenerated").textContent = "Данные на " + formatDateTime(overview.generated_at);
    renderCountryOptions(overview.countries);
  }

  async function loadOverview() {
    try {
      const data = await api("/admin/statistics/overview?days=" + state.days);
      renderOverview(data);
      showAlert("");
    } catch (error) {
      if (error.status !== 401 && error.status !== 403) showAlert(errorText(error));
    }
  }

  /* ---------- users ---------- */

  function updateSortIndicators() {
    document.querySelectorAll("[data-stats-sort]").forEach((button) => {
      const key = button.getAttribute("data-stats-sort");
      const active = key === state.sort && SORT_KEYS.indexOf(key) >= 0;
      button.classList.toggle("is-sorted", active);
      const icon = button.querySelector(".stats-sort-icon");
      if (icon) {
        const glyph = active
          ? state.order === "asc"
            ? "bi-arrow-up"
            : "bi-arrow-down"
          : "bi-arrow-down-up";
        icon.className = "bi " + glyph + " stats-sort-icon";
      }
      const header = button.closest("th");
      if (header) {
        header.setAttribute(
          "aria-sort",
          active ? (state.order === "asc" ? "ascending" : "descending") : "none",
        );
      }
    });
  }

  function applySort(key) {
    if (SORT_KEYS.indexOf(key) < 0) return;
    if (state.sort === key) {
      state.order = state.order === "asc" ? "desc" : "asc";
    } else {
      state.sort = key;
      state.order = TEXT_SORTS.indexOf(key) >= 0 ? "asc" : "desc";
    }
    state.offset = 0;
    updateSortIndicators();
    loadUsers();
  }

  function userRow(user) {
    const activity = lastActivity(user);
    const activityMs = activity ? new Date(activity).getTime() : 0;
    const isOnline = Boolean(activityMs) && Date.now() - activityMs < ONLINE_WINDOW_MS;
    const stateLabel = user.is_active ? relativeTime(activity) : "аккаунт отключён";
    const nick = user.display_name
      ? escapeHtml(user.display_name)
      : '<span class="text-secondary">—</span>';
    const isSelf = currentAdminId() === String(user.user_id);
    const bestScore =
      user.best_score == null
        ? ""
        : '<div class="stats-mono">лучший ' + formatNumber(user.best_score) + "</div>";
    const deleteButton = isSelf
      ? '<button type="button" class="action-btn" disabled title="Нельзя удалить свою учётную запись" aria-label="Нельзя удалить свою учётную запись">' +
        '<i class="bi bi-trash3"></i></button>'
      : '<button type="button" class="action-btn action-danger" data-stats-delete="' +
        escapeHtml(user.user_id) +
        '" title="Удалить пользователя" aria-label="Удалить пользователя">' +
        '<i class="bi bi-trash3"></i></button>';

    return (
      "<tr>" +
      "<td>" +
      '<div class="stats-login">' + escapeHtml(user.username) + "</div>" +
      '<div class="stats-mono">' + escapeHtml(user.user_id) + "</div>" +
      (isSelf ? '<div class="stats-mono stats-self">это вы</div>' : "") +
      "</td>" +
      "<td>" + nick + "</td>" +
      '<td class="d-none d-md-table-cell">' + countryCell(user) + "</td>" +
      '<td class="d-none d-lg-table-cell">' + escapeHtml(user.email) + "</td>" +
      "<td>" +
      '<span class="text-nowrap">' + escapeHtml(formatAppTime(user.app_seconds_total)) + "</span>" +
      '<div class="stats-mono">' + formatNumber(user.sessions_count) + " сессий · " +
      formatNumber(user.games_distinct) + " игр</div>" +
      "</td>" +
      '<td class="d-none d-md-table-cell"><span class="text-nowrap">' +
      formatNumber(user.sessions_count) + "</span></td>" +
      "<td><span class=\"text-nowrap\">" + formatNumber(user.launches_count) + "</span></td>" +
      "<td><span class=\"text-nowrap\">" + formatNumber(user.scores_count) + "</span>" +
      bestScore + "</td>" +
      "<td><span class=\"text-nowrap\">LVL " + formatNumber(user.level) + "</span>" +
      (user.xp_total == null ? "" :
        '<div class="stats-mono">' + formatNumber(user.xp_total) + " XP</div>") + "</td>" +
      '<td class="d-none d-lg-table-cell">' + platformsCell(user) + "</td>" +
      '<td class="d-none d-xl-table-cell">' +
      '<span class="text-nowrap small text-secondary">' + formatDateTime(user.last_launch_at) + "</span>" +
      "</td>" +
      "<td>" +
      '<div class="stats-activity">' +
      '<span class="stats-activity-date">' + formatDateTime(activity) + "</span>" +
      '<span class="stats-activity-state' + (isOnline ? " is-online" : "") + '">' +
      escapeHtml(stateLabel) + "</span>" +
      "</div>" +
      "</td>" +
      '<td class="text-end"><div class="action-group">' + deleteButton + "</div></td>" +
      "</tr>"
    );
  }

  function renderUsers() {
    const body = el("statsUsersBody");
    if (!body) return;
    if (!state.users.length) {
      const message = state.search
        ? "Никого не нашли по этому запросу."
        : state.country
          ? "В этой стране пока нет игроков."
          : "Пока нет ни одного игрока.";
      body.innerHTML =
        '<tr><td colspan="13" class="stats-table-empty">' + escapeHtml(message) + "</td></tr>";
    } else {
      body.innerHTML = state.users.map(userRow).join("");
    }

    const first = state.total ? state.offset + 1 : 0;
    const last = Math.min(state.offset + state.users.length, state.total);
    el("statsPagerInfo").textContent =
      first + "–" + last + " из " + formatNumber(state.total);
    el("statsPrevPage").disabled = state.offset <= 0;
    el("statsNextPage").disabled = state.offset + state.users.length >= state.total;
  }

  function setUsersLoading(loading) {
    ["statsRefresh", "statsRefreshUsers"].forEach((id) => {
      const button = el(id);
      if (button) button.disabled = loading;
    });
    if (loading) {
      el("statsUsersBody").innerHTML =
        '<tr><td colspan="13" class="table-loading">' +
        '<span class="spinner-border spinner-border-sm me-2"></span>Загрузка…</td></tr>';
    }
  }

  async function loadUsers() {
    const requestId = ++state.usersRequestId;
    setUsersLoading(true);
    try {
      const params = new URLSearchParams({
        sort: state.sort,
        order: state.order,
        limit: String(PAGE_SIZE),
        offset: String(state.offset),
      });
      if (state.search) params.set("search", state.search);
      if (state.country) params.set("country", state.country);
      const data = await api("/admin/statistics/users?" + params.toString());
      if (requestId !== state.usersRequestId) return;
      state.users = data.items || [];
      state.total = data.total || 0;
      updateSortIndicators();
      renderUsers();
    } catch (error) {
      if (requestId !== state.usersRequestId) return;
      if (error.status !== 401 && error.status !== 403) showAlert(errorText(error));
    } finally {
      if (requestId === state.usersRequestId) setUsersLoading(false);
    }
  }

  async function deleteUser(userId) {
    const user = state.users.find((item) => String(item.user_id) === String(userId));
    if (!user || String(user.user_id) === currentAdminId()) return;

    const question =
      "Удалить пользователя " + user.username + " (" + user.email + ")?\n\n" +
      "Аккаунт, сессии, запуски и результаты будут удалены безвозвратно.";
    if (!window.confirm(question)) return;

    const buttons = Array.prototype.slice.call(
      document.querySelectorAll("[data-stats-delete]"),
    );
    buttons.forEach((button) => {
      button.disabled = true;
    });
    showAlert("");

    try {
      await api("/admin/users/" + encodeURIComponent(user.user_id), { method: "DELETE" });
      // The last row of a page may have just disappeared; step back one page
      // instead of showing an empty list.
      if (state.users.length === 1 && state.offset > 0) {
        state.offset = Math.max(0, state.offset - PAGE_SIZE);
      }
      notify("Пользователь " + user.username + " удалён.");
      await Promise.all([loadUsers(), loadOverview(), loadGames()]);
    } catch (error) {
      if (error.status !== 401 && error.status !== 403) {
        showAlert(errorText(error));
        notify(errorText(error), "danger");
      }
    } finally {
      buttons.forEach((button) => {
        button.disabled = false;
      });
    }
  }

  /* ---------- games ---------- */

  function gameRow(game) {
    return (
      "<tr>" +
      "<td>" +
      '<div class="stats-login">' + escapeHtml(game.title) + "</div>" +
      '<div class="stats-mono">' + escapeHtml(game.game_key) + "</div>" +
      "</td>" +
      "<td><span class=\"text-nowrap\">" + formatNumber(game.launches) + "</span></td>" +
      "<td><span class=\"text-nowrap\">" + formatNumber(game.unique_users) + "</span></td>" +
      '<td><span class="text-nowrap small text-secondary">' +
      formatDateTime(game.last_launch_at) + "</span></td>" +
      "</tr>"
    );
  }

  function renderGames() {
    const body = el("statsGamesBody");
    const empty = el("statsGamesEmpty");
    if (!body) return;
    const table = body.closest(".table-responsive");
    if (!state.games.length) {
      body.innerHTML = '<tr><td colspan="4" class="stats-table-empty">Запусков за период нет.</td></tr>';
      if (empty) empty.hidden = false;
      if (table) table.hidden = true;
      return;
    }
    if (table) table.hidden = false;
    if (empty) empty.hidden = true;
    body.innerHTML = state.games.map(gameRow).join("");
  }

  async function loadGames() {
    const requestId = ++state.gamesRequestId;
    const button = el("statsRefreshGames");
    if (button) button.disabled = true;
    try {
      const data = await api("/admin/statistics/games?days=" + state.days + "&limit=100");
      if (requestId !== state.gamesRequestId) return;
      state.games = data.items || [];
      el("statsGamesNote").textContent =
        "Запуски за " + state.days + " дн · игр: " + formatNumber(data.total);
      renderGames();
    } catch (error) {
      if (requestId !== state.gamesRequestId) return;
      if (error.status !== 401 && error.status !== 403) showAlert(errorText(error));
    } finally {
      if (requestId === state.gamesRequestId && button) button.disabled = false;
    }
  }

  /* ---------- public surface ---------- */

  function loadAll() {
    showAlert("");
    loadOverview();
    loadUsers();
    loadGames();
  }

  function show() {
    // Lazy first load; later visits refresh the numbers instead of stacking
    // yet another copy of the same state.
    state.loaded = true;
    updateSortIndicators();
    loadAll();
  }

  function refresh() {
    if (!state.loaded) return;
    loadAll();
  }

  /* ---------- events ---------- */

  function bindEvents() {
    el("statsRefresh")?.addEventListener("click", loadAll);
    el("statsRefreshUsers")?.addEventListener("click", loadUsers);
    el("statsRefreshGames")?.addEventListener("click", loadGames);

    document.querySelectorAll("[data-stats-sort]").forEach((button) => {
      button.addEventListener("click", () => {
        applySort(button.getAttribute("data-stats-sort"));
      });
    });

    el("statsUsersBody")?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-stats-delete]");
      if (button) deleteUser(button.getAttribute("data-stats-delete"));
    });

    el("statsPeriod")?.addEventListener("change", (event) => {
      state.days = Number(event.target.value) || 30;
      state.offset = 0;
      loadAll();
    });

    el("statsCountry")?.addEventListener("change", (event) => {
      state.country = event.target.value || "";
      state.offset = 0;
      loadUsers();
    });

    let searchTimer = null;
    el("statsSearch")?.addEventListener("input", (event) => {
      window.clearTimeout(searchTimer);
      searchTimer = window.setTimeout(() => {
        state.search = event.target.value.trim();
        state.offset = 0;
        loadUsers();
      }, 320);
    });

    el("statsPrevPage")?.addEventListener("click", () => {
      state.offset = Math.max(0, state.offset - PAGE_SIZE);
      loadUsers();
    });
    el("statsNextPage")?.addEventListener("click", () => {
      if (state.offset + state.users.length >= state.total) return;
      state.offset += PAGE_SIZE;
      loadUsers();
    });

    updateSortIndicators();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", bindEvents);
  } else {
    bindEvents();
  }

  window.FFAdminStatistics = { show, refresh };
})();
