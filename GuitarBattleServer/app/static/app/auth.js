/* fretfight — sign in, registration with email confirmation, password reset */

(function () {
  'use strict';

  var FF = window.FF;
  var I18N = window.FFI18N;

  function t(key, vars) {
    return I18N ? I18N.t(key, vars) : key;
  }

  var PANELS = ['login', 'register', 'verify', 'forgot', 'reset'];

  var alertBox = document.getElementById('alert');
  var titleEl = document.getElementById('cardTitle');
  var subtitleEl = document.getElementById('cardSubtitle');
  var tabs = document.getElementById('tabs');
  var sessionCheck = document.getElementById('sessionCheck');
  var panels = Array.prototype.slice.call(document.querySelectorAll('[data-panel]'));

  var cooldowns = {};
  var currentPanel = 'login';

  function showAlert(kind, text) {
    alertBox.className = 'alert alert--' + kind;
    alertBox.textContent = text;
  }

  function clearAlert() {
    alertBox.className = 'alert hidden';
    alertBox.textContent = '';
  }

  function showPanel(name, options) {
    options = options || {};
    currentPanel = name;
    clearAlert();
    hideSessionCheck();

    panels.forEach(function (panel) {
      panel.classList.toggle('hidden', panel.getAttribute('data-panel') !== name);
    });

    titleEl.textContent = t('auth.panel.' + name + '.title');
    subtitleEl.textContent = t('auth.panel.' + name + '.subtitle');
    document.title = t('auth.docTitle.' + name) === 'auth.docTitle.' + name
      ? t('auth.panel.' + name + '.title') + ' — fretfight'
      : t('auth.docTitle.' + name);

    var showTabs = name === 'login' || name === 'register';
    tabs.classList.toggle('hidden', !showTabs);
    Array.prototype.forEach.call(tabs.querySelectorAll('.tab'), function (tab) {
      var active = tab.getAttribute('data-tab') === name;
      tab.classList.toggle('is-active', active);
      tab.setAttribute('aria-selected', active ? 'true' : 'false');
    });

    if (options.email) {
      var emailInput = document.querySelector('[data-panel="' + name + '"] input[type="email"]');
      if (emailInput) emailInput.value = options.email;
    }

    var firstInput = document.querySelector('[data-panel="' + name + '"] .input');
    if (firstInput) firstInput.focus();

    if (window.history && history.replaceState) {
      var url = name === 'login'
        ? '/login'
        : name === 'register'
          ? '/register'
          : '/login?mode=' + name;
      history.replaceState(null, '', url);
    }
  }

  function setBusy(button, busy) {
    button.disabled = busy;
    var spinner = button.querySelector('.btn__spinner');
    if (spinner) spinner.classList.toggle('hidden', !busy);
  }

  function startCooldown(button, seconds, labelKey) {
    if (cooldowns[button.id]) clearInterval(cooldowns[button.id]);
    var end = Date.now() + seconds * 1000;
    button.disabled = true;
    button.textContent = t('auth.msg.cooldown', { label: t(labelKey), seconds: seconds });
    cooldowns[button.id] = setInterval(function () {
      var left = Math.ceil((end - Date.now()) / 1000);
      if (left <= 0) {
        clearInterval(cooldowns[button.id]);
        delete cooldowns[button.id];
        button.disabled = false;
        button.textContent = t(labelKey);
        return;
      }
      button.textContent = t('auth.msg.cooldown', { label: t(labelKey), seconds: left });
    }, 250);
  }

  function detailsOf(error) {
    return error && error.payload && typeof error.payload.detail === 'string'
      ? error.payload.detail
      : '';
  }

  function hideSessionCheck() {
    document.body.classList.remove('is-session-checking');
    if (sessionCheck) sessionCheck.classList.add('hidden');
  }

  // Only the registration form carries the privacy-policy checkbox. Signing in
  // never asks for consent again.
  function requireRegistrationConsent(form) {
    if (!form.reportValidity()) return false;
    var consent = document.getElementById('regConsent');
    if (!consent || !consent.checked) {
      showAlert('info', t('auth.consentRequired'));
      if (consent) consent.focus();
      return false;
    }
    return true;
  }

  /* ---------- login ---------- */

  var loginForm = document.getElementById('loginForm');
  loginForm.addEventListener('submit', function (event) {
    event.preventDefault();
    if (!loginForm.reportValidity()) return;
    clearAlert();

    var button = loginForm.querySelector('button[type="submit"]');
    var identifier = document.getElementById('loginIdentifier').value.trim();
    setBusy(button, true);

    FF.api('/auth/login', {
      method: 'POST',
      body: { identifier: identifier, password: document.getElementById('loginPassword').value }
    }).then(function (data) {
      FF.saveSession(data);
      location.replace('/account');
    }).catch(function (error) {
      var detail = detailsOf(error);
      if (error.status === 403 && /not verified/i.test(detail)) {
        showPanel('verify', { email: identifier.indexOf('@') > 0 ? identifier : '' });
        showAlert('info', t('auth.msg.unverified'));
        return;
      }
      showAlert('error', FF.errorMessage(error));
      setBusy(button, false);
    });
  });

  /* ---------- registration ---------- */

  var registerForm = document.getElementById('registerForm');
  registerForm.addEventListener('submit', function (event) {
    event.preventDefault();
    if (!requireRegistrationConsent(registerForm)) return;
    clearAlert();

    var button = registerForm.querySelector('button[type="submit"]');
    var email = document.getElementById('regEmail').value.trim();
    var payload = {
      username: document.getElementById('regUsername').value.trim(),
      email: email,
      password: document.getElementById('regPassword').value,
      // Explicit consent, recorded once on the server as users.privacy_consent_at.
      privacy_consent: true
    };
    var displayName = document.getElementById('regDisplayName').value.trim();
    if (displayName) payload.display_name = displayName;

    setBusy(button, true);
    FF.api('/auth/register', { method: 'POST', body: payload })
      .then(function () {
        showPanel('verify', { email: email });
        showAlert('ok', t('auth.msg.registered', { email: email }));
        startCooldown(document.getElementById('resendVerify'), 60, 'auth.verify.resend');
      })
      .catch(function (error) {
        showAlert('error', FF.errorMessage(error));
        setBusy(button, false);
      });
  });

  /* ---------- email confirmation ---------- */

  var verifyForm = document.getElementById('verifyForm');
  verifyForm.addEventListener('submit', function (event) {
    event.preventDefault();
    if (!verifyForm.reportValidity()) return;
    clearAlert();

    var button = verifyForm.querySelector('button[type="submit"]');
    setBusy(button, true);
    FF.api('/auth/verify-email', {
      method: 'POST',
      body: {
        email: document.getElementById('verifyEmail').value.trim(),
        code: document.getElementById('verifyCode').value.trim()
      }
    }).then(function (data) {
      FF.saveSession(data);
      location.replace('/account');
    }).catch(function (error) {
      showAlert('error', FF.errorMessage(error));
      setBusy(button, false);
    });
  });

  document.getElementById('resendVerify').addEventListener('click', function () {
    var button = this;
    var email = document.getElementById('verifyEmail').value.trim();
    if (!email) {
      showAlert('error', t('auth.msg.emailRequired'));
      document.getElementById('verifyEmail').focus();
      return;
    }
    clearAlert();
    FF.api('/auth/resend-verification', { method: 'POST', body: { email: email } })
      .then(function () {
        showAlert('ok', t('auth.msg.resent', { email: email }));
        startCooldown(button, 60, 'auth.verify.resend');
      })
      .catch(function (error) {
        showAlert('error', FF.errorMessage(error));
      });
  });

  /* ---------- forgot password ---------- */

  var forgotForm = document.getElementById('forgotForm');
  forgotForm.addEventListener('submit', function (event) {
    event.preventDefault();
    if (!forgotForm.reportValidity()) return;
    clearAlert();

    var button = forgotForm.querySelector('button[type="submit"]');
    var email = document.getElementById('forgotEmail').value.trim();
    setBusy(button, true);
    FF.api('/auth/forgot-password', { method: 'POST', body: { email: email } })
      .then(function () {
        showPanel('reset', { email: email });
        showAlert('ok', t('auth.msg.resetSent'));
      })
      .catch(function (error) {
        showAlert('error', FF.errorMessage(error));
      })
      .then(function () { setBusy(button, false); });
  });

  /* ---------- new password ---------- */

  var resetForm = document.getElementById('resetForm');
  resetForm.addEventListener('submit', function (event) {
    event.preventDefault();
    if (!resetForm.reportValidity()) return;
    clearAlert();

    var button = resetForm.querySelector('button[type="submit"]');
    setBusy(button, true);
    FF.api('/auth/reset-password', {
      method: 'POST',
      body: {
        email: document.getElementById('resetEmail').value.trim(),
        code: document.getElementById('resetCode').value.trim(),
        new_password: document.getElementById('resetPassword').value
      }
    }).then(function (data) {
      FF.saveSession(data);
      location.replace('/account');
    }).catch(function (error) {
      showAlert('error', FF.errorMessage(error));
      setBusy(button, false);
    });
  });

  /* ---------- wiring ---------- */

  Array.prototype.forEach.call(tabs.querySelectorAll('.tab'), function (tab) {
    tab.addEventListener('click', function () {
      showPanel(tab.getAttribute('data-tab'));
    });
  });

  Array.prototype.forEach.call(document.querySelectorAll('[data-goto]'), function (button) {
    button.addEventListener('click', function () {
      var target = button.getAttribute('data-goto');
      showPanel(target, {
        email: document.getElementById('verifyEmail').value.trim() ||
               document.getElementById('resetEmail').value.trim()
      });
    });
  });

  Array.prototype.forEach.call(document.querySelectorAll('input[inputmode="numeric"]'), function (input) {
    input.addEventListener('input', function () {
      input.value = input.value.replace(/\D+/g, '').slice(0, 6);
    });
  });

  if (I18N) {
    I18N.onChange(function () {
      if (currentPanel && !document.body.classList.contains('is-session-checking')) {
        showPanel(currentPanel);
      }
    });
  }

  function boot() {
    var params = new URLSearchParams(location.search);
    var mode = params.get('mode') || params.get('tab');
    var email = params.get('email') || '';
    var initialPanel = mode && PANELS.indexOf(mode) !== -1
      ? mode
      : (location.pathname === '/register' ? 'register' : 'login');

    // Restore the saved session before revealing any form: the login card must
    // not flash for a visitor whose refresh cookie is still valid.
    FF.restoreSession()
      .then(function (me) {
        if (me) {
          location.replace('/account');
          return;
        }
        showPanel(initialPanel, { email: email });
        if (params.get('changed') === '1') {
          showAlert('ok', t('auth.msg.passwordChanged'));
        }
      })
      .catch(function (error) {
        showPanel(initialPanel, { email: email });
        // A network error means "unknown", not "signed out".
        if (error && error.status === 0) showAlert('error', FF.errorMessage(error));
      });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }
})();
