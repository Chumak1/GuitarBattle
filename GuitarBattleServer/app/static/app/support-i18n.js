/* fretfight — support page dictionary (ru/en). Loaded after /app/i18n.js.
 *
 * Only the page chrome lives here (title, loading/error states, labels). The
 * content itself comes from /api/v1/support with ru/en fields and a ru fallback.
 */

window.FFI18N.add({
  ru: {
    'support.docTitle': 'Поддержка Fretfight — fretfight',
    'support.meta': 'Поддержать Fretfight: помощь развитию игровой платформы для гитаристов, музыкального каталога и будущих гитарных дуэлей.',
    'support.ogTitle': 'Поддержать Fretfight',
    'support.ogDescription': 'Помоги развивать Fretfight: игровой процесс, музыкальный каталог, соревновательные возможности и инфраструктуру.',
    'support.ogLocale': 'ru_RU',

    'support.navHome': 'На главную',
    'support.navLabel': 'Навигация по странице поддержки',
    'support.loading': 'Загружаем страницу…',
    'support.errorTitle': 'Не удалось загрузить страницу',
    'support.errorText': 'Проверьте соединение и попробуйте ещё раз. Контакты проекта остаются доступны ниже.',
    'support.retry': 'Попробовать снова',
    'support.methodsLabel': 'Способы поддержки',
    'support.faqLabel': 'Частые вопросы',
    'support.noscript': 'Для отображения страницы поддержки требуется JavaScript.',
    'support.footerNote': 'Fretfight — игра на настоящей гитаре. Поддержка не является покупкой и не даёт игровых преимуществ.',
    'support.externalNote': 'Открывается на внешнем сайте'
  },
  en: {
    'support.docTitle': 'Support Fretfight — fretfight',
    'support.meta': 'Support Fretfight: help build a game platform for guitarists, grow the music catalogue and make future guitar duels possible.',
    'support.ogTitle': 'Support Fretfight',
    'support.ogDescription': 'Help Fretfight grow: gameplay, the music catalogue, competitive features and infrastructure.',
    'support.ogLocale': 'en_US',

    'support.navHome': 'Home',
    'support.navLabel': 'Support page navigation',
    'support.loading': 'Loading the page…',
    'support.errorTitle': 'The page could not be loaded',
    'support.errorText': 'Check your connection and try again. The project contacts are still available below.',
    'support.retry': 'Try again',
    'support.methodsLabel': 'Support methods',
    'support.faqLabel': 'Frequently asked questions',
    'support.noscript': 'JavaScript is required to display the support page.',
    'support.footerNote': 'Fretfight is a game played on a real guitar. Support is not a purchase and gives no in-game advantage.',
    'support.externalNote': 'Opens on an external site'
  }
});
