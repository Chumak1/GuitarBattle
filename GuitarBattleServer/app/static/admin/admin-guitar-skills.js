/* fretfight admin — Guitar Skills.
 *
 * The competitive rating is a weighted blend of five components (accuracy,
 * challenge, score quality, duels, progression). This module owns:
 *
 *  - the single tuning row (`GET/PUT /admin/guitar-skills/config`), with a
 *    live check that the five component weights sum to 100% (±0.01) and that
 *    the two duel-strength weights do as well;
 *  - the append-only tuning history (`GET .../config/history`);
 *  - the coverage/status block (`GET .../status`);
 *  - a Dry Run and the real full recalculation of every active player
 *    (`POST .../recalculate`), the second one behind an explicit modal.
 *
 * Public surface: window.FFAdminGuitarSkills.show() / .refresh().
 */
(() => {
  "use strict";

  const API_BASE = "/api/v1";
  const TOKEN_KEY = "guitarbattle.admin.token";
  const WEIGHT_TOLERANCE = 0.01;

  /* DOM id -> API field. `percent` fields are shown in percent but stored as
   * a 0..1 fraction; `integer` fields are truncated; `nullable` allows empty. */
  const FIELDS = [
    { id: "gsAccuracyWeight", key: "accuracy_weight", percent: true },
    { id: "gsChallengeWeight", key: "challenge_weight", percent: true },
    { id: "gsScoreWeight", key: "score_weight", percent: true },
    { id: "gsDuelWeight", key: "duel_weight", percent: true },
    { id: "gsProgressionWeight", key: "progression_weight", percent: true },
    { id: "gsWindowSize", key: "window_size", integer: true },
    { id: "gsRecencyDecay", key: "recency_decay" },
    { id: "gsPerSongAttemptCap", key: "per_song_attempt_cap" },
    { id: "gsMinRankedEvents", key: "min_ranked_events", integer: true },
    { id: "gsDifficultyEasyMax", key: "difficulty_easy_max", integer: true },
    { id: "gsDifficultyEasyFactor", key: "difficulty_easy_factor" },
    { id: "gsDifficultyNormalMax", key: "difficulty_normal_max", integer: true },
    { id: "gsDifficultyNormalFactor", key: "difficulty_normal_factor" },
    { id: "gsDifficultyHardFactor", key: "difficulty_hard_factor" },
    { id: "gsNoteFactorMin", key: "note_factor_min" },
    { id: "gsNoteFactorMax", key: "note_factor_max" },
    { id: "gsMedianNoteCount", key: "median_note_count", integer: true, nullable: true },
    { id: "gsLevelNormalizationMax", key: "level_normalization_max", integer: true },
    { id: "gsXpReference", key: "xp_reference", integer: true },
    { id: "gsDuelStrengthGuitarWeight", key: "duel_strength_guitar_weight", percent: true },
    { id: "gsDuelStrengthLevelWeight", key: "duel_strength_level_weight", percent: true },
    { id: "gsExpectedScoreScale", key: "expected_score_scale" },
    { id: "gsReliabilityConstant", key: "reliability_constant" },
    { id: "gsConfidenceExpectedEvents", key: "confidence_expected_events", integer: true },
    { id: "gsConfidenceExpectedDuels", key: "confidence_expected_duels", integer: true },
    { id: "gsProvisionalThreshold", key: "provisional_threshold" },
  ];

  const WEIGHT_IDS = [
    "gsAccuracyWeight",
    "gsChallengeWeight",
    "gsScoreWeight",
    "gsDuelWeight",
    "gsProgressionWeight",
  ];
  const STRENGTH_IDS = ["gsDuelStrengthGuitarWeight", "gsDuelStrengthLevelWeight"];

  const state = {
    config: null,
    busy: false,
    recalculating: false,
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
    const box = el("gsAlert");
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
    const init = { method, headers, credentials: "omit" };
    if (body !== null) {
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

  /* ---------- formatting ---------- */

  function formatDate(value) {
    if (!value) return "—";
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return "—";
    return date.toLocaleString("ru-RU", {
      day: "2-digit",
      month: "2-digit",
      year: "numeric",
      hour: "2-digit",
      minute: "2-digit",
    });
  }

  function formatNumber(value, digits = 1) {
    const number = Number(value);
    if (!Number.isFinite(number)) return "—";
    return number.toFixed(digits);
  }

  function formatPercent(fraction, digits = 2) {
    const number = Number(fraction);
    if (!Number.isFinite(number)) return "—";
    return (number * 100).toFixed(digits);
  }

  function formatDelta(delta) {
    const number = Number(delta);
    if (!Number.isFinite(number)) return "—";
    const sign = number > 0 ? "+" : "";
    return `${sign}${number.toFixed(1)}`;
  }

  function deltaClass(delta) {
    const number = Number(delta);
    if (number > 0) return "text-success";
    if (number < 0) return "text-danger";
    return "text-secondary";
  }

  /* ---------- validation ---------- */

  function readNumber(input) {
    if (!input) return NaN;
    const raw = String(input.value ?? "").trim();
    if (raw === "") return NaN;
    return Number(raw.replace(",", "."));
  }

  function sumOfPercentageInputs(ids) {
    return ids.reduce((total, id) => {
      const value = readNumber(el(id));
      return total + (Number.isFinite(value) ? value : 0);
    }, 0);
  }

  /* Percent inputs are shown in percent, so compare against 100. */
  function sumIsOneHundred(ids) {
    return Math.abs(sumOfPercentageInputs(ids) - 100) <= WEIGHT_TOLERANCE;
  }

  function markInputs(ids, valid) {
    ids.forEach((id) => {
      const input = el(id);
      if (!input) return;
      input.classList.toggle("is-invalid", !valid);
    });
  }

  function writeMessage(id, text, valid) {
    const box = el(id);
    if (!box) return;
    box.textContent = text;
    box.classList.toggle("text-danger", !valid);
    box.classList.toggle("text-success", valid);
  }

  function validateForm() {
    const weightsTotal = sumOfPercentageInputs(WEIGHT_IDS);
    const weightsValid = sumIsOneHundred(WEIGHT_IDS);
    markInputs(WEIGHT_IDS, weightsValid);
    writeMessage(
      "gsWeightsMessage",
      `Сумма весов: ${weightsTotal.toFixed(2)}%${weightsValid ? " — ок" : " (нужно 100% ±0.01)"}`,
      weightsValid,
    );

    const strengthTotal = sumOfPercentageInputs(STRENGTH_IDS);
    const strengthValid = sumIsOneHundred(STRENGTH_IDS);
    markInputs(STRENGTH_IDS, strengthValid);
    writeMessage(
      "gsStrengthMessage",
      `Сумма: ${strengthTotal.toFixed(2)}%${strengthValid ? " — ок" : " (нужно 100%)"}`,
      strengthValid,
    );

    const save = el("gsConfigSave");
    if (save) save.disabled = state.busy || !(weightsValid && strengthValid);
    return weightsValid && strengthValid;
  }

  /* ---------- config ---------- */

  function renderConfig() {
    const config = state.config;
    if (!config) return;
    FIELDS.forEach((field) => {
      const input = el(field.id);
      if (!input) return;
      const value = config[field.key];
      if (value === null || value === undefined) {
        input.value = "";
      } else if (field.percent) {
        input.value = String(Math.round(Number(value) * 100 * 100) / 100);
      } else {
        input.value = String(value);
      }
    });
    const version = el("gsConfigVersion");
    if (version) {
      version.textContent = `calculation_version ${config.calculation_version} · обновлено ${formatDate(config.updated_at)}`;
    }
    validateForm();
  }

  async function loadConfig({ silent = false } = {}) {
    const config = await api("/admin/guitar-skills/config");
    state.config = config;
    renderConfig();
    if (!silent) showAlert("");
  }

  function fractionFromPercent(input) {
    const percent = readNumber(input);
    if (!Number.isFinite(percent)) return NaN;
    return Math.round((percent / 100) * 1e6) / 1e6;
  }

  function collectChanges() {
    const changes = {};
    const config = state.config || {};
    FIELDS.forEach((field) => {
      const input = el(field.id);
      if (!input) return;
      const current = config[field.key];

      if (field.nullable && String(input.value ?? "").trim() === "") {
        if (current !== null && current !== undefined) changes[field.key] = null;
        return;
      }

      let next;
      if (field.percent) {
        next = fractionFromPercent(input);
      } else if (field.integer) {
        const raw = readNumber(input);
        next = Number.isFinite(raw) ? Math.trunc(raw) : NaN;
      } else {
        next = readNumber(input);
      }
      if (!Number.isFinite(next)) return;

      const previous = Number(current);
      if (!Number.isFinite(previous) || Math.abs(next - previous) > 1e-6) {
        changes[field.key] = next;
      }
    });
    return changes;
  }

  function setBusy(busy) {
    state.busy = busy;
    const spinner = el("gsConfigSpinner");
    if (spinner) spinner.hidden = !busy;
    ["gsConfigReload", "gsReload", "gsHistoryReload", "gsDryRun", "gsRecalculate"].forEach((id) => {
      const button = el(id);
      if (button) button.disabled = busy || state.recalculating;
    });
    validateForm();
  }

  async function saveConfig(event) {
    if (event) event.preventDefault();
    if (state.busy) return;
    showAlert("");
    if (!validateForm()) {
      showAlert("Суммы весов должны равняться 100%: проверьте сообщения под полями.");
      return;
    }
    const changes = collectChanges();
    if (!Object.keys(changes).length) {
      toast("Изменений нет.", "warning");
      return;
    }
    setBusy(true);
    try {
      const updated = await api("/admin/guitar-skills/config", {
        method: "PUT",
        body: { ...changes, recalculate: false },
      });
      state.config = updated;
      renderConfig();
      toast("Параметры сохранены. Запустите пересчёт, чтобы применить их к рейтингу.");
      loadHistory({ silent: true });
      loadStatus({ silent: true });
    } catch (error) {
      if (error.status !== 401 && error.status !== 403) {
        showAlert(error.message || "Не удалось сохранить параметры.");
      }
    } finally {
      setBusy(false);
    }
  }

  /* ---------- status ---------- */

  function renderStatus(status) {
    const running = el("gsStatusRunning");
    if (running) {
      running.textContent = status.running ? "идёт" : "простой";
      running.classList.toggle("text-warning", Boolean(status.running));
    }
    const users = el("gsStatusUsers");
    if (users) users.textContent = String(Number(status.users) || 0);
    const provisional = el("gsStatusProvisional");
    if (provisional) provisional.textContent = String(Number(status.provisional) || 0);
    const incomplete = el("gsStatusIncomplete");
    if (incomplete) incomplete.textContent = String(Number(status.incomplete_history) || 0);
    const version = el("gsStatusVersion");
    if (version) version.textContent = String(Number(status.calculation_version) || 0);
    const calculatedAt = el("gsStatusCalculatedAt");
    if (calculatedAt) calculatedAt.textContent = formatDate(status.last_calculated_at);
  }

  async function loadStatus({ silent = false } = {}) {
    try {
      const status = await api("/admin/guitar-skills/status");
      renderStatus(status || {});
      if (status && status.running && !state.recalculating) {
        // The recalculation is running in another request/worker: poll once in a
        // while so the operator sees the final state without a manual refresh.
        setTimeout(() => {
          loadStatus({ silent: true });
          loadConfig({ silent: true });
        }, 4000);
      }
    } catch (error) {
      if (!silent && error.status !== 401 && error.status !== 403) {
        showAlert(error.message || "Не удалось загрузить статус.");
      }
    }
  }

  /* ---------- history ---------- */

  function historyRow(item) {
    const config = item.config || {};
    const weights = [
      formatPercent(config.accuracy_weight, 0),
      formatPercent(config.challenge_weight, 0),
      formatPercent(config.score_weight, 0),
      formatPercent(config.duel_weight, 0),
      formatPercent(config.progression_weight, 0),
    ].join(" / ");
    return `
      <tr>
        <td>${escapeHtml(formatDate(item.created_at))}</td>
        <td class="text-end">${escapeHtml(config.calculation_version ?? "—")}</td>
        <td class="font-monospace small">${escapeHtml(weights)}</td>
        <td class="d-none d-md-table-cell text-end">${escapeHtml(config.window_size ?? "—")}</td>
        <td class="d-none d-lg-table-cell text-end">${escapeHtml(config.recency_decay ?? "—")}</td>
        <td class="d-none d-lg-table-cell text-end">${escapeHtml(config.xp_reference ?? "—")}</td>
      </tr>
    `;
  }

  function renderHistory(items) {
    const body = el("gsHistoryBody");
    if (!body) return;
    if (!items.length) {
      body.innerHTML = '<tr><td colspan="6" class="text-secondary text-center py-4">История пуста: параметры ещё не менялись.</td></tr>';
      return;
    }
    body.innerHTML = items.map(historyRow).join("");
  }

  async function loadHistory({ silent = false } = {}) {
    try {
      const payload = await api("/admin/guitar-skills/config/history?limit=50");
      const items = Array.isArray(payload?.items) ? payload.items : [];
      renderHistory(items);
      const note = el("gsHistoryNote");
      if (note) note.textContent = `Последние ${items.length} сохранений из 50`;
    } catch (error) {
      if (!silent && error.status !== 401 && error.status !== 403) {
        showAlert(error.message || "Не удалось загрузить историю параметров.");
      }
    }
  }

  /* ---------- recalculation ---------- */

  function renderChanges(payload, dryRun) {
    const body = el("gsRecalcBody");
    const items = Array.isArray(payload?.items) ? payload.items : [];
    const summary = el("gsRecalcSummary");
    const users = Number(payload?.users) || 0;
    const changed = Number(payload?.changed) || 0;
    if (summary) {
      const prefix = dryRun ? "Dry Run (ничего не записано)" : "Пересчёт выполнен";
      const truncated = changed > items.length ? ` Показаны первые ${items.length}.` : "";
      summary.textContent = `${prefix}: игроков ${users}, изменений ${changed}.${truncated}`;
    }
    if (!body) return;
    if (!items.length) {
      body.innerHTML = '<tr><td colspan="6" class="text-secondary text-center py-4">Изменений нет: значения и места совпадают.</td></tr>';
      return;
    }
    body.innerHTML = items
      .map((item) => {
        const delta = Number(item.delta);
        return `
          <tr>
            <td class="text-truncate">${escapeHtml(item.username)}</td>
            <td class="text-end">${item.old_skills === null || item.old_skills === undefined ? "—" : escapeHtml(formatNumber(item.old_skills))}</td>
            <td class="text-end">${escapeHtml(formatNumber(item.new_skills))}</td>
            <td class="text-end ${deltaClass(delta)}">${escapeHtml(formatDelta(delta))}</td>
            <td class="d-none d-md-table-cell text-end">${item.old_rank === null || item.old_rank === undefined ? "—" : escapeHtml(item.old_rank)}</td>
            <td class="text-end">${escapeHtml(item.new_rank)}</td>
          </tr>
        `;
      })
      .join("");
  }

  async function runRecalculation(dryRun) {
    if (state.busy || state.recalculating) return;
    showAlert("");
    state.recalculating = true;
    setBusy(true);
    const spinner = el("gsRecalcSpinner");
    if (spinner) spinner.hidden = false;
    try {
      const payload = await api("/admin/guitar-skills/recalculate", {
        method: "POST",
        body: {
          dry_run: Boolean(dryRun),
          reason: dryRun ? "admin_dry_run" : "admin_manual",
        },
      });
      renderChanges(payload || {}, Boolean(dryRun));
      toast(
        dryRun
          ? "Dry Run завершён: данные не изменены."
          : "Глобальный рейтинг пересчитан.",
        dryRun ? "warning" : "success",
      );
      if (!dryRun) {
        loadConfig({ silent: true });
        loadHistory({ silent: true });
      }
      loadStatus({ silent: true });
    } catch (error) {
      if (error.status !== 401 && error.status !== 403) {
        if (error.status === 409) {
          showAlert("Пересчёт уже выполняется. Дождитесь завершения.");
        } else {
          showAlert(error.message || "Не удалось запустить пересчёт.");
        }
      }
    } finally {
      state.recalculating = false;
      if (spinner) spinner.hidden = true;
      setBusy(false);
    }
  }

  function openConfirmModal() {
    const modalEl = el("gsRecalcModal");
    if (!modalEl) {
      if (window.confirm("Пересчитать Guitar Skills всех активных игроков?")) {
        runRecalculation(false);
      }
      return;
    }
    const bootstrap = window.bootstrap;
    if (bootstrap && typeof bootstrap.Modal === "function") {
      bootstrap.Modal.getOrCreateInstance(modalEl).show();
    } else if (window.confirm("Пересчитать Guitar Skills всех активных игроков? XP и уровни не меняются.")) {
      runRecalculation(false);
    }
  }

  /* ---------- wiring ---------- */

  function bind() {
    el("gsReload")?.addEventListener("click", () => reload());
    el("gsHistoryReload")?.addEventListener("click", () => loadHistory());
    el("gsConfigReload")?.addEventListener("click", () => {
      renderConfig();
      showAlert("");
      toast("Несохранённые правки сброшены.", "warning");
    });
    el("gsConfigForm")?.addEventListener("submit", saveConfig);
    el("gsDryRun")?.addEventListener("click", () => runRecalculation(true));
    el("gsRecalculate")?.addEventListener("click", openConfirmModal);
    el("gsRecalcConfirm")?.addEventListener("click", () => {
      const modalEl = el("gsRecalcModal");
      const bootstrap = window.bootstrap;
      if (modalEl && bootstrap && typeof bootstrap.Modal === "function") {
        bootstrap.Modal.getOrCreateInstance(modalEl).hide();
      }
      runRecalculation(false);
    });

    // Live weight validation: the save button is blocked until both sums hold.
    [...WEIGHT_IDS, ...STRENGTH_IDS].forEach((id) => {
      el(id)?.addEventListener("input", validateForm);
      el(id)?.addEventListener("change", validateForm);
    });
  }

  async function reload({ silent = false } = {}) {
    if (state.busy) return;
    setBusy(true);
    try {
      if (!silent) showAlert("");
      await loadConfig({ silent });
      await Promise.all([loadStatus({ silent }), loadHistory({ silent })]);
    } catch (error) {
      if (error.status !== 401 && error.status !== 403) {
        showAlert(error.message || "Не удалось загрузить Guitar Skills.");
      }
    } finally {
      setBusy(false);
    }
  }

  window.FFAdminGuitarSkills = {
    show() {
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
