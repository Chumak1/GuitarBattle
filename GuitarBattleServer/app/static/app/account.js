/* fretfight — personal account: global rating, attempt history, profile */

(function () {
  'use strict';

  var FF = window.FF;
  var I18N = window.FFI18N;

  function t(key, vars) {
    return I18N ? I18N.t(key, vars) : key;
  }

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

  function applyMe(me) {
    state.me = me;
    document.getElementById('chipName').textContent = me.display_name || me.username;
    document.getElementById('welcomeLine').textContent =
      t('account.welcome', { username: me.username, email: me.email });
    document.getElementById('profileUsername').value = me.username;
    document.getElementById('profileDisplayName').value = me.display_name || '';
    document.getElementById('accountEmail').textContent = me.email;
    document.getElementById('accountSince').textContent = FF.formatDateTime(me.created_at);
  }

  function loadMe() {
    return FF.api('/auth/me', { auth: true }).then(applyMe);
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
        rankNote.textContent = t('account.stat.rankTop');
      } else {
        rankValue.textContent = '—';
        rankNote.textContent = t('account.stat.rankNone');
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
      badge.textContent = t('account.history.finished');
    } else if (status === 'started') {
      badge.className = 'badge badge--live';
      badge.textContent = t('account.history.playing');
    } else {
      badge.className = 'badge badge--slow';
      badge.textContent = status || '—';
    }
    statusTd.appendChild(badge);
    tr.appendChild(statusTd);

    tr.appendChild(cell(item.score_points === null ? '—' : FF.formatNumber(item.score_points), { className: 'num' }));
    tr.appendChild(cell(FF.formatPercent(item.accuracy_percent), { className: 'num' }));
    tr.appendChild(cell(item.max_combo === null ? '—' : FF.formatNumber(item.max_combo), { className: 'num' }));

    if (item.finished_at) tr.title = t('account.history.finishedAt', { date: FF.formatDateTime(item.finished_at) });
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

  /* ---------- achievements ---------- */

  var achievementsGrid = document.getElementById('achievementsGrid');
  var achievementsEmpty = document.getElementById('achievementsEmpty');
  var achievementsSummary = document.getElementById('achievementsSummary');

  function achievementIcon(item) {
    // Locked cards get a grey version of the same icon via CSS.
    var src = item.icon_url;
    if (!src && item.icon) src = '/media/achievements/' + item.icon;
    if (!src) return null;
    var img = document.createElement('img');
    img.src = src;
    img.alt = '';
    img.loading = 'lazy';
    img.decoding = 'async';
    img.addEventListener('error', function () {
      // A missing file must not leave a broken-image glyph in the grid.
      img.remove();
    });
    return img;
  }

  function achievementNode(item) {
    var card = document.createElement('article');
    card.className = 'ach' + (item.unlocked ? '' : ' ach--locked');

    var iconBox = document.createElement('div');
    iconBox.className = 'ach__icon';
    var icon = achievementIcon(item);
    if (icon) iconBox.appendChild(icon);
    card.appendChild(iconBox);

    var body = document.createElement('div');
    body.className = 'ach__body';

    var title = document.createElement('div');
    title.className = 'ach__title';
    title.textContent = item.title || item.code;
    body.appendChild(title);

    if (item.description) {
      var desc = document.createElement('div');
      desc.className = 'ach__desc';
      desc.textContent = item.description;
      body.appendChild(desc);
    }

    var meta = document.createElement('div');
    meta.className = 'ach__meta';

    if (item.xp_bonus) {
      var xp = document.createElement('span');
      xp.className = 'ach__xp';
      xp.textContent = '+' + FF.formatNumber(item.xp_bonus) + ' XP';
      meta.appendChild(xp);
    }

    if (item.unlocked) {
      if (item.awarded_at) {
        var date = document.createElement('span');
        date.textContent = t('account.achievements.unlockedAt', {
          date: FF.formatDateTime(item.awarded_at)
        });
        meta.appendChild(date);
      }
    } else {
      var locked = document.createElement('span');
      locked.textContent = t('account.achievements.locked');
      meta.appendChild(locked);
    }

    body.appendChild(meta);

    if (!item.unlocked && item.goal) {
      var goal = Number(item.goal) || 0;
      var progress = Math.max(0, Number(item.progress) || 0);
      var shown = Math.min(progress, goal);

      var progressText = document.createElement('div');
      progressText.className = 'ach__meta';
      progressText.textContent = t('account.achievements.progress', {
        progress: FF.formatNumber(shown),
        goal: FF.formatNumber(goal)
      });
      body.appendChild(progressText);

      var bar = document.createElement('div');
      bar.className = 'ach__bar';
      var fill = document.createElement('span');
      fill.style.width = (goal > 0 ? Math.min(100, Math.round((shown / goal) * 100)) : 0) + '%';
      bar.appendChild(fill);
      body.appendChild(bar);
    }

    card.appendChild(body);
    return card;
  }

  function renderAchievements(data) {
    var items = (data && data.items) || [];
    achievementsGrid.textContent = '';
    items.forEach(function (item) { achievementsGrid.appendChild(achievementNode(item)); });

    var unlocked = Number(data && data.unlocked) || 0;
    var total = Number(data && data.total) || items.length;
    achievementsSummary.textContent = total
      ? t('account.achievements.summary', {
          unlocked: FF.formatNumber(unlocked),
          total: FF.formatNumber(total)
        })
      : '';
    achievementsEmpty.classList.toggle('hidden', items.length > 0);
    if (!items.length) {
      achievementsEmpty.textContent = t('account.achievements.empty');
    }
  }

  function loadAchievements() {
    return FF.api('/achievements', { auth: true })
      .then(function (data) {
        renderAchievements(data);
      })
      .catch(function (error) {
        if (handleAuthError(error)) return;
        // Offline or a server error: keep the section usable and explain it.
        achievementsGrid.textContent = '';
        achievementsSummary.textContent = '';
        achievementsEmpty.textContent = t('account.achievements.unavailable');
        achievementsEmpty.classList.remove('hidden');
        throw error;
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
      showAlert('info', t('account.msg.noChanges'));
      return;
    }

    var button = profileForm.querySelector('button[type="submit"]');
    setBusy(button, true);
    FF.api('/auth/me', { method: 'PATCH', auth: true, body: payload })
      .then(function (me) {
        state.me = me;
        document.getElementById('chipName').textContent = me.display_name || me.username;
        document.getElementById('welcomeLine').textContent =
          t('account.welcome', { username: me.username, email: me.email });
        showAlert('ok', t('account.msg.profileUpdated'));
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
    // Revokes the refresh session server-side and clears the HttpOnly cookie.
    var buttons = [document.getElementById('logoutBtn'), document.getElementById('logoutBtn2')];
    buttons.forEach(function (button) { if (button) button.disabled = true; });
    FF.logout().then(function () { location.replace('/login'); });
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
          loadHistory(true).catch(function (error) { if (!handleAuthError(error)) throw error; }),
          loadAchievements().catch(function (error) { if (!handleAuthError(error)) throw error; })
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

  document.getElementById('achievementsRefresh').addEventListener('click', function () {
    var button = this;
    setBusy(button, true);
    loadAchievements()
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

  if (I18N) {
    I18N.onChange(function () {
      if (!FF.getToken()) return;
      if (state.me) loadMe().catch(function () { /* ignore */ });
      loadSummary().catch(function () { /* ignore */ });
      loadRating().catch(function () { /* ignore */ });
      loadHistory(true).catch(function () { /* ignore */ });
      loadAchievements().catch(function () { /* ignore */ });
    });
  }

  function boot() {
    // The access token lives in memory only, so every page load restores the
    // session from the HttpOnly refresh cookie before rendering anything.
    FF.restoreSession()
      .then(function (me) {
        if (!me) {
          location.replace('/login');
          return;
        }
        applyMe(me);
        return Promise.all([
          loadSummary().catch(function (error) { if (!handleAuthError(error)) throw error; }),
          loadRating().catch(function (error) { if (!handleAuthError(error)) throw error; }),
          loadHistory(true).catch(function (error) { if (!handleAuthError(error)) throw error; }),
          loadAchievements().catch(function (error) { if (!handleAuthError(error)) throw error; })
        ]);
      })
      .catch(function (error) {
        if (error && error.status === 0) {
          // Offline is not a sign-out: keep the session and explain it.
          showAlert('error', FF.errorMessage(error));
          return;
        }
        if (!handleAuthError(error)) showAlert('error', FF.errorMessage(error));
      });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }
})();
