/* fretfight — personal account: global rating, attempt history, profile */

(function () {
  'use strict';

  var FF = window.FF;

  var alertBox = document.getElementById('alert');
  var ratingBody = document.getElementById('ratingBody');
  var ratingEmpty = document.getElementById('ratingEmpty');
  var historyBody = document.getElementById('historyBody');
  var historyEmpty = document.getElementById('historyEmpty');
  var historyMore = document.getElementById('historyMore');

  var state = { me: null, historyOffset: 0 };
  var HISTORY_PAGE = 20;

  function showAlert(kind, text) {
    alertBox.className = 'alert alert--' + kind;
    alertBox.textContent = text;
  }

  function clearAlert() {
    alertBox.className = 'alert hidden';
    alertBox.textContent = '';
  }

  function handleAuthError(error) {
    if (error && error.status === 401) {
      FF.clearSession();
      location.replace('/login');
      return true;
    }
    return false;
  }

  function setBusy(button, busy) {
    button.disabled = busy;
    var spinner = button.querySelector('.btn__spinner');
    if (spinner) spinner.classList.toggle('hidden', !busy);
  }

  function cell(text, options) {
    options = options || {};
    var td = document.createElement('td');
    if (options.className) td.className = options.className;
    if (options.node) td.appendChild(options.node);
    else td.textContent = text;
    return td;
  }

  function rankNode(rank) {
    var span = document.createElement('span');
    span.className = 'rank' + (rank <= 3 ? ' rank--top' : '');
    span.textContent = rank;
    return span;
  }

  /* ---------- identity ---------- */

  function loadMe() {
    return FF.api('/auth/me', { auth: true }).then(function (me) {
      state.me = me;
      document.getElementById('chipName').textContent = me.display_name || me.username;
      document.getElementById('welcomeLine').textContent =
        'Вы вошли как ' + me.username + ' · ' + me.email;
      document.getElementById('profileUsername').value = me.username;
      document.getElementById('profileDisplayName').value = me.display_name || '';
      document.getElementById('accountEmail').textContent = me.email;
      document.getElementById('accountSince').textContent = FF.formatDateTime(me.created_at);
    });
  }

  /* ---------- personal summary ---------- */

  function loadSummary() {
    return FF.api('/games/best-scores', { auth: true }).then(function (data) {
      document.getElementById('statRating').textContent = FF.formatNumber(data.rating_points);
      document.getElementById('statSongs').textContent = FF.formatNumber(data.songs_played);
      document.getElementById('statAttempts').textContent = FF.formatNumber(data.games_played);
    });
  }

  /* ---------- global leaderboard ---------- */

  function loadRating() {
    return FF.api('/leaderboard/global?limit=50').then(function (data) {
      ratingBody.textContent = '';
      (data.items || []).forEach(function (row) {
        var tr = document.createElement('tr');
        if (state.me && row.username === state.me.username) tr.className = 'is-me';

        var rankTd = document.createElement('td');
        rankTd.appendChild(rankNode(row.rank));
        tr.appendChild(rankTd);

        var player = document.createElement('td');
        player.textContent = row.display_name || row.username;
        if (row.display_name && row.display_name !== row.username) {
          var handle = document.createElement('span');
          handle.className = 'muted';
          handle.textContent = ' @' + row.username;
          player.appendChild(handle);
        }
        tr.appendChild(player);

        tr.appendChild(cell(FF.formatNumber(row.rating_points), { className: 'num' }));
        tr.appendChild(cell(FF.formatNumber(row.games_played), { className: 'num' }));
        tr.appendChild(cell(FF.formatPercent(row.best_accuracy), { className: 'num' }));
        ratingBody.appendChild(tr);
      });

      var hasRows = !!(data.items && data.items.length);
      ratingEmpty.classList.toggle('hidden', hasRows);

      var rankValue = document.getElementById('statRank');
      var rankNote = document.getElementById('statRankNote');
      var mine = state.me
        ? (data.items || []).find(function (row) { return row.username === state.me.username; })
        : null;
      if (mine) {
        rankValue.textContent = '#' + mine.rank;
        rankNote.textContent = 'в топ-50';
      } else {
        rankValue.textContent = '—';
        rankNote.textContent = 'нет в топ-50';
      }
    });
  }

  /* ---------- attempt history ---------- */

  function historyRow(item) {
    var tr = document.createElement('tr');
    tr.appendChild(cell(item.song_title || '—'));

    tr.appendChild(cell(FF.formatDateTime(item.started_at), { className: 'muted' }));

    var statusTd = document.createElement('td');
    var badge = document.createElement('span');
    var status = item.status || '';
    if (status === 'finished') {
      badge.className = 'badge badge--done';
      badge.textContent = 'завершена';
    } else if (status === 'started') {
      badge.className = 'badge badge--live';
      badge.textContent = 'в игре';
    } else {
      badge.className = 'badge badge--slow';
      badge.textContent = status || '—';
    }
    statusTd.appendChild(badge);
    tr.appendChild(statusTd);

    tr.appendChild(cell(item.score_points === null ? '—' : FF.formatNumber(item.score_points), { className: 'num' }));
    tr.appendChild(cell(FF.formatPercent(item.accuracy_percent), { className: 'num' }));
    tr.appendChild(cell(item.max_combo === null ? '—' : FF.formatNumber(item.max_combo), { className: 'num' }));

    if (item.finished_at) tr.title = 'Завершена: ' + FF.formatDateTime(item.finished_at);
    return tr;
  }

  function loadHistory(reset) {
    if (reset) {
      state.historyOffset = 0;
      historyBody.textContent = '';
    }
    var offset = state.historyOffset;
    return FF.api('/games/history?limit=' + HISTORY_PAGE + '&offset=' + offset, { auth: true })
      .then(function (items) {
        items = items || [];
        items.forEach(function (item) { historyBody.appendChild(historyRow(item)); });
        state.historyOffset += items.length;
        historyEmpty.classList.toggle('hidden', state.historyOffset > 0);
        historyMore.classList.toggle('hidden', items.length < HISTORY_PAGE);
      });
  }

  /* ---------- profile ---------- */

  var profileForm = document.getElementById('profileForm');
  profileForm.addEventListener('submit', function (event) {
    event.preventDefault();
    if (!profileForm.reportValidity()) return;
    clearAlert();

    var username = document.getElementById('profileUsername').value.trim();
    var displayName = document.getElementById('profileDisplayName').value.trim();
    var payload = {};
    if (!state.me || username !== state.me.username) payload.username = username;
    if (!state.me || displayName !== (state.me.display_name || '')) payload.display_name = displayName;

    if (!Object.keys(payload).length) {
      showAlert('info', 'Изменений нет.');
      return;
    }

    var button = profileForm.querySelector('button[type="submit"]');
    setBusy(button, true);
    FF.api('/auth/me', { method: 'PATCH', auth: true, body: payload })
      .then(function (me) {
        state.me = me;
        document.getElementById('chipName').textContent = me.display_name || me.username;
        document.getElementById('welcomeLine').textContent =
          'Вы вошли как ' + me.username + ' · ' + me.email;
        showAlert('ok', 'Профиль обновлён.');
        return loadRating();
      })
      .catch(function (error) {
        if (handleAuthError(error)) return;
        showAlert('error', FF.errorMessage(error));
      })
      .then(function () { setBusy(button, false); });
  });

  /* ---------- password ---------- */

  var passwordForm = document.getElementById('passwordForm');
  passwordForm.addEventListener('submit', function (event) {
    event.preventDefault();
    if (!passwordForm.reportValidity()) return;
    clearAlert();

    var button = passwordForm.querySelector('button[type="submit"]');
    setBusy(button, true);
    FF.api('/auth/change-password', {
      method: 'POST',
      auth: true,
      body: {
        current_password: document.getElementById('currentPassword').value,
        new_password: document.getElementById('newPassword').value
      }
    }).then(function () {
      FF.clearSession();
      location.replace('/login?changed=1');
    }).catch(function (error) {
      if (handleAuthError(error)) return;
      showAlert('error', FF.errorMessage(error));
      setBusy(button, false);
    });
  });

  /* ---------- logout / refresh ---------- */

  function logout() {
    FF.clearSession();
    location.replace('/login');
  }

  document.getElementById('logoutBtn').addEventListener('click', logout);
  document.getElementById('logoutBtn2').addEventListener('click', logout);

  function refreshAll(button) {
    clearAlert();
    if (button) setBusy(button, true);
    loadMe()
      .then(function () {
        return Promise.all([
          loadSummary().catch(function (error) { if (!handleAuthError(error)) throw error; }),
          loadRating().catch(function (error) { if (!handleAuthError(error)) throw error; }),
          loadHistory(true).catch(function (error) { if (!handleAuthError(error)) throw error; })
        ]);
      })
      .catch(function (error) {
        if (!handleAuthError(error)) showAlert('error', FF.errorMessage(error));
      })
      .then(function () { if (button) setBusy(button, false); });
  }

  document.getElementById('refreshBtn').addEventListener('click', function () { refreshAll(this); });
  document.getElementById('ratingRefresh').addEventListener('click', function () {
    var button = this;
    setBusy(button, true);
    loadRating()
      .catch(function (error) { if (!handleAuthError(error)) showAlert('error', FF.errorMessage(error)); })
      .then(function () { setBusy(button, false); });
  });

  historyMore.addEventListener('click', function () {
    var button = this;
    button.disabled = true;
    loadHistory(false)
      .catch(function (error) {
        if (!handleAuthError(error)) showAlert('error', FF.errorMessage(error));
      })
      .then(function () { button.disabled = false; });
  });

  /* ---------- boot ---------- */

  if (!FF.getToken()) {
    location.replace('/login');
    return;
  }

  loadMe()
    .then(function () {
      return Promise.all([
        loadSummary().catch(function (error) { if (!handleAuthError(error)) throw error; }),
        loadRating().catch(function (error) { if (!handleAuthError(error)) throw error; }),
        loadHistory(true).catch(function (error) { if (!handleAuthError(error)) throw error; })
      ]);
    })
    .catch(function (error) {
      if (!handleAuthError(error)) showAlert('error', FF.errorMessage(error));
    });
})();
