/* fretfight — tiny shared client for the public API (no dependencies) */

window.FF = (function () {
  'use strict';

  var BASE = '/api/v1';
  var TOKEN_KEY = 'ff.token';
  var EXPIRES_KEY = 'ff.tokenExpires';

  function L(key, vars) {
    return window.FFI18N ? window.FFI18N.t(key, vars) : key;
  }

  function locale() {
    return window.FFI18N && window.FFI18N.language() === 'en' ? 'en-US' : 'ru-RU';
  }

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
    'Incorrect email/username or password': 'errors.badCredentials',
    'Email address is not verified': 'errors.emailNotVerified',
    'Username or email is already registered': 'errors.usernameTaken',
    'Username is already taken': 'errors.usernameOnlyTaken',
    'Current password is incorrect': 'errors.currentPassword',
    'Invalid or expired code': 'errors.invalidCode',
    'Email address is already verified': 'errors.alreadyVerified',
    'Could not send the verification email. Try again later.': 'errors.emailUnavailable',
    'Could not send the email. Try again later.': 'errors.emailUnavailable',
    'Registration is temporarily unavailable': 'errors.registrationUnavailable',
    'Too many attempts. Try again later.': 'errors.tooMany'
  };

  var FIELD_NAMES = {
    username: 'errors.field.username',
    email: 'errors.field.email',
    password: 'errors.field.password',
    display_name: 'errors.field.display_name',
    code: 'errors.field.code',
    new_password: 'errors.field.new_password',
    current_password: 'errors.field.current_password',
    identifier: 'errors.field.identifier'
  };

  function validationMessage(items) {
    if (!Array.isArray(items)) return null;
    var parts = items.map(function (item) {
      var field = item.loc && item.loc.length ? item.loc[item.loc.length - 1] : '';
      var name = FIELD_NAMES[field] ? L(FIELD_NAMES[field]) : field;
      var ctx = item.ctx || {};
      var text;
      switch (item.type) {
        case 'missing': text = L('errors.fieldRequired'); break;
        case 'string_too_short': text = L('errors.fieldMin', { n: ctx.min_length || '?' }); break;
        case 'string_too_long': text = L('errors.fieldMax', { n: ctx.max_length || '?' }); break;
        case 'string_pattern_mismatch': text = L('errors.fieldPattern'); break;
        case 'value_error':
          if (field === 'email') text = L('errors.fieldEmail');
          else if (/at least/i.test(item.msg || '')) text = L('errors.fieldShort');
          else text = L('errors.fieldValue');
          break;
        case 'bool_type':
        case 'int_type':
        case 'string_type': text = L('errors.fieldFormat'); break;
        default: text = item.msg || L('errors.fieldValue');
      }
      return name ? name + ': ' + text : text;
    });
    return parts.length ? parts.join('; ') : null;
  }

  function errorMessage(error) {
    if (!error || error.name !== 'ApiError') return L('errors.generic');
    if (error.status === 0) return L('errors.network');

    var payload = error.payload;
    var detail = payload && payload.detail;

    if (error.status === 422) {
      var validation = validationMessage(detail);
      if (validation) return L('errors.checkFields', { details: validation });
    }
    if (typeof detail === 'string' && KNOWN_DETAILS[detail]) return L(KNOWN_DETAILS[detail]);
    if (Array.isArray(detail)) {
      var message = validationMessage(detail);
      if (message) return L('errors.checkFields', { details: message });
    }

    var byStatus = {
      400: 'errors.badRequest',
      401: 'errors.badCredentials',
      403: 'errors.forbidden',
      404: 'errors.notFound',
      409: 'errors.conflict',
      429: 'errors.tooMany',
      500: 'errors.server',
      503: 'errors.unavailable'
    };
    if (byStatus[error.status]) return L(byStatus[error.status]);
    return typeof detail === 'string' ? detail : L('errors.request');
  }

  function formatDateTime(value) {
    if (!value) return '—';
    var date = new Date(value);
    if (isNaN(date.getTime())) return '—';
    return new Intl.DateTimeFormat(locale(), {
      day: '2-digit', month: '2-digit', year: '2-digit',
      hour: '2-digit', minute: '2-digit'
    }).format(date);
  }

  function formatNumber(value) {
    var number = Number(value || 0);
    return new Intl.NumberFormat(locale()).format(number);
  }

  function formatPercent(value) {
    if (value === null || value === undefined) return '—';
    var number = Number(value);
    if (isNaN(number)) return '—';
    var text = number.toFixed(1);
    if (locale() !== 'en-US') text = text.replace('.', ',');
    return text + '%';
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
