/* fretfight — landing page controller.
 *
 * Fetches landing content overrides from the public API and paints them over the
 * static (translated) markup. The page stays fully usable when the API is down or
 * no admin overrides exist: every field falls back to the HTML text.
 */
(function () {
  'use strict';

  var I18N = window.FFI18N;
  var state = { data: null };

  function lang() { return I18N ? I18N.language() : 'ru'; }

  function text(el, value) {
    if (el && typeof value === 'string') el.textContent = value;
  }

  function block(key) {
    var raw = rawBlock(key);
    if (!raw) return null;
    var localized = raw[lang()] || null;
    return { visible: raw.visible !== false, data: localized };
  }

  function rawBlock(key) {
    if (!state.data || !state.data.blocks) return null;
    var raw = state.data.blocks[key];
    return raw && typeof raw === 'object' ? raw : null;
  }

  function setVisible(selector, visible) {
    var el = document.querySelector(selector);
    if (el) el.hidden = !visible;
  }

  function fillList(list, items) {
    if (!list || !Array.isArray(items)) return;
    while (list.firstChild) list.removeChild(list.firstChild);
    items.forEach(function (item) {
      var li = document.createElement('li');
      li.textContent = String(item == null ? '' : item);
      list.appendChild(li);
    });
  }

  function fillPanel(panel, data) {
    if (!panel || !data) return;
    text(panel.querySelector('h3'), data.title);
    if (!Array.isArray(data.items)) return;
    Array.prototype.forEach.call(panel.querySelectorAll('.spec'), function (row) { row.remove(); });
    data.items.forEach(function (item) {
      var row = document.createElement('div');
      row.className = 'spec';
      var label = document.createElement('span');
      label.textContent = String((item && item.label) || '');
      var value = document.createElement('b');
      value.textContent = String((item && item.value) || '');
      row.appendChild(label);
      row.appendChild(value);
      panel.appendChild(row);
    });
  }

  function fillParagraphs(container, paragraphs) {
    if (!container || !Array.isArray(paragraphs)) return;
    Array.prototype.forEach.call(container.querySelectorAll(':scope > p'), function (p) { p.remove(); });
    var anchor = container.querySelector('.section-head');
    paragraphs.forEach(function (value) {
      var p = document.createElement('p');
      p.textContent = String(value == null ? '' : value);
      if (anchor) {
        anchor.parentNode.insertBefore(p, anchor.nextSibling);
        anchor = p;
      } else {
        container.appendChild(p);
      }
    });
  }

  function fillCards(container, items, className, withIcon) {
    if (!container || !Array.isArray(items)) return;
    var icons = Array.prototype.map.call(container.children, function (child) {
      var icon = child.querySelector('.icon');
      return icon ? icon.textContent : '';
    });

    while (container.firstChild) container.removeChild(container.firstChild);

    items.forEach(function (item, index) {
      var card = document.createElement('article');
      card.className = className;
      if (withIcon) {
        var icon = document.createElement('div');
        icon.className = 'icon';
        icon.setAttribute('aria-hidden', 'true');
        icon.textContent = icons[index] || '♪';
        card.appendChild(icon);
      }
      var title = document.createElement('h3');
      title.textContent = String((item && item.title) || '');
      var body = document.createElement('p');
      body.textContent = String((item && item.text) || '');
      card.appendChild(title);
      card.appendChild(body);
      container.appendChild(card);
    });
  }

  function fillFacts(items) {
    var wrap = document.querySelector('.facts .wrap');
    if (!wrap || !Array.isArray(items)) return;
    while (wrap.firstChild) wrap.removeChild(wrap.firstChild);
    items.forEach(function (item) {
      var fact = document.createElement('div');
      fact.className = 'fact';
      var title = document.createElement('b');
      title.textContent = String((item && item.title) || '');
      var value = document.createElement('span');
      value.textContent = String((item && item.text) || '');
      fact.appendChild(title);
      fact.appendChild(value);
      wrap.appendChild(fact);
    });
  }

  /* ---------- screenshot gallery ---------- */

  // Only origin-relative uploads of this server are rendered: the API validates
  // them too, but the page does not trust a hand-edited CMS document.
  var SCREEN_PREFIX = '/media/landing/';

  function captionFor(item) {
    var ru = typeof item.caption_ru === 'string' ? item.caption_ru : '';
    var en = typeof item.caption_en === 'string' ? item.caption_en : '';
    return lang() === 'en' ? (en || ru) : (ru || en);
  }

  function renderScreens() {
    var section = document.getElementById('screens');
    var grid = document.getElementById('screensGallery');
    if (!section || !grid) return;

    var b = block('screens');
    if (b && b.data) {
      text(document.querySelector('#screens .kicker'), b.data.kicker);
      text(document.getElementById('screens-title'), b.data.title);
      text(document.querySelector('#screens .section-head > p:last-child'), b.data.text);
    }

    var items = state.data && Array.isArray(state.data.screenshots)
      ? state.data.screenshots
      : [];
    var shown = 0;
    grid.textContent = '';

    items.forEach(function (item) {
      if (!item || typeof item.url !== 'string' || item.url.indexOf(SCREEN_PREFIX) !== 0) return;
      var caption = captionFor(item);

      var figure = document.createElement('figure');
      figure.className = 'screen';

      var link = document.createElement('a');
      link.className = 'screen__link';
      link.href = item.url;
      link.target = '_blank';
      link.rel = 'noopener noreferrer';

      var image = document.createElement('img');
      image.src = item.url;
      image.alt = caption || (I18N ? I18N.t('landing.screens.title') : 'fretfight');
      image.loading = 'lazy';
      image.decoding = 'async';
      link.appendChild(image);
      figure.appendChild(link);

      if (caption) {
        var label = document.createElement('figcaption');
        label.textContent = caption;
        figure.appendChild(label);
      }

      grid.appendChild(figure);
      shown += 1;
    });

    var visible = shown > 0 && (!b || b.visible);
    section.hidden = !visible;
  }

  /* ---------- trailer ---------- */

  // Accepts a bare 11-character id or a YouTube link; everything else is
  // rejected so an arbitrary URL can never reach the iframe. Only the id is
  // ever injected into the embed URL.
  function youtubeId(value) {
    var raw = String(value == null ? '' : value).trim();
    if (/^[A-Za-z0-9_-]{11}$/.test(raw)) return raw;
    var url;
    try {
      url = new URL(raw);
    } catch (error) {
      return '';
    }
    if (url.protocol !== 'https:' && url.protocol !== 'http:') return '';
    var host = url.hostname.toLowerCase().replace(/^(www|m)\./, '');
    var id = '';
    if (host === 'youtu.be') {
      id = url.pathname.slice(1).split('/')[0];
    } else if (host === 'youtube.com') {
      if (url.pathname === '/watch') id = url.searchParams.get('v') || '';
      else if (url.pathname.indexOf('/shorts/') === 0) id = url.pathname.slice(8).split('/')[0];
      else if (url.pathname.indexOf('/embed/') === 0) id = url.pathname.slice(7).split('/')[0];
    }
    return /^[A-Za-z0-9_-]{11}$/.test(id) ? id : '';
  }

  function trailerId() {
    var holder = document.querySelector('[data-trailer]');
    var fallback = holder ? holder.getAttribute('data-video-id') || '' : '';
    var raw = rawBlock('trailer');
    if (!raw) return youtubeId(fallback);

    // The language of the page wins; an empty field falls back to the other
    // language before the built-in default.
    var order = [lang(), lang() === 'en' ? 'ru' : 'en'];
    for (var index = 0; index < order.length; index += 1) {
      var part = raw[order[index]];
      if (part && typeof part === 'object') {
        var id = youtubeId(part.url);
        if (id) return id;
      }
    }
    return youtubeId(fallback);
  }

  // YouTube's image CDN is unreachable in some regions. Walk the quality
  // chain and hide the image entirely when none of them loads: the gradient
  // card behind it then looks intentional instead of a black rectangle.
  var TRAILER_POSTERS = ['maxresdefault', 'sddefault', 'hqdefault'];

  function loadTrailerPoster(poster, id) {
    var attempt = 0;
    poster.hidden = false;
    poster.onerror = function () {
      attempt += 1;
      if (attempt >= TRAILER_POSTERS.length) {
        poster.hidden = true;
        return;
      }
      poster.src = 'https://i.ytimg.com/vi/' + id + '/' + TRAILER_POSTERS[attempt] + '.jpg';
    };
    poster.onload = function () { poster.hidden = false; };
    poster.removeAttribute('src');
    poster.src = 'https://i.ytimg.com/vi/' + id + '/' + TRAILER_POSTERS[0] + '.jpg';
  }

  function renderTrailer() {
    var holder = document.querySelector('[data-trailer]');
    var section = document.getElementById('trailer');
    if (!holder || !section) return;

    var b = block('trailer');
    if (b && b.data) {
      text(document.querySelector('#trailer .kicker'), b.data.kicker);
      text(document.getElementById('trailer-title'), b.data.title);
      text(document.querySelector('#trailer .section-head > p:last-child'), b.data.text);
    }
    var id = b && b.visible === false ? '' : trailerId();
    section.hidden = !id;
    if (!id) return;

    // The open link always points at the canonical watch page; it stays
    // visible under the player, so the trailer is reachable without the
    // facade click (and when the embed itself is blocked).
    var openLink = document.querySelector('[data-trailer-open]');
    if (openLink) openLink.href = 'https://www.youtube.com/watch?v=' + id;

    var existing = holder.querySelector('iframe');
    if (existing) holder.removeChild(existing);

    holder.dataset.videoId = id;
    var poster = holder.querySelector('[data-trailer-poster]');
    if (poster) loadTrailerPoster(poster, id);

    var link = holder.querySelector('[data-trailer-play]');
    if (link) {
      link.hidden = false;
      link.href = 'https://www.youtube.com/watch?v=' + id;
    }
  }

  // A click swaps the facade for the privacy-enhanced embed. Without
  // JavaScript the facade is a plain link to YouTube.
  function bindTrailer() {
    var holder = document.querySelector('[data-trailer]');
    var link = holder && holder.querySelector('[data-trailer-play]');
    if (!holder || !link) return;

    link.addEventListener('click', function (event) {
      var id = youtubeId(holder.getAttribute('data-video-id'));
      if (!id) return;
      event.preventDefault();

      var frame = document.createElement('iframe');
      frame.src = 'https://www.youtube-nocookie.com/embed/' + id + '?autoplay=1&rel=0';
      frame.title = link.getAttribute('aria-label') || 'YouTube';
      frame.setAttribute(
        'allow',
        'accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share'
      );
      frame.allowFullscreen = true;
      frame.setAttribute('referrerpolicy', 'strict-origin-when-cross-origin');
      holder.appendChild(frame);
      link.hidden = true;
      var poster = holder.querySelector('[data-trailer-poster]');
      if (poster) {
        // A late load must not bring the poster back under the iframe.
        poster.onload = null;
        poster.onerror = null;
        poster.hidden = true;
      }
    });
  }

  function applyHeroBackground() {
    var holder = document.getElementById('heroBg');
    var background = state.data && state.data.hero_background;
    if (!holder || !background || !background.url || background.type === 'none') return;

    holder.textContent = '';
    var media;
    if (background.type === 'video') {
      media = document.createElement('video');
      media.autoplay = true;
      media.muted = true;
      media.loop = true;
      media.playsInline = true;
      media.setAttribute('playsinline', '');
      if (background.poster_url) media.poster = background.poster_url;
    } else {
      media = document.createElement('img');
      media.alt = '';
      media.loading = 'eager';
    }
    media.src = background.url;
    holder.appendChild(media);
    holder.hidden = false;
  }

  function applyDownload() {
    var download = state.data && state.data.download;
    if (!download || !download.available) return;
    Array.prototype.forEach.call(document.querySelectorAll('[data-download]'), function (link) {
      link.hidden = false;
      link.href = download.url || '/api/v1/downloads/game';
      if (download.version) link.title = 'v' + download.version;
    });
  }

  /* ---------- macOS download ---------- */

  function formatSize(bytes) {
    var value = Number(bytes) || 0;
    if (value <= 0) return '';
    var megabytes = value / (1024 * 1024);
    if (megabytes >= 1024) {
      return (megabytes / 1024).toFixed(1).replace('.', ',') + ' GB';
    }
    return Math.round(megabytes) + ' MB';
  }

  function downloadMeta(info) {
    var parts = [];
    if (info.version) parts.push('v' + info.version);
    var size = formatSize(info.size_bytes);
    if (size) parts.push(size);
    if (!parts.length && info.filename) parts.push(info.filename);
    return parts.join(' · ');
  }

  // The macOS build is one universal binary, so there is no architecture
  // picker: the button is a plain link. The markup ships it disabled with a
  // "coming soon" hint; here it becomes a real download as soon as the API
  // reports a published macOS build.
  function applyDownloads() {
    var downloads = (state.data && state.data.downloads) || null;
    var info = downloads ? downloads.macos : null;
    var available = !!(info && info.available);
    var metaText = available
      ? downloadMeta(info)
      : (I18N ? I18N.t('landing.mac.soon') : '');

    Array.prototype.forEach.call(
      document.querySelectorAll('[data-download-macos]'),
      function (link) {
        if (available && info.url) link.href = info.url;

        if (available) {
          link.classList.remove('is-disabled');
          link.removeAttribute('aria-disabled');
          if (metaText) link.setAttribute('title', metaText);
        } else {
          link.classList.add('is-disabled');
          link.setAttribute('aria-disabled', 'true');
          link.removeAttribute('title');
        }

        var meta = link.querySelector('[data-download-macos-meta]');
        if (meta && metaText) meta.textContent = metaText;
      }
    );
  }

  // A link marked as disabled must not navigate, whether it is clicked or
  // activated from the keyboard.
  function bindMacDownload() {
    Array.prototype.forEach.call(
      document.querySelectorAll('[data-download-macos]'),
      function (link) {
        link.addEventListener('click', function (event) {
          if (link.getAttribute('aria-disabled') === 'true') event.preventDefault();
        });
      }
    );
  }

  function apply() {
    if (!state.data) return;
    var b;

    b = block('hero');
    if (b) {
      setVisible('.hero', b.visible);
      if (b.data) {
        text(document.querySelector('.hero .kicker'), b.data.kicker);
        // Only the wordmark is replaced: the <h1> also carries the tagline
        // ("fretfight — гитарный батл онлайн") that search engines index.
        text(
          document.querySelector('#hero-title .wordmark__brand') ||
            document.getElementById('hero-title'),
          b.data.title
        );
        text(document.querySelector('.hero .lead'), b.data.text);
      }
    }

    b = block('facts');
    if (b) {
      setVisible('.facts', b.visible);
      if (b.data) fillFacts(b.data.items);
    }

    b = block('modes');
    if (b) {
      setVisible('#modes', b.visible);
      if (b.data) {
        text(document.querySelector('#modes .kicker'), b.data.kicker);
        text(document.getElementById('modes-title'), b.data.title);
        text(document.querySelector('#modes .section-head > p:last-child'), b.data.text);
        fillCards(document.querySelector('#modes .cards'), b.data.cards, 'card', false);
      }
    }

    renderScreens();

    b = block('about');
    if (b) {
      setVisible('#about', b.visible);
      if (b.data) {
        text(document.querySelector('#about .kicker'), b.data.kicker);
        text(document.getElementById('about-title'), b.data.title);
        fillParagraphs(document.querySelector('.about'), b.data.paragraphs);
        fillList(document.querySelector('.about .tick-list'), b.data.items);
        fillPanel(document.querySelector('#about .panel'), b.data.panel);
      }
    }

    b = block('how');
    if (b) {
      setVisible('#how', b.visible);
      if (b.data) {
        text(document.querySelector('#how .kicker'), b.data.kicker);
        text(document.getElementById('how-title'), b.data.title);
        text(document.querySelector('#how .section-head > p:last-child'), b.data.text);
        fillCards(document.querySelector('#how .steps'), b.data.steps, 'step', false);
      }
    }

    b = block('features');
    if (b) {
      setVisible('#features', b.visible);
      if (b.data) {
        text(document.querySelector('#features .kicker'), b.data.kicker);
        text(document.getElementById('features-title'), b.data.title);
        fillCards(document.querySelector('#features .cards'), b.data.cards, 'card', true);
      }
    }

    b = block('requirements');
    if (b) {
      setVisible('#requirements', b.visible);
      if (b.data) {
        text(document.querySelector('#requirements .kicker'), b.data.kicker);
        text(document.getElementById('requirements-title'), b.data.title);
        text(document.querySelector('#requirements .section-head > p:last-child'), b.data.text);
        fillList(document.querySelector('#requirements .tick-list'), b.data.items);
        fillPanel(document.querySelector('#requirements .panel'), b.data.panel);
      }
    }

    b = block('support');
    if (b) {
      setVisible('#support-project', b.visible);
      if (b.data) {
        text(document.querySelector('#support-project .kicker'), b.data.kicker);
        text(document.getElementById('support-project-title'), b.data.title);
        text(
          document.querySelector('#support-project .support-project__copy > p:last-child'),
          b.data.text
        );
        // The button is a plain link: only its label is editable.
        text(document.querySelector('#support-project .support-project__action'), b.data.button);
      }
    }

    b = block('cta');
    if (b) {
      setVisible('.cta', b.visible);
      if (b.data) {
        text(document.getElementById('cta-title'), b.data.title);
        text(document.querySelector('.cta-inner > p'), b.data.text);
      }
    }

    applyHeroBackground();
    applyDownload();
    applyDownloads();
    renderTrailer();
  }

  fetch('/api/v1/landing', { credentials: 'omit', cache: 'no-store' })
    .then(function (response) {
      if (!response.ok) return null;
      return response.json();
    })
    .then(function (data) {
      if (!data) return;
      state.data = data;
      apply();
    })
    .catch(function () {
      /* Static content remains in place. */
    });

  if (I18N) I18N.onChange(apply);
  bindMacDownload();
  bindTrailer();
})();
