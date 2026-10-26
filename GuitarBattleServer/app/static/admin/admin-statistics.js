/* fretfight admin — statistics tab.
 *
 * Product analytics: app time, sessions and game launches per account plus
 * aggregates per game. Kept separate from admin.js (songs) and admin-site.js
 * (landing CMS, builds); it only needs the administrator JWT from sessionStorage
 * and the shared authorized-request error handling from admin.js.
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

  const state = {
    loaded: false,
    days: 30,
    search: "",
    sort: "last_seen",
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

  async function api(path) {
    const token = sessionStorage.getItem(TOKEN_KEY) || "";
    const headers = token ? { Authorization: "Bearer " + token } : {};

    const response = await fetch(API_BASE + path, {
      method: "GET",
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

  function userRow(user) {
    const activity = lastActivity(user);
    const activityMs = activity ? new Date(activity).getTime() : 0;
    const isOnline = Boolean(activityMs) && Date.now() - activityMs < ONLINE_WINDOW_MS;
    const stateLabel = user.is_active ? relativeTime(activity) : "аккаунт отключён";
    const nick = user.display_name
      ? escapeHtml(user.display_name)
      : '<span class="text-secondary">—</span>';
    return (
      "<tr>" +
      "<td>" +
      '<div class="stats-login">' + escapeHtml(user.username) + "</div>" +
      '<div class="stats-mono">' + escapeHtml(user.user_id) + "</div>" +
      "</td>" +
      "<td>" + nick + "</td>" +
      '<td class="d-none d-lg-table-cell">' + escapeHtml(user.email) + "</td>" +
      "<td>" +
      '<span class="text-nowrap">' + escapeHtml(formatAppTime(user.app_seconds_total)) + "</span>" +
      '<div class="stats-mono">' + formatNumber(user.sessions_count) + " сессий · " +
      formatNumber(user.games_distinct) + " игр</div>" +
      "</td>" +
      '<td class="d-none d-md-table-cell"><span class="text-nowrap">' +
      formatNumber(user.sessions_count) + "</span></td>" +
      "<td><span class=\"text-nowrap\">" + formatNumber(user.launches_count) + "</span></td>" +
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
      "</tr>"
    );
  }

  function renderUsers() {
    const body = el("statsUsersBody");
    if (!body) return;
    if (!state.users.length) {
      const message = state.search
        ? "Никого не нашли по этому запросу."
        : "Пока нет ни одного игрока.";
      body.innerHTML =
        '<tr><td colspan="8" class="stats-table-empty">' + escapeHtml(message) + "</td></tr>";
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
        '<tr><td colspan="8" class="table-loading">' +
        '<span class="spinner-border spinner-border-sm me-2"></span>Загрузка…</td></tr>';
    }
  }

  async function loadUsers() {
    const requestId = ++state.usersRequestId;
    setUsersLoading(true);
    try {
      const params = new URLSearchParams({
        sort: state.sort,
        limit: String(PAGE_SIZE),
        offset: String(state.offset),
      });
      if (state.search) params.set("search", state.search);
      const data = await api("/admin/statistics/users?" + params.toString());
      if (requestId !== state.usersRequestId) return;
      state.users = data.items || [];
      state.total = data.total || 0;
      renderUsers();
    } catch (error) {
      if (requestId !== state.usersRequestId) return;
      if (error.status !== 401 && error.status !== 403) showAlert(errorText(error));
    } finally {
      if (requestId === state.usersRequestId) setUsersLoading(false);
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

    el("statsPeriod")?.addEventListener("change", (event) => {
      state.days = Number(event.target.value) || 30;
      state.offset = 0;
      loadAll();
    });
    el("statsSort")?.addEventListener("change", (event) => {
      state.sort = event.target.value;
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
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", bindEvents);
  } else {
    bindEvents();
  }

  window.FFAdminStatistics = { show, refresh };
})();
