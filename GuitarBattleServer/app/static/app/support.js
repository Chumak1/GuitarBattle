/* fretfight — support page controller.
 *
 * Fetches the published content from /api/v1/support and renders it with the
 * shared renderer. The page is content-free on purpose: everything (sections,
 * order, visibility, buttons, custom code) is managed in the admin panel.
 */
(function () {
  'use strict';

  var I18N = window.FFI18N;
  var RENDER = window.FFSupportRender;
  var API = '/api/v1/support';
  var EVENTS = '/api/v1/support/events';

  var state = { data: null, requestId: 0, viewed: false };

  function t(key, fallback) {
    if (I18N && typeof I18N.t === 'function') {
      var value = I18N.t(key);
      if (value && value !== key) return value;
    }
    return fallback;
  }

  function lang() {
    return I18N && typeof I18N.language === 'function' && I18N.language() === 'en'
      ? 'en'
      : 'ru';
  }

  function track(event, methodKey) {
    // Aggregated counters only: no user id, no cookie, no address.
    try {
      var payload = JSON.stringify({ event: event, method_key: methodKey || '' });
      if (typeof navigator.sendBeacon === 'function') {
        if (navigator.sendBeacon(EVENTS, new Blob([payload], { type: 'application/json' }))) {
          return;
        }
      }
      if (typeof fetch === 'function') {
        fetch(EVENTS, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: payload,
          credentials: 'omit',
          keepalive: true
        }).catch(function () { /* analytics is optional */ });
      }
    } catch (error) {
      /* analytics must never break the page */
    }
  }

  function safeCss(css) {
    if (typeof css !== 'string' || !css.trim()) return '';
    var lowered = css.toLowerCase();
    var forbidden = ['@import', '@font-face', 'expression(', 'javascript:', 'vbscript:', 'url(', '</style', '<script'];
    for (var i = 0; i < forbidden.length; i += 1) {
      if (lowered.indexOf(forbidden[i]) !== -1) return '';
    }
    return css;
  }

  function applyCustomCss(css) {
    var value = safeCss(css);
    var style = document.getElementById('supportCustomCss');
    if (!value) {
      if (style) style.parentNode.removeChild(style);
      return;
    }
    if (!style) {
      style = document.createElement('style');
      style.id = 'supportCustomCss';
      document.head.appendChild(style);
    }
    style.textContent = value;
  }

  function ensureHeading(root) {
    if (root.querySelector('h1')) return;
    var heading = document.createElement('h1');
    heading.className = 'visually-hidden';
    heading.textContent = t('support.ogTitle', 'Поддержать Fretfight');
    root.appendChild(heading);
  }

  function showError(show) {
    var box = document.getElementById('supportError');
    if (box) box.hidden = !show;
  }

  function render(data) {
    var root = document.getElementById('supportRoot');
    if (!root || !RENDER) return;
    // The renderer re-checks every value on the client, including custom HTML.
    var count = RENDER.renderInto(root, data, { withCode: true });
    ensureHeading(root);
    root.setAttribute('aria-busy', 'false');
    applyCustomCss(data && data.code ? data.code.custom_css : '');
    showError(count === 0);
  }

  function load() {
    var root = document.getElementById('supportRoot');
    state.requestId += 1;
    var requestId = state.requestId;

    if (root) {
      root.setAttribute('aria-busy', 'true');
      if (!state.data) {
        root.textContent = '';
        var loading = document.createElement('p');
        loading.className = 'support-state';
        loading.setAttribute('role', 'status');
        loading.textContent = t('support.loading', 'Загрузка…');
        root.appendChild(loading);
      }
    }
    showError(false);

    if (typeof fetch !== 'function') {
      showError(true);
      return;
    }

    fetch(API + '?lang=' + lang(), { credentials: 'omit', cache: 'no-store' })
      .then(function (response) {
        if (!response.ok) throw new Error('HTTP ' + response.status);
        return response.json();
      })
      .then(function (data) {
        if (requestId !== state.requestId) return;
        if (!data || typeof data !== 'object' || !data.sections) {
          throw new Error('Unexpected payload');
        }
        state.data = data;
        render(data);
        if (!state.viewed) {
          state.viewed = true;
          track('support_view');
        }
      })
      .catch(function () {
        if (requestId !== state.requestId) return;
        if (state.data) {
          // Keep the previous render when only the language switch failed.
          if (root) root.setAttribute('aria-busy', 'false');
          return;
        }
        var current = document.getElementById('supportRoot');
        if (current) current.textContent = '';
        showError(true);
      });
  }

  function bindFaq() {
    var root = document.getElementById('supportRoot');
    if (!root) return;
    root.addEventListener('click', function (event) {
      var trigger = event.target.closest ? event.target.closest('.faq-trigger') : null;
      if (trigger && root.contains(trigger)) {
        var panelId = trigger.getAttribute('aria-controls');
        var panel = panelId ? document.getElementById(panelId) : null;
        var expanded = trigger.getAttribute('aria-expanded') === 'true';
        trigger.setAttribute('aria-expanded', expanded ? 'false' : 'true');
        if (panel) panel.hidden = expanded;
        if (!expanded) track('support_faq_open');
        return;
      }
      var link = event.target.closest ? event.target.closest('[data-method-key]') : null;
      if (link) {
        track('support_click', link.getAttribute('data-method-key') || '');
      }
    });
  }

  function init() {
    bindFaq();
    var retry = document.getElementById('supportRetry');
    if (retry) retry.addEventListener('click', load);
    if (I18N && typeof I18N.onChange === 'function') I18N.onChange(load);
    load();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
