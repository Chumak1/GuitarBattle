/* fretfight — contacts block controller.
 *
 * The block is rendered statically in every public page, so the page looks
 * complete without JavaScript. This script asks the public API for the values
 * managed in the admin panel and paints them over the markup. Any failure
 * leaves the static (translated) content untouched.
 */
(function () {
  'use strict';

  var I18N = window.FFI18N;
  var API = '/api/v1/site/contacts';

  // Fixed icon order on every page.
  var NETWORKS = [
    { key: 'youtube', label: 'YouTube' },
    { key: 'instagram', label: 'Instagram' },
    { key: 'twitter', label: 'Twitter / X' },
    { key: 'vk', label: 'VK' }
  ];

  var state = { data: null };
  var list = null;
  var originals = {};

  function t(key, fallback) {
    if (I18N && typeof I18N.t === 'function') {
      var value = I18N.t(key);
      if (value && value !== key) return value;
    }
    return fallback;
  }

  function lang() {
    return I18N && typeof I18N.language === 'function' ? I18N.language() : 'ru';
  }

  // Server-side validation already rejects non-http(s) URLs; keep a cheap
  // client-side guard so a broken response can never inject a scheme.
  function safeUrl(value) {
    if (typeof value !== 'string') return '';
    var trimmed = value.trim();
    return /^https?:\/\//i.test(trimmed) ? trimmed : '';
  }

  /* ---------- static icons ---------- */

  function captureOriginals() {
    if (!list) return;
    Array.prototype.forEach.call(list.querySelectorAll('[data-social]'), function (node) {
      var key = node.getAttribute('data-social');
      if (key && !originals[key]) originals[key] = node.cloneNode(true);
    });
  }

  function iconOf(network) {
    var source = originals[network];
    if (!source) return null;
    var svg = source.querySelector('svg');
    return svg ? svg.cloneNode(true) : null;
  }

  /* ---------- fields ---------- */

  function applyName() {
    var node = document.querySelector('[data-contacts-name]');
    if (!node) return;
    var name = state.data && state.data.name ? state.data.name[lang()] : '';
    // An empty value means "use the dictionary default" (i18n owns the text).
    if (typeof name === 'string' && name) node.textContent = name;
  }

  function applyEmail(email) {
    var node = document.querySelector('[data-contacts-email]');
    if (!node || typeof email !== 'string' || !email) return;
    node.textContent = email;
    node.setAttribute('href', 'mailto:' + email);
  }

  function applyTelegram(telegram, url) {
    var node = document.querySelector('[data-contacts-telegram]');
    if (!node) return;
    var row = node.closest ? node.closest('.contacts__link') : null;
    if (typeof telegram !== 'string' || !telegram || !url) {
      // An empty handle hides the row entirely.
      if (row) row.hidden = true;
      return;
    }
    if (row) row.hidden = false;
    node.textContent = telegram;
    node.setAttribute('href', url);
  }

  /* ---------- socials ---------- */

  function placeholder(network) {
    var source = originals[network];
    if (!source) return null;
    var node = source.cloneNode(true);
    node.classList.add('is-placeholder');
    node.setAttribute('aria-disabled', 'true');
    node.setAttribute('tabindex', '-1');
    node.setAttribute('title', t('contacts.soon', 'Скоро'));
    return node;
  }

  function link(network, url, label) {
    var node = document.createElement('a');
    node.className = 'contacts__social';
    node.setAttribute('href', url);
    node.setAttribute('target', '_blank');
    node.setAttribute('rel', 'noopener noreferrer');
    node.setAttribute('data-social', network);
    node.setAttribute('aria-label', label);
    var svg = iconOf(network);
    if (svg) node.appendChild(svg);
    return node;
  }

  function renderSocials(socials) {
    if (!list) return;
    var items = [];
    NETWORKS.forEach(function (network) {
      var url = safeUrl(socials ? socials[network.key] : null);
      var node = url
        ? link(network.key, url, network.label)
        : placeholder(network.key);
      if (!node) return;
      var item = document.createElement('li');
      item.appendChild(node);
      items.push(item);
    });
    while (list.firstChild) list.removeChild(list.firstChild);
    items.forEach(function (item) { list.appendChild(item); });
  }

  function applyLabels() {
    if (!list) return;
    list.setAttribute('aria-label', t('contacts.socialsLabel', 'Мы в соцсетях'));
    Array.prototype.forEach.call(list.querySelectorAll('.is-placeholder'), function (node) {
      node.setAttribute('title', t('contacts.soon', 'Скоро'));
    });
  }

  /* ---------- lifecycle ---------- */

  function apply() {
    if (state.data) {
      applyEmail(state.data.email);
      applyTelegram(state.data.telegram, safeUrl(state.data.telegram_url));
      renderSocials(state.data.socials || {});
    }
    applyName();
    applyLabels();
  }

  function load() {
    if (typeof fetch !== 'function') return;
    fetch(API, { credentials: 'omit', cache: 'no-store' })
      .then(function (response) {
        return response.ok ? response.json() : null;
      })
      .then(function (data) {
        if (!data || typeof data !== 'object') return;
        state.data = data;
        apply();
      })
      .catch(function () {
        /* Keep the static block: the API is optional for this page. */
      });
  }

  function init() {
    list = document.querySelector('[data-contacts-socials]');
    captureOriginals();
    applyLabels();
    if (I18N && typeof I18N.onChange === 'function') I18N.onChange(apply);
    load();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
