"""SEO surface of the public site: language variants, Open Graph, JSON-LD.

The landing page is rendered by the API from one template, so both ``/`` and
``/en`` must ship a complete ``<head>`` (title, description, canonical,
hreflang, Open Graph, Twitter card, JSON-LD) and body copy in the matching
language without JavaScript. ``robots.txt`` and ``sitemap.xml`` are generated
too. The browser dictionary in ``app/static/app/landing-i18n.js`` mirrors
:mod:`app.services.landing_content`; the last test keeps them in sync.
"""
from __future__ import annotations

import json
import re
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

from tests import helpers

STATIC_DIR = Path(__file__).resolve().parents[1] / "app" / "static"
TEMPLATE = STATIC_DIR / "landing" / "index.html"
LANDING_I18N = STATIC_DIR / "app" / "landing-i18n.js"
ADMIN_SITE = STATIC_DIR / "admin" / "admin-site.js"
OG_IMAGES = {
    "ru": STATIC_DIR / "app" / "og-image.png",
    "en": STATIC_DIR / "app" / "og-image-en.png",
}

ORIGIN = "https://fretfight.online"
TITLES = {
    "ru": "fretfight — гитарный батл онлайн: табы и тренажёр гитары",
    "en": "fretfight — online guitar battle: tabs and a guitar trainer",
}
H1_PHRASES = {
    "ru": "— гитарный батл онлайн",
    "en": "— guitar battle online",
}
# Phrases the copy must target (search queries for the project).
KEYWORDS = {
    "ru": ("гитарный батл", "игра на гитаре онлайн", "тренажёр гитары", "табы", "минусовки", "рейтинг гитаристов"),
    "en": ("guitar battle", "guitar trainer", "guitar tabs", "backing tracks", "leaderboard"),
}
ROUTES = {"ru": "/", "en": "/en"}


def _html(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _attr(html: str, pattern: str) -> str:
    match = re.search(pattern, html)
    assert match is not None, f"{pattern!r} not found"
    return match.group(1)


def _meta(html: str, name: str) -> str | None:
    for attr in ("property", "name"):
        match = re.search(
            r'<meta ' + attr + r'="' + re.escape(name) + r'" content="([^"]*)"', html
        )
        if match is not None:
            return match.group(1)
    return None


def _json_ld(html: str) -> dict:
    match = re.search(
        r'<script type="application/ld\+json">(.*?)</script>', html, re.DOTALL
    )
    assert match is not None, "JSON-LD block is missing"
    return json.loads(match.group(1))


def _png_size(path: Path) -> tuple[int, int]:
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG file"
    width, height = struct.unpack(">II", data[16:24])
    return width, height


def _dictionary(source: str) -> dict[str, dict[str, str]]:
    """Parse the ``ru``/``en`` halves of a JS dictionary module."""
    en_start = source.index("  en: {")
    parts = {"ru": source[:en_start], "en": source[en_start:]}
    entry = re.compile(r"^\s*'([^']+)':\s*'([^']*)',?\s*$", re.M)
    return {lang: {m.group(1): m.group(2) for m in entry.finditer(part)} for lang, part in parts.items()}


# ---------------------------------------------------------------------------
# Rendered documents
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("lang", sorted(ROUTES))
def test_route_returns_html_in_the_right_language(client, lang: str) -> None:
    response = client.get(ROUTES[lang])

    assert response.status_code == 200
    assert response.headers["content-type"].startswith("text/html")
    assert "charset=utf-8" in response.headers["content-type"].lower()

    html = response.text
    assert f'<html lang="{lang}"' in html
    assert f"<title>{TITLES[lang]}</title>" in html
    assert _meta(html, "description") is not None
    # No unresolved template placeholders ever reach the visitor.
    assert "{{" not in html


@pytest.mark.parametrize("lang", sorted(ROUTES))
def test_canonical_and_hreflang_point_at_both_variants(client, lang: str) -> None:
    html = client.get(ROUTES[lang]).text

    assert _attr(html, r'<link rel="canonical" href="([^"]+)"') == ORIGIN + ROUTES[lang]
    alternates = dict(
        re.findall(r'<link rel="alternate" hreflang="([^"]+)" href="([^"]+)"', html)
    )
    assert alternates == {
        "ru": f"{ORIGIN}/",
        "en": f"{ORIGIN}/en",
        "x-default": f"{ORIGIN}/",
    }


@pytest.mark.parametrize("lang", sorted(ROUTES))
def test_open_graph_and_twitter_card(client, lang: str) -> None:
    html = client.get(ROUTES[lang]).text

    assert _meta(html, "og:type") == "website"
    assert _meta(html, "og:site_name") == "fretfight"
    assert _meta(html, "og:url") == ORIGIN + ROUTES[lang]
    assert _meta(html, "og:locale") == ("ru_RU" if lang == "ru" else "en_US")
    assert _meta(html, "og:locale:alternate") == ("en_US" if lang == "ru" else "ru_RU")
    assert _meta(html, "og:image") == f"{ORIGIN}/app/og-image" + ("" if lang == "ru" else "-en") + ".png"
    assert _meta(html, "og:image:type") == "image/png"
    assert _meta(html, "og:image:width") == "1200"
    assert _meta(html, "og:image:height") == "630"
    assert _meta(html, "og:image:alt")

    assert _meta(html, "twitter:card") == "summary_large_image"
    assert _meta(html, "twitter:title") == _meta(html, "og:title")
    assert _meta(html, "twitter:description") == _meta(html, "og:description")
    assert _meta(html, "twitter:image") == _meta(html, "og:image")


@pytest.mark.parametrize("lang", sorted(ROUTES))
def test_robots_meta_and_keywords(client, lang: str) -> None:
    html = client.get(ROUTES[lang]).text

    robots = _meta(html, "robots") or ""
    for token in ("index", "follow", "max-image-preview:large"):
        assert token in robots

    keywords = (_meta(html, "keywords") or "").lower()
    assert keywords, "meta keywords is missing"
    assert 8 <= len([part for part in keywords.split(",") if part.strip()]) <= 12
    for phrase in KEYWORDS[lang]:
        assert phrase in keywords or phrase in html.lower()


@pytest.mark.parametrize("lang", sorted(ROUTES))
def test_heading_and_body_carry_the_search_phrases(client, lang: str) -> None:
    html = client.get(ROUTES[lang]).text

    heading = re.search(r'<h1[^>]*id="hero-title"[^>]*>(.*?)</h1>', html, re.DOTALL)
    assert heading is not None
    text = re.sub(r"<[^>]+>", " ", heading.group(1))
    text = re.sub(r"\s+", " ", text).strip().lower()
    assert text == f"fretfight {H1_PHRASES[lang]}"

    visible = re.sub(r"<script.*?</script>|<style.*?</style>", " ", html, flags=re.DOTALL)
    visible = re.sub(r"<[^>]+>", " ", visible).lower()
    for phrase in KEYWORDS[lang]:
        assert phrase in visible, f"{phrase!r} missing from the visible copy"


@pytest.mark.parametrize("lang", sorted(ROUTES))
def test_structured_data_describes_the_app(client, lang: str) -> None:
    data = _json_ld(client.get(ROUTES[lang]).text)

    assert data["@context"] == "https://schema.org"
    graph = {node["@type"]: node for node in data["@graph"]}
    assert set(graph) == {"SoftwareApplication", "Organization", "WebSite", "FAQPage"}

    app = graph["SoftwareApplication"]
    assert app["name"] == "fretfight"
    assert app["alternateName"] == "GuitarBattle"
    assert app["applicationCategory"] == "GameApplication"
    assert app["operatingSystem"] == "Windows 10, Windows 11"
    assert app["url"] == f"{ORIGIN}/"
    assert app["image"].startswith(ORIGIN)
    assert app["inLanguage"] == ("ru-RU" if lang == "ru" else "en-US")
    assert {offer["priceCurrency"] for offer in app["offers"]} == {"RUB", "USD"}
    assert all(offer["price"] == "0" for offer in app["offers"])
    # No invented ratings or reviews.
    assert "aggregateRating" not in app
    assert "review" not in app

    organization = graph["Organization"]
    assert organization["name"] == "fretfight"
    assert organization["url"] == f"{ORIGIN}/"
    # The square brand icon, not the 600px marketing artwork: Google shows
    # ``Organization.logo`` next to the site in results.
    assert organization["logo"] == f"{ORIGIN}/app/icon-512.png"
    assert organization["image"] == f"{ORIGIN}/app/icon-512.png"

    site = graph["WebSite"]
    assert site["url"] == f"{ORIGIN}/"
    assert site["inLanguage"] == ["ru-RU", "en-US"]
    # The site has no search box, so no SearchAction is advertised.
    assert "potentialAction" not in site

    faq = graph["FAQPage"]
    assert faq["inLanguage"] == app["inLanguage"]
    assert len(faq["mainEntity"]) == 4
    for question in faq["mainEntity"]:
        assert question["@type"] == "Question"
        assert question["acceptedAnswer"]["text"]


def test_english_page_is_not_the_russian_one(client) -> None:
    ru = client.get("/").text
    en = client.get("/en").text

    assert TITLES["ru"] in ru and TITLES["ru"] not in en
    assert TITLES["en"] in en and TITLES["en"] not in ru
    assert 'data-lang-default="en"' in en
    # / must keep the cookie/browser language switching, /en pins English.
    assert "data-lang-default" not in ru.split(">", 2)[1]


def test_language_switcher_hooks_are_kept(client) -> None:
    for lang, path in ROUTES.items():
        html = client.get(path).text
        assert 'data-lang-paths=\'{"ru":"/","en":"/en"}\'' in html
        assert 'data-lang="ru"' in html and 'data-lang="en"' in html
        assert 'data-i18n="landing.hero.h1"' in html


def test_existing_public_routes_still_work(client) -> None:
    for path in ("/login", "/register", "/policy", "/account", "/support"):
        assert client.get(path).status_code == 200, path
    redirect = client.get("/donate", follow_redirects=False)
    assert redirect.status_code in (307, 308)
    assert redirect.headers["location"] == "/support"


# ---------------------------------------------------------------------------
# Search engine ownership confirmation (Yandex.Webmaster, Google Search Console)
# ---------------------------------------------------------------------------
YANDEX_CODE = "13c642a7d8dc4d50"
YANDEX_FILE = f"/yandex_{YANDEX_CODE}.html"

GOOGLE_TOKEN = "uRKDy8NvkkgwlIOcR7kUjheH0_3b8J53Vqn89GbweK0"
GOOGLE_FILENAME = f"google{GOOGLE_TOKEN}.html"
GOOGLE_FILE = f"/{GOOGLE_FILENAME}"


def test_yandex_verification_file_is_served_from_the_root(client) -> None:
    response = client.get(YANDEX_FILE)

    assert response.status_code == 200
    content_type = response.headers["content-type"].lower()
    assert content_type.startswith("text/html")
    assert "charset=utf-8" in content_type
    body = response.text
    assert f"Verification: {YANDEX_CODE}" in body
    assert "charset=UTF-8" in body
    assert "<html>" in body and "</html>" in body


def test_yandex_verification_file_body_is_exact(client) -> None:
    """Yandex compares the body it was given, so it is reproduced verbatim."""
    expected = (
        "<html>\n"
        "    <head>\n"
        '        <meta http-equiv="Content-Type" content="text/html; charset=UTF-8">\n'
        "    </head>\n"
        f"    <body>Verification: {YANDEX_CODE}</body>\n"
        "</html>\n"
    )
    assert client.get(YANDEX_FILE).text == expected


def test_google_verification_file_is_served_from_the_root(client) -> None:
    response = client.get(GOOGLE_FILE)

    assert response.status_code == 200
    content_type = response.headers["content-type"].lower()
    assert content_type.startswith("text/html")
    assert "charset=utf-8" in content_type
    assert f"google-site-verification: {GOOGLE_FILENAME}" in response.text


def test_google_verification_file_body_is_exact(client) -> None:
    """Google compares the body verbatim: one line naming the file itself."""
    assert client.get(GOOGLE_FILE).text == (
        f"google-site-verification: {GOOGLE_FILENAME}"
    )


@pytest.mark.parametrize("path", sorted(ROUTES.values()))
def test_landing_has_the_yandex_verification_meta(client, path: str) -> None:
    html = client.get(path).text

    assert f'<meta name="yandex-verification" content="{YANDEX_CODE}">' in html


@pytest.mark.parametrize("path", sorted(ROUTES.values()))
def test_landing_has_the_google_verification_meta(client, path: str) -> None:
    html = client.get(path).text

    assert (
        f'<meta name="google-site-verification" content="{GOOGLE_TOKEN}">' in html
    )


@pytest.mark.parametrize("path", sorted(ROUTES.values()))
def test_both_verification_metas_share_one_head(client, path: str) -> None:
    """The template is one document: both tags must land in the same <head>."""
    html = client.get(path).text

    assert html.count("<head>") == 1
    head = re.search(r"<head>(.*?)</head>", html, re.DOTALL)
    assert head is not None
    for meta in (
        f'<meta name="yandex-verification" content="{YANDEX_CODE}">',
        f'<meta name="google-site-verification" content="{GOOGLE_TOKEN}">',
    ):
        assert meta in head.group(1), meta
    # Unresolved placeholders never reach either head.
    assert "{{" not in head.group(1)


def test_public_routes_survive_the_verification_routes(client) -> None:
    for path in ("/", "/en", "/robots.txt", "/sitemap.xml", "/support"):
        assert client.get(path).status_code == 200, path


# ---------------------------------------------------------------------------
# Social preview images
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("lang", sorted(OG_IMAGES))
def test_og_image_is_a_1200x630_png(lang: str) -> None:
    path = OG_IMAGES[lang]
    assert path.is_file(), f"{path.name} is missing"
    assert _png_size(path) == (1200, 630)
    assert path.stat().st_size > 10_000


def test_og_image_is_served_from_the_app_mount(client) -> None:
    for name in ("og-image.png", "og-image-en.png", "logo.png"):
        response = client.get(f"/app/{name}")
        assert response.status_code == 200, name
        assert response.headers["content-type"] == "image/png"
    assert _png_size(OG_IMAGES["ru"]) == (1200, 630)


# ---------------------------------------------------------------------------
# Browser/search-engine icons and the web manifest
# ---------------------------------------------------------------------------
ICON_CACHE_CONTROL = "public, max-age=604800"
PNG_ICONS = {
    "icon-48.png": (48, 48),
    "icon-96.png": (96, 96),
    "icon-192.png": (192, 192),
    "icon-512.png": (512, 512),
    "apple-touch-icon.png": (180, 180),
}
HEAD_LINKS = (
    '<link rel="icon" href="/favicon.ico" sizes="any">',
    '<link rel="icon" type="image/png" sizes="48x48" href="/app/icon-48.png">',
    '<link rel="icon" type="image/png" sizes="96x96" href="/app/icon-96.png">',
    '<link rel="icon" type="image/png" sizes="192x192" href="/app/icon-192.png">',
    '<link rel="apple-touch-icon" sizes="180x180" href="/app/apple-touch-icon.png">',
    '<link rel="manifest" href="/app/site.webmanifest">',
)


def _ico_sizes(path: Path) -> list[tuple[int, int]]:
    data = path.read_bytes()
    assert data[:4] == b"\x00\x00\x01\x00", "not an ICO file"
    count = struct.unpack("<H", data[4:6])[0]
    sizes = []
    for index in range(count):
        offset = 6 + index * 16
        width = data[offset] or 256
        height = data[offset + 1] or 256
        sizes.append((width, height))
    return sizes


def test_favicon_ico_contains_the_small_sizes() -> None:
    path = STATIC_DIR / "app" / "favicon.ico"

    assert path.is_file(), "favicon.ico is missing"
    assert sorted(_ico_sizes(path)) == [(16, 16), (32, 32), (48, 48)]


@pytest.mark.parametrize("name", sorted(PNG_ICONS))
def test_png_icons_have_the_expected_size(name: str) -> None:
    path = STATIC_DIR / "app" / name

    assert path.is_file(), f"{name} is missing"
    assert _png_size(path) == PNG_ICONS[name]
    assert path.stat().st_size > 1_000


def test_root_favicon_is_served_with_the_ico_type(client) -> None:
    response = client.get("/favicon.ico")

    assert response.status_code == 200
    assert response.headers["content-type"] == "image/x-icon"
    assert response.headers["cache-control"] == ICON_CACHE_CONTROL
    assert response.content[:4] == b"\x00\x00\x01\x00"


@pytest.mark.parametrize(
    "path", ["/apple-touch-icon.png", "/apple-touch-icon-precomposed.png"]
)
def test_root_apple_touch_icon_aliases(client, path: str) -> None:
    response = client.get(path)

    assert response.status_code == 200, path
    assert response.headers["content-type"] == "image/png"
    assert response.headers["cache-control"] == ICON_CACHE_CONTROL
    assert response.content[:8] == b"\x89PNG\r\n\x1a\n"
    assert struct.unpack(">II", response.content[16:24]) == (180, 180)


def test_icons_are_also_reachable_under_the_app_mount(client) -> None:
    response = client.get("/app/favicon.ico")
    assert response.status_code == 200
    assert response.headers["cache-control"] == ICON_CACHE_CONTROL

    for name, size in PNG_ICONS.items():
        response = client.get(f"/app/{name}")
        assert response.status_code == 200, name
        assert response.headers["content-type"] == "image/png"
        assert response.headers["cache-control"] == ICON_CACHE_CONTROL
        assert struct.unpack(">II", response.content[16:24]) == size


def test_webmanifest_is_served_with_the_manifest_type(client) -> None:
    response = client.get("/app/site.webmanifest")

    assert response.status_code == 200
    assert response.headers["content-type"].startswith("application/manifest+json")

    manifest = json.loads(response.text)
    assert manifest["name"] == "FretFight"
    assert manifest["short_name"] == "FretFight"
    assert manifest["start_url"] == "/"
    assert manifest["display"] == "standalone"
    assert manifest["background_color"] == "#070b10"
    assert manifest["theme_color"] == "#070b10"
    assert {(icon["sizes"], icon["src"]) for icon in manifest["icons"]} == {
        ("48x48", "/app/icon-48.png"),
        ("96x96", "/app/icon-96.png"),
        ("192x192", "/app/icon-192.png"),
        ("512x512", "/app/icon-512.png"),
    }
    for icon in manifest["icons"]:
        assert (STATIC_DIR / "app" / Path(icon["src"]).name).is_file(), icon["src"]


@pytest.mark.parametrize(
    "path", ["/", "/en", "/login", "/account", "/policy", "/support"]
)
def test_every_page_head_links_the_icons_and_the_manifest(client, path: str) -> None:
    html = client.get(path).text

    head = re.search(r"<head>(.*?)</head>", html, re.DOTALL)
    assert head is not None, path
    head_html = head.group(1)
    for link in HEAD_LINKS:
        assert head_html.count(link) == 1, f"{path}: {link}"
    # The 600px artwork is no longer the touch icon, and iOS reads exactly one.
    assert head_html.count('rel="apple-touch-icon"') == 1, path
    assert 'href="/app/logo.png"' not in head_html, path
    # The SVG variant stays as an extra, for browsers that prefer vectors.
    assert '<link rel="icon" type="image/svg+xml" href="/app/logo.svg">' in head_html


@pytest.mark.parametrize("path", ["/robots.txt"])
def test_icons_are_not_disallowed_for_crawlers(client, path: str) -> None:
    lines = [line.strip() for line in client.get(path).text.splitlines()]

    for disallow in ("Disallow: /app/", "Disallow: /favicon.ico", "Disallow: /app"):
        assert disallow not in lines


# ---------------------------------------------------------------------------
# robots.txt / sitemap.xml
# ---------------------------------------------------------------------------
def test_robots_txt(client) -> None:
    response = client.get("/robots.txt")

    assert response.status_code == 200
    assert response.headers["content-type"].startswith("text/plain")
    lines = [line.strip() for line in response.text.splitlines()]
    assert "User-agent: *" in lines
    assert "Allow: /" in lines
    assert "Disallow: /account" in lines
    assert f"Sitemap: {ORIGIN}/sitemap.xml" in lines
    # The landing page itself must never be blocked.
    assert "Disallow: /" not in lines


def test_sitemap_xml(client) -> None:
    response = client.get("/sitemap.xml")

    assert response.status_code == 200
    assert response.headers["content-type"].startswith("application/xml")

    ns = {"s": "http://www.sitemaps.org/schemas/sitemap/0.9", "x": "http://www.w3.org/1999/xhtml"}
    root = ET.fromstring(response.text)
    urls = root.findall("s:url", ns)
    locations = [url.findtext("s:loc", namespaces=ns) for url in urls]
    assert locations == [
        f"{ORIGIN}/",
        f"{ORIGIN}/en",
        f"{ORIGIN}/support",
        f"{ORIGIN}/register",
        f"{ORIGIN}/policy",
        f"{ORIGIN}/login",
    ]

    for url in urls:
        lastmod = url.findtext("s:lastmod", namespaces=ns)
        assert re.fullmatch(r"\d{4}-\d{2}-\d{2}", lastmod or ""), lastmod

    landing = urls[0]
    alternates = {
        link.get("hreflang"): link.get("href")
        for link in landing.findall("x:link", ns)
    }
    assert alternates == {
        "ru": f"{ORIGIN}/",
        "en": f"{ORIGIN}/en",
        "x-default": f"{ORIGIN}/",
    }
    # Only the landing pair is language-dependent.
    assert not urls[2].findall("x:link", ns)


# ---------------------------------------------------------------------------
# Template and dictionary consistency
# ---------------------------------------------------------------------------
def test_template_has_no_hardcoded_copy() -> None:
    """The template is a template: every translatable node is a placeholder."""
    template = _html(TEMPLATE)

    for key in re.findall(r'data-i18n="([^"]+)"', template):
        assert f"{{{{ {key} }}}}" in template, f"{key} has no server-side placeholder"
    for key in re.findall(r'data-i18n-aria-label="([^"]+)"', template):
        assert f'aria-label="{{{{ {key} }}}}"' in template
    assert "{{! landing.hero.lead }}" in template


def test_server_and_browser_dictionaries_match() -> None:
    from app.services.landing_content import LANDING_TEXT

    browser = _dictionary(_html(LANDING_I18N))
    shared = {"common.login", "common.policy"}  # defined in app/static/app/i18n.js
    for lang in ("ru", "en"):
        assert set(browser[lang]) | shared == set(LANDING_TEXT[lang]), lang
        for key, value in browser[lang].items():
            assert LANDING_TEXT[lang][key] == value, f"{lang}/{key} drifted"


def test_removed_button_labels_are_gone() -> None:
    """The hero/CTA buttons were dropped, so their labels left the dictionaries."""
    from app.services.landing_content import LANDING_TEXT

    for lang in ("ru", "en"):
        assert "landing.how" not in LANDING_TEXT[lang], lang
        assert "landing.supportCta" not in LANDING_TEXT[lang], lang
        # The navigation link and the "how" section copy stay.
        assert "landing.nav.how" in LANDING_TEXT[lang], lang
        assert "landing.how.title" in LANDING_TEXT[lang], lang


# ---------------------------------------------------------------------------
# Support block and its removal from the hero/CTA
# ---------------------------------------------------------------------------
def test_landing_has_no_duplicate_support_buttons() -> None:
    template = _html(TEMPLATE)

    # "Как это работает" and "Поддержать проект" buttons are gone from the hero
    # and the final CTA; the #how anchor in the navigation stays.
    assert 'href="#how" data-i18n="landing.how"' not in template
    assert "landing.supportCta" not in template
    assert "{{ landing.how }}" not in template
    assert 'href="#how" data-i18n="landing.nav.how"' in template

    header = re.search(r'<header class="site-header">.*?</header>', template, re.DOTALL)
    assert header is not None
    assert 'href="/support"' in header.group(0)
    assert 'data-i18n="landing.support"' in header.group(0)

    hero = re.search(r'<section class="hero".*?</section>', template, re.DOTALL)
    assert hero is not None
    hero_actions = re.search(r'<div class="hero-actions">.*?</div>', hero.group(0), re.DOTALL)
    assert hero_actions is not None
    assert 'href="/support"' not in hero_actions.group(0)

    cta = re.search(r'<section class="cta".*?</section>', template, re.DOTALL)
    assert cta is not None
    cta_actions = re.search(r'<div class="hero-actions">.*?</div>', cta.group(0), re.DOTALL)
    assert cta_actions is not None
    assert 'href="/support"' not in cta_actions.group(0)

    # Exactly two support actions remain: the header link and the new block.
    assert template.count('href="/support"') == 2


def test_support_project_block_is_standalone() -> None:
    template = _html(TEMPLATE)

    match = re.search(r'<section id="support-project".*?</section>', template, re.DOTALL)
    assert match is not None, "the #support-project block is missing"
    section = match.group(0)

    assert 'aria-labelledby="support-project-title"' in section
    assert 'id="support-project-title"' in section
    assert 'href="/support"' in section
    # Accent button in the style of the site.
    assert "btn btn-primary" in section

    for key in ("kicker", "title", "text", "button"):
        full = f"landing.supportProject.{key}"
        assert f'data-i18n="{full}"' in section, full
        assert f"{{{{ {full} }}}}" in section, full

    # Its own CSS hook: landing.js hides #support-project, never .cta.
    assert ".support-project__inner" in template
    assert ".support-project__action" in template

    # Between the requirements section and the final CTA.
    assert (
        template.index('id="requirements"')
        < template.index('id="support-project"')
        < template.index('<section class="cta"')
    )


def test_admin_editor_exposes_the_support_block() -> None:
    script = _html(ADMIN_SITE)

    block = re.search(r'\{\s*key: "support",.*?\n    \},', script, re.DOTALL)
    assert block is not None, "the support block is missing from SCHEMA"
    schema = block.group(0)
    assert 'title: "Поддержать проект"' in schema
    for key in ("kicker", "title", "text", "button"):
        assert f'key: "{key}"' in schema, key

    for key in ("kicker", "title", "text", "button"):
        assert f'r("landing.supportProject.{key}")' in script, key


# ---------------------------------------------------------------------------
# Admin CMS roundtrip (needs the disposable PostgreSQL database)
# ---------------------------------------------------------------------------
SUPPORT_BLOCK = {
    "visible": True,
    "ru": {
        "kicker": "Поддержать проект",
        "title": "Помогите FretFight расти",
        "text": "FretFight делает небольшая команда.",
        "button": "Поддержать проект",
    },
    "en": {
        "kicker": "Support the project",
        "title": "Help FretFight grow",
        "text": "FretFight is built by a small team.",
        "button": "Support the project",
    },
}


@pytest.mark.db
def test_support_block_roundtrip_through_the_admin_api(db_client, admin_user) -> None:
    """Saving the block in the editor returns it from both API views."""
    response = db_client.put(
        f"{helpers.API}/admin/site/landing",
        json={"blocks": {"support": SUPPORT_BLOCK}},
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    assert response.json()["blocks"]["support"] == SUPPORT_BLOCK

    admin = db_client.get(
        f"{helpers.API}/admin/site/landing", headers=admin_user["headers"]
    )
    assert admin.status_code == 200, admin.text
    assert admin.json()["blocks"]["support"] == SUPPORT_BLOCK

    public = db_client.get(f"{helpers.API}/landing")
    assert public.status_code == 200, public.text
    assert public.json()["blocks"]["support"] == SUPPORT_BLOCK


@pytest.mark.db
def test_hidden_support_block_is_still_served_to_the_page(db_client, admin_user) -> None:
    """The visibility switch travels with the block; landing.js hides it."""
    block = json.loads(json.dumps(SUPPORT_BLOCK))
    block["visible"] = False

    response = db_client.put(
        f"{helpers.API}/admin/site/landing",
        json={"blocks": {"support": block}},
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    assert response.json()["blocks"]["support"]["visible"] is False

    public = db_client.get(f"{helpers.API}/landing")
    assert public.status_code == 200, public.text
    assert public.json()["blocks"]["support"]["visible"] is False
