/* fretfight — sign in, registration with email confirmation, password reset */

(function () {
  'use strict';

  var FF = window.FF;

  var TITLES = {
    login: ['С возвращением', 'Войдите, чтобы смотреть рейтинг и историю попыток.'],
    register: ['Создать аккаунт', 'Пара минут — и вы в общем рейтинге.'],
    verify: ['Подтвердите почту', 'Мы отправили шестизначный код на ваш email.'],
    forgot: ['Забыли пароль?', 'Пришлём код для смены пароля на почту аккаунта.'],
    reset: ['Новый пароль', 'Введите код из письма и придумайте новый пароль.']
  };

  var alertBox = document.getElementById('alert');
  var titleEl = document.getElementById('cardTitle');
  var subtitleEl = document.getElementById('cardSubtitle');
  var tabs = document.getElementById('tabs');
  var panels = Array.prototype.slice.call(document.querySelectorAll('[data-panel]'));

  var cooldowns = {};

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
    clearAlert();

    panels.forEach(function (panel) {
      panel.classList.toggle('hidden', panel.getAttribute('data-panel') !== name);
    });

    var copy = TITLES[name] || TITLES.login;
    titleEl.textContent = copy[0];
    subtitleEl.textContent = copy[1];

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
      var url = name === 'login' ? '/login' : '/login?mode=' + name;
      history.replaceState(null, '', url);
    }
  }

  function setBusy(button, busy) {
    button.disabled = busy;
    var spinner = button.querySelector('.btn__spinner');
    if (spinner) spinner.classList.toggle('hidden', !busy);
  }

  function startCooldown(button, seconds, label) {
    if (cooldowns[button.id]) clearInterval(cooldowns[button.id]);
    var end = Date.now() + seconds * 1000;
    button.disabled = true;
    button.textContent = label + ' (' + seconds + ' с)';
    cooldowns[button.id] = setInterval(function () {
      var left = Math.ceil((end - Date.now()) / 1000);
      if (left <= 0) {
        clearInterval(cooldowns[button.id]);
        delete cooldowns[button.id];
        button.disabled = false;
        button.textContent = label;
        return;
      }
      button.textContent = label + ' (' + left + ' с)';
    }, 250);
  }

  function detailsOf(error) {
    return error && error.payload && typeof error.payload.detail === 'string'
      ? error.payload.detail
      : '';
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
        showAlert('info', 'Почта ещё не подтверждена. Введите код из письма или запросите новый.');
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
    if (!registerForm.reportValidity()) return;
    clearAlert();

    var button = registerForm.querySelector('button[type="submit"]');
    var email = document.getElementById('regEmail').value.trim();
    var payload = {
      username: document.getElementById('regUsername').value.trim(),
      email: email,
      password: document.getElementById('regPassword').value
    };
    var displayName = document.getElementById('regDisplayName').value.trim();
    if (displayName) payload.display_name = displayName;

    setBusy(button, true);
    FF.api('/auth/register', { method: 'POST', body: payload })
      .then(function () {
        showPanel('verify', { email: email });
        showAlert('ok', 'Аккаунт создан. Код подтверждения отправлен на ' + email + '.');
        startCooldown(document.getElementById('resendVerify'), 60, 'Отправить код ещё раз');
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
      showAlert('error', 'Сначала укажите email, на который пришёл код.');
      document.getElementById('verifyEmail').focus();
      return;
    }
    clearAlert();
    FF.api('/auth/resend-verification', { method: 'POST', body: { email: email } })
      .then(function () {
        showAlert('ok', 'Новый код отправлен на ' + email + '.');
        startCooldown(button, 60, 'Отправить код ещё раз');
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
        showAlert('ok', 'Если аккаунт с таким email существует, код уже отправлен.');
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

  // Already signed in? Go straight to the account page.
  if (FF.getToken()) {
    FF.api('/auth/me', { auth: true })
      .then(function () { location.replace('/account'); })
      .catch(function () { FF.clearSession(); });
  }

  var params = new URLSearchParams(location.search);
  var mode = params.get('mode') || params.get('tab');
  var email = params.get('email') || '';
  if (mode && TITLES[mode]) showPanel(mode, { email: email });
  else if (email) showPanel('login', { email: email });

  if (params.get('changed') === '1') {
    showAlert('ok', 'Пароль изменён. Войдите с новым паролем.');
  }
})();
