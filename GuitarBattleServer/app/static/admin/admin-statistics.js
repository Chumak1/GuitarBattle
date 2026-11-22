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
    // "all" | "included" | "excluded": filters the table by the statistics
    // flag. The aggregates ignore excluded accounts, so this filter explains
    // why the overview is smaller than the account list.
    include: "all",
    sort: "last_seen",
    order: "desc",
    offset: 0,
    total: 0,
    users: [],
    games: [],
    usersRequestId: 0,
    gamesRequestId: 0,
    // One account card: fetched on row click, re-fetched after a character
    // change. ``requestId`` drops answers for a card that is already closed.
    detail: {
      userId: "",
      data: null,
      requestId: 0,
      busy: false,
    },
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
    // PATCH requests carry a JSON body; GET/DELETE keep the plain headers.
    if (options.body !== undefined) headers["Content-Type"] = "application/json";

    const response = await fetch(API_BASE + path, {
      method: options.method || "GET",
      headers,
      body: options.body,
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

  function formatPercent(value) {
    const number = Number(value);
    if (!Number.isFinite(number)) return "—";
    return (
      new Intl.NumberFormat("ru-RU", { maximumFractionDigits: 2 }).format(number) + " %"
    );
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

  /* ---------- game-mode columns ---------- */

  /* Compact "Online" column: the current Elo rating over the W/L record. A
   * player with no finished online duel shows a dash instead of a misleading
   * start rating. */
  function onlineCell(user) {
    const matches = Number(user.online_matches) || 0;
    const wins = Number(user.online_wins) || 0;
    const losses = Number(user.online_losses) || 0;
    if (!matches && user.online_rating == null) {
      return '<span class="stats-mode-empty">—</span>';
    }
    const tooltip = "Онлайн-дуэли: " + matches + " матчей, " + wins + " побед, " +
      losses + " поражений";
    const rating = user.online_rating == null ? "—" : formatNumber(user.online_rating);
    return (
      '<div class="stats-mode" title="' + escapeHtml(tooltip) + '">' +
      '<span class="stats-mode-value">' + rating + "</span>" +
      '<span class="stats-mono">' + formatNumber(wins) + "W / " + formatNumber(losses) +
      "L</span>" +
      "</div>"
    );
  }

  /* Compact "Battle" column: completed towers / cleared stages. */
  function battleCell(user) {
    const towers = Number(user.battle_towers_completed) || 0;
    const stages = Number(user.battle_stages_cleared) || 0;
    if (!towers && !stages) {
      return '<span class="stats-mode-empty">—</span>';
    }
    const tooltip = "Battle: " + towers + " башен пройдено, " + stages + " этапов зачищено";
    return (
      '<div class="stats-mode" title="' + escapeHtml(tooltip) + '">' +
      '<span class="stats-mode-value">' + formatNumber(towers) + " / " +
      formatNumber(stages) + "</span>" +
      '<span class="stats-mono">башен / этапов</span>' +
      "</div>"
    );
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
    const excluded = Boolean(user.statistics_excluded);
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
      '<tr class="stats-user-row' + (excluded ? " is-excluded" : "") +
      '" data-stats-user="' + escapeHtml(user.user_id) +
      '" tabindex="0" title="Открыть карточку игрока">' +
      "<td>" +
      '<div class="stats-login">' + escapeHtml(user.username) + "</div>" +
      '<div class="stats-mono">' + escapeHtml(user.user_id) + "</div>" +
      (isSelf ? '<div class="stats-mono stats-self">это вы</div>' : "") +
      (excluded ? '<div class="stats-excluded-badge">исключён</div>' : "") +
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
      '<td class="d-none d-lg-table-cell">' + onlineCell(user) + "</td>" +
      '<td class="d-none d-lg-table-cell">' + battleCell(user) + "</td>" +
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
      '<td class="text-center">' +
      '<input class="form-check-input stats-include-check" type="checkbox" ' +
      'data-stats-include="' + escapeHtml(user.user_id) + '"' +
      (excluded ? "" : " checked") +
      ' title="Учитывать в статистике" aria-label="Учитывать в статистике">' +
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
          : state.include === "excluded"
            ? "Исключённых аккаунтов нет."
            : state.include === "included"
              ? "Все аккаунты исключены из статистики."
              : "Пока нет ни одного игрока.";
      body.innerHTML =
        '<tr><td colspan="16" class="stats-table-empty">' + escapeHtml(message) + "</td></tr>";
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
        '<tr><td colspan="16" class="table-loading">' +
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
      if (state.include && state.include !== "all") params.set("include", state.include);
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
      // The card of a deleted account cannot be refreshed any more.
      if (state.detail.userId === String(user.user_id)) {
        state.detail.requestId += 1;
        state.detail.userId = "";
        state.detail.data = null;
        window.bootstrap?.Modal?.getInstance(el("statsUserModal"))?.hide();
      }
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

  /* Optimistic toggle of the "count in statistics" flag. The checkbox flips
   * immediately; a failed PATCH restores the previous value and reports through
   * the page toast (the alert box is the fallback when the toast helper is not
   * loaded). The whole tab is reloaded afterwards because the overview, the
   * per-game counters and the include filter all depend on the flag. */
  async function setStatisticsExcluded(userId, excluded) {
    const user = state.users.find((item) => String(item.user_id) === String(userId));
    if (!user) return;
    const previous = Boolean(user.statistics_excluded);
    if (previous === excluded) return;

    user.statistics_excluded = excluded;
    renderUsers();
    showAlert("");

    try {
      const result = await api(
        "/admin/statistics/users/" + encodeURIComponent(userId),
        {
          method: "PATCH",
          body: JSON.stringify({ statistics_excluded: excluded }),
        },
      );
      user.statistics_excluded = Boolean(result.statistics_excluded);
      renderUsers();
      notify(
        user.statistics_excluded
          ? "Аккаунт " + user.username + " исключён из статистики."
          : "Аккаунт " + user.username + " снова учитывается в статистике.",
      );
      await Promise.all([loadUsers(), loadOverview(), loadGames()]);
    } catch (error) {
      user.statistics_excluded = previous;
      renderUsers();
      if (error.status !== 401 && error.status !== 403) {
        const message = errorText(error);
        showAlert(message);
        notify(message, "danger");
      }
    }
  }

  /* ---------- one account card ---------- */
  /* The API answers these English codes for the character endpoints; the panel
   * shows a Russian sentence instead of the raw code. */
  const CHARACTER_ERRORS = {
    starter_character: "Стартовый персонаж — всегда доступен, убрать его нельзя.",
    not_owned: "Этот персонаж не добавлен этому игроку.",
    character_not_found: "Персонаж не найден.",
    user_not_found: "Пользователь не найден.",
  };

  function characterError(error) {
    return CHARACTER_ERRORS[error && error.message] || errorText(error);
  }

  function detailAlert(message) {
    const box = el("statsUserAlert");
    if (!box) return;
    box.textContent = message || "";
    box.hidden = !message;
  }

  function detailMetric(label, value, hint) {
    return (
      '<div class="stats-detail-metric">' +
      '<span class="stats-detail-label">' + escapeHtml(label) + "</span>" +
      '<span class="stats-detail-value">' + value + "</span>" +
      (hint ? '<span class="stats-detail-hint">' + escapeHtml(hint) + "</span>" : "") +
      "</div>"
    );
  }

  function detailBasics(basics) {
    const selected = basics.selected_character
      ? escapeHtml(basics.selected_character.name)
      : '<span class="stats-mode-empty">не выбран</span>';
    const rows = [
      ["Почта", escapeHtml(basics.email)],
      ["Уровень", "LVL " + formatNumber(basics.level) + " · " +
        formatNumber(basics.xp_total) + " XP"],
      ["Создан", formatDateTime(basics.created_at)],
      ["Статус", basics.is_active
        ? '<span class="stats-activity-state is-online">активен</span>'
        : '<span class="stats-activity-state">отключён</span>'],
      ["Персонаж", selected],
      ["ID", '<span class="stats-mono">' + escapeHtml(basics.user_id) + "</span>"],
    ];
    const items = rows
      .map(
        ([label, value]) =>
          "<div><dt>" + escapeHtml(label) + "</dt><dd>" + value + "</dd></div>",
      )
      .join("");
    return (
      '<section class="stats-detail-block">' +
      '<div class="stats-detail-title">Основное</div>' +
      '<dl class="stats-detail-kv">' + items + "</dl>" +
      "</section>"
    );
  }

  function detailOnline(online) {
    const metrics = [
      detailMetric("Матчи", formatNumber(online.matches)),
      detailMetric(
        "Победы / поражения",
        formatNumber(online.wins) + " / " + formatNumber(online.losses),
        online.draws ? "ничьих: " + formatNumber(online.draws) : "",
      ),
      detailMetric("Винрейт", formatPercent(online.win_rate)),
      detailMetric(
        "Рейтинг",
        online.rating == null ? "—" : formatNumber(online.rating),
        online.rating == null ? "нет рейтинговых матчей" : "текущий Elo",
      ),
      detailMetric("Лучшая серия", formatNumber(online.best_win_streak), "побед подряд"),
      detailMetric("Точность", formatPercent(online.accuracy)),
      detailMetric("Урон", formatNumber(online.damage_dealt)),
      detailMetric("XP", formatNumber(online.xp_earned)),
    ].join("");
    return (
      '<section class="stats-detail-block">' +
      '<div class="stats-detail-head"><div class="stats-detail-title">Онлайн-дуэли</div>' +
      '<div class="stats-detail-sub">последний матч: ' +
      escapeHtml(relativeTime(online.last_match_at)) + "</div></div>" +
      '<div class="stats-detail-metrics">' + metrics + "</div>" +
      "</section>"
    );
  }

  function detailTowers(towers) {
    if (!towers || !towers.length) {
      return '<p class="stats-detail-empty">Прогресса в башнях пока нет.</p>';
    }
    const rows = towers
      .map((tower) => {
        const done = tower.completed_at
          ? '<span class="stats-activity-state is-online">да</span>'
          : '<span class="stats-activity-state">нет</span>';
        return (
          "<tr>" +
          "<td>" + escapeHtml(tower.name) + "</td>" +
          '<td><span class="text-nowrap">' + formatNumber(tower.best_stage) + " / " +
          formatNumber(tower.stage_count) + "</span></td>" +
          "<td>" + formatNumber(tower.completions) + "</td>" +
          "<td>" + done + "</td>" +
          "</tr>"
        );
      })
      .join("");
    return (
      '<div class="stats-towers-wrap">' +
      '<table class="table stats-towers-table align-middle mb-0">' +
      "<thead><tr><th>Башня</th><th>Этапы</th><th>Прохождений</th><th>Завершена</th></tr></thead>" +
      "<tbody>" + rows + "</tbody></table></div>"
    );
  }

  function detailBattle(battle) {
    const metrics = [
      detailMetric("Башен пройдено", formatNumber(battle.towers_completed)),
      detailMetric("Этапов зачищено", formatNumber(battle.stages_cleared)),
      detailMetric("Забегов", formatNumber(battle.runs)),
      detailMetric("XP", formatNumber(battle.xp_earned)),
    ].join("");
    return (
      '<section class="stats-detail-block">' +
      '<div class="stats-detail-head"><div class="stats-detail-title">Battle</div>' +
      '<div class="stats-detail-sub">последний забег: ' +
      escapeHtml(relativeTime(battle.last_run_at)) + "</div></div>" +
      '<div class="stats-detail-metrics">' + metrics + "</div>" +
      detailTowers(battle.towers) +
      "</section>"
    );
  }

  function detailCharacters(characters, basics) {
    const owned = characters.owned || [];
    const ownedIds = new Set(owned.map((item) => item.id));
    const available = (characters.available || []).filter(
      (item) => !ownedIds.has(item.id),
    );
    const selectedId =
      characters.selected_character_id == null
        ? null
        : String(characters.selected_character_id);

    const items = owned
      .map((character) => {
        const selected =
          selectedId != null && String(character.public_id) === selectedId;
        const meta = character.is_starter
          ? "стартовый"
          : "открыт " + formatDateTime(character.unlocked_at);
        const button = character.can_remove
          ? '<button type="button" class="action-btn action-danger" ' +
            'data-stats-char-remove="' + escapeHtml(character.id) + '" ' +
            'title="Убрать персонажа" aria-label="Убрать персонажа">' +
            '<i class="bi bi-x-lg"></i></button>'
          : '<button type="button" class="action-btn" disabled ' +
            'title="Стартовый персонаж — всегда доступен" ' +
            'aria-label="Стартовый персонаж — всегда доступен">' +
            '<i class="bi bi-lock"></i></button>';
        return (
          '<li class="stats-char' + (selected ? " is-selected" : "") + '">' +
          '<div class="stats-char-text">' +
          '<span class="stats-char-name">' + escapeHtml(character.name) +
          (selected ? ' <span class="stats-char-badge">выбран</span>' : "") +
          "</span>" +
          '<span class="stats-char-meta">' + escapeHtml(meta) + "</span>" +
          "</div>" + button + "</li>"
        );
      })
      .join("");

    const options = available
      .map(
        (character) =>
          '<option value="' + escapeHtml(character.id) + '">' +
          escapeHtml(character.name) + "</option>",
      )
      .join("");
    const addBlock = available.length
      ? '<div class="stats-chars-add">' +
        '<select id="statsCharSelect" class="form-select form-select-sm" ' +
        'aria-label="Персонаж для добавления">' + options + "</select>" +
        '<button id="statsCharAdd" class="btn btn-sm btn-warning" type="button">' +
        '<i class="bi bi-plus-lg me-1"></i>Добавить</button></div>'
      : '<div class="form-hint">Все персонажи уже добавлены.</div>';

    return (
      '<section class="stats-detail-block">' +
      '<div class="stats-detail-head"><div class="stats-detail-title">Персонажи</div>' +
      '<div class="stats-detail-sub">выбран: ' +
      (basics.selected_character
        ? escapeHtml(basics.selected_character.name)
        : "не выбран") +
      "</div></div>" +
      (items
        ? '<ul class="stats-char-list">' + items + "</ul>"
        : '<p class="stats-detail-empty">Персонажей пока нет.</p>') +
      addBlock +
      "</section>"
    );
  }

  function renderDetail(data) {
    const body = el("statsUserBody");
    if (!body) return;
    const basics = data.basics;
    el("statsUserModalTitle").textContent =
      basics.display_name || basics.username;
    el("statsUserModalSubtitle").textContent =
      basics.username + " · " + basics.email;
    body.innerHTML =
      detailBasics(basics) +
      '<div class="stats-detail-grid">' +
      detailOnline(data.online) +
      detailBattle(data.battle) +
      "</div>" +
      detailCharacters(data.characters, basics);
  }

  function setDetailBusy(busy) {
    state.detail.busy = busy;
    document
      .querySelectorAll(
        "#statsUserBody [data-stats-char-remove], #statsUserBody #statsCharAdd, #statsUserBody #statsCharSelect",
      )
      .forEach((element) => {
        element.disabled = busy;
      });
  }

  async function loadDetail(userId, options = {}) {
    const requestId = ++state.detail.requestId;
    state.detail.userId = String(userId);
    if (!options.silent) {
      const body = el("statsUserBody");
      if (body) {
        body.innerHTML =
          '<div class="table-loading"><span class="spinner-border spinner-border-sm me-2"></span>Загрузка…</div>';
      }
    }
    try {
      const data = await api(
        "/admin/users/" + encodeURIComponent(userId) + "/overview",
      );
      if (requestId !== state.detail.requestId) return;
      state.detail.data = data;
      detailAlert("");
      renderDetail(data);
    } catch (error) {
      if (requestId !== state.detail.requestId) return;
      state.detail.data = null;
      if (error.status === 401 || error.status === 403) return;
      detailAlert(errorText(error));
      const body = el("statsUserBody");
      if (body) {
        body.innerHTML =
          '<p class="stats-detail-empty">Не удалось загрузить карточку игрока.</p>';
      }
    }
  }

  function openUser(userId) {
    const user = state.users.find((item) => String(item.user_id) === String(userId));
    state.detail.data = null;
    state.detail.userId = String(userId);
    el("statsUserModalTitle").textContent =
      (user && (user.display_name || user.username)) || "Игрок";
    el("statsUserModalSubtitle").textContent =
      user ? user.username + " · " + user.email : "Загрузка…";
    detailAlert("");
    const modal = window.bootstrap?.Modal
      ? window.bootstrap.Modal.getOrCreateInstance(el("statsUserModal"))
      : null;
    modal?.show();
    loadDetail(userId);
  }

  async function mutateCharacter(userId, characterId, method) {
    if (state.detail.busy) return;
    setDetailBusy(true);
    try {
      const result = await api(
        "/admin/users/" + encodeURIComponent(userId) + "/characters/" +
          encodeURIComponent(characterId),
        { method },
      );
      if (method === "POST") {
        notify(result.created ? "Персонаж добавлен." : "Персонаж уже был добавлен.");
      } else {
        notify(
          result.cleared_selected
            ? "Персонаж убран, выбранный персонаж сброшен."
            : "Персонаж убран из коллекции.",
        );
      }
      await loadDetail(userId, { silent: true });
    } catch (error) {
      if (error.status !== 401 && error.status !== 403) {
        const message = characterError(error);
        notify(message, "danger");
        detailAlert(message);
      }
    } finally {
      setDetailBusy(false);
    }
  }

  function addCharacter() {
    const userId = state.detail.userId;
    const select = el("statsCharSelect");
    if (!userId || !select || !select.value) return;
    mutateCharacter(userId, select.value, "POST");
  }

  function removeCharacter(characterId) {
    const userId = state.detail.userId;
    if (!userId || !characterId) return;
    const owned = (state.detail.data?.characters?.owned) || [];
    const character = owned.find((item) => String(item.id) === String(characterId));
    const name = character ? character.name : "персонажа";
    if (!window.confirm("Убрать «" + name + "» из коллекции игрока?")) return;
    mutateCharacter(userId, characterId, "DELETE");
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
      const toggle = event.target.closest("[data-stats-include]");
      if (toggle) {
        // The checkbox lives inside a clickable row: toggle the flag instead of
        // opening the account card.
        setStatisticsExcluded(
          toggle.getAttribute("data-stats-include"),
          toggle.checked,
        );
        return;
      }
      const remove = event.target.closest("[data-stats-delete]");
      if (remove) {
        deleteUser(remove.getAttribute("data-stats-delete"));
        return;
      }
      const row = event.target.closest("tr[data-stats-user]");
      if (row) openUser(row.getAttribute("data-stats-user"));
    });

    /* Rows are focusable so the card is reachable from the keyboard. */
    el("statsUsersBody")?.addEventListener("keydown", (event) => {
      if (event.key !== "Enter" && event.key !== " ") return;
      if (event.target.closest("button, input")) return;
      const row = event.target.closest("tr[data-stats-user]");
      if (!row) return;
      event.preventDefault();
      openUser(row.getAttribute("data-stats-user"));
    });

    el("statsUserBody")?.addEventListener("click", (event) => {
      if (event.target.closest("#statsCharAdd")) {
        addCharacter();
        return;
      }
      const remove = event.target.closest("[data-stats-char-remove]");
      if (remove) removeCharacter(remove.getAttribute("data-stats-char-remove"));
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

    el("statsInclude")?.addEventListener("change", (event) => {
      state.include = event.target.value || "all";
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
