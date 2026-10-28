/* fretfight — language selection (cookie) and UI dictionaries for public pages.
 *
 * Usage:
 *   <script src="/app/i18n.js"></script>          (define engine + common/account/auth dicts)
 *   <script src="/app/landing-i18n.js"></script>  (optional page dictionary, calls FFI18N.add)
 *   markup: data-i18n="key" / data-i18n-html="key" / data-i18n-placeholder="key"
 *           data-i18n-aria-label="key" / data-i18n-content="key" / data-i18n-title="key"
 *
 * The chosen language is stored in the `ff_lang` cookie. Without a cookie the
 * browser language is used for the first paint and then /api/v1/geo (local
 * DB-IP lookup on the server) provides the country-based default.
 */
window.FFI18N = (function () {
  'use strict';

  var COOKIE = 'ff_lang';
  var STORAGE = 'ff_lang';
  var dict = { ru: {}, en: {} };
  var current = null;
  var explicit = false;
  var initialised = false;
  var listeners = [];

  /* ---------- storage ---------- */

  function readCookie(name) {
    var match = document.cookie.match(new RegExp('(?:^|; )' + name + '=([^;]*)'));
    return match ? decodeURIComponent(match[1]) : null;
  }

  function writeCookie(name, value, days) {
    var expires = new Date(Date.now() + days * 864e5).toUTCString();
    document.cookie = name + '=' + encodeURIComponent(value) +
      '; expires=' + expires + '; path=/; SameSite=Lax';
  }

  function readStore() {
    try { return localStorage.getItem(STORAGE); } catch (e) { return null; }
  }

  function writeStore(value) {
    try { localStorage.setItem(STORAGE, value); } catch (e) { /* private mode */ }
  }

  /* ---------- dictionary ---------- */

  function add(source) {
    ['ru', 'en'].forEach(function (code) {
      var part = source && source[code];
      if (!part) return;
      Object.keys(part).forEach(function (key) { dict[code][key] = part[key]; });
    });
  }

  function t(key, vars) {
    var lang = current || 'ru';
    var text = dict[lang][key];
    if (text === undefined) text = dict.ru[key];
    if (text === undefined) return key;
    if (vars) {
      Object.keys(vars).forEach(function (name) {
        text = text.replace(new RegExp('\\{' + name + '\\}', 'g'), String(vars[name]));
      });
    }
    return text;
  }

  function raw(key, lang) {
    if (lang && dict[lang] && dict[lang][key] !== undefined) return dict[lang][key];
    if (dict.ru[key] !== undefined) return dict.ru[key];
    return '';
  }

  /* ---------- language ---------- */

  function guessLanguage() {
    var list = navigator.languages && navigator.languages.length
      ? navigator.languages
      : [navigator.language || 'ru'];
    for (var i = 0; i < list.length; i += 1) {
      var code = String(list[i] || '').toLowerCase();
      if (/^(ru|be|uk|kk|ky|uz|tg|tk)(-|$)/.test(code)) return 'ru';
      if (/^(en)(-|$)/.test(code)) return 'en';
    }
    return 'en';
  }

  function apply(root) {
    root = root || document;
    Array.prototype.forEach.call(root.querySelectorAll('[data-i18n]'), function (el) {
      el.textContent = t(el.getAttribute('data-i18n'));
    });
    Array.prototype.forEach.call(root.querySelectorAll('[data-i18n-html]'), function (el) {
      el.innerHTML = t(el.getAttribute('data-i18n-html'));
    });
    Array.prototype.forEach.call(root.querySelectorAll('[data-i18n-placeholder]'), function (el) {
      el.setAttribute('placeholder', t(el.getAttribute('data-i18n-placeholder')));
    });
    Array.prototype.forEach.call(root.querySelectorAll('[data-i18n-aria-label]'), function (el) {
      el.setAttribute('aria-label', t(el.getAttribute('data-i18n-aria-label')));
    });
    Array.prototype.forEach.call(root.querySelectorAll('[data-i18n-title]'), function (el) {
      el.setAttribute('title', t(el.getAttribute('data-i18n-title')));
    });
    Array.prototype.forEach.call(root.querySelectorAll('[data-i18n-content]'), function (el) {
      el.setAttribute('content', t(el.getAttribute('data-i18n-content')));
    });
    var titleKey = document.documentElement.getAttribute('data-title');
    if (titleKey) document.title = t(titleKey);
  }

  function paintButtons() {
    Array.prototype.forEach.call(document.querySelectorAll('[data-lang]'), function (button) {
      var active = button.getAttribute('data-lang') === current;
      button.classList.toggle('is-active', active);
      button.setAttribute('aria-pressed', active ? 'true' : 'false');
    });
  }

  function setLanguage(code, remember) {
    if (code !== 'ru' && code !== 'en') return;
    current = code;
    document.documentElement.lang = code;
    if (remember) {
      explicit = true;
      writeCookie(COOKIE, code, 365);
      writeStore(code);
    }
    apply();
    paintButtons();
    listeners.forEach(function (fn) {
      try { fn(code); } catch (e) { /* keep other listeners alive */ }
    });
  }

  function onChange(fn) {
    if (typeof fn === 'function') listeners.push(fn);
  }

  function detectByIp() {
    if (explicit) return;
    if (typeof fetch !== 'function') return;
    var controller = typeof AbortController === 'function' ? new AbortController() : null;
    var timer = controller ? setTimeout(function () { controller.abort(); }, 2500) : null;
    fetch('/api/v1/geo', {
      credentials: 'omit',
      signal: controller ? controller.signal : undefined
    }).then(function (response) {
      if (!response.ok) return null;
      return response.json();
    }).then(function (data) {
      if (timer) clearTimeout(timer);
      if (data && (data.language === 'ru' || data.language === 'en')) {
        setLanguage(data.language, true);
      }
    }).catch(function () {
      if (timer) clearTimeout(timer);
      /* Geo is a nicety: keep the browser-language default. */
    });
  }

  function bind() {
    document.addEventListener('click', function (event) {
      var button = event.target.closest ? event.target.closest('[data-lang]') : null;
      if (!button) return;
      event.preventDefault();
      setLanguage(button.getAttribute('data-lang'), true);
    });
    paintButtons();
  }

  function init() {
    if (initialised) return;
    if (document.documentElement.hasAttribute('data-i18n-off')) return;
    initialised = true;

    var stored = readCookie(COOKIE) || readStore();
    if (stored === 'ru' || stored === 'en') {
      current = stored;
      explicit = true;
    } else {
      current = guessLanguage();
    }
    document.documentElement.lang = current;
    apply();
    bind();
    detectByIp();
  }

  function language() { return current || 'ru'; }

  /* ---------- dictionaries ---------- */

  add({
    ru: {
      'common.home': 'На главную',
      'common.login': 'Войти',
      'common.policy': 'Политика конфиденциальности',
      'common.policyShort': 'Политика',
      'common.language': 'Язык сайта',
      'common.ru': 'Русский',
      'common.en': 'English',
      'common.retry': 'Попробовать ещё раз',
      'common.refresh': 'Обновить',
      'common.back': 'Назад',
      'common.loading': 'загрузка…',

      'contacts.title': 'Контакты',
      'contacts.name': 'Чумак Евгений Александрович',
      'contacts.emailLabel': 'Email',
      'contacts.telegramLabel': 'Telegram',

      'policy.docTitle': 'Политика конфиденциальности — fretfight',
      'policy.kicker': 'Документы',
      'policy.title': 'Политика конфиденциальности',
      'policy.updated': 'Дата вступления в силу: 25 октября 2026 года',

      'auth.docTitle.login': 'Вход — fretfight',
      'auth.docTitle.register': 'Регистрация — fretfight',
      'auth.meta': 'Вход и регистрация fretfight — гитарные битвы онлайн.',
      'auth.panel.login.title': 'С возвращением',
      'auth.panel.login.subtitle': 'Войдите, чтобы смотреть рейтинг и историю попыток.',
      'auth.panel.register.title': 'Создать аккаунт',
      'auth.panel.register.subtitle': 'Пара минут — и вы в общем рейтинге.',
      'auth.panel.verify.title': 'Подтвердите почту',
      'auth.panel.verify.subtitle': 'Мы отправили шестизначный код на ваш email.',
      'auth.panel.forgot.title': 'Забыли пароль?',
      'auth.panel.forgot.subtitle': 'Пришлём код для смены пароля на почту аккаунта.',
      'auth.panel.reset.title': 'Новый пароль',
      'auth.panel.reset.subtitle': 'Введите код из письма и придумайте новый пароль.',

      'auth.tab.login': 'Вход',
      'auth.tab.register': 'Регистрация',
      'auth.login.identifier': 'Email или логин',
      'auth.login.password': 'Пароль',
      'auth.login.submit': 'Войти',
      'auth.login.forgot': 'Забыли пароль?',
      'auth.login.verify': 'Подтвердить почту',
      'auth.register.username': 'Логин',
      'auth.register.usernameHint': '3–32 символа: латиница, цифры, точка, дефис, подчёркивание.',
      'auth.register.email': 'Email',
      'auth.register.displayName': 'Отображаемое имя <span class="muted">(необязательно)</span>',
      'auth.register.displayPlaceholder': 'Как вас показывать в рейтинге',
      'auth.register.password': 'Пароль',
      'auth.register.passwordPlaceholder': 'Минимум 10 символов',
      'auth.register.submit': 'Создать аккаунт',
      'auth.register.hint': 'После регистрации пришлём на почту шестизначный код подтверждения.',
      'auth.verify.email': 'Email',
      'auth.verify.code': 'Код из письма',
      'auth.verify.submit': 'Подтвердить и войти',
      'auth.verify.resend': 'Отправить код ещё раз',
      'auth.verify.back': 'Назад ко входу',
      'auth.forgot.email': 'Email аккаунта',
      'auth.forgot.submit': 'Отправить код',
      'auth.forgot.hint': 'Пришлём шестизначный код для смены пароля. Сам пароль по почте не отправляется.',
      'auth.reset.email': 'Email',
      'auth.reset.code': 'Код из письма',
      'auth.reset.password': 'Новый пароль',
      'auth.reset.submit': 'Сохранить пароль и войти',
      'auth.consent': 'Я согласен с <a href="/policy" target="_blank" rel="noopener">политикой конфиденциальности</a> и обработкой персональных данных.',
      'auth.consentRequired': 'Отметьте согласие с политикой конфиденциальности.',
      'auth.session.checking': 'Проверяем сохранённую сессию…',

      'auth.msg.unverified': 'Почта ещё не подтверждена. Введите код из письма или запросите новый.',
      'auth.msg.registered': 'Аккаунт создан. Код подтверждения отправлен на {email}.',
      'auth.msg.resent': 'Новый код отправлен на {email}.',
      'auth.msg.emailRequired': 'Сначала укажите email, на который пришёл код.',
      'auth.msg.resetSent': 'Если аккаунт с таким email существует, код уже отправлен.',
      'auth.msg.passwordChanged': 'Пароль изменён. Войдите с новым паролем.',
      'auth.msg.cooldown': '{label} ({seconds} с)',

      'account.docTitle': 'Личный кабинет — fretfight',
      'account.meta': 'Личный кабинет fretfight: рейтинг, история попыток и настройки аккаунта.',
      'account.title': 'Личный кабинет',
      'account.subtitle': 'Рейтинг, история попыток и настройки аккаунта.',
      'account.refresh': 'Обновить',
      'account.logout': 'Выйти',
      'account.logoutFull': 'Выйти из аккаунта',
      'account.stat.rating': 'Рейтинг',
      'account.stat.ratingNote': 'лучшие результаты по песням',
      'account.stat.rank': 'Место в рейтинге',
      'account.stat.rankNote': 'среди всех игроков',
      'account.stat.rankTop': 'в топ-50',
      'account.stat.rankNone': 'нет в топ-50',
      'account.stat.songs': 'Песен в зачёте',
      'account.stat.songsNote': 'минимум одна попытка',
      'account.stat.attempts': 'Попыток',
      'account.stat.attemptsNote': 'всего сыграно',
      'account.rating.title': 'Глобальный рейтинг',
      'account.rating.hint': 'Сумма лучших результатов по каждой песне. Топ-50.',
      'account.rating.empty': 'Пока никто не играл — самое время быть первым.',
      'account.rating.player': 'Игрок',
      'account.rating.points': 'Рейтинг',
      'account.rating.songs': 'Песен',
      'account.rating.accuracy': 'Точность',
      'account.history.title': 'История попыток',
      'account.history.hint': 'Ваши последние игры: очки, точность и максимальное комбо.',
      'account.history.empty': 'Попыток пока нет. Запустите песню в клиенте — результат появится здесь.',
      'account.history.song': 'Песня',
      'account.history.started': 'Начата',
      'account.history.status': 'Статус',
      'account.history.score': 'Очки',
      'account.history.combo': 'Комбо',
      'account.history.more': 'Показать ещё',
      'account.history.finished': 'завершена',
      'account.history.playing': 'в игре',
      'account.history.finishedAt': 'Завершена: {date}',
      'account.profile.title': 'Профиль',
      'account.profile.hint': 'Логин и имя видны в общем рейтинге.',
      'account.profile.username': 'Логин',
      'account.profile.displayName': 'Отображаемое имя',
      'account.profile.save': 'Сохранить профиль',
      'account.password.title': 'Пароль',
      'account.password.hint': 'После смены пароля все сессии завершатся.',
      'account.password.current': 'Текущий пароль',
      'account.password.new': 'Новый пароль',
      'account.password.submit': 'Сменить пароль',
      'account.account.title': 'Аккаунт',
      'account.account.email': 'Email',
      'account.account.since': 'С нами с',
      'account.welcome': 'Вы вошли как {username} · {email}',
      'account.msg.noChanges': 'Изменений нет.',
      'account.msg.profileUpdated': 'Профиль обновлён.',
      'account.achievements.title': 'Достижения',
      'account.achievements.hint': 'Открывайте достижения за точность, новые песни и испытание дня.',
      'account.achievements.empty': 'Достижений пока нет. Сыграйте первую песню в клиенте.',
      'account.achievements.unavailable': 'Не удалось загрузить достижения. Проверьте связь и обновите страницу.',
      'account.achievements.summary': 'Открыто {unlocked} из {total}',
      'account.achievements.unlockedAt': 'Получено {date}',
      'account.achievements.locked': 'Не открыто',
      'account.achievements.progress': 'Прогресс: {progress} из {goal}',

      'errors.generic': 'Что-то пошло не так. Попробуйте ещё раз.',
      'errors.network': 'Нет связи с сервером. Проверьте интернет и попробуйте снова.',
      'errors.checkFields': 'Проверьте поля — {details}.',
      'errors.badCredentials': 'Неверный email/логин или пароль.',
      'errors.emailNotVerified': 'Почта ещё не подтверждена. Введите код из письма.',
      'errors.usernameTaken': 'Этот логин или email уже заняты.',
      'errors.usernameOnlyTaken': 'Этот логин уже занят.',
      'errors.currentPassword': 'Текущий пароль неверен.',
      'errors.invalidCode': 'Неверный или просроченный код.',
      'errors.alreadyVerified': 'Почта уже подтверждена — можно войти.',
      'errors.emailUnavailable': 'Не удалось отправить письмо: почтовый сервис недоступен. Попробуйте позже.',
      'errors.registrationUnavailable': 'Регистрация временно недоступна.',
      'errors.consentRequired': 'Нужно согласие с политикой конфиденциальности.',
      'errors.tooMany': 'Слишком много попыток. Подождите немного.',
      'errors.badRequest': 'Проверьте введённые данные.',
      'errors.forbidden': 'Доступ запрещён. Возможно, нужно подтвердить почту.',
      'errors.notFound': 'Не найдено.',
      'errors.conflict': 'Этот логин или email уже заняты.',
      'errors.server': 'Ошибка на сервере. Попробуйте позже.',
      'errors.unavailable': 'Сервис временно недоступен. Попробуйте позже.',
      'errors.request': 'Ошибка запроса.',
      'errors.fieldRequired': 'обязательное поле',
      'errors.fieldMin': 'минимум {n} символов',
      'errors.fieldMax': 'максимум {n} символов',
      'errors.fieldPattern': 'недопустимые символы',
      'errors.fieldEmail': 'неверный email',
      'errors.fieldShort': 'слишком короткий',
      'errors.fieldValue': 'неверное значение',
      'errors.fieldFormat': 'неверный формат',
      'errors.field.email': 'email',
      'errors.field.username': 'логин',
      'errors.field.password': 'пароль',
      'errors.field.display_name': 'имя',
      'errors.field.code': 'код',
      'errors.field.new_password': 'новый пароль',
      'errors.field.current_password': 'текущий пароль',
      'errors.field.identifier': 'email или логин'
    },
    en: {
      'common.home': 'Home',
      'common.login': 'Sign in',
      'common.policy': 'Privacy Policy',
      'common.policyShort': 'Privacy',
      'common.language': 'Site language',
      'common.ru': 'Russian',
      'common.en': 'English',
      'common.retry': 'Try again',
      'common.refresh': 'Refresh',
      'common.back': 'Back',
      'common.loading': 'loading…',

      'contacts.title': 'Contacts',
      'contacts.name': 'Evgeniy Aleksandrovich Chumak',
      'contacts.emailLabel': 'Email',
      'contacts.telegramLabel': 'Telegram',

      'policy.docTitle': 'Privacy Policy — fretfight',
      'policy.kicker': 'Legal',
      'policy.title': 'Privacy Policy',
      'policy.updated': 'Effective date: 25 October 2026',

      'auth.docTitle.login': 'Sign in — fretfight',
      'auth.docTitle.register': 'Create account — fretfight',
      'auth.meta': 'Sign in or create a fretfight account — online guitar battles.',
      'auth.panel.login.title': 'Welcome back',
      'auth.panel.login.subtitle': 'Sign in to see the leaderboard and your attempt history.',
      'auth.panel.register.title': 'Create an account',
      'auth.panel.register.subtitle': 'A couple of minutes — and you are on the leaderboard.',
      'auth.panel.verify.title': 'Confirm your email',
      'auth.panel.verify.subtitle': 'We sent a six-digit code to your email.',
      'auth.panel.forgot.title': 'Forgot your password?',
      'auth.panel.forgot.subtitle': 'We will email a code to reset the password.',
      'auth.panel.reset.title': 'New password',
      'auth.panel.reset.subtitle': 'Enter the code from the email and choose a new password.',

      'auth.tab.login': 'Sign in',
      'auth.tab.register': 'Sign up',
      'auth.login.identifier': 'Email or username',
      'auth.login.password': 'Password',
      'auth.login.submit': 'Sign in',
      'auth.login.forgot': 'Forgot your password?',
      'auth.login.verify': 'Confirm email',
      'auth.register.username': 'Username',
      'auth.register.usernameHint': '3–32 characters: latin letters, digits, dot, hyphen, underscore.',
      'auth.register.email': 'Email',
      'auth.register.displayName': 'Display name <span class="muted">(optional)</span>',
      'auth.register.displayPlaceholder': 'How you appear on the leaderboard',
      'auth.register.password': 'Password',
      'auth.register.passwordPlaceholder': 'At least 6 characters',
      'auth.register.submit': 'Create account',
      'auth.register.hint': 'After registration we will email you a six-digit confirmation code.',
      'auth.verify.email': 'Email',
      'auth.verify.code': 'Code from the email',
      'auth.verify.submit': 'Confirm and sign in',
      'auth.verify.resend': 'Send the code again',
      'auth.verify.back': 'Back to sign in',
      'auth.forgot.email': 'Account email',
      'auth.forgot.submit': 'Send the code',
      'auth.forgot.hint': 'We will send a six-digit password reset code. The password itself is never emailed.',
      'auth.reset.email': 'Email',
      'auth.reset.code': 'Code from the email',
      'auth.reset.password': 'New password',
      'auth.reset.submit': 'Save password and sign in',
      'auth.consent': 'I agree to the <a href="/policy" target="_blank" rel="noopener">Privacy Policy</a> and to the processing of my personal data.',
      'auth.consentRequired': 'Please accept the Privacy Policy.',
      'auth.session.checking': 'Checking your saved session…',

      'auth.msg.unverified': 'Email is not confirmed yet. Enter the code from the email or request a new one.',
      'auth.msg.registered': 'Account created. A confirmation code was sent to {email}.',
      'auth.msg.resent': 'A new code was sent to {email}.',
      'auth.msg.emailRequired': 'Enter the email address the code was sent to.',
      'auth.msg.resetSent': 'If an account with this email exists, the code has already been sent.',
      'auth.msg.passwordChanged': 'Password changed. Sign in with the new password.',
      'auth.msg.cooldown': '{label} ({seconds}s)',

      'account.docTitle': 'Account — fretfight',
      'account.meta': 'Your fretfight account: rating, attempt history and account settings.',
      'account.title': 'Account',
      'account.subtitle': 'Your rating, attempt history and account settings.',
      'account.refresh': 'Refresh',
      'account.logout': 'Sign out',
      'account.logoutFull': 'Sign out of the account',
      'account.stat.rating': 'Rating',
      'account.stat.ratingNote': 'best results per song',
      'account.stat.rank': 'Leaderboard rank',
      'account.stat.rankNote': 'among all players',
      'account.stat.rankTop': 'in the top 50',
      'account.stat.rankNone': 'outside the top 50',
      'account.stat.songs': 'Songs counted',
      'account.stat.songsNote': 'at least one attempt',
      'account.stat.attempts': 'Attempts',
      'account.stat.attemptsNote': 'played in total',
      'account.rating.title': 'Global leaderboard',
      'account.rating.hint': 'The sum of your best result for each song. Top 50.',
      'account.rating.empty': 'Nobody has played yet — be the first.',
      'account.rating.player': 'Player',
      'account.rating.points': 'Rating',
      'account.rating.songs': 'Songs',
      'account.rating.accuracy': 'Accuracy',
      'account.history.title': 'Attempt history',
      'account.history.hint': 'Your latest games: score, accuracy and best combo.',
      'account.history.empty': 'No attempts yet. Play a song in the client and the result will appear here.',
      'account.history.song': 'Song',
      'account.history.started': 'Started',
      'account.history.status': 'Status',
      'account.history.score': 'Score',
      'account.history.combo': 'Combo',
      'account.history.more': 'Show more',
      'account.history.finished': 'finished',
      'account.history.playing': 'playing',
      'account.history.finishedAt': 'Finished: {date}',
      'account.profile.title': 'Profile',
      'account.profile.hint': 'Your username and name are visible on the leaderboard.',
      'account.profile.username': 'Username',
      'account.profile.displayName': 'Display name',
      'account.profile.save': 'Save profile',
      'account.password.title': 'Password',
      'account.password.hint': 'Changing the password signs out all sessions.',
      'account.password.current': 'Current password',
      'account.password.new': 'New password',
      'account.password.submit': 'Change password',
      'account.account.title': 'Account',
      'account.account.email': 'Email',
      'account.account.since': 'Member since',
      'account.welcome': 'Signed in as {username} · {email}',
      'account.msg.noChanges': 'Nothing changed.',
      'account.msg.profileUpdated': 'Profile updated.',
      'account.achievements.title': 'Achievements',
      'account.achievements.hint': 'Unlock achievements for accuracy, new songs and the daily challenge.',
      'account.achievements.empty': 'No achievements yet. Play your first song in the client.',
      'account.achievements.unavailable': 'Could not load achievements. Check your connection and reload the page.',
      'account.achievements.summary': '{unlocked} of {total} unlocked',
      'account.achievements.unlockedAt': 'Earned {date}',
      'account.achievements.locked': 'Locked',
      'account.achievements.progress': 'Progress: {progress} of {goal}',

      'errors.generic': 'Something went wrong. Please try again.',
      'errors.network': 'Cannot reach the server. Check your connection and try again.',
      'errors.checkFields': 'Check the fields — {details}.',
      'errors.badCredentials': 'Incorrect email/username or password.',
      'errors.emailNotVerified': 'Email is not confirmed yet. Enter the code from the email.',
      'errors.usernameTaken': 'That username or email is already taken.',
      'errors.usernameOnlyTaken': 'That username is already taken.',
      'errors.currentPassword': 'The current password is incorrect.',
      'errors.invalidCode': 'Invalid or expired code.',
      'errors.alreadyVerified': 'Email is already confirmed — you can sign in.',
      'errors.emailUnavailable': 'Could not send the email: the mail service is unavailable. Try again later.',
      'errors.registrationUnavailable': 'Registration is temporarily unavailable.',
      'errors.consentRequired': 'Please accept the Privacy Policy.',
      'errors.tooMany': 'Too many attempts. Try again later.',
      'errors.badRequest': 'Check the entered data.',
      'errors.forbidden': 'Access denied. You may need to confirm your email.',
      'errors.notFound': 'Not found.',
      'errors.conflict': 'That username or email is already taken.',
      'errors.server': 'Server error. Try again later.',
      'errors.unavailable': 'The service is temporarily unavailable. Try again later.',
      'errors.request': 'Request failed.',
      'errors.fieldRequired': 'required field',
      'errors.fieldMin': 'at least {n} characters',
      'errors.fieldMax': 'at most {n} characters',
      'errors.fieldPattern': 'invalid characters',
      'errors.fieldEmail': 'invalid email',
      'errors.fieldShort': 'too short',
      'errors.fieldValue': 'invalid value',
      'errors.fieldFormat': 'invalid format',
      'errors.field.email': 'email',
      'errors.field.username': 'username',
      'errors.field.password': 'password',
      'errors.field.display_name': 'display name',
      'errors.field.code': 'code',
      'errors.field.new_password': 'new password',
      'errors.field.current_password': 'current password',
      'errors.field.identifier': 'email or username'
    }
  });

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

  return {
    add: add,
    t: t,
    raw: raw,
    apply: apply,
    init: init,
    setLanguage: setLanguage,
    language: language,
    onChange: onChange
  };
})();
