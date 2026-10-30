/* fretfight admin — support page CMS (/support).
 *
 * Draft editing, publication, history, support methods and the HTML/CSS tab.
 * The public page and the preview share `/app/support-render.js`, so the
 * preview shows the real markup and the same client-side sanitiser.
 */
(() => {
  "use strict";

  const API_BASE = "/api/v1";
  const TOKEN_KEY = "guitarbattle.admin.token";
  const RENDER = window.FFSupportRender;

  const ICON_OPTIONS = [
    ["game", "Игра / геймпад"],
    ["catalog", "Каталог / список"],
    ["versus", "Дуэль / противостояние"],
    ["infra", "Инфраструктура / сервер"],
    ["test", "Тест / галочка"],
    ["bug", "Ошибка / жук"],
    ["idea", "Идея / лампочка"],
    ["song", "Песня / нота"],
    ["share", "Поделиться"],
    ["note", "Звезда"],
  ];

  const SECTION_META = [
    {
      key: "hero",
      title: "Первый экран",
      methodToggle: true,
      fields: [
        { kind: "text", key: "kicker", label: "Надзаголовок" },
        { kind: "text", key: "title", label: "Заголовок" },
        { kind: "text", key: "subtitle", label: "Подзаголовок" },
        { kind: "rich", key: "text", label: "Основной текст" },
        { kind: "rich", key: "extra_text", label: "Дополнительный текст" },
      ],
    },
    {
      key: "duels",
      title: "Гитарные дуэли (анонс)",
      fields: [
        { kind: "text", key: "badge", label: "Бейдж" },
        { kind: "text", key: "title", label: "Заголовок" },
        { kind: "list", key: "paragraphs", label: "Абзацы — по одному на строку" },
        { kind: "rich", key: "extra_text", label: "Дополнительный текст" },
        { kind: "text", key: "closing", label: "Заключительная фраза" },
        { kind: "button", key: "button", label: "Кнопка" },
      ],
    },
    {
      key: "usage",
      title: "На что пойдёт поддержка",
      fields: [
        { kind: "text", key: "title", label: "Заголовок" },
        { kind: "rich", key: "text", label: "Описание" },
      ],
      cards: "cards",
    },
    {
      key: "help",
      title: "Как ещё можно помочь",
      fields: [
        { kind: "text", key: "title", label: "Заголовок" },
        { kind: "rich", key: "text", label: "Описание" },
      ],
      cards: "cards",
    },
    {
      key: "why",
      title: "Почему Fretfight важен",
      fields: [
        { kind: "text", key: "title", label: "Заголовок" },
        { kind: "list", key: "paragraphs", label: "Абзацы — по одному на строку" },
        { kind: "list", key: "ideas", label: "Выделенные идеи — по одной на строку" },
      ],
    },
    {
      key: "faq",
      title: "Частые вопросы",
      fields: [{ kind: "text", key: "title", label: "Заголовок" }],
      faq: true,
    },
    {
      key: "cta",
      title: "Финальный призыв",
      methodToggle: true,
      fields: [
        { kind: "text", key: "title", label: "Заголовок" },
        { kind: "rich", key: "text", label: "Описание" },
        { kind: "text", key: "slogan", label: "Слоган" },
      ],
    },
  ];

  const state = {
    loaded: false,
    loading: false,
    content: null,
    methods: [],
    newMethod: null,
    revision: 0,
    meta: {},
    dirty: false,
    codeDirty: false,
    tab: "sections",
    versions: [],
    versionsLoaded: false,
    cssText: null,
    stats: null,
  };

  const el = (id) => document.getElementById(id);

  /* ---------- generic helpers ---------- */

  function node(tag, className, text) {
    const element = document.createElement(tag);
    if (className) element.className = className;
    if (text !== undefined && text !== null) element.textContent = String(text);
    return element;
  }

  function clone(value) {
    return JSON.parse(JSON.stringify(value));
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

  function showAlert(message, variant) {
    const box = el("supportAlert");
    if (!box) return;
    box.className = "alert alert-" + (variant || "danger");
    box.textContent = message || "";
    box.hidden = !message;
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

  async function api(path, options = {}) {
    const token = sessionStorage.getItem(TOKEN_KEY) || "";
    const headers = {};
    if (token) headers.Authorization = "Bearer " + token;
    const init = {
      method: options.method || "GET",
      headers,
      credentials: "omit",
    };
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
        data = text;
      }
    }
    if (!response.ok) {
      const error = new Error(
        detailOf(data) || "Ошибка запроса (" + response.status + ")"
      );
      error.status = response.status;
      error.data = data;
      throw error;
    }
    return data;
  }

  function errorText(error) {
    if (error && error.status === 401) {
      return "Сессия истекла. Войдите в админку заново.";
    }
    if (error && error.status === 403) {
      return "Недостаточно прав: раздел доступен только администратору.";
    }
    return (error && error.message) || "Не удалось выполнить запрос.";
  }

  function setBusy(button, spinner, busy) {
    if (button) button.disabled = busy;
    if (spinner) spinner.hidden = !busy;
  }

  function hasChanges() {
    return state.dirty || state.codeDirty;
  }

  function markDirty() {
    state.dirty = true;
    setStatus();
  }

  function markCodeDirty() {
    state.codeDirty = true;
    setStatus();
  }

  function setStatus() {
    const box = el("supportStatus");
    if (!box) return;
    box.textContent = "";

    const draft = node("div", "support-status__line");
    draft.appendChild(node("strong", "", "Черновик"));
    draft.appendChild(
      node(
        "span",
        "",
        " · ревизия " + state.revision + " · изменён " +
          formatDate(state.meta.draft_updated_at) +
          (state.meta.draft_updated_by ? " (" + state.meta.draft_updated_by + ")" : "")
      )
    );
    box.appendChild(draft);

    const published = node("div", "support-status__line");
    published.appendChild(node("strong", "", "Опубликовано"));
    published.appendChild(
      node(
        "span",
        "",
        " · ревизия " + (state.meta.published_revision || 0) + " · " +
          formatDate(state.meta.published_at) +
          (state.meta.published_by ? " (" + state.meta.published_by + ")" : "")
      )
    );
    box.appendChild(published);

    const badges = node("div", "support-status__badges");
    if (hasChanges()) {
      badges.appendChild(node("span", "badge badge-partial", "Есть несохранённые изменения"));
    }
    if (state.meta.has_unpublished_changes) {
      badges.appendChild(node("span", "badge badge-partial", "Черновик новее публикации"));
    } else if (!hasChanges()) {
      badges.appendChild(node("span", "badge badge-complete", "Опубликовано"));
    }
    box.appendChild(badges);

    if (state.stats) {
      box.appendChild(
        node(
          "div",
          "support-status__stats",
          "За " + state.stats.days + " дней: просмотры " + state.stats.views +
            " · клики " + state.stats.clicks +
            " · открытия FAQ " + state.stats.faq_opens
        )
      );
    }
  }

  /* ---------- field binding ---------- */

  function bindText(input, holder, key, lang) {
    const source = holder && holder[key];
    input.value = source && typeof source[lang] === "string" ? source[lang] : "";
    input.addEventListener("input", () => {
      if (!holder[key] || typeof holder[key] !== "object") {
        holder[key] = { visible: true, ru: "", en: "" };
      }
      holder[key][lang] = input.value;
      markDirty();
    });
  }

  function bindLocalized(holder, key, lang, input) {
    const source = holder && holder[key];
    input.value = source && typeof source[lang] === "string" ? source[lang] : "";
    input.addEventListener("input", () => {
      if (!holder[key] || typeof holder[key] !== "object") holder[key] = { ru: "", en: "" };
      holder[key][lang] = input.value;
      markDirty();
    });
  }

  function bindRaw(input, holder, key, silent) {
    input.value = holder && typeof holder[key] === "string" ? holder[key] : "";
    input.addEventListener("input", () => {
      holder[key] = input.value;
      if (!silent) markDirty();
    });
  }

  function bindList(input, holder, key, lang) {
    const source = holder && holder[key];
    const items = source && Array.isArray(source[lang]) ? source[lang] : [];
    input.value = items.join("\n");
    input.addEventListener("input", () => {
      if (!holder[key] || typeof holder[key] !== "object") {
        holder[key] = { visible: true, ru: [], en: [] };
      }
      holder[key][lang] = input.value.split("\n");
      markDirty();
    });
  }

  function bindSwitch(checkbox, holder, key, silent) {
    checkbox.checked = holder[key] !== false;
    checkbox.addEventListener("change", () => {
      holder[key] = checkbox.checked;
      if (!silent) markDirty();
    });
  }

  function switchControl(id, labelText, checked, onChange) {
    const wrapper = node("div", "form-check form-switch mb-0");
    const input = document.createElement("input");
    input.type = "checkbox";
    input.className = "form-check-input";
    input.id = id;
    input.checked = checked;
    input.addEventListener("change", onChange);
    const label = node("label", "form-check-label", labelText);
    label.setAttribute("for", id);
    wrapper.appendChild(input);
    wrapper.appendChild(label);
    return { wrapper, input };
  }

  function langColumn(lang, control) {
    const column = node("div", "col-md-6");
    column.appendChild(node("div", "landing-lang-label", lang.toUpperCase()));
    column.appendChild(control);
    return column;
  }

  function editorToolbar(textarea) {
    const bar = node("div", "editor-toolbar");
    const actions = [
      ["B", "Жирный", () => surround(textarea, "**", "**", "жирный")],
      ["I", "Курсив", () => surround(textarea, "*", "*", "курсив")],
      ["Ссылка", "Вставить ссылку", () => surround(textarea, "[", "](https://example.com)", "текст ссылки")],
      ["Список", "Маркированный список", () => toggleList(textarea)],
    ];
    actions.forEach(([label, title, handler]) => {
      const button = node("button", "editor-toolbar__button", label);
      button.type = "button";
      button.title = title;
      button.setAttribute("aria-label", title);
      button.addEventListener("click", handler);
      bar.appendChild(button);
    });
    return bar;
  }

  function surround(textarea, before, after, placeholder) {
    const start = textarea.selectionStart;
    const end = textarea.selectionEnd;
    const value = textarea.value;
    const selected = value.slice(start, end) || placeholder;
    textarea.value = value.slice(0, start) + before + selected + after + value.slice(end);
    textarea.focus();
    textarea.selectionStart = start + before.length;
    textarea.selectionEnd = start + before.length + selected.length;
    textarea.dispatchEvent(new Event("input"));
  }

  function toggleList(textarea) {
    const value = textarea.value;
    const start = textarea.selectionStart;
    const end = textarea.selectionEnd;
    const lineStart = value.lastIndexOf("\n", start - 1) + 1;
    let lineEnd = value.indexOf("\n", end);
    if (lineEnd === -1) lineEnd = value.length;
    const block = value.slice(lineStart, lineEnd);
    const lines = block.split("\n");
    const allMarked = lines.every((line) => !line.trim() || line.startsWith("- "));
    const updated = lines
      .map((line) => {
        if (!line.trim()) return line;
        return allMarked ? line.replace(/^- /, "") : "- " + line;
      })
      .join("\n");
    textarea.value = value.slice(0, lineStart) + updated + value.slice(lineEnd);
    textarea.focus();
    textarea.dispatchEvent(new Event("input"));
  }

  /* ---------- section editor ---------- */

  function renderSections() {
    const container = el("supportSections");
    if (!container || !state.content) return;
    container.textContent = "";
    const order = Array.isArray(state.content.order) ? state.content.order : [];

    order.forEach((key, index) => {
      const meta = SECTION_META.find((item) => item.key === key);
      if (!meta) return;
      container.appendChild(renderSectionCard(meta, index, order.length));
    });
  }

  function renderSectionCard(meta, index, total) {
    const section = state.content.sections[meta.key];
    const card = node("section", "card content-card block-editor");
    card.dataset.block = meta.key;
    const body = node("div", "card-body p-4");

    const head = node("div", "support-section-head");
    head.appendChild(node("h2", "h5 mb-0", index + 1 + ". " + meta.title));

    const tools = node("div", "support-section-tools");
    tools.appendChild(moveButton("Вверх", index === 0, () => moveSection(meta.key, -1)));
    tools.appendChild(moveButton("Вниз", index === total - 1, () => moveSection(meta.key, 1)));

    const visible = switchControl(
      "support-visible-" + meta.key,
      "Показывать секцию",
      section.visible !== false,
      () => {}
    );
    bindSwitch(visible.input, section, "visible");
    tools.appendChild(visible.wrapper);
    head.appendChild(tools);
    body.appendChild(head);

    const rows = node("div", "row g-3");
    meta.fields.forEach((field) => rows.appendChild(renderFieldRow(section, field, meta.key)));
    body.appendChild(rows);

    if (meta.methodToggle) {
      const toggle = switchControl(
        "support-methods-" + meta.key,
        "Показывать кнопки поддержки в этой секции",
        section.methods_visible !== false,
        () => {}
      );
      bindSwitch(toggle.input, section, "methods_visible");
      const wrap = node("div", "mt-3");
      wrap.appendChild(toggle.wrapper);
      body.appendChild(wrap);
    }

    if (meta.cards) body.appendChild(renderCardsEditor(section, meta.key));
    if (meta.faq) body.appendChild(renderFaqEditor(section));

    card.appendChild(body);
    return card;
  }

  function moveButton(label, disabled, handler) {
    const button = node("button", "btn btn-sm btn-outline-light btn-icon", label === "Вверх" ? "↑" : "↓");
    button.type = "button";
    button.title = label;
    button.setAttribute("aria-label", label);
    button.disabled = disabled;
    button.addEventListener("click", handler);
    return button;
  }

  function renderFieldRow(section, field, sectionKey) {
    const row = node("div", "col-12 landing-row");
    const labelRow = node("div", "d-flex justify-content-between align-items-center gap-2");
    labelRow.appendChild(node("label", "form-label mb-1", field.label));
    row.appendChild(labelRow);

    if (field.kind === "button") {
      const button = section.button || (section.button = { visible: true, label: { ru: "", en: "" }, url: "" });
      const visible = switchControl(
        "support-visible-" + sectionKey + "-button",
        "Показывать кнопку",
        button.visible !== false,
        () => {}
      );
      bindSwitch(visible.input, button, "visible");
      labelRow.appendChild(visible.wrapper);

      const columns = node("div", "row g-2");
      ["ru", "en"].forEach((lang) => {
        const input = document.createElement("input");
        input.type = "text";
        input.className = "form-control";
        input.maxLength = 200;
        bindLocalized(button, "label", lang, input);
        columns.appendChild(langColumn(lang + " · текст кнопки", input));
      });
      row.appendChild(columns);

      const urlWrap = node("div", "mt-2");
      urlWrap.appendChild(node("div", "landing-lang-label", "URL (http(s) или анкор #support-methods)"));
      const url = document.createElement("input");
      url.type = "text";
      url.className = "form-control";
      url.maxLength = 2048;
      bindRaw(url, button, "url");
      urlWrap.appendChild(url);
      row.appendChild(urlWrap);
      return row;
    }

    const fieldValue =
      section[field.key] ||
      (section[field.key] =
        field.kind === "list"
          ? { visible: true, ru: [], en: [] }
          : { visible: true, ru: "", en: "" });
    const visible = switchControl(
      "support-visible-" + sectionKey + "-" + field.key,
      "Показывать",
      fieldValue.visible !== false,
      () => {}
    );
    bindSwitch(visible.input, fieldValue, "visible");
    labelRow.appendChild(visible.wrapper);

    const columns = node("div", "row g-2");
    ["ru", "en"].forEach((lang) => {
      let control;
      if (field.kind === "list") {
        control = document.createElement("textarea");
        control.rows = 4;
        control.spellcheck = true;
        bindList(control, section, field.key, lang);
      } else if (field.kind === "rich") {
        control = document.createElement("textarea");
        control.rows = 5;
        control.spellcheck = true;
        bindText(control, section, field.key, lang);
        const column = node("div", "col-md-6");
        column.appendChild(node("div", "landing-lang-label", lang.toUpperCase()));
        column.appendChild(editorToolbar(control));
        column.appendChild(control);
        columns.appendChild(column);
        return;
      } else {
        control = document.createElement("input");
        control.type = "text";
        control.maxLength = 5000;
        bindText(control, section, field.key, lang);
      }
      control.className = "form-control";
      if (field.kind !== "list") control.maxLength = field.kind === "rich" ? 5000 : 5000;
      columns.appendChild(langColumn(lang, control));
    });
    row.appendChild(columns);
    return row;
  }

  function renderCardsEditor(section, sectionKey) {
    const wrap = node("div", "col-12 support-cards-editor mt-3");
    const header = node("div", "d-flex flex-wrap justify-content-between align-items-center gap-2");
    header.appendChild(node("div", "form-label mb-0", "Карточки"));
    const visible = switchControl(
      "support-cards-visible-" + sectionKey,
      "Показывать блок карточек",
      section.cards.visible !== false,
      () => {}
    );
    bindSwitch(visible.input, section.cards, "visible");
    header.appendChild(visible.wrapper);
    wrap.appendChild(header);

    const list = node("div", "vstack gap-2 mt-2");
    section.cards.items.forEach((card, index) => {
      list.appendChild(renderCardItem(section.cards.items, card, index));
    });
    wrap.appendChild(list);

    const add = node("button", "btn btn-sm btn-outline-light mt-2", "+ Добавить карточку");
    add.type = "button";
    add.addEventListener("click", () => {
      section.cards.items.push(newCard());
      markDirty();
      renderSections();
    });
    wrap.appendChild(add);
    return wrap;
  }

  function newCard() {
    return {
      id: "card-" + Math.random().toString(36).slice(2, 10),
      visible: true,
      icon: "note",
      title: { ru: "", en: "" },
      text: { ru: "", en: "" },
      url: "",
      link_label: { ru: "", en: "" },
    };
  }

  function renderCardItem(items, card, index) {
    const box = node("div", "support-item-editor");
    const head = node("div", "support-item-editor__head");
    head.appendChild(node("div", "landing-lang-label", "Карточка " + (index + 1)));
    const tools = node("div", "d-flex align-items-center gap-2 flex-wrap");
    tools.appendChild(moveButton("Вверх", index === 0, () => moveItem(items, index, -1, renderSections)));
    tools.appendChild(moveButton("Вниз", index === items.length - 1, () => moveItem(items, index, 1, renderSections)));
    const visible = switchControl(
      "support-card-visible-" + card.id,
      "Показывать",
      card.visible !== false,
      () => {}
    );
    bindSwitch(visible.input, card, "visible");
    tools.appendChild(visible.wrapper);
    const remove = node("button", "btn btn-sm btn-outline-danger", "Удалить");
    remove.type = "button";
    remove.addEventListener("click", () => {
      if (!window.confirm("Удалить карточку?")) return;
      items.splice(index, 1);
      markDirty();
      renderSections();
    });
    tools.appendChild(remove);
    head.appendChild(tools);
    box.appendChild(head);

    const grid = node("div", "row g-2");
    const iconColumn = node("div", "col-12 col-md-4");
    iconColumn.appendChild(node("div", "landing-lang-label", "Иконка"));
    const select = document.createElement("select");
    select.className = "form-select";
    ICON_OPTIONS.forEach(([value, label]) => {
      const option = document.createElement("option");
      option.value = value;
      option.textContent = label;
      select.appendChild(option);
    });
    select.value = card.icon || "note";
    select.addEventListener("change", () => {
      card.icon = select.value;
      markDirty();
    });
    iconColumn.appendChild(select);
    grid.appendChild(iconColumn);

    ["ru", "en"].forEach((lang) => {
      const column = node("div", "col-12 col-md-4");
      column.appendChild(node("div", "landing-lang-label", "Заголовок " + lang.toUpperCase()));
      const input = document.createElement("input");
      input.type = "text";
      input.className = "form-control";
      input.maxLength = 200;
      bindLocalized(card, "title", lang, input);
      column.appendChild(input);
      grid.appendChild(column);
    });

    ["ru", "en"].forEach((lang) => {
      const column = node("div", "col-12 col-md-6");
      column.appendChild(node("div", "landing-lang-label", "Текст " + lang.toUpperCase()));
      const area = document.createElement("textarea");
      area.className = "form-control";
      area.rows = 3;
      bindLocalized(card, "text", lang, area);
      column.appendChild(area);
      grid.appendChild(column);
    });

    const urlColumn = node("div", "col-12 col-md-6");
    urlColumn.appendChild(node("div", "landing-lang-label", "Ссылка (необязательно)"));
    const url = document.createElement("input");
    url.type = "text";
    url.className = "form-control";
    url.maxLength = 2048;
    url.placeholder = "https://...";
    bindRaw(url, card, "url");
    urlColumn.appendChild(url);
    grid.appendChild(urlColumn);

    ["ru", "en"].forEach((lang) => {
      const column = node("div", "col-12 col-md-3");
      column.appendChild(node("div", "landing-lang-label", "Текст ссылки " + lang.toUpperCase()));
      const input = document.createElement("input");
      input.type = "text";
      input.className = "form-control";
      input.maxLength = 120;
      bindLocalized(card, "link_label", lang, input);
      column.appendChild(input);
      grid.appendChild(column);
    });

    box.appendChild(grid);
    return box;
  }

  function renderFaqEditor(section) {
    const wrap = node("div", "col-12 support-cards-editor mt-3");
    wrap.appendChild(node("div", "form-label", "Вопросы и ответы"));
    const list = node("div", "vstack gap-2");
    section.items.forEach((item, index) => {
      list.appendChild(renderFaqItem(section.items, item, index));
    });
    wrap.appendChild(list);

    const add = node("button", "btn btn-sm btn-outline-light mt-2", "+ Добавить вопрос");
    add.type = "button";
    add.addEventListener("click", () => {
      section.items.push({
        id: "faq-" + Math.random().toString(36).slice(2, 10),
        visible: true,
        question: { ru: "", en: "" },
        answer: { ru: "", en: "" },
      });
      markDirty();
      renderSections();
    });
    wrap.appendChild(add);
    return wrap;
  }

  function renderFaqItem(items, item, index) {
    const box = node("div", "support-item-editor");
    const head = node("div", "support-item-editor__head");
    head.appendChild(node("div", "landing-lang-label", "Вопрос " + (index + 1)));
    const tools = node("div", "d-flex align-items-center gap-2 flex-wrap");
    tools.appendChild(moveButton("Вверх", index === 0, () => moveItem(items, index, -1, renderSections)));
    tools.appendChild(moveButton("Вниз", index === items.length - 1, () => moveItem(items, index, 1, renderSections)));
    const visible = switchControl(
      "support-faq-visible-" + item.id,
      "Показывать",
      item.visible !== false,
      () => {}
    );
    bindSwitch(visible.input, item, "visible");
    tools.appendChild(visible.wrapper);
    const remove = node("button", "btn btn-sm btn-outline-danger", "Удалить");
    remove.type = "button";
    remove.addEventListener("click", () => {
      if (!window.confirm("Удалить вопрос?")) return;
      items.splice(index, 1);
      markDirty();
      renderSections();
    });
    tools.appendChild(remove);
    head.appendChild(tools);
    box.appendChild(head);

    const grid = node("div", "row g-2");
    ["ru", "en"].forEach((lang) => {
      const column = node("div", "col-12 col-md-6");
      column.appendChild(node("div", "landing-lang-label", "Вопрос " + lang.toUpperCase()));
      const input = document.createElement("input");
      input.type = "text";
      input.className = "form-control";
      input.maxLength = 300;
      bindLocalized(item, "question", lang, input);
      column.appendChild(input);

      column.appendChild(node("div", "landing-lang-label mt-2", "Ответ " + lang.toUpperCase()));
      const area = document.createElement("textarea");
      area.className = "form-control";
      area.rows = 3;
      bindLocalized(item, "answer", lang, area);
      column.appendChild(area);
      grid.appendChild(column);
    });
    box.appendChild(grid);
    return box;
  }

  function moveSection(key, delta) {
    const order = state.content.order;
    const index = order.indexOf(key);
    const target = index + delta;
    if (index < 0 || target < 0 || target >= order.length) return;
    order.splice(index, 1);
    order.splice(target, 0, key);
    markDirty();
    renderSections();
  }

  function moveItem(items, index, delta, redraw) {
    const target = index + delta;
    if (target < 0 || target >= items.length) return;
    const [item] = items.splice(index, 1);
    items.splice(target, 0, item);
    markDirty();
    redraw();
  }

  /* ---------- methods ---------- */

  function renderMethods() {
    const container = el("supportMethodsList");
    if (!container) return;
    container.textContent = "";
    (state.methods || []).forEach((method, index) => {
      container.appendChild(renderMethodCard(method, index));
    });
    if (state.newMethod) {
      const card = renderMethodCard(state.newMethod, null);
      container.insertBefore(card, container.firstChild);
    }
    if (!state.methods.length && !state.newMethod) {
      container.appendChild(
        node("p", "text-secondary small mb-0", "Способов поддержки пока нет. Добавьте Boosty, DALink или другой сервис.")
      );
    }
  }

  function renderMethodCard(method, index) {
    const isNew = index === null;
    const card = node("div", "support-item-editor method-editor");

    const head = node("div", "support-item-editor__head");
    head.appendChild(
      node("div", "landing-lang-label", isNew ? "Новый способ" : (method.title || method.key || "Способ"))
    );
    const tools = node("div", "d-flex align-items-center gap-2 flex-wrap");
    if (!isNew) {
      tools.appendChild(
        moveButton("Вверх", index === 0, () => reorderMethod(index, -1))
      );
      tools.appendChild(
        moveButton("Вниз", index === state.methods.length - 1, () => reorderMethod(index, 1))
      );
      const active = switchControl(
        "support-method-active-" + method.id,
        "Активен",
        method.is_active !== false,
        () => {}
      );
      bindSwitch(active.input, method, "is_active", true);
      tools.appendChild(active.wrapper);
      const remove = node("button", "btn btn-sm btn-outline-danger", "Удалить");
      remove.type = "button";
      remove.addEventListener("click", () => deleteMethod(method));
      tools.appendChild(remove);
    }
    head.appendChild(tools);
    card.appendChild(head);

    const grid = node("div", "row g-2");
    const field = (label, key, options = {}) => {
      const column = node("div", options.column || "col-12 col-md-4");
      column.appendChild(node("div", "landing-lang-label", label));
      const input = document.createElement(options.multiline ? "textarea" : "input");
      if (!options.multiline) input.type = options.type || "text";
      if (options.multiline) input.rows = 2;
      input.className = "form-control";
      input.maxLength = options.maxLength || 2048;
      bindRaw(input, method, key, true);
      column.appendChild(input);
      grid.appendChild(column);
    };

    field("Ключ (латиница, цифры, - _)", "key", { maxLength: 40, column: "col-12 col-md-3" });
    field("Название", "title", { maxLength: 120, column: "col-12 col-md-3" });
    field("Текст кнопки", "button_label", { maxLength: 160, column: "col-12 col-md-6" });
    field("URL (только http/https)", "url", { maxLength: 2048, column: "col-12 col-md-8" });

    const styleColumn = node("div", "col-12 col-md-4");
    styleColumn.appendChild(node("div", "landing-lang-label", "Стиль кнопки"));
    const select = document.createElement("select");
    select.className = "form-select";
    [["primary", "Основная"], ["secondary", "Вторичная"], ["link", "Текстовая ссылка"]].forEach(
      ([value, label]) => {
        const option = document.createElement("option");
        option.value = value;
        option.textContent = label;
        select.appendChild(option);
      }
    );
    select.value = method.style || "primary";
    select.addEventListener("change", () => {
      method.style = select.value;
    });
    styleColumn.appendChild(select);
    grid.appendChild(styleColumn);

    field("Описание (необязательно)", "description", { maxLength: 500, multiline: true, column: "col-12" });

    const sectionsColumn = node("div", "col-12");
    sectionsColumn.appendChild(node("div", "landing-lang-label", "Показывать в секциях"));
    const sectionsWrap = node("div", "d-flex gap-3 flex-wrap");
    const current = Array.isArray(method.sections) ? method.sections.slice() : ["hero", "cta"];
    [["hero", "Первый экран"], ["cta", "Финальный призыв"]].forEach(([key, label]) => {
      const box = node("label", "form-check form-check-inline mb-0");
      const input = document.createElement("input");
      input.type = "checkbox";
      input.className = "form-check-input";
      input.checked = current.indexOf(key) !== -1;
      input.addEventListener("change", () => {
        const list = Array.isArray(method.sections) ? method.sections : [];
        const at = list.indexOf(key);
        if (input.checked && at === -1) list.push(key);
        if (!input.checked && at !== -1) list.splice(at, 1);
        method.sections = list;
      });
      box.appendChild(input);
      box.appendChild(node("span", "form-check-label", label));
      sectionsWrap.appendChild(box);
    });
    sectionsColumn.appendChild(sectionsWrap);
    grid.appendChild(sectionsColumn);

    const openColumn = node("div", "col-12");
    const open = switchControl(
      "support-method-newtab-" + (method.id || "new"),
      "Открывать в новой вкладке",
      method.open_in_new_tab !== false,
      () => {}
    );
    bindSwitch(open.input, method, "open_in_new_tab", true);
    openColumn.appendChild(open.wrapper);
    grid.appendChild(openColumn);

    card.appendChild(grid);

    const actions = node("div", "d-flex gap-2 mt-2");
    const save = node("button", "btn btn-sm btn-warning", isNew ? "Создать" : "Сохранить");
    save.type = "button";
    save.addEventListener("click", () => (isNew ? createMethod() : saveMethod(method)));
    actions.appendChild(save);
    if (isNew) {
      const cancel = node("button", "btn btn-sm btn-outline-secondary", "Отмена");
      cancel.type = "button";
      cancel.addEventListener("click", () => {
        state.newMethod = null;
        renderMethods();
      });
      actions.appendChild(cancel);
    }
    card.appendChild(actions);
    return card;
  }

  function methodPayload(method) {
    return {
      key: (method.key || "").trim(),
      title: method.title || "",
      button_label: method.button_label || "",
      url: (method.url || "").trim(),
      description: method.description || "",
      is_active: method.is_active !== false,
      open_in_new_tab: method.open_in_new_tab !== false,
      sort_order: typeof method.sort_order === "number" ? method.sort_order : 0,
      style: method.style || "primary",
      sections: Array.isArray(method.sections) ? method.sections : ["hero", "cta"],
    };
  }

  async function createMethod() {
    showAlert("");
    try {
      await api("/admin/support/methods", {
        method: "POST",
        body: methodPayload(state.newMethod),
      });
      state.newMethod = null;
      await loadMethods();
      showAlert("Способ поддержки создан.", "success");
    } catch (error) {
      showAlert(errorText(error));
    }
  }

  async function saveMethod(method) {
    showAlert("");
    try {
      await api("/admin/support/methods/" + method.id, {
        method: "PUT",
        body: methodPayload(method),
      });
      await loadMethods();
      showAlert("Способ поддержки сохранён.", "success");
    } catch (error) {
      showAlert(errorText(error));
    }
  }

  async function deleteMethod(method) {
    if (!window.confirm("Удалить способ поддержки «" + (method.title || method.key) + "»?")) return;
    try {
      await api("/admin/support/methods/" + method.id, { method: "DELETE" });
      await loadMethods();
      showAlert("Способ поддержки удалён.", "success");
    } catch (error) {
      showAlert(errorText(error));
    }
  }

  async function reorderMethod(index, delta) {
    const target = index + delta;
    if (target < 0 || target >= state.methods.length) return;
    const ids = state.methods.map((item) => item.id);
    const [moved] = ids.splice(index, 1);
    ids.splice(target, 0, moved);
    try {
      const data = await api("/admin/support/methods/order", {
        method: "PUT",
        body: { ids },
      });
      state.methods = data.items || [];
      renderMethods();
    } catch (error) {
      showAlert(errorText(error));
    }
  }

  async function loadMethods() {
    const data = await api("/admin/support/methods");
    state.methods = (data && data.items) || [];
    renderMethods();
  }

  function addMethod() {
    state.newMethod = {
      key: "",
      title: "",
      button_label: "",
      url: "",
      description: "",
      is_active: true,
      open_in_new_tab: true,
      sort_order: state.methods.length,
      style: "primary",
      sections: ["hero", "cta"],
    };
    renderMethods();
    const first = el("supportMethodsList");
    if (first) first.scrollIntoView({ behavior: "smooth", block: "start" });
  }

  /* ---------- code tab ---------- */

  function fillCode() {
    if (!state.content || !state.content.code) return;
    const html = el("supportCustomHtml");
    const css = el("supportCustomCss");
    if (html) html.value = state.content.code.custom_html || "";
    if (css) css.value = state.content.code.custom_css || "";
    updateGutter("supportCustomHtml", "supportHtmlGutter", "supportHtmlCount");
    updateGutter("supportCustomCss", "supportCssGutter", "supportCssCount");
  }

  function updateGutter(textareaId, gutterId, counterId) {
    const textarea = el(textareaId);
    const gutter = el(gutterId);
    const counter = el(counterId);
    if (!textarea) return;
    const lines = textarea.value.split("\n").length;
    if (gutter) {
      const numbers = [];
      for (let i = 1; i <= lines; i += 1) numbers.push(i);
      gutter.textContent = numbers.join("\n");
      gutter.scrollTop = textarea.scrollTop;
    }
    if (counter) counter.textContent = textarea.value.length;
  }

  function bindCodeEditors() {
    const bindings = [
      ["supportCustomHtml", "supportHtmlGutter", "supportHtmlCount", "custom_html"],
      ["supportCustomCss", "supportCssGutter", "supportCssCount", "custom_css"],
    ];
    bindings.forEach(([areaId, gutterId, counterId, key]) => {
      const textarea = el(areaId);
      if (!textarea) return;
      textarea.addEventListener("input", () => {
        if (!state.content.code) state.content.code = { custom_html: "", custom_css: "" };
        state.content.code[key] = textarea.value;
        updateGutter(areaId, gutterId, counterId);
        markCodeDirty();
      });
      textarea.addEventListener("scroll", () => {
        const gutter = el(gutterId);
        if (gutter) gutter.scrollTop = textarea.scrollTop;
      });
    });
  }

  function safeCss(css) {
    if (typeof css !== "string" || !css.trim()) return "";
    const lowered = css.toLowerCase();
    const forbidden = ["@import", "@font-face", "expression(", "javascript:", "vbscript:", "url(", "</style", "<script"];
    return forbidden.some((token) => lowered.indexOf(token) !== -1) ? "" : css;
  }

  /* ---------- preview ---------- */

  async function baseCss() {
    if (state.cssText) return state.cssText;
    const files = ["/app/styles.css", "/app/support.css"];
    try {
      const parts = await Promise.all(
        files.map((url) =>
          fetch(url, { credentials: "omit" }).then((response) =>
            response.ok ? response.text() : ""
          )
        )
      );
      state.cssText = parts.join("\n");
    } catch (error) {
      state.cssText = "";
    }
    return state.cssText;
  }

  async function buildPreviewDocument(data) {
    const css = await baseCss();
    const holder = document.createElement("div");
    holder.className = "support-root";
    if (RENDER) {
      RENDER.renderInto(holder, data, { withCode: true, openFaq: true });
    }
    const customCss = safeCss(data && data.code ? data.code.custom_css : "");
    const lang = data && data.lang === "en" ? "en" : "ru";
    const fontStack = '"Segoe UI", "Helvetica Neue", Arial, sans-serif';
    const fallbackCss = css
      ? ""
      : "body{margin:0;background:#070b10;color:#e9eff6;font-family:" +
        fontStack +
        ";line-height:1.55}main{width:min(94vw,1120px);margin:0 auto;padding:32px 0}.support-section{margin-top:48px}h1,h2,h3{line-height:1.2}";
    return (
      '<!doctype html><html lang="' + lang + '"><head><meta charset="utf-8">' +
      '<meta name="viewport" content="width=device-width, initial-scale=1">' +
      '<base href="/">' +
      (css ? "<style>" + css + "</style>" : "") +
      (fallbackCss ? "<style>" + fallbackCss + "</style>" : "") +
      (customCss ? "<style>" + customCss + "</style>" : "") +
      '</head><body class="support-preview-body"><main class="shell support-shell"><div class="support-root">' +
      holder.innerHTML +
      "</div></main></body></html>"
    );
  }

  async function openPreview(langValue) {
    const modalEl = el("supportPreviewModal");
    if (!modalEl || typeof window.bootstrap === "undefined") return;
    const modal = window.bootstrap.Modal.getOrCreateInstance(modalEl);
    const frame = el("supportPreviewFrame");
    const loading = el("supportPreviewLoading");
    if (loading) loading.hidden = false;
    if (frame) frame.srcdoc = "";
    modal.show();
    try {
      const lang = langValue || (el("supportPreviewLang") ? el("supportPreviewLang").value : "ru");
      const data = await api("/admin/support/preview?lang=" + encodeURIComponent(lang));
      if (frame) frame.srcdoc = await buildPreviewDocument(data);
      const revision = el("supportPreviewRevision");
      if (revision) revision.textContent = data.revision;
    } catch (error) {
      showAlert(errorText(error));
    } finally {
      if (loading) loading.hidden = true;
    }
  }

  async function previewCode() {
    if (!state.content) return;
    const payload = clone(state.content);
    if (!payload.code) payload.code = { custom_html: "", custom_css: "" };
    payload.code.custom_html = el("supportCustomHtml") ? el("supportCustomHtml").value : "";
    payload.code.custom_css = el("supportCustomCss") ? el("supportCustomCss").value : "";
    // Build the same projection the API would produce, straight from the draft.
    const frame = el("supportCodePreview");
    if (!frame) return;
    try {
      const data = await localPreviewPayload(payload);
      frame.srcdoc = await buildPreviewDocument(data);
    } catch (error) {
      showAlert("Не удалось построить предпросмотр: " + errorText(error));
    }
  }

  async function localPreviewPayload(content) {
    // A tiny client-side projection for the code preview: hidden sections are
    // skipped and each text field is rendered by the shared renderer? The
    // renderer expects the public shape, so ask the server for the draft
    // preview and only patch the code. Simpler and always in sync.
    const data = await api("/admin/support/preview?lang=ru");
    data.code = {
      custom_html: content.code.custom_html || "",
      custom_css: content.code.custom_css || "",
    };
    return data;
  }

  /* ---------- document lifecycle ---------- */

  function applyState(data) {
    state.revision = data.revision;
    state.content = data.content;
    state.methods = data.methods || [];
    state.meta = {
      revision: data.revision,
      published_revision: data.published_revision,
      draft_updated_at: data.draft_updated_at,
      draft_updated_by: data.draft_updated_by,
      published_at: data.published_at,
      published_by: data.published_by,
      has_unpublished_changes: data.has_unpublished_changes,
    };
    state.dirty = false;
    state.codeDirty = false;
    state.loaded = true;
  }

  async function loadStats() {
    try {
      state.stats = await api("/admin/support/stats?days=30");
    } catch (error) {
      state.stats = null;
    }
    setStatus();
  }

  async function load() {
    if (state.loading) return;
    state.loading = true;
    showAlert("");
    const container = el("supportSections");
    if (container) {
      container.innerHTML =
        '<div class="text-secondary small"><span class="spinner-border spinner-border-sm me-2"></span>Загрузка…</div>';
    }
    try {
      const data = await api("/admin/support");
      applyState(data);
      renderSections();
      renderMethods();
      fillCode();
      setStatus();
      loadStats();
    } catch (error) {
      state.loaded = false;
      showAlert(errorText(error));
      if (container) container.textContent = "";
    } finally {
      state.loading = false;
    }
  }

  function reload() {
    if (hasChanges() && !window.confirm("Несохранённые изменения будут потеряны. Продолжить?")) {
      return;
    }
    state.loaded = false;
    load();
  }

  async function saveDraft() {
    if (!state.content) return false;
    const button = el("supportSave");
    const spinner = el("supportSaveSpinner");
    setBusy(button, spinner, true);
    showAlert("");
    try {
      const data = await api("/admin/support/draft", {
        method: "PUT",
        body: { revision: state.revision, content: state.content },
      });
      state.content = data.content;
      state.revision = data.revision;
      state.meta.revision = data.revision;
      state.meta.has_unpublished_changes = true;
      state.meta.draft_updated_at = data.draft_updated_at || state.meta.draft_updated_at;
      state.meta.draft_updated_by = data.draft_updated_by || state.meta.draft_updated_by;
      state.dirty = false;
      state.codeDirty = false;
      renderSections();
      fillCode();
      setStatus();
      if (data.warnings && data.warnings.length) {
        showAlert(data.warnings.join(" "), "warning");
      } else {
        showAlert("Черновик сохранён. Публичная страница не изменилась.", "success");
      }
      return true;
    } catch (error) {
      if (error.status === 409) {
        showAlert(
          errorText(error) + " Нажмите «Обновить», чтобы загрузить актуальную версию черновика.",
          "danger"
        );
      } else {
        showAlert(errorText(error));
      }
      return false;
    } finally {
      setBusy(button, spinner, false);
    }
  }

  async function saveCode() {
    if (!state.content) return;
    if (state.dirty) {
      // Other fields have unsaved changes: save the whole draft, which
      // includes the code, instead of overwriting them with the code endpoint.
      await saveDraft();
      return;
    }
    const button = el("supportSaveCode");
    const spinner = el("supportSaveCodeSpinner");
    setBusy(button, spinner, true);
    showAlert("");
    try {
      const data = await api("/admin/support/code", {
        method: "PUT",
        body: {
          revision: state.revision,
          custom_html: el("supportCustomHtml") ? el("supportCustomHtml").value : "",
          custom_css: el("supportCustomCss") ? el("supportCustomCss").value : "",
        },
      });
      state.content = data.content;
      state.revision = data.revision;
      state.meta.revision = data.revision;
      state.meta.has_unpublished_changes = true;
      state.meta.draft_updated_at = data.draft_updated_at || state.meta.draft_updated_at;
      state.meta.draft_updated_by = data.draft_updated_by || state.meta.draft_updated_by;
      state.dirty = false;
      state.codeDirty = false;
      // ``data.content`` is a new object: rebuild the section editor so its
      // inputs bind to the returned document instead of the replaced one.
      renderSections();
      fillCode();
      setStatus();
      showAlert(
        data.warnings && data.warnings.length
          ? data.warnings.join(" ")
          : "Код сохранён в черновик.",
        data.warnings && data.warnings.length ? "warning" : "success"
      );
    } catch (error) {
      showAlert(
        error.status === 409
          ? errorText(error) + " Нажмите «Обновить» и повторите."
          : errorText(error)
      );
    } finally {
      setBusy(button, spinner, false);
    }
  }

  async function publish() {
    if (!state.content) return;
    if (hasChanges()) {
      const saved = await saveDraft();
      if (!saved) return;
    }
    const comment = window.prompt("Комментарий к публикации (необязательно)", "") || "";
    if (!window.confirm("Опубликовать черновик? Публичная страница обновится сразу.")) return;

    const button = el("supportPublish");
    const spinner = el("supportPublishSpinner");
    setBusy(button, spinner, true);
    showAlert("");
    try {
      const data = await api("/admin/support/publish", {
        method: "POST",
        body: { comment },
      });
      state.meta.published_revision = data.published_revision;
      state.meta.published_at = data.published_at;
      state.meta.has_unpublished_changes = false;
      state.codeDirty = false;
      setStatus();
      showAlert("Страница опубликована.", "success");
      loadStats();
      loadVersions();
    } catch (error) {
      showAlert(errorText(error));
    } finally {
      setBusy(button, spinner, false);
    }
  }

  async function loadVersions() {
    const body = el("supportHistoryBody");
    if (body) {
      body.innerHTML =
        '<tr><td colspan="5" class="table-loading"><span class="spinner-border spinner-border-sm me-2"></span>Загрузка…</td></tr>';
    }
    try {
      const data = await api("/admin/support/versions?limit=100");
      state.versions = (data && data.items) || [];
      state.versionsLoaded = true;
      renderVersions();
    } catch (error) {
      showAlert(errorText(error));
      if (body) body.textContent = "";
    }
  }

  function renderVersions() {
    const body = el("supportHistoryBody");
    const empty = el("supportHistoryEmpty");
    if (!body) return;
    body.textContent = "";
    if (empty) empty.hidden = state.versions.length > 0;

    state.versions.forEach((version) => {
      const row = document.createElement("tr");
      row.appendChild(node("td", "", "r" + version.revision));

      const created = node("td", "text-secondary", formatDate(version.created_at));
      row.appendChild(created);

      row.appendChild(node("td", "d-none d-md-table-cell", version.created_by || "—"));
      row.appendChild(node("td", "d-none d-lg-table-cell", version.comment || "—"));

      const actions = node("td", "text-end");
      const restore = node("button", "btn btn-sm btn-outline-light", "Восстановить в черновик");
      restore.type = "button";
      restore.addEventListener("click", () => restoreVersion(version));
      actions.appendChild(restore);
      row.appendChild(actions);
      body.appendChild(row);
    });
  }

  async function restoreVersion(version) {
    if (
      !window.confirm(
        "Восстановить ревизию r" + version.revision +
          " в черновик? Публичная страница не изменится до публикации."
      )
    ) {
      return;
    }
    try {
      const data = await api("/admin/support/versions/" + version.id + "/restore", {
        method: "POST",
      });
      state.content = data.content;
      state.revision = data.revision;
      state.meta.revision = data.revision;
      state.meta.has_unpublished_changes = true;
      state.meta.draft_updated_at = data.draft_updated_at || state.meta.draft_updated_at;
      state.meta.draft_updated_by = data.draft_updated_by || state.meta.draft_updated_by;
      state.dirty = false;
      state.codeDirty = false;
      renderSections();
      fillCode();
      setStatus();
      showAlert(
        "Ревизия r" + version.revision + " восстановлена в черновик. Опубликуйте изменения.",
        "success"
      );
    } catch (error) {
      showAlert(errorText(error));
    }
  }

  /* ---------- tabs and navigation ---------- */

  function showTab(name) {
    state.tab = name;
    document.querySelectorAll("[data-support-panel]").forEach((panel) => {
      panel.hidden = panel.getAttribute("data-support-panel") !== name;
    });
    document.querySelectorAll("[data-support-tab]").forEach((button) => {
      const active = button.getAttribute("data-support-tab") === name;
      button.classList.toggle("active", active);
      button.setAttribute("aria-selected", active ? "true" : "false");
    });
    if (name === "history" && !state.versionsLoaded) loadVersions();
    if (name === "code") updateGutter("supportCustomHtml", "supportHtmlGutter", "supportHtmlCount");
  }

  function init() {
    document.querySelectorAll("[data-support-tab]").forEach((button) => {
      button.addEventListener("click", () => showTab(button.getAttribute("data-support-tab")));
    });
    el("supportReload")?.addEventListener("click", reload);
    el("supportSave")?.addEventListener("click", saveDraft);
    el("supportPublish")?.addEventListener("click", publish);
    el("supportPreview")?.addEventListener("click", () => openPreview());
    el("supportHistoryReload")?.addEventListener("click", loadVersions);
    el("supportSaveCode")?.addEventListener("click", saveCode);
    el("supportPreviewCode")?.addEventListener("click", previewCode);
    el("supportAddMethod")?.addEventListener("click", addMethod);
    el("supportPreviewLang")?.addEventListener("change", (event) => openPreview(event.target.value));
    bindCodeEditors();

    window.FFAdminSupport = {
      show() {
        if (!state.loaded) load();
      },
      refresh() {
        reload();
      },
      load,
    };
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
