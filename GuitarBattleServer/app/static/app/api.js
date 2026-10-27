/* fretfight — tiny shared client for the public API (no dependencies) */

window.FF = (function () {
  'use strict';

  var BASE = '/api/v1';
  // Access tokens live in memory only. A page reload restores the session from
  // the HttpOnly `ff_refresh` cookie via POST /auth/refresh, so a token is never
  // exposed to localStorage. Legacy keys from the old client are removed.
  var LEGACY_TOKEN_KEY = 'ff.token';
  var LEGACY_EXPIRES_KEY = 'ff.tokenExpires';

  var accessToken = null;
  var accessExpiresAt = 0;
  var refreshInFlight = null;

  (function dropLegacyTokens() {
    try {
      localStorage.removeItem(LEGACY_TOKEN_KEY);
      localStorage.removeItem(LEGACY_EXPIRES_KEY);
    } catch (e) { /* private mode */ }
  })();

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
    if (!accessToken) return null;
    if (accessExpiresAt && Date.now() > accessExpiresAt - 5000) return null;
    return accessToken;
  }

  function saveSession(data) {
    if (!data || !data.access_token) return;
    accessToken = data.access_token;
    accessExpiresAt = Date.now() + (Number(data.expires_in) || 0) * 1000;
    // The refresh token stays in the HttpOnly cookie; it is never stored here.
  }

  function clearSession() {
    accessToken = null;
    accessExpiresAt = 0;
  }

  function rawRequest(path, options) {
    options = options || {};
    var method = options.method || 'GET';
    var headers = {};
    var init = { method: method, headers: headers, credentials: 'same-origin' };

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

  /* ---------- session ---------- */

  // One in-flight refresh per page: parallel 401s must not rotate the token
  // twice (which the server would treat as reuse).
  function refreshOnce() {
    return rawRequest('/auth/refresh', { method: 'POST' }).then(function (data) {
      saveSession(data);
      return true;
    }, function (error) {
      if (error && error.status === 401) return false;
      // A network error or 5xx is not a reason to drop the session: the cookie
      // is still valid and the next attempt may succeed.
      throw error;
    });
  }

  function refreshSession() {
    if (refreshInFlight) return refreshInFlight;

    refreshInFlight = refreshOnce().then(function (ok) {
      if (ok) return true;
      // Another tab may have rotated the cookie while this request was in
      // flight; one short retry picks up the fresh cookie.
      return new Promise(function (resolve) { setTimeout(resolve, 250); })
        .then(refreshOnce)
        .then(function (retried) {
          if (!retried) clearSession();
          return retried;
        });
    });

    var settle = function () { refreshInFlight = null; };
    refreshInFlight.then(settle, settle);
    return refreshInFlight;
  }

  // Restore the session after a page load: refresh, then load the profile.
  // Resolves with the profile, or with null when there is no session.
  // Rejects only on a network/server error so callers can keep the user signed in.
  function restoreSession() {
    return refreshSession().then(function (ok) {
      if (!ok) return null;
      return rawRequest('/auth/me', { auth: true }).catch(function (error) {
        if (error && error.status === 401) {
          clearSession();
          return null;
        }
        throw error;
      });
    });
  }

  function logout() {
    return rawRequest('/auth/logout', { method: 'POST' })
      .catch(function () { /* a missing or already revoked session is fine */ })
      .then(function () { clearSession(); });
  }

  function api(path, options) {
    options = options || {};
    return rawRequest(path, options).catch(function (error) {
      var canRetry = options.auth && error && error.status === 401 && !options.retried;
      if (!canRetry) throw error;
      return refreshSession().then(function (ok) {
        if (!ok) throw error;
        var retry = {};
        Object.keys(options).forEach(function (key) { retry[key] = options[key]; });
        retry.retried = true;
        return rawRequest(path, retry);
      });
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
    'Privacy policy consent is required': 'errors.consentRequired',
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
    refreshSession: refreshSession,
    restoreSession: restoreSession,
    logout: logout,
    errorMessage: errorMessage,
    formatDateTime: formatDateTime,
    formatNumber: formatNumber,
    formatPercent: formatPercent
  };
})();
