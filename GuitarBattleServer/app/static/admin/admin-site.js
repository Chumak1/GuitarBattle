/* fretfight admin — landing CMS and game build uploads.
 *
 * Kept separate from admin.js: the songs section owns its own state and this
 * script only needs the administrator JWT from sessionStorage.
 */
(() => {
  "use strict";

  const API_BASE = "/api/v1";
  const TOKEN_KEY = "guitarbattle.admin.token";
  const I18N = window.FFI18N;

  const SCHEMA = [
    {
      key: "hero",
      title: "Первый экран",
      rows: [
        { kind: "field", key: "kicker", label: "Надзаголовок" },
        { kind: "field", key: "title", label: "Заголовок" },
        { kind: "text", key: "text", label: "Описание" },
      ],
    },
    {
      key: "facts",
      title: "Коротко о проекте",
      rows: [{ kind: "pairs", key: "items", label: "Факты — заголовок | текст" }],
    },
    {
      key: "about",
      title: "О проекте",
      rows: [
        { kind: "field", key: "kicker", label: "Надзаголовок" },
        { kind: "field", key: "title", label: "Заголовок" },
        { kind: "paragraphs", key: "paragraphs", label: "Абзацы — по одному на строку" },
        { kind: "lines", key: "items", label: "Список — по одному пункту на строку" },
        { kind: "panel", key: "panel", label: "Панель «Как считается результат»" },
      ],
    },
    {
      key: "how",
      title: "Как играть",
      rows: [
        { kind: "field", key: "kicker", label: "Надзаголовок" },
        { kind: "field", key: "title", label: "Заголовок" },
        { kind: "text", key: "text", label: "Описание" },
        { kind: "pairs", key: "steps", label: "Шаги — заголовок | текст" },
      ],
    },
    {
      key: "features",
      title: "Возможности",
      rows: [
        { kind: "field", key: "kicker", label: "Надзаголовок" },
        { kind: "field", key: "title", label: "Заголовок" },
        { kind: "pairs", key: "cards", label: "Карточки — заголовок | текст" },
      ],
    },
    {
      key: "requirements",
      title: "Требования",
      rows: [
        { kind: "field", key: "kicker", label: "Надзаголовок" },
        { kind: "field", key: "title", label: "Заголовок" },
        { kind: "text", key: "text", label: "Описание" },
        { kind: "lines", key: "items", label: "Список — по одному пункту на строку" },
        { kind: "panel", key: "panel", label: "Панель «Статус проекта»" },
      ],
    },
    {
      key: "support",
      title: "Поддержать проект",
      rows: [
        { kind: "field", key: "kicker", label: "Надзаголовок" },
        { kind: "field", key: "title", label: "Заголовок" },
        { kind: "text", key: "text", label: "Описание" },
        { kind: "field", key: "button", label: "Текст кнопки" },
      ],
    },
    {
      key: "cta",
      title: "Финальный блок",
      rows: [
        { kind: "field", key: "title", label: "Заголовок" },
        { kind: "text", key: "text", label: "Описание" },
      ],
    },
  ];

  const state = {
    content: null,
    loaded: false,
    builds: [],
    buildsLoaded: false,
    buildsPlatform: "",
    buildsArtifact: "",
    buildNotesId: null,
    buildNotesModal: null,
    contacts: null,
    contactsLoaded: false,
    section: "songs",
  };

  // Platform metadata of a game build: the value is the API ``platform``.
  // macOS ships one universal build for Apple Silicon and Intel.
  const BUILD_PLATFORMS = [
    {
      value: "windows",
      label: "Windows",
      short: "Windows",
      badge: "badge-platform-windows",
    },
    {
      value: "macos",
      label: "macOS",
      short: "macOS",
      badge: "badge-platform-macos",
    },
  ];

  // Artifact metadata: ``installer`` is what the download button on the site
  // serves, ``portable`` is the ZIP the installed client unpacks to update
  // itself. The extensions mirror app/services/game_builds.py; the server still
  // validates every pair.
  const BUILD_ARTIFACTS = [
    {
      value: "installer",
      label: "Установщик",
      short: "Установщик",
      badge: "badge-artifact-installer",
      accept: { windows: ".exe,.msi,.zip", macos: ".dmg,.zip" },
      extensions: { windows: "EXE, MSI или ZIP", macos: "DMG или ZIP" },
    },
    {
      value: "portable",
      label: "Portable ZIP для автообновления",
      short: "Portable",
      badge: "badge-artifact-portable",
      accept: { windows: ".zip", macos: ".zip" },
      extensions: { windows: "ZIP", macos: "ZIP" },
    },
  ];

  function platformInfo(value) {
    return (
      BUILD_PLATFORMS.find((item) => item.value === value) || BUILD_PLATFORMS[0]
    );
  }

  function artifactInfo(value) {
    return (
      BUILD_ARTIFACTS.find((item) => item.value === value) || BUILD_ARTIFACTS[0]
    );
  }

  // Builds uploaded before the split have no ``artifact`` field: they are
  // installers, which is also what the column default backfills.
  function buildArtifact(build) {
    return (build && build.artifact) || "installer";
  }

  const el = (id) => document.getElementById(id);

  function raw(key, lang) {
    return I18N ? I18N.raw(key, lang) : "";
  }

  function stripTags(value) {
    return String(value || "").replace(/<[^>]*>/g, "");
  }

  function clone(value) {
    return JSON.parse(JSON.stringify(value));
  }

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
    if (value < 1024) return value + " Б";
    if (value < 1024 * 1024) return (value / 1024).toFixed(1).replace(".", ",") + " КБ";
    if (value < 1024 * 1024 * 1024) {
      return (value / (1024 * 1024)).toFixed(1).replace(".", ",") + " МБ";
    }
    return (value / (1024 * 1024 * 1024)).toFixed(2).replace(".", ",") + " ГБ";
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

  function showAlert(target, message, variant = "danger") {
    const box = typeof target === "string" ? el(target) : target;
    if (!box) return;
    box.className = "alert alert-" + variant;
    box.textContent = message || "";
    box.hidden = !message;
  }

  async function api(path, options = {}) {
    const token = sessionStorage.getItem(TOKEN_KEY) || "";
    const headers = {};
    if (token) headers.Authorization = "Bearer " + token;

    const init = {
      method: options.method || "GET",
      headers,
      credentials: "omit",
    };
    if (options.body instanceof FormData) {
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
        data = text;
      }
    }
    if (!response.ok) {
      const error = new Error(detailOf(data) || "Ошибка запроса (" + response.status + ")");
      error.status = response.status;
      throw error;
    }
    return data;
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
    if (error && error.status === 413) {
      return "Файл слишком большой.";
    }
    return (error && error.message) || "Не удалось выполнить запрос.";
  }

  /* ---------- defaults ---------- */

  function defaultsFor(lang) {
    const r = (key) => raw(key, lang);
    const fact = (index) => ({
      title: r("landing.fact" + index + ".title"),
      text: r("landing.fact" + index + ".text"),
    });
    const score = (name, valueKey) => ({ label: r("landing.score." + name), value: r(valueKey) });

    return {
      hero: {
        kicker: r("landing.hero.kicker"),
        title: r("landing.hero.title"),
        text: stripTags(r("landing.hero.lead")),
      },
      facts: {
        items: [1, 2, 3, 4].map(fact),
      },
      about: {
        kicker: r("landing.about.kicker"),
        title: r("landing.about.title"),
        paragraphs: [r("landing.about.p1"), r("landing.about.p2")],
        items: [1, 2, 3, 4].map((index) => r("landing.about.list" + index)),
        panel: {
          title: r("landing.about.panel"),
          items: [
            score("accuracy", "landing.score.accuracyValue"),
            score("combo", "landing.score.comboValue"),
            score("points", "landing.score.pointsValue"),
            score("level", "landing.score.levelValue"),
          ],
        },
      },
      how: {
        kicker: r("landing.how.kicker"),
        title: r("landing.how.title"),
        text: r("landing.how.text"),
        steps: [1, 2, 3].map((index) => ({
          title: r("landing.how.step" + index + ".title"),
          text: r("landing.how.step" + index + ".text"),
        })),
      },
      features: {
        kicker: r("landing.features.kicker"),
        title: r("landing.features.title"),
        cards: [1, 2, 3, 4].map((index) => ({
          title: r("landing.features.card" + index + ".title"),
          text: r("landing.features.card" + index + ".text"),
        })),
      },
      requirements: {
        kicker: r("landing.req.kicker"),
        title: r("landing.req.title"),
        text: r("landing.req.text"),
        items: [1, 2, 3, 4].map((index) => r("landing.req.list" + index)),
        panel: {
          title: r("landing.req.panel"),
          items: [
            { label: r("landing.req.platform"), value: r("landing.req.platformValue") },
            { label: r("landing.req.state"), value: r("landing.status") },
            { label: r("landing.req.catalog"), value: r("landing.req.catalogValue") },
            { label: r("landing.req.site"), value: r("landing.req.siteValue") },
          ],
        },
      },
      support: {
        kicker: r("landing.supportProject.kicker"),
        title: r("landing.supportProject.title"),
        text: r("landing.supportProject.text"),
        button: r("landing.supportProject.button"),
      },
      cta: {
        title: r("landing.cta.title"),
        text: r("landing.cta.text"),
      },
    };
  }

  function baseBlocks() {
    const ru = defaultsFor("ru");
    const en = defaultsFor("en");
    const blocks = {};
    Object.keys(ru).forEach((key) => {
      blocks[key] = { visible: true, ru: clone(ru[key]), en: clone(en[key]) };
    });
    return blocks;
  }

  function mergeSaved(blocks, saved) {
    if (!saved || typeof saved !== "object") return blocks;
    Object.keys(blocks).forEach((key) => {
      const part = saved[key];
      if (!part || typeof part !== "object") return;
      if (part.visible === false) blocks[key].visible = false;
      ["ru", "en"].forEach((lang) => {
        if (part[lang] && typeof part[lang] === "object") {
          blocks[key][lang] = Object.assign({}, blocks[key][lang], part[lang]);
        }
      });
    });
    return blocks;
  }

  /* ---------- editor values ---------- */

  function linesText(value) {
    return Array.isArray(value) ? value.join("\n") : "";
  }

  function pairsText(value, first, second) {
    if (!Array.isArray(value)) return "";
    return value
      .map((item) => {
        const left = item && item[first] != null ? item[first] : "";
        const right = item && item[second] != null ? item[second] : "";
        return right ? left + " | " + right : left;
      })
      .join("\n");
  }

  function parseLines(text) {
    return String(text || "")
      .split("\n")
      .map((line) => line.trim())
      .filter(Boolean);
  }

  function parsePairs(text, first, second) {
    return parseLines(text).map((line) => {
      const index = line.indexOf("|");
      const left = index >= 0 ? line.slice(0, index).trim() : line.trim();
      const right = index >= 0 ? line.slice(index + 1).trim() : "";
      const item = {};
      item[first] = left;
      item[second] = right;
      return item;
    });
  }

  function fieldValue(block, row, lang) {
    const data = (block && block[lang]) || {};
    if (row.kind === "field" || row.kind === "text") return data[row.key] || "";
    if (row.kind === "paragraphs" || row.kind === "lines") return linesText(data[row.key]);
    if (row.kind === "pairs") return pairsText(data[row.key], "title", "text");
    if (row.kind === "panel") {
      const panel = data.panel || {};
      return {
        title: panel.title || "",
        items: pairsText(panel.items, "label", "value"),
      };
    }
    return "";
  }

  function control(value, multiline) {
    const element = document.createElement(multiline ? "textarea" : "input");
    if (multiline) element.rows = 3;
    else element.type = "text";
    element.className = "form-control";
    element.value = value || "";
    return element;
  }

  function labeledControl(lang, label, value, multiline) {
    const wrapper = document.createElement("div");
    wrapper.className = "col-md-6";

    const caption = document.createElement("div");
    caption.className = "landing-lang-label";
    caption.textContent = label;

    const input = control(value, multiline);
    wrapper.appendChild(caption);
    wrapper.appendChild(input);
    wrapper.dataset.lang = lang;

    return { wrapper, input };
  }

  function renderRow(card, block, row) {
    const rowElement = document.createElement("div");
    rowElement.className = "col-12 landing-row";

    const label = document.createElement("label");
    label.className = "form-label";
    label.textContent = row.label;
    rowElement.appendChild(label);

    const columns = document.createElement("div");
    columns.className = "row g-2";

    ["ru", "en"].forEach((lang) => {
      const value = fieldValue(block, row, lang);

      if (row.kind === "panel") {
        const wrapper = document.createElement("div");
        wrapper.className = "col-md-6";
        wrapper.dataset.lang = lang;

        const caption = document.createElement("div");
        caption.className = "landing-lang-label";
        caption.textContent = lang.toUpperCase() + " · заголовок";
        wrapper.appendChild(caption);

        const title = control(value.title, false);
        title.dataset.field = "panel-title:" + row.key + ":" + lang;
        wrapper.appendChild(title);

        const itemsCaption = document.createElement("div");
        itemsCaption.className = "landing-lang-label mt-2";
        itemsCaption.textContent = "Пункты — название | значение";
        wrapper.appendChild(itemsCaption);

        const items = control(value.items, true);
        items.rows = 4;
        items.dataset.field = "panel-items:" + row.key + ":" + lang;
        wrapper.appendChild(items);

        columns.appendChild(wrapper);
        return;
      }

      const multiline = row.kind !== "field";
      const { wrapper, input } = labeledControl(lang, lang.toUpperCase(), value, multiline);
      input.dataset.field = row.kind + ":" + row.key + ":" + lang;
      columns.appendChild(wrapper);
    });

    rowElement.appendChild(columns);
    card.appendChild(rowElement);
  }

  function renderEditor() {
    const container = el("landingBlocks");
    if (!container || !state.content) return;
    container.textContent = "";

    SCHEMA.forEach((meta) => {
      const block = state.content.blocks[meta.key];
      if (!block) return;

      const card = document.createElement("section");
      card.className = "card content-card block-editor";
      card.dataset.block = meta.key;

      const body = document.createElement("div");
      body.className = "card-body p-4";

      const head = document.createElement("div");
      head.className = "d-flex flex-wrap justify-content-between align-items-center gap-2 mb-3";

      const title = document.createElement("h2");
      title.className = "h5 mb-0";
      title.textContent = meta.title;

      const switchWrap = document.createElement("div");
      switchWrap.className = "form-check form-switch mb-0";
      const checkbox = document.createElement("input");
      checkbox.type = "checkbox";
      checkbox.className = "form-check-input block-visible";
      checkbox.id = "visible-" + meta.key;
      checkbox.checked = block.visible !== false;
      const label = document.createElement("label");
      label.className = "form-check-label";
      label.setAttribute("for", checkbox.id);
      label.textContent = "Показывать блок";
      switchWrap.appendChild(checkbox);
      switchWrap.appendChild(label);

      head.appendChild(title);
      head.appendChild(switchWrap);
      body.appendChild(head);

      const rows = document.createElement("div");
      rows.className = "row g-3";
      meta.rows.forEach((row) => renderRow(rows, block, row));
      body.appendChild(rows);

      card.appendChild(body);
      container.appendChild(card);
    });
  }

  function readEditor() {
    const blocks = {};
    SCHEMA.forEach((meta) => {
      const card = document.querySelector('[data-block="' + meta.key + '"]');
      if (!card) return;
      const visible = card.querySelector(".block-visible");
      const block = { visible: !visible || visible.checked, ru: {}, en: {} };

      meta.rows.forEach((row) => {
        ["ru", "en"].forEach((lang) => {
          if (row.kind === "panel") {
            const title = card.querySelector(
              '[data-field="panel-title:' + row.key + ":" + lang + '"]'
            );
            const items = card.querySelector(
              '[data-field="panel-items:' + row.key + ":" + lang + '"]'
            );
            block[lang].panel = {
              title: title ? title.value : "",
              items: items ? parsePairs(items.value, "label", "value") : [],
            };
            return;
          }

          const input = card.querySelector(
            '[data-field="' + row.kind + ":" + row.key + ":" + lang + '"]'
          );
          if (!input) return;
          if (row.kind === "field" || row.kind === "text") {
            block[lang][row.key] = input.value;
          } else if (row.kind === "paragraphs" || row.kind === "lines") {
            block[lang][row.key] = parseLines(input.value);
          } else if (row.kind === "pairs") {
            block[lang][row.key] = parsePairs(input.value, "title", "text");
          }
        });
      });

      blocks[meta.key] = block;
    });
    return blocks;
  }

  /* ---------- hero background ---------- */

  function renderBackground() {
    const holder = el("landingBgPreview");
    if (!holder) return;
    holder.textContent = "";

    const background = (state.content && state.content.hero_background) || { type: "none" };
    if (!background.url || background.type === "none") {
      const empty = document.createElement("span");
      empty.className = "text-secondary small";
      empty.textContent = "Фон не задан";
      holder.appendChild(empty);
      return;
    }

    let media;
    if (background.type === "video") {
      media = document.createElement("video");
      media.muted = true;
      media.loop = true;
      media.autoplay = true;
      media.playsInline = true;
      media.setAttribute("playsinline", "");
      if (background.poster_url) media.poster = background.poster_url;
    } else {
      media = document.createElement("img");
      media.alt = "Фон первого экрана";
    }
    media.src = background.url;
    holder.appendChild(media);

    const note = document.createElement("div");
    note.className = "landing-bg-url";
    note.textContent = background.url;
    holder.appendChild(note);
  }

  async function uploadBackground() {
    const input = el("landingBgFile");
    const file = input && input.files && input.files[0];
    if (!file) {
      showAlert("landingAlert", "Выберите изображение или видео.");
      return;
    }

    const button = el("landingBgUpload");
    const spinner = el("landingBgSpinner");
    showAlert("landingAlert", "");
    button.disabled = true;
    if (spinner) spinner.hidden = false;

    try {
      const form = new FormData();
      form.append("file", file);
      const data = await api("/admin/site/landing/background", { method: "POST", body: form });
      state.content.hero_background = data.hero_background;
      renderBackground();
      input.value = "";
      showAlert("landingAlert", "Фон обновлён.", "success");
    } catch (error) {
      showAlert("landingAlert", errorText(error));
    } finally {
      button.disabled = false;
      if (spinner) spinner.hidden = true;
    }
  }

  async function removeBackground() {
    const button = el("landingBgRemove");
    button.disabled = true;
    try {
      await api("/admin/site/landing/background", { method: "DELETE" });
      state.content.hero_background = { type: "none", url: null, poster_url: null };
      renderBackground();
      showAlert("landingAlert", "Фон удалён.", "success");
    } catch (error) {
      showAlert("landingAlert", errorText(error));
    } finally {
      button.disabled = false;
    }
  }

  /* ---------- landing load/save ---------- */

  async function loadLanding() {
    state.loaded = true;
    showAlert("landingAlert", "");
    const container = el("landingBlocks");
    if (container) {
      container.innerHTML =
        '<div class="text-secondary small"><span class="spinner-border spinner-border-sm me-2"></span>Загрузка…</div>';
    }

    try {
      const data = await api("/admin/site/landing");
      state.content = {
        blocks: mergeSaved(baseBlocks(), data.blocks),
        hero_background: data.hero_background || { type: "none", url: null, poster_url: null },
      };
      renderEditor();
      renderBackground();
    } catch (error) {
      state.loaded = false;
      showAlert("landingAlert", errorText(error));
      if (container) container.textContent = "";
    }
  }

  async function saveLanding() {
    if (!state.content) return;
    const button = el("landingSave");
    const spinner = el("landingSaveSpinner");
    button.disabled = true;
    if (spinner) spinner.hidden = false;
    showAlert("landingAlert", "");

    try {
      const blocks = readEditor();
      const data = await api("/admin/site/landing", {
        method: "PUT",
        body: { blocks, hero_background: state.content.hero_background },
      });
      state.content = {
        blocks: mergeSaved(baseBlocks(), data.blocks),
        hero_background: data.hero_background || { type: "none", url: null, poster_url: null },
      };
      renderEditor();
      renderBackground();
      showAlert("landingAlert", "Сохранено. Изменения уже на сайте.", "success");
    } catch (error) {
      showAlert("landingAlert", errorText(error));
    } finally {
      button.disabled = false;
      if (spinner) spinner.hidden = true;
    }
  }

  /* ---------- game builds ---------- */

  function renderBuilds() {
    const body = el("buildsBody");
    const empty = el("buildsEmpty");
    if (!body) return;
    body.textContent = "";
    // Group by platform (Windows, macOS) and inside it by artifact (installer,
    // portable), newest first: those are the builds the admin compares.
    const order = (items, value) => {
      const index = items.findIndex((item) => item.value === value);
      return index === -1 ? items.length : index;
    };
    const items = (state.builds || []).slice().sort((a, b) => {
      const byPlatform =
        order(BUILD_PLATFORMS, a.platform) - order(BUILD_PLATFORMS, b.platform);
      if (byPlatform !== 0) return byPlatform;
      const byArtifact =
        order(BUILD_ARTIFACTS, buildArtifact(a)) -
        order(BUILD_ARTIFACTS, buildArtifact(b));
      if (byArtifact !== 0) return byArtifact;
      return new Date(b.uploaded_at || 0) - new Date(a.uploaded_at || 0);
    });
    empty.hidden = items.length > 0;

    renderBuildsSummary(items);

    items.forEach((build) => {
      const row = document.createElement("tr");
      const info = platformInfo(build.platform);
      const artifact = artifactInfo(buildArtifact(build));

      // PLATFORM: the badge tells Windows and macOS apart at a glance.
      const platform = document.createElement("td");
      const badge = document.createElement("span");
      badge.className = "badge " + info.badge;
      badge.textContent = info.short;
      platform.appendChild(badge);
      row.appendChild(platform);

      // ARTIFACT: installer for the site button, portable for auto-updates.
      const artifactCell = document.createElement("td");
      const artifactBadge = document.createElement("span");
      artifactBadge.className = "badge " + artifact.badge;
      artifactBadge.textContent = artifact.short;
      artifactBadge.title = artifact.label;
      artifactCell.appendChild(artifactBadge);
      row.appendChild(artifactCell);

      const version = document.createElement("td");
      version.textContent = build.version || "—";
      row.appendChild(version);

      const filename = document.createElement("td");
      filename.className = "build-filename";
      filename.textContent = build.filename || "—";
      row.appendChild(filename);

      const notes = document.createElement("td");
      notes.className = "text-secondary";
      const notesText = String(build.release_notes || "").trim();
      if (!notesText) {
        notes.textContent = "—";
      } else {
        const firstLine = (notesText.split(/\r?\n/)[0] || "").trim() || notesText;
        notes.textContent =
          firstLine.length > 80 ? firstLine.slice(0, 80) + "…" : firstLine;
        notes.title = notesText;
      }
      row.appendChild(notes);

      const size = document.createElement("td");
      size.textContent = formatBytes(build.size_bytes);
      row.appendChild(size);

      const uploaded = document.createElement("td");
      uploaded.className = "d-none d-lg-table-cell text-secondary";
      uploaded.textContent = formatDate(build.uploaded_at);
      row.appendChild(uploaded);

      const status = document.createElement("td");
      status.innerHTML = build.is_active
        ? '<span class="badge badge-complete">Активна</span>'
        : '<span class="badge badge-partial">Не активна</span>';
      row.appendChild(status);

      const actions = document.createElement("td");
      actions.className = "text-end";
      actions.innerHTML =
        '<div class="d-inline-flex gap-2">' +
        '<button class="btn btn-sm btn-outline-light" data-build-notes="' +
        escapeHtml(build.id) +
        '">Описание</button>' +
        (build.is_active
          ? ""
          : '<button class="btn btn-sm btn-outline-light" data-build-activate="' +
            escapeHtml(build.id) +
            '">Активировать</button>') +
        '<button class="btn btn-sm btn-outline-danger" data-build-delete="' +
        escapeHtml(build.id) +
        '">Удалить</button>' +
        "</div>";
      row.appendChild(actions);

      body.appendChild(row);
    });
  }

  // "Windows: Установщик v1.2.0 · Portable v1.2.1 | macOS: Установщик —, ..." -
  // the admin sees which build is published for every platform and artifact
  // without scanning the table. Installer and portable are reported separately
  // because they are activated independently.
  function renderBuildsSummary(items) {
    const target = el("buildsActiveSummary");
    if (!target) return;
    // With an active filter the response only carries the matching rows, so
    // only the visible platforms/artifacts are summarised.
    const visiblePlatforms = BUILD_PLATFORMS.filter(
      (info) => !state.buildsPlatform || info.value === state.buildsPlatform
    );
    const visibleArtifacts = BUILD_ARTIFACTS.filter(
      (info) => !state.buildsArtifact || info.value === state.buildsArtifact
    );
    const parts = visiblePlatforms.map((platform) => {
      const perArtifact = visibleArtifacts.map((artifact) => {
        const active = items.find(
          (build) =>
            build.platform === platform.value &&
            buildArtifact(build) === artifact.value &&
            build.is_active
        );
        const value = active ? "v" + (active.version || "—") : "нет сборки";
        return artifact.short + ": " + value;
      });
      return platform.short + " — " + perArtifact.join(", ");
    });
    target.textContent = parts.join(" | ");
  }

  async function loadBuilds() {
    state.buildsLoaded = true;
    showAlert("buildAlert", "");
    try {
      const params = new URLSearchParams();
      if (state.buildsPlatform) params.set("platform", state.buildsPlatform);
      if (state.buildsArtifact) params.set("artifact", state.buildsArtifact);
      const query = params.toString() ? "?" + params.toString() : "";
      const data = await api("/admin/site/game-builds" + query);
      state.builds = (data && data.items) || [];
      renderBuilds();
    } catch (error) {
      showAlert("buildAlert", errorText(error));
    }
  }

  async function uploadBuild(event) {
    event.preventDefault();
    const input = el("buildFile");
    const file = input && input.files && input.files[0];
    if (!file) {
      showAlert("buildAlert", "Выберите файл сборки.");
      return;
    }

    const platform = platformInfo(el("buildPlatform").value);
    const artifact = artifactInfo(el("buildArtifact").value);

    const button = el("buildUpload");
    const spinner = el("buildSpinner");
    button.disabled = true;
    if (spinner) spinner.hidden = false;
    showAlert("buildAlert", "");

    try {
      const form = new FormData();
      form.append("file", file);
      form.append("platform", platform.value);
      form.append("artifact", artifact.value);
      form.append("version", el("buildVersion").value.trim());
      form.append("release_notes", el("buildReleaseNotes").value.trim());
      await api("/admin/site/game-builds", { method: "POST", body: form });
      el("buildForm").reset();
      syncBuildHints();
      await loadBuilds();
      showAlert(
        "buildAlert",
        "Сборка «" + artifact.short + "», " + platform.label +
          " загружена. Активируйте её, чтобы опубликовать.",
        "success"
      );
    } catch (error) {
      showAlert("buildAlert", errorText(error));
    } finally {
      button.disabled = false;
      if (spinner) spinner.hidden = true;
    }
  }

  async function activateBuild(id) {
    try {
      await api("/admin/site/game-builds/" + encodeURIComponent(id) + "/activate", {
        method: "POST",
      });
      await loadBuilds();
      showAlert(
        "buildAlert",
        "Сборка опубликована для своей платформы и артефакта.",
        "success"
      );
    } catch (error) {
      showAlert("buildAlert", errorText(error));
    }
  }

  async function deleteBuild(id) {
    if (!window.confirm("Удалить сборку? Файл будет удалён безвозвратно.")) return;
    try {
      await api("/admin/site/game-builds/" + encodeURIComponent(id), { method: "DELETE" });
      await loadBuilds();
      showAlert("buildAlert", "Сборка удалена.", "success");
    } catch (error) {
      showAlert("buildAlert", errorText(error));
    }
  }

  // Keep the file picker and the hint in step with the selected platform and
  // artifact, so a .dmg cannot be chosen for the Windows slot and a .exe cannot
  // be chosen as a portable ZIP (the server rejects both with 415).
  function syncBuildHints() {
    const platformSelect = el("buildPlatform");
    if (!platformSelect) return;
    const platform = platformInfo(platformSelect.value);
    const artifactSelect = el("buildArtifact");
    const artifact = artifactInfo(
      artifactSelect ? artifactSelect.value : "installer"
    );
    const input = el("buildFile");
    if (input) {
      input.setAttribute("accept", artifact.accept[platform.value]);
      input.setAttribute("data-platform", platform.value);
      input.setAttribute("data-artifact", artifact.value);
    }
    const hint = el("buildFileHint");
    if (hint) {
      hint.textContent =
        platform.label + " — " + artifact.label + ": " +
        artifact.extensions[platform.value] + ". До 500 МБ. " +
        "Новая сборка не публикуется автоматически.";
    }
  }

  function openBuildNotes(id) {
    const build = (state.builds || []).find((item) => String(item.id) === String(id));
    if (!build) return;
    const modalEl = el("buildNotesModal");
    if (!modalEl || typeof window.bootstrap === "undefined") return;

    state.buildNotesId = build.id;
    const text = el("buildNotesText");
    if (text) text.value = build.release_notes || "";
    showAlert("buildNotesAlert", "");
    if (!state.buildNotesModal) {
      state.buildNotesModal = window.bootstrap.Modal.getOrCreateInstance(modalEl);
    }
    state.buildNotesModal.show();
  }

  async function saveBuildNotes() {
    if (state.buildNotesId === null || state.buildNotesId === undefined) return;
    const button = el("buildNotesSave");
    const spinner = el("buildNotesSpinner");
    if (button) button.disabled = true;
    if (spinner) spinner.hidden = false;
    showAlert("buildNotesAlert", "");

    try {
      await api("/admin/site/game-builds/" + encodeURIComponent(state.buildNotesId), {
        method: "PATCH",
        body: { release_notes: el("buildNotesText").value.trim() },
      });
      if (state.buildNotesModal) state.buildNotesModal.hide();
      await loadBuilds();
      showAlert("buildAlert", "Описание сборки сохранено.", "success");
    } catch (error) {
      showAlert("buildNotesAlert", errorText(error));
    } finally {
      if (button) button.disabled = false;
      if (spinner) spinner.hidden = true;
    }
  }

  /* ---------- contacts ---------- */

  const SOCIAL_INPUTS = [
    { key: "youtube", id: "contactsYoutube" },
    { key: "instagram", id: "contactsInstagram" },
    { key: "twitter", id: "contactsTwitter" },
    { key: "vk", id: "contactsVk" },
  ];

  function setInput(id, value) {
    const input = el(id);
    if (input) input.value = typeof value === "string" ? value : "";
  }

  function fillContacts(data) {
    const values = data || {};
    setInput("contactsNameRu", values.name_ru);
    setInput("contactsNameEn", values.name_en);
    setInput("contactsEmail", values.email);
    setInput("contactsTelegram", values.telegram);
    const socials = values.socials || {};
    SOCIAL_INPUTS.forEach((network) => setInput(network.id, socials[network.key]));
  }

  function readContacts() {
    const inputValue = (id) => {
      const input = el(id);
      return input ? input.value.trim() : "";
    };
    const socials = {};
    SOCIAL_INPUTS.forEach((network) => {
      socials[network.key] = inputValue(network.id);
    });
    return {
      name_ru: inputValue("contactsNameRu"),
      name_en: inputValue("contactsNameEn"),
      email: inputValue("contactsEmail"),
      telegram: inputValue("contactsTelegram"),
      socials,
    };
  }

  async function loadContacts() {
    state.contactsLoaded = true;
    showAlert("contactsAlert", "");
    try {
      const data = await api("/admin/site/contacts");
      state.contacts = data;
      fillContacts(data);
    } catch (error) {
      state.contactsLoaded = false;
      showAlert("contactsAlert", errorText(error));
    }
  }

  async function saveContacts() {
    const button = el("contactsSave");
    const spinner = el("contactsSaveSpinner");
    if (button) button.disabled = true;
    if (spinner) spinner.hidden = false;
    showAlert("contactsAlert", "");

    try {
      const data = await api("/admin/site/contacts", {
        method: "PUT",
        body: readContacts(),
      });
      state.contacts = data;
      fillContacts(data);
      showAlert("contactsAlert", "Контакты сохранены.", "success");
    } catch (error) {
      showAlert("contactsAlert", errorText(error));
    } finally {
      if (button) button.disabled = false;
      if (spinner) spinner.hidden = true;
    }
  }

  /* ---------- navigation ---------- */

  function showSection(name) {
    state.section = name;
    document.querySelectorAll("[data-admin-panel]").forEach((panel) => {
      panel.hidden = panel.getAttribute("data-admin-panel") !== name;
    });
    document.querySelectorAll("[data-admin-section]").forEach((button) => {
      const active = button.getAttribute("data-admin-section") === name;
      button.classList.toggle("active", active);
      button.setAttribute("aria-selected", active ? "true" : "false");
    });

    if (name === "landing") {
      if (!state.loaded) loadLanding();
      // Contacts live in the same panel/card now; load them with the landing
      // section instead of a separate tab.
      if (!state.contactsLoaded) loadContacts();
    }
    if (name === "campaigns") {
      const campaigns = window.FFAdminCampaigns;
      if (campaigns) campaigns.show();
    }
    if (name === "builds" && !state.buildsLoaded) loadBuilds();
    if (name === "artwork") {
      // Lazy first load; the module keeps its own state.
      const artwork = window.FFAdminArtwork;
      if (artwork) artwork.show();
    }
    if (name === "support") {
      const support = window.FFAdminSupport;
      if (support) support.show();
    }
    if (name === "daily") {
      const daily = window.FFAdminDaily;
      if (daily) daily.show();
    }
    if (name === "ui-levels" || name === "stage-levels") {
      const gameplay = window.FFAdminGameplay;
      if (gameplay) gameplay.show();
    }
    if (name === "ui-sounds") {
      const uiSounds = window.FFAdminUiSounds;
      if (uiSounds) uiSounds.show();
    }
    if (name === "guitar-skills") {
      const guitarSkills = window.FFAdminGuitarSkills;
      if (guitarSkills) guitarSkills.show();
    }
    if (name === "statistics") {
      // Lazy first load, refresh on every later visit. The statistics module
      // owns its own state and reuses the shared admin token and error handling.
      const statistics = window.FFAdminStatistics;
      if (statistics) statistics.show();
    }
  }

  function refreshCurrent() {
    if (state.section === "landing") {
      loadLanding();
      loadContacts();
    } else if (state.section === "campaigns") {
      const campaigns = window.FFAdminCampaigns;
      if (campaigns) campaigns.refresh();
    } else if (state.section === "builds") loadBuilds();
    else if (state.section === "artwork") {
      const artwork = window.FFAdminArtwork;
      if (artwork) artwork.refresh();
    }
    else if (state.section === "support") {
      const support = window.FFAdminSupport;
      if (support) support.refresh();
    } else if (state.section === "daily") {
      const daily = window.FFAdminDaily;
      if (daily) daily.refresh();
    } else if (state.section === "statistics") {
      const statistics = window.FFAdminStatistics;
      if (statistics) statistics.refresh();
    } else if (state.section === "ui-levels" || state.section === "stage-levels") {
      const gameplay = window.FFAdminGameplay;
      if (gameplay) gameplay.refresh();
    } else if (state.section === "ui-sounds") {
      const uiSounds = window.FFAdminUiSounds;
      if (uiSounds) uiSounds.refresh();
    } else if (state.section === "guitar-skills") {
      const guitarSkills = window.FFAdminGuitarSkills;
      if (guitarSkills) guitarSkills.refresh();
    }
  }

  function init() {
    document.querySelectorAll("[data-admin-section]").forEach((button) => {
      button.addEventListener("click", () => {
        showSection(button.getAttribute("data-admin-section"));
      });
    });

    el("landingReload")?.addEventListener("click", loadLanding);
    el("landingSave")?.addEventListener("click", saveLanding);
    el("contactsReload")?.addEventListener("click", loadContacts);
    el("contactsSave")?.addEventListener("click", saveContacts);
    el("landingBgUpload")?.addEventListener("click", uploadBackground);
    el("landingBgRemove")?.addEventListener("click", removeBackground);
    el("buildsReload")?.addEventListener("click", loadBuilds);
    el("buildForm")?.addEventListener("submit", uploadBuild);
    el("buildPlatform")?.addEventListener("change", syncBuildHints);
    el("buildArtifact")?.addEventListener("change", syncBuildHints);
    el("buildsPlatformFilter")?.addEventListener("change", (event) => {
      state.buildsPlatform = event.target.value;
      loadBuilds();
    });
    el("buildsArtifactFilter")?.addEventListener("change", (event) => {
      state.buildsArtifact = event.target.value;
      loadBuilds();
    });
    el("buildNotesSave")?.addEventListener("click", saveBuildNotes);
    el("buildsBody")?.addEventListener("click", (event) => {
      const notes = event.target.closest("[data-build-notes]");
      if (notes) {
        openBuildNotes(notes.getAttribute("data-build-notes"));
        return;
      }
      const activate = event.target.closest("[data-build-activate]");
      if (activate) {
        activateBuild(activate.getAttribute("data-build-activate"));
        return;
      }
      const remove = event.target.closest("[data-build-delete]");
      if (remove) deleteBuild(remove.getAttribute("data-build-delete"));
    });
    el("refreshButtonTop")?.addEventListener("click", refreshCurrent);
    syncBuildHints();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
