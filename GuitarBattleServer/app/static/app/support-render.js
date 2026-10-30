/* fretfight — shared renderer for the support page.
 *
 * Used by the public page (`support.js`) and by the admin preview, so the
 * editor shows exactly what visitors will get. Everything from the API is
 * inserted through a client-side allow-list sanitiser (defence in depth on top
 * of the server-side one): only text, the whitelisted tags and http(s) links
 * can reach the DOM, and scripts are never executed by the template parser.
 */
window.FFSupportRender = (function () {
  'use strict';

  var ALLOWED = {
    p: 1, br: 1, div: 1, span: 1, section: 1, h2: 1, h3: 1, h4: 1,
    ul: 1, ol: 1, li: 1, b: 1, strong: 1, i: 1, em: 1, a: 1,
    blockquote: 1, hr: 1
  };
  var VOID = { br: 1, hr: 1 };
  // Tags removed together with their content: a script body must never be
  // re-added as text, and media could leak a request.
  var DROP = {
    script: 1, style: 1, iframe: 1, object: 1, embed: 1, svg: 1, math: 1,
    template: 1, noscript: 1, img: 1, picture: 1, source: 1, video: 1,
    audio: 1, form: 1, input: 1, button: 1, textarea: 1, select: 1,
    link: 1, meta: 1, base: 1
  };

  var ICONS = {
    game: '<rect x="3" y="7.5" width="18" height="9" rx="4"/><path d="M8 10.5v3M6.5 12h3"/><circle cx="16" cy="11" r=".9"/><circle cx="18" cy="13.6" r=".9"/>',
    catalog: '<path d="M4 6h16M4 11.5h10M4 17h7"/><circle cx="18" cy="16" r="2.4"/><path d="M20.4 16V8l-2.7 1"/>',
    versus: '<path d="M3 12h7M7 8l4 4-4 4"/><path d="M21 12h-7M17 8l-4 4 4 4"/>',
    infra: '<rect x="4" y="4" width="16" height="6" rx="2"/><rect x="4" y="14" width="16" height="6" rx="2"/><path d="M7.5 7h.01M7.5 17h.01"/>',
    test: '<circle cx="12" cy="12" r="8"/><path d="m8.5 12.5 2.5 2.5 4.5-5"/>',
    bug: '<circle cx="12" cy="13" r="5"/><path d="M12 8V5M8 6l1.5 2M16 6l-1.5 2M7 13H4M20 13h-3M8 17l-1.5 2M16 17l1.5 2"/>',
    idea: '<path d="M9 18h6M10 21h4"/><path d="M12 3a6 6 0 0 0-3.6 10.8c.5.4.6.9.6 1.2h6c0-.3.1-.8.6-1.2A6 6 0 0 0 12 3z"/>',
    song: '<path d="M9 18V6l10-2v12"/><circle cx="7" cy="18" r="2.4"/><circle cx="17" cy="16" r="2.4"/>',
    share: '<circle cx="6" cy="12" r="2.4"/><circle cx="18" cy="6" r="2.4"/><circle cx="18" cy="18" r="2.4"/><path d="m8.2 10.9 7.6-3.7M8.2 13.1l7.6 3.7"/>',
    note: '<path d="m12 3 2.6 5.5 6 .8-4.4 4.2 1.1 6-5.3-3-5.3 3 1.1-6L3.4 9.3l6-.8z"/>'
  };

  function iconSvg(name) {
    var key = typeof name === 'string' && ICONS[name] ? name : 'note';
    var svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('viewBox', '0 0 24 24');
    svg.setAttribute('aria-hidden', 'true');
    svg.setAttribute('focusable', 'false');
    svg.classList.add('support-icon-svg');
    svg.innerHTML = ICONS[key];
    return svg;
  }

  function safeUrl(value) {
    if (typeof value !== 'string') return '';
    var trimmed = value.trim();
    return /^https?:\/\/[^\s]+$/i.test(trimmed) ? trimmed : '';
  }

  function copyChildren(source, target) {
    Array.prototype.forEach.call(source.childNodes, function (node) {
      if (node.nodeType === 3) {
        target.appendChild(document.createTextNode(node.nodeValue));
        return;
      }
      if (node.nodeType !== 1) return;
      var tag = node.tagName ? node.tagName.toLowerCase() : '';
      if (DROP[tag]) return;
      if (!ALLOWED[tag]) {
        copyChildren(node, target);
        return;
      }
      if (tag === 'a') {
        var href = safeUrl(node.getAttribute('href') || '');
        if (!href) {
          copyChildren(node, target);
          return;
        }
        var link = document.createElement('a');
        link.setAttribute('href', href);
        link.setAttribute('target', '_blank');
        link.setAttribute('rel', 'noopener noreferrer nofollow');
        copyChildren(node, link);
        target.appendChild(link);
        return;
      }
      if (VOID[tag]) {
        target.appendChild(document.createElement(tag));
        return;
      }
      var element = document.createElement(tag);
      copyChildren(node, element);
      target.appendChild(element);
    });
  }

  function fragment(html) {
    if (!html || typeof html !== 'string') return null;
    var template = document.createElement('template');
    // Parsing into a template never executes scripts; the walker below then
    // rebuilds the tree with only whitelisted nodes.
    template.innerHTML = html;
    var output = document.createDocumentFragment();
    copyChildren(template.content, output);
    return output;
  }

  function setHtml(node, html) {
    node.textContent = '';
    var content = fragment(html);
    if (content) node.appendChild(content);
  }

  function appendHtml(parent, tag, className, html) {
    if (!html) return null;
    var node = document.createElement(tag);
    if (className) node.className = className;
    setHtml(node, html);
    if (!node.childNodes.length) return null;
    parent.appendChild(node);
    return node;
  }

  function appendMethodButtons(container, methods, sectionKey) {
    if (!container || !Array.isArray(methods)) return;
    methods.forEach(function (method) {
      if (!method || !method.url) return;
      if (Array.isArray(method.sections) && method.sections.indexOf(sectionKey) === -1) return;
      var url = safeUrl(method.url);
      if (!url) return;
      var item = document.createElement('div');
      item.className = 'support-methods__item';
      var link = document.createElement('a');
      link.className = methodClass(method.style);
      link.href = url;
      if (method.open_in_new_tab !== false) {
        link.setAttribute('target', '_blank');
        link.setAttribute('rel', 'noopener noreferrer');
      }
      link.setAttribute('data-method-key', method.key || '');
      link.textContent = method.button_label || method.title || method.key || '';
      item.appendChild(link);
      if (method.description) {
        var note = document.createElement('p');
        note.className = 'support-methods__note';
        note.textContent = method.description;
        item.appendChild(note);
      }
      container.appendChild(item);
    });
  }

  function methodClass(style) {
    if (style === 'secondary') return 'btn btn--ghost support-method support-method--secondary';
    if (style === 'link') return 'support-method-link';
    return 'btn btn--primary support-method support-method--primary';
  }

  /* ---------- section builders ---------- */

  function sectionElement(className) {
    var section = document.createElement('section');
    section.className = 'support-section ' + className;
    return section;
  }

  function buildHero(data) {
    var section = sectionElement('support-hero');
    var wrap = document.createElement('div');
    wrap.className = 'support-hero__inner';

    appendHtml(wrap, 'p', 'kicker', data.kicker && data.kicker.visible ? data.kicker.html : '');
    appendHtml(wrap, 'h1', 'support-title', data.title && data.title.visible ? data.title.html : '');
    appendHtml(wrap, 'p', 'support-subtitle',
      data.subtitle && data.subtitle.visible ? data.subtitle.html : '');
    appendHtml(wrap, 'div', 'support-prose', data.text && data.text.visible ? data.text.html : '');
    appendHtml(wrap, 'div', 'support-prose support-prose--muted',
      data.extra_text && data.extra_text.visible ? data.extra_text.html : '');
    if (data.methods_visible && Array.isArray(data.methods) && data.methods.length) {
      var methods = document.createElement('div');
      methods.className = 'support-methods';
      methods.setAttribute('data-methods', 'hero');
      appendMethodButtons(methods, data.methods, 'hero');
      if (methods.childNodes.length) wrap.appendChild(methods);
    }
    if (!wrap.childNodes.length) return null;
    section.appendChild(wrap);
    return section;
  }

  function buildDuels(data) {
    var section = sectionElement('support-duels');
    var card = document.createElement('div');
    card.className = 'support-duels__card';
    var badge = appendHtml(card, 'p', 'support-badge',
      data.badge && data.badge.visible ? data.badge.html : '');
    if (badge) badge.setAttribute('role', 'note');
    appendHtml(card, 'h2', 'support-title', data.title && data.title.visible ? data.title.html : '');

    var paragraphs = data.paragraphs && data.paragraphs.visible ? data.paragraphs.items : null;
    if (Array.isArray(paragraphs) && paragraphs.length) {
      var prose = document.createElement('div');
      prose.className = 'support-prose support-prose--lead';
      paragraphs.forEach(function (item) { appendHtml(prose, 'p', '', item); });
      if (prose.childNodes.length) card.appendChild(prose);
    }

    appendHtml(card, 'div', 'support-prose',
      data.extra_text && data.extra_text.visible ? data.extra_text.html : '');
    appendHtml(card, 'p', 'support-closing',
      data.closing && data.closing.visible ? data.closing.html : '');

    var button = data.button || {};
    var label = button.label && button.label.visible ? button.label.html : '';
    if (button.visible !== false && label) {
      var link = document.createElement('a');
      link.className = 'btn btn--primary support-duels__button';
      link.setAttribute('href', button.url || '#support-methods');
      link.setAttribute('data-duels-button', '1');
      if (/^https?:\/\//i.test(button.url || '')) {
        link.setAttribute('target', '_blank');
        link.setAttribute('rel', 'noopener noreferrer');
      }
      setHtml(link, label);
      card.appendChild(link);
    }

    if (!card.childNodes.length) return null;
    section.appendChild(card);
    return section;
  }

  function buildCards(data) {
    var section = sectionElement('support-cards-section');
    var head = document.createElement('div');
    head.className = 'support-head';
    appendHtml(head, 'h2', 'support-title', data.title && data.title.visible ? data.title.html : '');
    appendHtml(head, 'div', 'support-prose', data.text && data.text.visible ? data.text.html : '');
    if (head.childNodes.length) section.appendChild(head);

    var cards = data.cards || {};
    if (cards.visible !== false && Array.isArray(cards.items) && cards.items.length) {
      var grid = document.createElement('div');
      grid.className = 'support-grid';
      cards.items.forEach(function (card) {
        var article = document.createElement('article');
        article.className = 'support-card';
        var title = card.title && card.title.html ? card.title.html : '';
        var text = card.text && card.text.html ? card.text.html : '';
        if (!title && !text) return;
        if (card.icon) {
          var badge = document.createElement('div');
          badge.className = 'support-card__icon';
          badge.setAttribute('aria-hidden', 'true');
          badge.appendChild(iconSvg(card.icon));
          article.appendChild(badge);
        }
        appendHtml(article, 'h3', 'support-card__title', title);
        appendHtml(article, 'div', 'support-card__text', text);
        var url = safeUrl(card.url);
        var label = card.link_label && card.link_label.html ? card.link_label.html : '';
        if (url && label) {
          var link = document.createElement('a');
          link.className = 'support-card__link';
          link.href = url;
          link.setAttribute('target', '_blank');
          link.setAttribute('rel', 'noopener noreferrer');
          setHtml(link, label);
          article.appendChild(link);
        }
        grid.appendChild(article);
      });
      if (grid.childNodes.length) section.appendChild(grid);
    }
    if (!section.childNodes.length) return null;
    return section;
  }

  function buildWhy(data) {
    var section = sectionElement('support-why');
    appendHtml(section, 'h2', 'support-title',
      data.title && data.title.visible ? data.title.html : '');

    var paragraphs = data.paragraphs && data.paragraphs.visible ? data.paragraphs.items : null;
    if (Array.isArray(paragraphs) && paragraphs.length) {
      var prose = document.createElement('div');
      prose.className = 'support-prose';
      paragraphs.forEach(function (item) { appendHtml(prose, 'p', '', item); });
      if (prose.childNodes.length) section.appendChild(prose);
    }

    var ideas = data.ideas && data.ideas.visible ? data.ideas.items : null;
    if (Array.isArray(ideas) && ideas.length) {
      var list = document.createElement('ul');
      list.className = 'support-ideas';
      ideas.forEach(function (item) {
        var li = document.createElement('li');
        li.className = 'support-ideas__item';
        var mark = document.createElement('span');
        mark.className = 'support-ideas__mark';
        mark.setAttribute('aria-hidden', 'true');
        mark.textContent = '\u2713';
        li.appendChild(mark);
        appendHtml(li, 'span', '', item);
        list.appendChild(li);
      });
      section.appendChild(list);
    }
    if (!section.childNodes.length) return null;
    return section;
  }

  function buildFaq(data, options) {
    var section = sectionElement('support-faq');
    appendHtml(section, 'h2', 'support-title',
      data.title && data.title.visible ? data.title.html : '');
    if (!Array.isArray(data.items) || !data.items.length) {
      return section.childNodes.length ? section : null;
    }

    var list = document.createElement('div');
    list.className = 'support-faq__list';
    data.items.forEach(function (item, index) {
      var question = item.question && item.question.html ? item.question.html : '';
      if (!question) return;
      var wrapper = document.createElement('div');
      wrapper.className = 'faq-item';

      var heading = document.createElement('h3');
      heading.className = 'faq-item__heading';
      var trigger = document.createElement('button');
      trigger.type = 'button';
      trigger.className = 'faq-trigger';
      trigger.id = 'faq-trigger-' + index;
      trigger.setAttribute('aria-expanded', 'false');
      trigger.setAttribute('aria-controls', 'faq-panel-' + index);
      var label = document.createElement('span');
      label.className = 'faq-question';
      setHtml(label, question);
      var sign = document.createElement('span');
      sign.className = 'faq-sign';
      sign.setAttribute('aria-hidden', 'true');
      trigger.appendChild(label);
      trigger.appendChild(sign);
      heading.appendChild(trigger);
      wrapper.appendChild(heading);

      var panel = document.createElement('div');
      panel.className = 'faq-panel';
      panel.id = 'faq-panel-' + index;
      panel.setAttribute('role', 'region');
      panel.setAttribute('aria-labelledby', trigger.id);
      panel.hidden = true;
      appendHtml(panel, 'div', 'support-prose', item.answer ? item.answer.html : '');
      wrapper.appendChild(panel);

      if (options && options.openFaq) {
        panel.hidden = false;
        trigger.setAttribute('aria-expanded', 'true');
      }
      list.appendChild(wrapper);
    });
    if (list.childNodes.length) section.appendChild(list);
    return section;
  }

  function buildCta(data) {
    var section = sectionElement('support-cta');
    var inner = document.createElement('div');
    inner.className = 'support-cta__inner';
    appendHtml(inner, 'h2', 'support-title',
      data.title && data.title.visible ? data.title.html : '');
    appendHtml(inner, 'div', 'support-prose',
      data.text && data.text.visible ? data.text.html : '');
    if (data.methods_visible && Array.isArray(data.methods) && data.methods.length) {
      var methods = document.createElement('div');
      methods.className = 'support-methods';
      methods.setAttribute('data-methods', 'cta');
      appendMethodButtons(methods, data.methods, 'cta');
      if (methods.childNodes.length) inner.appendChild(methods);
    }
    appendHtml(inner, 'p', 'support-slogan',
      data.slogan && data.slogan.visible ? data.slogan.html : '');
    if (!inner.childNodes.length) return null;
    section.appendChild(inner);
    return section;
  }

  var BUILDERS = {
    hero: buildHero,
    duels: buildDuels,
    usage: buildCards,
    help: buildCards,
    why: buildWhy,
    faq: buildFaq,
    cta: buildCta
  };

  function renderInto(container, data, options) {
    var opts = options || {};
    container.textContent = '';
    if (!data || typeof data !== 'object') return 0;

    var order = Array.isArray(data.order) ? data.order : [];
    var sections = data.sections && typeof data.sections === 'object' ? data.sections : {};
    var rendered = 0;

    order.forEach(function (key) {
      var section = sections[key];
      if (!section || section.visible === false) return;
      var builder = BUILDERS[key];
      if (!builder) return;
      var node = builder(section, opts);
      if (!node) return;
      node.setAttribute('data-section', key);
      container.appendChild(node);
      rendered += 1;
    });

    // The duel CTA links to "#support-methods": anchor the first visible group.
    var groups = container.querySelectorAll('[data-methods]');
    if (groups.length) {
      groups[0].id = 'support-methods';
      var anchors = container.querySelectorAll('[data-duels-button]');
      Array.prototype.forEach.call(anchors, function (link) {
        if ((link.getAttribute('href') || '') === '#support-methods') {
          link.setAttribute('href', '#' + groups[0].id);
        }
      });
    } else {
      var orphan = container.querySelectorAll('[data-duels-button]');
      Array.prototype.forEach.call(orphan, function (link) {
        if ((link.getAttribute('href') || '') === '#support-methods') {
          link.hidden = true;
        }
      });
    }

    if (opts.withCode !== false && data.code) {
      var code = document.createElement('div');
      code.className = 'support-section support-custom';
      if (data.code.custom_html) {
        setHtml(code, data.code.custom_html);
      }
      if (code.childNodes.length) container.appendChild(code);
    }

    return rendered;
  }

  return {
    renderInto: renderInto,
    sanitize: fragment,
    setHtml: setHtml,
    iconSvg: iconSvg,
    safeUrl: safeUrl
  };
})();
