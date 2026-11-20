/* FretFight admin — artwork slots.
 *
 * Three administrator-uploaded images used by the desktop client: the BATTLE
 * and ONLINE hero backgrounds of the HOME dashboard and the default avatar of
 * players without their own picture. One image per slot; uploading replaces
 * the previous file and the previous URL stops working.
 *
 * Kept separate from admin-site.js: this module only needs the administrator
 * JWT from sessionStorage and the same fetch wrapper pattern.
 */
(() => {
  "use strict";

  const API_BASE = "/api/v1";
  const TOKEN_KEY = "guitarbattle.admin.token";

  const SLOTS = [
    {
      key: "hero-battle",
      title: "Battle background",
      hint:
        "Фон карточки BATTLE на главном экране клиента. Без файла клиент " +
        "рисует процедурную башню. PNG/JPEG/WebP, до 8 МБ.",
    },
    {
      key: "hero-duel",
      title: "Duel background",
      hint:
        "Фон карточки ONLINE на главном экране клиента. Без файла клиент " +
        "рисует процедурную дуэль. PNG/JPEG/WebP, до 8 МБ.",
    },
    {
      key: "default-avatar",
      title: "Default avatar",
      hint:
        "Аватар игроков без собственной картинки. Без файла показывается " +
        "первая буква имени. PNG/JPEG/WebP, до 2 МБ.",
    },
  ];

  const state = {
    loaded: false,
    items: {},
  };

  const el = (id) => document.getElementById(id);

  function detailOf(payload) {
    if (!payload) return "";
    if (typeof payload.detail === "string") return payload.detail;
    if (Array.isArray(payload.detail)) {
      return payload.detail
        .map((item) =>
          typeof item === "string" ? item : item.msg || item.detail || ""
        )
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
      return "Недостаточно прав администратора.";
    }
    if (error && error.status === 413) {
      return "Файл слишком большой.";
    }
    if (error && error.status === 415) {
      return "Нужен PNG, JPEG или WebP.";
    }
    return (error && error.message) || "Не удалось выполнить запрос.";
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
      const error = new Error(
        detailOf(data) || "Ошибка запроса (" + response.status + ")"
      );
      error.status = response.status;
      throw error;
    }
    return data;
  }

  function showAlert(message, variant = "danger") {
    const box = el("artworkAlert");
    if (!box) return;
    box.className = "alert alert-" + variant;
    box.textContent = message || "";
    box.hidden = !message;
  }

  function formatDate(value) {
    if (!value) return "";
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return "";
    return new Intl.DateTimeFormat("ru-RU", {
      day: "2-digit",
      month: "2-digit",
      year: "numeric",
      hour: "2-digit",
      minute: "2-digit",
    }).format(date);
  }

  function slotIds(slot) {
    return {
      preview: el("artworkPreview-" + slot),
      empty: el("artworkEmpty-" + slot),
      file: el("artworkFile-" + slot),
      upload: el("artworkUpload-" + slot),
      remove: el("artworkRemove-" + slot),
      status: el("artworkStatus-" + slot),
    };
  }

  function renderSlot(slot, item) {
    const ids = slotIds(slot);
    const url = item && item.url ? item.url : "";
    if (ids.preview) {
      if (url) {
        ids.preview.src = url;
        ids.preview.hidden = false;
      } else {
        ids.preview.removeAttribute("src");
        ids.preview.hidden = true;
      }
    }
    if (ids.empty) ids.empty.hidden = Boolean(url);
    if (ids.remove) ids.remove.disabled = !url;
    if (ids.status) {
      ids.status.textContent = url
        ? "Загружено: " + (formatDate(item.updated_at) || "—")
        : "Файл не загружен.";
    }
  }

  function render(payload) {
    const items = (payload && payload.items) || [];
    state.items = {};
    SLOTS.forEach((slot) => {
      const item = items.find((entry) => entry.slot === slot.key) || null;
      state.items[slot.key] = item;
      renderSlot(slot.key, item);
    });
  }

  async function load() {
    showAlert("");
    try {
      const data = await api("/admin/artwork");
      state.loaded = true;
      render(data);
    } catch (error) {
      state.loaded = false;
      showAlert(errorText(error));
    }
  }

  async function upload(slot) {
    const ids = slotIds(slot);
    const file = ids.file && ids.file.files ? ids.file.files[0] : null;
    if (!file) {
      const meta = SLOTS.find((entry) => entry.key === slot);
      showAlert(
        "Сначала выберите файл: " + (meta ? meta.title : slot) + ".",
        "warning"
      );
      return;
    }
    if (ids.upload) ids.upload.disabled = true;
    showAlert("");
    try {
      const form = new FormData();
      form.append("file", file);
      await api("/admin/artwork/" + encodeURIComponent(slot) + "/image", {
        method: "POST",
        body: form,
      });
      if (ids.file) ids.file.value = "";
      await load();
      showAlert("Изображение загружено.", "success");
    } catch (error) {
      showAlert(errorText(error));
    } finally {
      if (ids.upload) ids.upload.disabled = false;
    }
  }

  async function remove(slot) {
    if (!window.confirm("Удалить изображение? Клиент вернётся к запасному варианту.")) {
      return;
    }
    const ids = slotIds(slot);
    if (ids.remove) ids.remove.disabled = true;
    showAlert("");
    try {
      await api("/admin/artwork/" + encodeURIComponent(slot) + "/image", {
        method: "DELETE",
      });
      await load();
      showAlert("Изображение удалено.", "success");
    } catch (error) {
      showAlert(errorText(error));
      if (ids.remove) ids.remove.disabled = false;
    }
  }

  function init() {
    SLOTS.forEach((slot) => {
      const ids = slotIds(slot.key);
      if (ids.upload) {
        ids.upload.addEventListener("click", () => upload(slot.key));
      }
      if (ids.remove) {
        ids.remove.addEventListener("click", () => remove(slot.key));
      }
    });
    const reload = el("artworkReload");
    if (reload) reload.addEventListener("click", load);
  }

  window.FFAdminArtwork = {
    show() {
      if (!state.loaded) load();
    },
    refresh: load,
  };

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
