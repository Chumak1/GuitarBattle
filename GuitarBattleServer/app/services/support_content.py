"""Support page content: defaults, validation, sanitising and rendering.

Stored content is a JSONB document::

    {
      "order": ["hero", "duels", ...],
      "sections": {"hero": {"visible": true, "title": {"ru": ..., "en": ...}}},
      "code": {"custom_css": "", "custom_html": ""}
    }

Text fields hold plain text with a deliberately small markup subset
(``**bold**``, ``*italic*``, ``[label](https://...)``, ``- list items``) and are
converted to HTML at read time. Raw HTML in a text field is therefore never
rendered: it is escaped. The only field that accepts HTML is ``custom_html``,
which is sanitised with an allow-list parser on save *and* again on read.

The module is intentionally independent from the ORM schemas so the public
renderer, the admin editor and the tests share exactly one implementation.
"""
from __future__ import annotations

import copy
import html
import json
import logging
import re
import uuid
from datetime import datetime, timezone
from html.parser import HTMLParser
from typing import Any, Iterable
from urllib.parse import urlsplit, urlunsplit

from sqlalchemy import select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.models.support_page import SupportMethod, SupportPage

logger = logging.getLogger(__name__)

SUPPORT_PAGE_ID = 1
SECTION_ORDER: tuple[str, ...] = (
    "hero",
    "duels",
    "usage",
    "help",
    "why",
    "faq",
    "cta",
)
TEXT_FIELDS: dict[str, tuple[str, ...]] = {
    "hero": ("kicker", "title", "subtitle", "text", "extra_text"),
    "duels": ("badge", "title", "extra_text", "closing"),
    "usage": ("title", "text"),
    "help": ("title", "text"),
    "why": ("title",),
    "faq": ("title",),
    "cta": ("title", "text", "slogan"),
}
LIST_FIELDS: dict[str, tuple[str, ...]] = {
    "duels": ("paragraphs",),
    "why": ("paragraphs", "ideas"),
}
CARD_SECTIONS: tuple[str, ...] = ("usage", "help")
METHOD_SECTIONS: tuple[str, ...] = ("hero", "cta")
BUTTON_STYLES: tuple[str, ...] = ("primary", "secondary", "link")

# Icons are drawn by ``support.js`` from a small local SVG set; an unknown
# token would render nothing, so the editor may only pick from this list.
ICON_KEYS: tuple[str, ...] = (
    "game",
    "catalog",
    "versus",
    "infra",
    "test",
    "bug",
    "idea",
    "song",
    "share",
    "note",
)

MAX_TEXT_LENGTH = 5_000
MAX_ITEMS = 60
MAX_CARDS = 12
MAX_FAQ_ITEMS = 40
MAX_CODE_LENGTH = 65_536
MAX_CONTENT_BYTES = 512 * 1024

# Only these tags survive ``sanitize_html``. Everything else is unwrapped (the
# text is kept) or, for the tags below, dropped together with its content.
ALLOWED_TAGS = frozenset(
    {
        "p",
        "br",
        "div",
        "span",
        "section",
        "h2",
        "h3",
        "h4",
        "ul",
        "ol",
        "li",
        "b",
        "strong",
        "i",
        "em",
        "a",
        "blockquote",
        "hr",
    }
)
VOID_TAGS = frozenset({"br", "hr"})
# Tags dropped as a whole. ``DROP_SELF_TAGS`` have no end tag, so they must not
# open a "skip everything until the closing tag" region.
DROP_SELF_TAGS = frozenset(
    {
        "img",
        "input",
        "link",
        "meta",
        "base",
        "source",
        "embed",
        "param",
        "track",
        "area",
        "col",
    }
)
DROP_CONTENT_TAGS = frozenset(
    {
        "script",
        "style",
        "iframe",
        "object",
        "embed",
        "svg",
        "math",
        "template",
        "noscript",
        "form",
        "input",
        "button",
        "textarea",
        "select",
        "link",
        "meta",
        "base",
        "video",
        "audio",
        "source",
        "picture",
        "img",
    }
)
FORBIDDEN_CSS_TOKENS: tuple[str, ...] = (
    "@import",
    "@font-face",
    "expression(",
    "javascript:",
    "vbscript:",
    "behavior:",
    "-moz-binding",
    "url(",
    "</style",
    "<script",
)

_LINK_RE = re.compile(r"\[([^\]\n]{1,200})\]\(([^)\s]{1,2048})\)")
_BOLD_RE = re.compile(r"\*\*(?=\S)([^\n]+?)(?<=\S)\*\*")
_ITALIC_RE = re.compile(r"(?<!\*)\*(?=\S)([^*\n]+?)(?<=\S)\*(?!\*)")
_UL_LINE_RE = re.compile(r"^\s*[-*]\s+(.+)$")
_OL_LINE_RE = re.compile(r"^\s*\d+[.)]\s+(.+)$")
_ANCHOR_RE = re.compile(r"^#[A-Za-z0-9_-]{1,64}$")


class SupportValidationError(ValueError):
    """A single field failed validation; ``str()`` names the field path."""

    def __init__(self, field: str, message: str) -> None:
        self.field = field
        self.message = message
        super().__init__(f"{field}: {message}")


# ---------------------------------------------------------------------------
# Default content (seeded once, never overwritten)
# ---------------------------------------------------------------------------
def _field(ru: str = "", en: str = "", visible: bool = True) -> dict[str, Any]:
    return {"visible": visible, "ru": ru, "en": en}


def _list_field(
    ru: Iterable[str] = (), en: Iterable[str] = (), visible: bool = True
) -> dict[str, Any]:
    return {"visible": visible, "ru": list(ru), "en": list(en)}


def _card(
    key: str,
    icon: str,
    title_ru: str,
    title_en: str,
    text_ru: str,
    text_en: str,
) -> dict[str, Any]:
    return {
        "id": key,
        "visible": True,
        "icon": icon,
        "title": {"ru": title_ru, "en": title_en},
        "text": {"ru": text_ru, "en": text_en},
        "url": "",
        "link_label": {"ru": "", "en": ""},
    }


def _faq(
    key: str, question_ru: str, question_en: str, answer_ru: str, answer_en: str
) -> dict[str, Any]:
    return {
        "id": key,
        "visible": True,
        "question": {"ru": question_ru, "en": question_en},
        "answer": {"ru": answer_ru, "en": answer_en},
    }


DEFAULT_CONTENT: dict[str, Any] = {
    "order": list(SECTION_ORDER),
    "sections": {
        "hero": {
            "visible": True,
            "kicker": _field("Поддержка проекта", "Support the project"),
            "title": _field("Поддержать Fretfight", "Support Fretfight"),
            "subtitle": _field(
                "Помоги создать место, где гитаристы доказывают своё мастерство",
                "Help build a place where guitarists prove their skill",
            ),
            "text": _field(
                "Fretfight — это игровая платформа для тех, кто любит играть на "
                "гитаре, хочет становиться лучше и стремится проверить свои навыки. "
                "Мы развиваем проект, чтобы сделать игру на настоящей гитаре "
                "увлекательнее, а прогресс — заметнее и интереснее",
                "Fretfight is a game platform for people who love playing guitar, "
                "want to get better and want to test their skills. We are building "
                "the project to make playing a real guitar more exciting and "
                "progress more visible and rewarding.",
            ),
            "extra_text": _field(
                "Если тебе близка идея Fretfight, ты можешь помочь проекту "
                "развиваться. Финансовая поддержка помогает уделять больше времени "
                "разработке, улучшать игровой опыт и двигаться к новым "
                "соревновательным возможностям",
                "If you like the idea of Fretfight, you can help the project grow. "
                "Financial support lets us spend more time on development, improve "
                "the game experience and move towards new competitive features.",
            ),
            "methods_visible": True,
        },
        "duels": {
            "visible": True,
            "badge": _field("Скоро", "Coming soon"),
            "title": _field(
                "Скоро в Fretfight — гитарные дуэли",
                "Coming to Fretfight — guitar duels",
            ),
            "paragraphs": _list_field(
                [
                    "Играть и улучшать свои результаты — здорово. Но ещё интереснее "
                    "проверить своё мастерство в противостоянии с другими "
                    "гитаристами.",
                    "Мы движемся к соревновательному формату, в котором игроки "
                    "смогут бросать вызов друг другу, играть на настоящих гитарах и "
                    "выяснять, кто лучше справляется с выбранной композицией",
                ],
                [
                    "Playing and improving your results is great. But testing your "
                    "skill against other guitarists is even more interesting.",
                    "We are moving towards a competitive format where players can "
                    "challenge each other, play real guitars and find out who "
                    "handles the chosen song better.",
                ],
            ),
            "extra_text": _field(
                "Точность исполнения, чувство ритма, стабильность и умение "
                "сохранять серию правильных нот должны иметь значение. Наша цель — "
                "сделать соревнование интересным как для начинающих гитаристов, так "
                "и для опытных музыкантов",
                "Accuracy, sense of rhythm, consistency and the ability to keep a "
                "streak of correct notes should matter. Our goal is to make the "
                "competition interesting for both beginner and experienced "
                "musicians.",
            ),
            "closing": _field(
                "Играй. Соревнуйся. Докажи своё мастерство.",
                "Play. Compete. Prove your skill.",
            ),
            "button": {
                "visible": True,
                "label": {
                    "ru": "Поддержать развитие Fretfight",
                    "en": "Support Fretfight's development",
                },
                "url": "#support-methods",
            },
        },
        "usage": {
            "visible": True,
            "title": _field(
                "На что пойдёт твоя поддержка",
                "What your support goes towards",
            ),
            "text": _field(
                "Поддержка помогает развивать Fretfight и делать платформу "
                "интереснее, удобнее и стабильнее",
                "Support helps us develop Fretfight and make the platform more "
                "interesting, convenient and stable.",
            ),
            "cards": {
                "visible": True,
                "items": [
                    _card(
                        "usage-development",
                        "game",
                        "Разработка и улучшение игры",
                        "Development and gameplay",
                        "Работа над игровым процессом, точностью определения нот, "
                        "удобством интерфейса и качеством взаимодействия с настоящей "
                        "гитарой",
                        "Work on the game loop, note detection accuracy, interface "
                        "convenience and the quality of interaction with a real "
                        "guitar.",
                    ),
                    _card(
                        "usage-catalog",
                        "catalog",
                        "Музыкальный каталог",
                        "Music catalogue",
                        "Развитие каталога композиций и улучшение возможностей игры "
                        "с разными песнями и уровнями сложности",
                        "Growing the song catalogue and improving the way you play "
                        "different songs and difficulty levels.",
                    ),
                    _card(
                        "usage-versus",
                        "versus",
                        "Соревновательная система",
                        "Competitive system",
                        "Работа над будущими дуэлями, соревновательными режимами и "
                        "возможностями сравнивать свои результаты с другими игроками",
                        "Work on future duels, competitive modes and ways to compare "
                        "your results with other players.",
                    ),
                    _card(
                        "usage-infra",
                        "infra",
                        "Инфраструктура и стабильность",
                        "Infrastructure and stability",
                        "Поддержка необходимых сервисов, улучшение производительности "
                        "и обеспечение стабильной работы платформы",
                        "Keeping the required services running, improving performance "
                        "and making the platform stable.",
                    ),
                ],
            },
        },
        "help": {
            "visible": True,
            "title": _field(
                "Помочь можно не только донатом",
                "You can help with more than money",
            ),
            "text": _field(
                "Даже если ты пока не можешь поддержать Fretfight финансово, есть "
                "другие способы помочь проекту",
                "Even if you cannot support Fretfight financially right now, there "
                "are other ways to help the project.",
            ),
            "cards": {
                "visible": True,
                "items": [
                    _card(
                        "help-test",
                        "test",
                        "Тестируй игру",
                        "Test the game",
                        "Играй, проверяй новые возможности и помогай находить "
                        "проблемы",
                        "Play, try new features and help us find problems.",
                    ),
                    _card(
                        "help-bug",
                        "bug",
                        "Сообщай об ошибках",
                        "Report bugs",
                        "Рассказывай о багах, неточностях определения нот и "
                        "неудобствах интерфейса",
                        "Tell us about bugs, note detection inaccuracies and "
                        "interface issues.",
                    ),
                    _card(
                        "help-idea",
                        "idea",
                        "Предлагай идеи",
                        "Share ideas",
                        "Делись идеями для новых игровых режимов, улучшений и "
                        "соревновательных возможностей",
                        "Share ideas for new game modes, improvements and "
                        "competitive features.",
                    ),
                    _card(
                        "help-song",
                        "song",
                        "Предлагай композиции",
                        "Suggest songs",
                        "Рассказывай, какие песни и музыкальные направления тебе "
                        "хотелось бы видеть в Fretfight",
                        "Tell us which songs and musical styles you would like to "
                        "see in Fretfight.",
                    ),
                    _card(
                        "help-share",
                        "share",
                        "Рассказывай о проекте",
                        "Spread the word",
                        "Делись Fretfight с друзьями-гитаристами, которым может быть "
                        "интересен такой формат игры",
                        "Share Fretfight with guitarist friends who might enjoy this "
                        "format.",
                    ),
                ],
            },
        },
        "why": {
            "visible": True,
            "title": _field(
                "Мастерство должно быть доказуемым",
                "Skill should be provable",
            ),
            "paragraphs": _list_field(
                [
                    "Гитара — это не только инструмент для тренировок. Это "
                    "возможность выражать себя, преодолевать трудности и становиться "
                    "лучше с каждой сыгранной композицией.",
                    "Fretfight стремится объединить обучение, игровой процесс и "
                    "соревнование в одном пространстве. Здесь хочется не просто "
                    "сыграть песню, а улучшить результат, побить собственный рекорд "
                    "и со временем проверить свои навыки против других гитаристов.",
                    "Мы хотим, чтобы у каждого игрока был повод возвращаться, видеть "
                    "свой прогресс и двигаться дальше — независимо от того, только "
                    "ли он начинает играть или уже уверенно владеет инструментом.",
                ],
                [
                    "The guitar is not just a tool for practice. It is a way to "
                    "express yourself, overcome difficulties and get better with "
                    "every song you play.",
                    "Fretfight aims to bring learning, gameplay and competition into "
                    "one space. Here you want not just to play a song, but to improve "
                    "your result, beat your own record and eventually test your "
                    "skills against other guitarists.",
                    "We want every player to have a reason to come back, see their "
                    "progress and keep moving forward — whether they are just "
                    "starting out or already play confidently.",
                ],
            ),
            "ideas": _list_field(
                [
                    "Учись и становиться лучше.",
                    "Играй и получай удовольствие.",
                    "Соревнуйся и доказывай своё мастерство.",
                ],
                [
                    "Learn and get better.",
                    "Play and enjoy it.",
                    "Compete and prove your skill.",
                ],
            ),
        },
        "faq": {
            "visible": True,
            "title": _field("Частые вопросы", "Frequently asked questions"),
            "items": [
                _faq(
                    "faq-about",
                    "Что такое Fretfight?",
                    "What is Fretfight?",
                    "Fretfight — игровая платформа для гитаристов, которая "
                    "объединяет игру на настоящей гитаре, развитие навыков и "
                    "соревновательную концепцию",
                    "Fretfight is a game platform for guitarists that combines "
                    "playing a real guitar, developing your skills and a competitive "
                    "concept.",
                ),
                _faq(
                    "faq-audience",
                    "Для кого предназначен проект?",
                    "Who is it for?",
                    "Для начинающих и опытных гитаристов. Идея проекта — дать "
                    "возможность играть, развиваться и улучшать свои результаты на "
                    "подходящем уровне сложности",
                    "For beginner and experienced guitarists. The idea is to let you "
                    "play, improve and beat your results at a suitable difficulty "
                    "level.",
                ),
                _faq(
                    "faq-support",
                    "Как поддержать Fretfight?",
                    "How can I support Fretfight?",
                    "Ты можешь поддержать проект через Boosty или DALink, "
                    "воспользовавшись кнопками на этой странице",
                    "You can support the project through Boosty or DALink using the "
                    "buttons on this page.",
                ),
                _faq(
                    "faq-usage",
                    "На что пойдёт поддержка?",
                    "What does the support go towards?",
                    "На развитие платформы: улучшение игрового процесса, музыкального "
                    "каталога, соревновательных возможностей и инфраструктуры",
                    "Platform development: gameplay, the music catalogue, "
                    "competitive features and infrastructure.",
                ),
                _faq(
                    "faq-duels",
                    "Когда появятся дуэли?",
                    "When will duels appear?",
                    "Гитарные дуэли входят в планы развития Fretfight. Точная дата "
                    "запуска будет объявлена отдельно, когда появится подтверждённая "
                    "информация",
                    "Guitar duels are part of the Fretfight roadmap. The exact "
                    "launch date will be announced separately once there is "
                    "confirmed information.",
                ),
                _faq(
                    "faq-contact",
                    "Как предложить идею или сообщить об ошибке?",
                    "How can I suggest an idea or report a bug?",
                    "Пока этот канал связи не настроен. Следи за обновлениями "
                    "проекта — контакты появятся здесь.",
                    "This contact channel is not set up yet. Watch for project "
                    "updates — contacts will appear here.",
                ),
            ],
        },
        "cta": {
            "visible": True,
            "title": _field(
                "Давай развивать Fretfight вместе",
                "Let's grow Fretfight together",
            ),
            "text": _field(
                "Каждый новый игрок, полезный отзыв и вклад в развитие проекта "
                "помогают Fretfight двигаться вперёд. Спасибо, что играешь, следишь "
                "за проектом и помогаешь ему расти",
                "Every new player, useful piece of feedback and contribution to the "
                "project helps Fretfight move forward. Thank you for playing, "
                "following the project and helping it grow.",
            ),
            "slogan": _field(
                "Play. Compete. Prove your skill.",
                "Play. Compete. Prove your skill.",
            ),
            "methods_visible": True,
        },
    },
    "code": {"custom_css": "", "custom_html": ""},
}

DEFAULT_METHODS: tuple[dict[str, Any], ...] = (
    {
        "key": "boosty",
        "title": "Boosty",
        "button_label": "Поддержать на Boosty",
        "url": "https://boosty.to/fretfight/donate",
        "description": "",
        "is_active": True,
        "open_in_new_tab": True,
        "sort_order": 0,
        "style": "primary",
        "sections": ["hero", "cta"],
    },
    {
        "key": "dalink",
        "title": "DALink",
        "button_label": "Поддержать через DALink",
        "url": "https://dalink.to/fretfight",
        "description": "",
        "is_active": True,
        "open_in_new_tab": True,
        "sort_order": 1,
        "style": "secondary",
        "sections": ["hero", "cta"],
    },
)


# ---------------------------------------------------------------------------
# Validation helpers
# ---------------------------------------------------------------------------
def _as_dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def _as_bool(value: Any, default: bool) -> bool:
    return value if isinstance(value, bool) else default


def _clean_text(
    value: Any, field: str, *, max_length: int = MAX_TEXT_LENGTH
) -> str:
    if value is None:
        return ""
    if not isinstance(value, str):
        raise SupportValidationError(field, "must be a string")
    text = value.replace("\r\n", "\n").replace("\r", "\n").strip()
    if len(text) > max_length:
        raise SupportValidationError(
            field, f"must be at most {max_length} characters"
        )
    if "\x7f" in text or any(
        ord(char) < 32 and char not in "\n\t" for char in text
    ):
        raise SupportValidationError(field, "must not contain control characters")
    return text


def normalize_external_url(
    value: Any,
    field: str,
    *,
    required: bool = False,
    allow_anchor: bool = False,
) -> str:
    """Accept only absolute http(s) URLs (optionally a same-page anchor).

    ``javascript:``, ``data:``, ``vbscript:``, ``file:``, protocol-relative
    URLs, credentials and empty hosts are rejected, so a stored link can never
    become script execution or an open redirect.
    """
    if value is None:
        text = ""
    elif isinstance(value, str):
        text = value.strip()
    else:
        raise SupportValidationError(field, "must be a string")

    if not text:
        if required:
            raise SupportValidationError(field, "is required")
        return ""
    if len(text) > 2048:
        raise SupportValidationError(field, "must be at most 2048 characters")
    if any(ord(char) < 32 or ord(char) == 127 for char in text):
        raise SupportValidationError(field, "must not contain control characters")

    if allow_anchor and text.startswith("#"):
        if not _ANCHOR_RE.match(text):
            raise SupportValidationError(
                field, "an anchor may only contain letters, digits, '-' and '_'"
            )
        return text

    # Imported lazily so the module keeps working on Python builds where the
    # url parser raises for exotic values; the call itself is cheap.
    try:
        parts = urlsplit(text)
    except ValueError as exc:
        raise SupportValidationError(field, "must be an absolute http(s) URL") from exc

    scheme = parts.scheme.lower()
    if scheme not in ("http", "https"):
        raise SupportValidationError(field, "must be an absolute http(s) URL")
    if parts.username is not None or parts.password is not None:
        raise SupportValidationError(field, "must not contain credentials")
    try:
        host = (parts.hostname or "").lower().rstrip(".")
    except ValueError as exc:
        raise SupportValidationError(field, "must be a valid http(s) URL") from exc
    if not host:
        raise SupportValidationError(field, "must include a host")

    return urlunsplit((scheme, parts.netloc, parts.path, parts.query, parts.fragment))


def _normalize_localized(raw: Any, field: str, *, max_length: int) -> dict[str, str]:
    if isinstance(raw, str):
        return {"ru": _clean_text(raw, field, max_length=max_length), "en": ""}
    data = _as_dict(raw)
    return {
        "ru": _clean_text(data.get("ru"), f"{field}.ru", max_length=max_length),
        "en": _clean_text(data.get("en"), f"{field}.en", max_length=max_length),
    }


def _normalize_text_field(raw: Any, field: str) -> dict[str, Any]:
    if isinstance(raw, str):
        return {
            "visible": True,
            "ru": _clean_text(raw, field),
            "en": "",
        }
    data = _as_dict(raw)
    return {
        "visible": _as_bool(data.get("visible"), True),
        "ru": _clean_text(data.get("ru"), f"{field}.ru"),
        "en": _clean_text(data.get("en"), f"{field}.en"),
    }


def _normalize_list_field(raw: Any, field: str) -> dict[str, Any]:
    if isinstance(raw, list):
        visible = True
        values = {"ru": raw, "en": []}
    else:
        data = _as_dict(raw)
        visible = _as_bool(data.get("visible"), True)
        values = {"ru": data.get("ru"), "en": data.get("en")}

    result: dict[str, Any] = {"visible": visible}
    for lang in ("ru", "en"):
        items = values.get(lang)
        if items is None:
            items = []
        if not isinstance(items, list):
            raise SupportValidationError(f"{field}.{lang}", "must be a list")
        if len(items) > MAX_ITEMS:
            raise SupportValidationError(
                f"{field}.{lang}", f"must contain at most {MAX_ITEMS} items"
            )
        cleaned: list[str] = []
        for index, item in enumerate(items):
            text = _clean_text(item, f"{field}.{lang}[{index}]")
            if text:
                cleaned.append(text)
        result[lang] = cleaned
    return result


def _normalize_id(raw: Any) -> str:
    if isinstance(raw, str):
        cleaned = re.sub(r"[^A-Za-z0-9_-]", "", raw.strip())[:64]
        if cleaned:
            return cleaned
    return uuid.uuid4().hex[:12]


def _normalize_button(raw: Any, field: str) -> dict[str, Any]:
    data = _as_dict(raw)
    return {
        "visible": _as_bool(data.get("visible"), True),
        "label": _normalize_localized(
            data.get("label"), f"{field}.label", max_length=200
        ),
        "url": normalize_external_url(
            data.get("url"), f"{field}.url", allow_anchor=True
        ),
    }


def _normalize_icon(raw: Any, field: str) -> str:
    icon = str(raw or "").strip().lower()
    if not icon:
        return ""
    if icon not in ICON_KEYS:
        raise SupportValidationError(
            field, "unknown icon; allowed: " + ", ".join(ICON_KEYS)
        )
    return icon


def _normalize_cards(raw: Any, field: str) -> dict[str, Any]:
    data = _as_dict(raw)
    items_raw = data.get("items")
    if items_raw is None and isinstance(raw, list):
        items_raw = raw
    if items_raw is None:
        items_raw = []
    if not isinstance(items_raw, list):
        raise SupportValidationError(f"{field}.items", "must be a list")
    if len(items_raw) > MAX_CARDS:
        raise SupportValidationError(
            f"{field}.items", f"must contain at most {MAX_CARDS} cards"
        )

    items: list[dict[str, Any]] = []
    seen: set[str] = set()
    for index, item in enumerate(items_raw):
        if not isinstance(item, dict):
            raise SupportValidationError(f"{field}.items[{index}]", "must be an object")
        prefix = f"{field}.items[{index}]"
        item_id = _normalize_id(item.get("id"))
        if item_id in seen:
            item_id = uuid.uuid4().hex[:12]
        seen.add(item_id)
        items.append(
            {
                "id": item_id,
                "visible": _as_bool(item.get("visible"), True),
                "icon": _normalize_icon(item.get("icon"), f"{prefix}.icon"),
                "title": _normalize_localized(
                    item.get("title"), f"{prefix}.title", max_length=200
                ),
                "text": _normalize_localized(
                    item.get("text"), f"{prefix}.text", max_length=MAX_TEXT_LENGTH
                ),
                "url": normalize_external_url(
                    item.get("url"), f"{prefix}.url"
                ),
                "link_label": _normalize_localized(
                    item.get("link_label"), f"{prefix}.link_label", max_length=120
                ),
            }
        )
    return {"visible": _as_bool(data.get("visible"), True), "items": items}


def _normalize_faq(raw: Any, field: str) -> list[dict[str, Any]]:
    if raw is None:
        raw = []
    if isinstance(raw, dict):
        raw = raw.get("items", [])
    if not isinstance(raw, list):
        raise SupportValidationError(field, "must be a list")
    if len(raw) > MAX_FAQ_ITEMS:
        raise SupportValidationError(
            field, f"must contain at most {MAX_FAQ_ITEMS} items"
        )

    items: list[dict[str, Any]] = []
    seen: set[str] = set()
    for index, item in enumerate(raw):
        if not isinstance(item, dict):
            raise SupportValidationError(f"{field}[{index}]", "must be an object")
        prefix = f"{field}[{index}]"
        item_id = _normalize_id(item.get("id"))
        if item_id in seen:
            item_id = uuid.uuid4().hex[:12]
        seen.add(item_id)
        items.append(
            {
                "id": item_id,
                "visible": _as_bool(item.get("visible"), True),
                "question": _normalize_localized(
                    item.get("question"), f"{prefix}.question", max_length=300
                ),
                "answer": _normalize_localized(
                    item.get("answer"), f"{prefix}.answer", max_length=MAX_TEXT_LENGTH
                ),
            }
        )
    return items


def normalize_custom_css(value: Any) -> str:
    """Reject CSS that could load resources or execute legacy script."""
    if value is None:
        return ""
    if not isinstance(value, str):
        raise SupportValidationError("code.custom_css", "must be a string")
    text = value.replace("\r\n", "\n").strip()
    if len(text) > MAX_CODE_LENGTH:
        raise SupportValidationError(
            "code.custom_css", f"must be at most {MAX_CODE_LENGTH} characters"
        )
    lowered = text.lower()
    for token in FORBIDDEN_CSS_TOKENS:
        if token in lowered:
            raise SupportValidationError(
                "code.custom_css", f"must not contain {token!r}"
            )
    return text


def normalize_code(raw: Any) -> dict[str, str]:
    data = _as_dict(raw)
    custom_html_raw = _clean_text(
        data.get("custom_html"), "code.custom_html", max_length=MAX_CODE_LENGTH
    )
    return {
        "custom_css": normalize_custom_css(data.get("custom_css")),
        "custom_html": sanitize_html(custom_html_raw),
    }


def normalize_order(raw: Any) -> list[str]:
    order: list[str] = []
    if isinstance(raw, list):
        for key in raw:
            if isinstance(key, str) and key in SECTION_ORDER and key not in order:
                order.append(key)
    order.extend(key for key in SECTION_ORDER if key not in order)
    return order


def normalize_content(raw: Any) -> dict[str, Any]:
    """Validate and canonicalise a whole document (used on every write)."""
    if not isinstance(raw, dict):
        raise SupportValidationError("content", "must be an object")

    raw_sections = raw.get("sections")
    sections_in = raw_sections if isinstance(raw_sections, dict) else {}
    sections: dict[str, Any] = {}

    for key in SECTION_ORDER:
        data = _as_dict(sections_in.get(key))
        prefix = f"sections.{key}"
        section: dict[str, Any] = {"visible": _as_bool(data.get("visible"), True)}
        for field in TEXT_FIELDS[key]:
            section[field] = _normalize_text_field(data.get(field), f"{prefix}.{field}")
        for field in LIST_FIELDS.get(key, ()):
            section[field] = _normalize_list_field(data.get(field), f"{prefix}.{field}")
        if key == "duels":
            section["button"] = _normalize_button(
                data.get("button"), f"{prefix}.button"
            )
        if key in CARD_SECTIONS:
            section["cards"] = _normalize_cards(data.get("cards"), f"{prefix}.cards")
        if key == "faq":
            section["items"] = _normalize_faq(data.get("items"), f"{prefix}.items")
        if key in METHOD_SECTIONS:
            section["methods_visible"] = _as_bool(data.get("methods_visible"), True)
        sections[key] = section

    content = {
        "order": normalize_order(raw.get("order")),
        "sections": sections,
        "code": normalize_code(raw.get("code")),
    }
    _check_content_size(content)
    return content


def _check_content_size(content: dict[str, Any]) -> None:
    try:
        serialized = json.dumps(
            content, ensure_ascii=False, separators=(",", ":"), allow_nan=False
        )
    except (TypeError, ValueError) as exc:
        raise SupportValidationError(
            "content", "must be JSON-serialisable"
        ) from exc
    if len(serialized.encode("utf-8")) > MAX_CONTENT_BYTES:
        raise SupportValidationError("content", "exceeds the 512 KiB limit")


# ---------------------------------------------------------------------------
# HTML sanitising and text rendering
# ---------------------------------------------------------------------------
class _Sanitizer(HTMLParser):
    """Allow-list HTML sanitiser built on the standard library parser."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.parts: list[str] = []
        self.stack: list[str] = []
        self.drop_depth = 0

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        tag = tag.lower()
        if tag in DROP_CONTENT_TAGS:
            if tag not in DROP_SELF_TAGS:
                self.drop_depth += 1
            return
        if self.drop_depth or tag not in ALLOWED_TAGS:
            return

        if tag == "a":
            href = ""
            for name, value in attrs:
                if name.lower() == "href" and value:
                    href = value
                    break
            try:
                safe_href = normalize_external_url(href, "link")
            except SupportValidationError:
                safe_href = ""
            if not safe_href:
                return  # Keep the link text, drop the tag.
            self.parts.append(
                '<a href="'
                + html.escape(safe_href, quote=True)
                + '" rel="noopener noreferrer nofollow" target="_blank">'
            )
            self.stack.append("a")
            return

        if tag in VOID_TAGS:
            self.parts.append(f"<{tag}>")
            return

        self.parts.append(f"<{tag}>")
        self.stack.append(tag)

    def handle_startendtag(
        self, tag: str, attrs: list[tuple[str, str | None]]
    ) -> None:
        self.handle_starttag(tag, attrs)
        if tag.lower() not in VOID_TAGS:
            self.handle_endtag(tag)

    def handle_endtag(self, tag: str) -> None:
        tag = tag.lower()
        if tag in DROP_CONTENT_TAGS:
            if tag not in DROP_SELF_TAGS and self.drop_depth:
                self.drop_depth -= 1
            return
        if self.drop_depth or tag in VOID_TAGS or tag not in ALLOWED_TAGS:
            return
        if tag not in self.stack:
            return
        while self.stack:
            current = self.stack.pop()
            self.parts.append(f"</{current}>")
            if current == tag:
                return

    def handle_data(self, data: str) -> None:
        if self.drop_depth:
            return
        self.parts.append(html.escape(data, quote=False))

    def result(self) -> str:
        while self.stack:
            self.parts.append(f"</{self.stack.pop()}>")
        return "".join(self.parts)


def sanitize_html(value: str) -> str:
    if not value:
        return ""
    parser = _Sanitizer()
    try:
        parser.feed(value)
        parser.close()
    except Exception:  # pragma: no cover - a parser bug must not break a page
        logger.warning("Support HTML could not be parsed; the fragment was dropped")
        return ""
    return parser.result()


def _inline_markup(text: str) -> str:
    """Escape text, then apply the tiny markup subset (never raw HTML)."""
    escaped = html.escape(text, quote=False)

    def replace_link(match: re.Match[str]) -> str:
        label = match.group(1)
        raw_url = html.unescape(match.group(2))
        try:
            url = normalize_external_url(raw_url, "link")
        except SupportValidationError:
            return match.group(0)
        if not url:
            return match.group(0)
        return (
            '<a href="'
            + html.escape(url, quote=True)
            + '" rel="noopener noreferrer nofollow" target="_blank">'
            + label
            + "</a>"
        )

    escaped = _LINK_RE.sub(replace_link, escaped)
    escaped = _BOLD_RE.sub(r"<strong>\1</strong>", escaped)
    escaped = _ITALIC_RE.sub(r"<em>\1</em>", escaped)
    return escaped


def render_text(value: Any) -> str:
    """Render stored plain text with the markup subset into safe HTML."""
    if not isinstance(value, str):
        return ""
    text = value.replace("\r\n", "\n").replace("\r", "\n").strip()
    if not text:
        return ""
    if len(text) > MAX_TEXT_LENGTH:
        text = text[:MAX_TEXT_LENGTH]

    lines = [line.rstrip() for line in text.split("\n")]
    blocks: list[list[str]] = []
    current: list[str] = []
    for line in lines:
        if not line.strip():
            if current:
                blocks.append(current)
                current = []
            continue
        current.append(line.strip())
    if current:
        blocks.append(current)

    if len(blocks) == 1 and not any(
        _UL_LINE_RE.match(line) or _OL_LINE_RE.match(line) for line in blocks[0]
    ):
        # A single paragraph stays inline so it can live inside headings.
        return sanitize_html("<br>".join(_inline_markup(line) for line in blocks[0]))

    parts: list[str] = []
    for block in blocks:
        if all(_UL_LINE_RE.match(line) for line in block):
            items = [_UL_LINE_RE.match(line).group(1) for line in block]
            parts.append(
                "<ul>"
                + "".join(f"<li>{_inline_markup(item)}</li>" for item in items)
                + "</ul>"
            )
        elif all(_OL_LINE_RE.match(line) for line in block):
            items = [_OL_LINE_RE.match(line).group(1) for line in block]
            parts.append(
                "<ol>"
                + "".join(f"<li>{_inline_markup(item)}</li>" for item in items)
                + "</ol>"
            )
        else:
            parts.append(
                "<p>"
                + "<br>".join(_inline_markup(line) for line in block)
                + "</p>"
            )
    return sanitize_html("".join(parts))


# ---------------------------------------------------------------------------
# Reading and rendering
# ---------------------------------------------------------------------------
def read_content(raw: Any) -> dict[str, Any]:
    """Leniently read a stored document; a broken row never breaks a page."""
    if not isinstance(raw, dict) or not raw:
        return normalize_content(copy.deepcopy(DEFAULT_CONTENT))
    try:
        content = normalize_content(raw)
    except SupportValidationError:
        logger.warning("Stored support page content is invalid; using defaults")
        return normalize_content(copy.deepcopy(DEFAULT_CONTENT))

    # A section that is absent from the stored document stays hidden instead of
    # being re-materialised with default buttons: legacy or hand-written
    # documents must not resurrect content the editor never saved.
    raw_sections = raw.get("sections")
    present = raw_sections if isinstance(raw_sections, dict) else {}
    for key in SECTION_ORDER:
        if key not in present:
            content["sections"][key]["visible"] = False
    return content


def _localized(field: dict[str, Any], lang: str) -> str:
    value = field.get(lang)
    if not isinstance(value, str) or not value.strip():
        fallback = field.get("ru")
        return fallback if isinstance(fallback, str) else ""
    return value


def _localized_list(field: dict[str, Any], lang: str) -> list[str]:
    value = field.get(lang)
    if not isinstance(value, list) or not value:
        value = field.get("ru")
    if not isinstance(value, list):
        return []
    return [item for item in value if isinstance(item, str) and item.strip()]


def _render_field(field: Any, lang: str) -> dict[str, Any]:
    data = _as_dict(field)
    visible = _as_bool(data.get("visible"), True)
    return {
        "visible": visible,
        "html": render_text(_localized(data, lang)) if visible else "",
    }


def _render_list(field: Any, lang: str) -> dict[str, Any]:
    data = _as_dict(field)
    visible = _as_bool(data.get("visible"), True)
    items = _localized_list(data, lang) if visible else []
    return {
        "visible": visible,
        "items": [render_text(item) for item in items],
    }


def method_public_dict(method: SupportMethod) -> dict[str, Any]:
    sections = [
        key
        for key in (method.sections or [])
        if isinstance(key, str) and key in METHOD_SECTIONS
    ]
    return {
        "key": method.key,
        "title": method.title or "",
        "button_label": method.button_label or method.title or method.key,
        "url": method.url,
        "description": method.description or "",
        "open_in_new_tab": bool(method.open_in_new_tab),
        "style": method.style if method.style in BUTTON_STYLES else "primary",
        "sections": sections,
    }


def methods_for_section(
    methods: Iterable[SupportMethod], section: str
) -> list[dict[str, Any]]:
    active = [method for method in methods if method.is_active]
    active.sort(key=lambda item: (item.sort_order, item.id))
    return [
        method_public_dict(method)
        for method in active
        if section in (method.sections or [])
    ]


def _render_cards(field: Any, lang: str) -> dict[str, Any]:
    data = _as_dict(field)
    visible = _as_bool(data.get("visible"), True)
    items = data.get("items")
    rendered: list[dict[str, Any]] = []
    if visible and isinstance(items, list):
        for item in items:
            if not isinstance(item, dict) or item.get("visible") is False:
                continue
            title = _localized(_as_dict(item.get("title")), lang)
            text = _localized(_as_dict(item.get("text")), lang)
            if not title.strip() and not text.strip():
                continue
            rendered.append(
                {
                    "id": item.get("id") or "",
                    "icon": item.get("icon") or "",
                    "title": {"html": render_text(title)},
                    "text": {"html": render_text(text)},
                    "url": item.get("url") or "",
                    "link_label": {
                        "html": render_text(_localized(_as_dict(item.get("link_label")), lang))
                    },
                }
            )
    return {"visible": visible, "items": rendered}


def _render_faq(field: Any, lang: str) -> list[dict[str, Any]]:
    if not isinstance(field, list):
        return []
    rendered: list[dict[str, Any]] = []
    for item in field:
        if not isinstance(item, dict) or item.get("visible") is False:
            continue
        question = _localized(_as_dict(item.get("question")), lang)
        answer = _localized(_as_dict(item.get("answer")), lang)
        if not question.strip():
            continue
        rendered.append(
            {
                "id": item.get("id") or "",
                "question": {"html": render_text(question)},
                "answer": {"html": render_text(answer)},
            }
        )
    return rendered


def _render_section(key: str, section: dict[str, Any], lang: str) -> dict[str, Any] | None:
    if key == "hero":
        out = {
            "visible": True,
            "kicker": _render_field(section.get("kicker"), lang),
            "title": _render_field(section.get("title"), lang),
            "subtitle": _render_field(section.get("subtitle"), lang),
            "text": _render_field(section.get("text"), lang),
            "extra_text": _render_field(section.get("extra_text"), lang),
            "methods_visible": _as_bool(section.get("methods_visible"), True),
        }
    elif key == "duels":
        button = _as_dict(section.get("button"))
        out = {
            "visible": True,
            "badge": _render_field(section.get("badge"), lang),
            "title": _render_field(section.get("title"), lang),
            "paragraphs": _render_list(section.get("paragraphs"), lang),
            "extra_text": _render_field(section.get("extra_text"), lang),
            "closing": _render_field(section.get("closing"), lang),
            "button": {
                "visible": _as_bool(button.get("visible"), True),
                "label": _render_field(button.get("label"), lang),
                "url": button.get("url") or "",
            },
        }
    elif key in CARD_SECTIONS:
        out = {
            "visible": True,
            "title": _render_field(section.get("title"), lang),
            "text": _render_field(section.get("text"), lang),
            "cards": _render_cards(section.get("cards"), lang),
        }
    elif key == "why":
        out = {
            "visible": True,
            "title": _render_field(section.get("title"), lang),
            "paragraphs": _render_list(section.get("paragraphs"), lang),
            "ideas": _render_list(section.get("ideas"), lang),
        }
    elif key == "faq":
        out = {
            "visible": True,
            "title": _render_field(section.get("title"), lang),
            "items": _render_faq(section.get("items"), lang),
        }
    elif key == "cta":
        out = {
            "visible": True,
            "title": _render_field(section.get("title"), lang),
            "text": _render_field(section.get("text"), lang),
            "slogan": _render_field(section.get("slogan"), lang),
            "methods_visible": _as_bool(section.get("methods_visible"), True),
        }
    else:  # pragma: no cover - SECTION_ORDER is closed
        return None

    return out if _section_has_content(key, out) else None


def _section_has_content(key: str, out: dict[str, Any]) -> bool:
    def field_html(name: str) -> str:
        value = out.get(name)
        return value.get("html", "") if isinstance(value, dict) else ""

    def list_items(name: str) -> list[str]:
        value = out.get(name)
        return value.get("items", []) if isinstance(value, dict) else []

    if key == "hero":
        return any(
            field_html(name)
            for name in ("kicker", "title", "subtitle", "text", "extra_text")
        ) or bool(out.get("methods_visible"))
    if key == "duels":
        button = out.get("button") or {}
        return (
            any(field_html(name) for name in ("badge", "title", "extra_text", "closing"))
            or bool(list_items("paragraphs"))
            or bool(
                button.get("visible")
                and (button.get("label") or {}).get("html")
                and button.get("url")
            )
        )
    if key in CARD_SECTIONS:
        cards = out.get("cards") or {}
        return (
            bool(field_html("title"))
            or bool(field_html("text"))
            or bool(cards.get("items"))
        )
    if key == "why":
        return (
            bool(field_html("title"))
            or bool(list_items("paragraphs"))
            or bool(list_items("ideas"))
        )
    if key == "faq":
        return bool(field_html("title")) or bool(out.get("items"))
    if key == "cta":
        return (
            bool(field_html("title"))
            or bool(field_html("text"))
            or bool(field_html("slogan"))
            or bool(out.get("methods_visible"))
        )
    return False


def build_public_payload(
    content_raw: Any,
    methods: Iterable[SupportMethod],
    lang: str,
    *,
    revision: int = 0,
    updated_at: datetime | None = None,
) -> dict[str, Any]:
    """Public projection: stored content, one language, hidden parts removed."""
    language = "en" if lang == "en" else "ru"
    content = read_content(content_raw)

    method_list = list(methods)
    order: list[str] = []
    sections: dict[str, Any] = {}
    for key in content["order"]:
        raw_section = content["sections"].get(key)
        if not isinstance(raw_section, dict):
            continue
        if raw_section.get("visible") is False:
            continue
        rendered = _render_section(key, raw_section, language)
        if rendered is None:
            continue
        if key in METHOD_SECTIONS:
            shown = (
                methods_for_section(method_list, key)
                if rendered.get("methods_visible")
                else []
            )
            rendered["methods"] = shown
            rendered["methods_visible"] = bool(shown)
            probe = dict(rendered)
            probe["methods_visible"] = False
            if not shown and not _section_has_content(key, probe):
                continue
        sections[key] = rendered
        order.append(key)

    code = content.get("code") if isinstance(content.get("code"), dict) else {}
    custom_css = ""
    try:
        custom_css = normalize_custom_css(code.get("custom_css"))
    except SupportValidationError:
        custom_css = ""
    custom_html = sanitize_html(str(code.get("custom_html") or ""))

    return {
        "lang": language,
        "revision": revision,
        "updated_at": updated_at,
        "order": order,
        "sections": sections,
        "methods": [
            method_public_dict(method)
            for method in sorted(
                (item for item in method_list if item.is_active),
                key=lambda item: (item.sort_order, item.id),
            )
        ],
        "code": {"custom_css": custom_css, "custom_html": custom_html},
    }


# ---------------------------------------------------------------------------
# Seeding
# ---------------------------------------------------------------------------
SUPPORT_SEED_VERSION = 1


def ensure_support_seed(db: Session) -> SupportPage:
    """Create the default document and methods once; never overwrite values.

    Called on startup and from every support endpoint, so a database restored
    from an old backup or a freshly truncated test schema still serves a
    complete page. ``seed_version`` makes the step run exactly once: a default
    method deleted later is not re-created, and edited text is never reset.
    """
    row = db.get(SupportPage, SUPPORT_PAGE_ID)
    if row is None:
        now = datetime.now(timezone.utc)
        row = SupportPage(
            id=SUPPORT_PAGE_ID,
            draft_content=copy.deepcopy(DEFAULT_CONTENT),
            published_content=copy.deepcopy(DEFAULT_CONTENT),
            revision=1,
            published_revision=1,
            draft_updated_at=now,
            published_at=now,
            seed_version=0,
        )
        db.add(row)
        try:
            db.commit()
        except IntegrityError:
            # Another worker seeded the row first.
            db.rollback()
            row = db.get(SupportPage, SUPPORT_PAGE_ID)
            if row is None:
                raise
        else:
            db.refresh(row)

    if row.seed_version < SUPPORT_SEED_VERSION:
        existing = set(db.scalars(select(SupportMethod.key)).all())
        for item in DEFAULT_METHODS:
            if item["key"] not in existing:
                db.add(SupportMethod(**copy.deepcopy(item)))
        row.seed_version = SUPPORT_SEED_VERSION
        try:
            db.commit()
        except IntegrityError:
            # A concurrent worker inserted the same keys; retry next request.
            db.rollback()
        else:
            db.refresh(row)

    return row


def serialize_method_payload(method: SupportMethod) -> dict[str, Any]:
    data = method_public_dict(method)
    return {
        **data,
        "id": method.id,
        "is_active": bool(method.is_active),
        "sort_order": method.sort_order,
        "created_at": method.created_at,
        "updated_at": method.updated_at,
    }
