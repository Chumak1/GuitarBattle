/* fretfight — tiny shared client for the public API (no dependencies) */

window.FF = (function () {
  'use strict';

  var BASE = '/api/v1';
  var TOKEN_KEY = 'ff.token';
  var EXPIRES_KEY = 'ff.tokenExpires';

  function ApiError(status, payload) {
    this.name = 'ApiError';
    this.status = status;
    this.payload = payload;
    this.message = 'API error ' + status;
  }
  ApiError.prototype = Object.create(Error.prototype);

  function getToken() {
    try {
      var token = localStorage.getItem(TOKEN_KEY);
      var expires = Number(localStorage.getItem(EXPIRES_KEY) || 0);
      if (!token) return null;
      if (expires && Date.now() > expires - 5000) {
        clearSession();
        return null;
      }
      return token;
    } catch (e) {
      return null;
    }
  }

  function saveSession(data) {
    try {
      localStorage.setItem(TOKEN_KEY, data.access_token);
      var expires = Date.now() + (Number(data.expires_in) || 0) * 1000;
      localStorage.setItem(EXPIRES_KEY, String(expires));
    } catch (e) {
      /* private mode: the session will simply not survive a reload */
    }
  }

  function clearSession() {
    try {
      localStorage.removeItem(TOKEN_KEY);
      localStorage.removeItem(EXPIRES_KEY);
    } catch (e) { /* ignore */ }
  }

  function api(path, options) {
    options = options || {};
    var method = options.method || 'GET';
    var headers = {};
    var init = { method: method, headers: headers, credentials: 'omit' };

    if (options.body !== undefined && options.body !== null) {
      headers['Content-Type'] = 'application/json';
      init.body = JSON.stringify(options.body);
    }
    if (options.auth) {
      var token = getToken();
      if (token) headers.Authorization = 'Bearer ' + token;
    }

    return fetch(BASE + path, init).then(function (response) {
      return response.text().then(function (text) {
        var data = null;
        if (text) {
          try { data = JSON.parse(text); } catch (e) { data = text; }
        }
        if (!response.ok) throw new ApiError(response.status, data);
        return data;
      });
    }).catch(function (error) {
      if (error instanceof ApiError) throw error;
      throw new ApiError(0, null); // network failure / offline
    });
  }

  var KNOWN_DETAILS = {
    'Incorrect email/username or password': 'Неверный email/логин или пароль.',
    'Email address is not verified': 'Почта ещё не подтверждена. Введите код из письма.',
    'Username or email is already registered': 'Этот логин или email уже заняты.',
    'Username is already taken': 'Этот логин уже занят.',
    'Current password is incorrect': 'Текущий пароль неверен.',
    'Invalid or expired code': 'Неверный или просроченный код.',
    'Email address is already verified': 'Почта уже подтверждена — можно войти.',
    'Could not send the verification email. Try again later.':
      'Не удалось отправить письмо: почтовый сервис недоступен. Попробуйте позже.',
    'Could not send the email. Try again later.':
      'Не удалось отправить письмо: почтовый сервис недоступен. Попробуйте позже.',
    'Registration is temporarily unavailable': 'Регистрация временно недоступна.',
    'Too many attempts. Try again later.': 'Слишком много попыток. Попробуйте позже.'
  };

  var FIELD_NAMES = {
    username: 'логин',
    email: 'email',
    password: 'пароль',
    display_name: 'имя',
    code: 'код',
    new_password: 'новый пароль',
    current_password: 'текущий пароль',
    identifier: 'email или логин'
  };

  function validationMessage(items) {
    if (!Array.isArray(items)) return null;
    var parts = items.map(function (item) {
      var field = item.loc && item.loc.length ? item.loc[item.loc.length - 1] : '';
      var name = FIELD_NAMES[field] || field;
      var ctx = item.ctx || {};
      var text;
      switch (item.type) {
        case 'missing': text = 'обязательное поле'; break;
        case 'string_too_short': text = 'минимум ' + (ctx.min_length || '?') + ' символов'; break;
        case 'string_too_long': text = 'максимум ' + (ctx.max_length || '?') + ' символов'; break;
        case 'string_pattern_mismatch': text = 'недопустимые символы'; break;
        case 'value_error':
          if (field === 'email') text = 'неверный email';
          else if (/at least/i.test(item.msg || '')) text = 'слишком короткий';
          else text = 'неверное значение';
          break;
        case 'bool_type':
        case 'int_type':
        case 'string_type': text = 'неверный формат'; break;
        default: text = item.msg || 'неверное значение';
      }
      return name ? name + ': ' + text : text;
    });
    return parts.length ? parts.join('; ') : null;
  }

  function errorMessage(error) {
    if (!error || error.name !== 'ApiError') return 'Что-то пошло не так. Попробуйте ещё раз.';
    if (error.status === 0) return 'Нет связи с сервером. Проверьте интернет и попробуйте снова.';

    var payload = error.payload;
    var detail = payload && payload.detail;

    if (error.status === 422) {
      var validation = validationMessage(detail);
      if (validation) return 'Проверьте поля — ' + validation + '.';
    }
    if (typeof detail === 'string' && KNOWN_DETAILS[detail]) return KNOWN_DETAILS[detail];
    if (Array.isArray(detail)) {
      var message = validationMessage(detail);
      if (message) return 'Проверьте поля — ' + message + '.';
    }

    var byStatus = {
      400: 'Проверьте введённые данные.',
      401: 'Неверный email/логин или пароль.',
      403: 'Доступ запрещён. Возможно, нужно подтвердить почту.',
      404: 'Не найдено.',
      409: 'Этот логин или email уже заняты.',
      429: 'Слишком много попыток. Подождите немного.',
      500: 'Ошибка на сервере. Попробуйте позже.',
      503: 'Сервис временно недоступен. Попробуйте позже.'
    };
    return byStatus[error.status] || (typeof detail === 'string' ? detail : 'Ошибка запроса.');
  }

  function formatDateTime(value) {
    if (!value) return '—';
    var date = new Date(value);
    if (isNaN(date.getTime())) return '—';
    return new Intl.DateTimeFormat('ru-RU', {
      day: '2-digit', month: '2-digit', year: '2-digit',
      hour: '2-digit', minute: '2-digit'
    }).format(date);
  }

  function formatNumber(value) {
    var number = Number(value || 0);
    return new Intl.NumberFormat('ru-RU').format(number);
  }

  function formatPercent(value) {
    if (value === null || value === undefined) return '—';
    var number = Number(value);
    if (isNaN(number)) return '—';
    return number.toFixed(1).replace('.', ',') + '%';
  }

  return {
    api: api,
    ApiError: ApiError,
    getToken: getToken,
    saveSession: saveSession,
    clearSession: clearSession,
    errorMessage: errorMessage,
    formatDateTime: formatDateTime,
    formatNumber: formatNumber,
    formatPercent: formatPercent
  };
})();
