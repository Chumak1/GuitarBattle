"""Support page CMS: public projection, draft/publish, versions and safeguards."""
from __future__ import annotations

import re
from pathlib import Path

import pytest

from app.services.support_content import (
    SupportValidationError,
    normalize_external_url,
    render_text,
    sanitize_html,
)
from tests import helpers

SUPPORT = f"{helpers.API}/support"
ADMIN = f"{helpers.API}/admin/support"
DEFAULT_ORDER = ["hero", "duels", "usage", "help", "why", "faq", "cta"]
STATIC = Path(__file__).resolve().parents[1] / "app" / "static"


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
def _public(client, lang="ru", path=SUPPORT):
    response = client.get(f"{path}?lang={lang}")
    assert response.status_code == 200, response.text
    return response.json()


def _state(client, admin):
    response = client.get(ADMIN, headers=admin["headers"])
    assert response.status_code == 200, response.text
    return response.json()


def _save(client, admin, content, revision):
    return client.put(
        f"{ADMIN}/draft",
        json={"revision": revision, "content": content},
        headers=admin["headers"],
    )


def _publish(client, admin, comment=""):
    response = client.post(
        f"{ADMIN}/publish", json={"comment": comment}, headers=admin["headers"]
    )
    assert response.status_code == 200, response.text
    return response.json()


def _publish_change(client, admin, mutate):
    state = _state(client, admin)
    content = state["content"]
    mutate(content)
    saved = _save(client, admin, content, state["revision"])
    assert saved.status_code == 200, saved.text
    return _publish(client, admin)


def _title(content, section="hero"):
    return content["sections"][section]["title"]


# ---------------------------------------------------------------------------
# Pure validation / rendering units (no database)
# ---------------------------------------------------------------------------
@pytest.mark.parametrize(
    "value",
    [
        "javascript:alert(1)",
        "data:text/html,<script>alert(1)</script>",
        "vbscript:msgbox(1)",
        "file:///etc/passwd",
        "//evil.example/x",
        "https://",
        "https:///path",
        "https://user:pass@example.com/",
    ],
)
def test_external_url_validation_rejects_unsafe_values(value):
    with pytest.raises(SupportValidationError):
        normalize_external_url(value, "url", required=True)


@pytest.mark.parametrize(
    "value",
    ["https://boosty.to/fretfight/donate", "http://example.com", "https://пример.рф/путь"],
)
def test_external_url_validation_accepts_http_links(value):
    assert normalize_external_url(value, "url", required=True) == value


def test_render_text_escapes_raw_html_and_keeps_markup():
    rendered = render_text(
        "**bold** and *italic* [site](https://example.com) <script>alert(1)</script>"
    )
    assert "<strong>bold</strong>" in rendered
    assert "<em>italic</em>" in rendered
    assert 'href="https://example.com"' in rendered
    assert 'rel="noopener noreferrer nofollow"' in rendered
    assert "<script>" not in rendered
    assert "&lt;script&gt;" in rendered


def test_render_text_builds_lists():
    rendered = render_text("- first\n- second")
    assert "<ul>" in rendered and "<li>first</li>" in rendered


def test_sanitize_html_drops_scripts_handlers_and_unsafe_links():
    cleaned = sanitize_html(
        '<script>alert(1)</script><p onclick="x">ok</p>'
        "<img src=x onerror=alert(1)><a href=\"javascript:alert(1)\">bad</a>"
        '<a href="https://example.com" onmouseover="x">good</a>'
    )
    assert "<script" not in cleaned
    assert "onclick" not in cleaned
    assert "onerror" not in cleaned
    assert "javascript:" not in cleaned
    assert "<img" not in cleaned
    assert "<p>ok</p>" in cleaned
    assert '<a href="https://example.com"' in cleaned
    # Text of a dropped <a> survives instead of the tag.
    assert "bad" in cleaned


# ---------------------------------------------------------------------------
# Public endpoint and seed
# ---------------------------------------------------------------------------
def test_public_support_defaults(db_client):
    data = _public(db_client)

    assert data["order"] == DEFAULT_ORDER
    assert set(data["sections"]) == set(DEFAULT_ORDER)
    assert data["sections"]["hero"]["title"]["html"] == "Поддержать Fretfight"
    assert data["sections"]["duels"]["badge"]["html"] == "Скоро"
    assert data["sections"]["cta"]["slogan"]["html"] == "Play. Compete. Prove your skill."

    methods = {method["key"]: method for method in data["methods"]}
    assert set(methods) == {"boosty", "dalink"}
    assert methods["boosty"]["url"] == "https://boosty.to/fretfight/donate"
    assert methods["dalink"]["url"] == "https://dalink.to/fretfight"
    assert methods["boosty"]["style"] == "primary"
    assert methods["dalink"]["style"] == "secondary"
    assert methods["boosty"]["sections"] == ["hero", "cta"]
    assert [item["key"] for item in data["sections"]["hero"]["methods"]] == [
        "boosty",
        "dalink",
    ]


def test_public_support_english_with_russian_fallback(db_client, admin_user):
    data = _public(db_client, "en")
    assert data["sections"]["hero"]["title"]["html"] == "Support Fretfight"

    # A cleared translation falls back to Russian instead of rendering empty.
    state = _state(db_client, admin_user)
    content = state["content"]
    content["sections"]["hero"]["subtitle"]["en"] = ""
    saved = _save(db_client, admin_user, content, state["revision"])
    assert saved.status_code == 200, saved.text
    _publish(db_client, admin_user)

    english = _public(db_client, "en")
    assert (
        english["sections"]["hero"]["subtitle"]["html"]
        == "Помоги создать место, где гитаристы доказывают своё мастерство"
    )


# ---------------------------------------------------------------------------
# Visibility and order
# ---------------------------------------------------------------------------
def test_hidden_sections_are_omitted(db_client, admin_user):
    state = _state(db_client, admin_user)
    content = state["content"]
    content["sections"]["usage"]["visible"] = False
    content["sections"]["faq"]["visible"] = False
    # A section with no visible elements is omitted too.
    for field in ("kicker", "title", "subtitle", "text", "extra_text"):
        content["sections"]["hero"][field]["visible"] = False
    content["sections"]["hero"]["methods_visible"] = False

    saved = _save(db_client, admin_user, content, state["revision"])
    assert saved.status_code == 200, saved.text
    _publish(db_client, admin_user)

    data = _public(db_client)
    assert "usage" not in data["sections"]
    assert "faq" not in data["sections"]
    assert "hero" not in data["sections"]
    assert "duels" in data["sections"]
    assert "usage" not in data["order"]


def test_saved_order_is_respected(db_client, admin_user):
    state = _state(db_client, admin_user)
    content = state["content"]
    custom = ["faq", "hero", "cta", "duels", "usage", "help", "why"]
    content["order"] = custom

    saved = _save(db_client, admin_user, content, state["revision"])
    assert saved.status_code == 200, saved.text
    _publish(db_client, admin_user)

    assert _public(db_client)["order"] == custom


# ---------------------------------------------------------------------------
# Draft / publish / conflict
# ---------------------------------------------------------------------------
def test_draft_does_not_change_the_public_page(db_client, admin_user):
    before = _public(db_client)["sections"]["hero"]["title"]["html"]
    state = _state(db_client, admin_user)
    content = state["content"]
    _title(content)["ru"] = "Черновой заголовок"

    saved = _save(db_client, admin_user, content, state["revision"])
    assert saved.status_code == 200, saved.text
    assert _public(db_client)["sections"]["hero"]["title"]["html"] == before

    _publish(db_client, admin_user)
    assert (
        _public(db_client)["sections"]["hero"]["title"]["html"]
        == "Черновой заголовок"
    )


def test_stale_revision_returns_conflict(db_client, admin_user):
    state = _state(db_client, admin_user)
    content = state["content"]
    revision = state["revision"]
    _title(content)["ru"] = "Первое изменение"
    assert _save(db_client, admin_user, content, revision).status_code == 200

    second = _save(db_client, admin_user, content, revision)
    assert second.status_code == 409
    detail = second.json()["detail"]
    assert "revision" in detail.lower()
    assert admin_user["username"] in detail


def test_publish_is_recorded_in_history(db_client, admin_user):
    state = _state(db_client, admin_user)
    content = state["content"]
    _title(content)["ru"] = "Версия для истории"
    assert _save(db_client, admin_user, content, state["revision"]).status_code == 200
    published = _publish(db_client, admin_user, comment="Проверка истории")

    assert published["published_revision"] == state["revision"] + 1
    versions = db_client.get(f"{ADMIN}/versions", headers=admin_user["headers"])
    assert versions.status_code == 200, versions.text
    items = versions.json()["items"]
    assert items[0]["revision"] == published["published_revision"]
    assert items[0]["comment"] == "Проверка истории"
    assert items[0]["created_by"] == admin_user["username"]


# ---------------------------------------------------------------------------
# Access control
# ---------------------------------------------------------------------------
def test_support_admin_endpoints_reject_non_admins(db_client, user_account):
    draft = {"revision": 1, "content": {"order": [], "sections": {}}}
    checks = [
        ("get", ADMIN, None),
        ("put", f"{ADMIN}/draft", draft),
        ("put", f"{ADMIN}/code", {"revision": 1, "custom_html": "", "custom_css": ""}),
        ("post", f"{ADMIN}/publish", {"comment": ""}),
        ("get", f"{ADMIN}/preview", None),
        ("get", f"{ADMIN}/versions", None),
        ("get", f"{ADMIN}/methods", None),
        (
            "post",
            f"{ADMIN}/methods",
            {"key": "x", "button_label": "x", "url": "https://x.example"},
        ),
        ("get", f"{ADMIN}/stats", None),
    ]
    for method, path, payload in checks:
        call = getattr(db_client, method)
        anonymous = call(path, json=payload) if payload else call(path)
        assert anonymous.status_code == 401, f"anonymous {method} {path}"
        forbidden = (
            call(path, json=payload, headers=user_account["headers"])
            if payload
            else call(path, headers=user_account["headers"])
        )
        assert forbidden.status_code == 403, f"user {method} {path}"

    # The rejected writes changed nothing on the public page.
    assert (
        _public(db_client)["sections"]["hero"]["title"]["html"]
        == "Поддержать Fretfight"
    )


# ---------------------------------------------------------------------------
# URL and code safety
# ---------------------------------------------------------------------------
def test_method_urls_must_be_http(db_client, admin_user):
    for url in ("javascript:alert(1)", "data:text/html,x", "file:///etc/passwd"):
        response = db_client.post(
            f"{ADMIN}/methods",
            json={"key": "evil", "button_label": "Evil", "url": url},
            headers=admin_user["headers"],
        )
        assert response.status_code == 422, url
        assert "url" in response.text.lower()

    methods = db_client.get(f"{ADMIN}/methods", headers=admin_user["headers"]).json()
    boosty = next(item for item in methods["items"] if item["key"] == "boosty")
    update = db_client.put(
        f"{ADMIN}/methods/{boosty['id']}",
        json={
            "key": "boosty",
            "button_label": "Поддержать",
            "url": "javascript:alert(1)",
        },
        headers=admin_user["headers"],
    )
    assert update.status_code == 422
    # The stored method is unchanged.
    methods = db_client.get(f"{ADMIN}/methods", headers=admin_user["headers"]).json()
    stored = next(item for item in methods["items"] if item["key"] == "boosty")
    assert stored["url"] == "https://boosty.to/fretfight/donate"


def test_card_urls_are_validated(db_client, admin_user):
    state = _state(db_client, admin_user)
    content = state["content"]
    content["sections"]["usage"]["cards"]["items"][0]["url"] = "javascript:alert(1)"

    response = _save(db_client, admin_user, content, state["revision"])
    assert response.status_code == 422
    assert "usage" in response.text


def test_custom_html_is_sanitized(db_client, admin_user):
    state = _state(db_client, admin_user)
    content = state["content"]
    content["code"]["custom_html"] = (
        '<script>alert(1)</script><p onclick="steal()">Текст</p>'
        '<a href="javascript:alert(1)">плохая</a><b>жирный</b>'
    )

    saved = _save(db_client, admin_user, content, state["revision"])
    assert saved.status_code == 200, saved.text
    stored = saved.json()["content"]["code"]["custom_html"]
    assert "<script" not in stored
    assert "onclick" not in stored
    assert "javascript:" not in stored
    assert "<p>Текст</p>" in stored
    assert "<b>жирный</b>" in stored
    assert saved.json()["sanitized"] is True
    assert saved.json()["warnings"]

    _publish(db_client, admin_user)
    assert _public(db_client)["code"]["custom_html"] == stored


@pytest.mark.parametrize(
    "css",
    [
        "@import url('https://evil.example/x.css');",
        "@font-face { font-family: x; src: url(https://evil.example/x.woff); }",
        "body { background: url(data:image/png;base64,AAAA); }",
        "body { width: expression(alert(1)); }",
        "</style><script>alert(1)</script>",
    ],
)
def test_dangerous_css_is_rejected(db_client, admin_user, css):
    state = _state(db_client, admin_user)
    content = state["content"]
    content["code"]["custom_css"] = css

    response = _save(db_client, admin_user, content, state["revision"])
    assert response.status_code == 422, css
    assert "custom_css" in response.text


def test_code_endpoint_sanitizes_and_guards_revision(db_client, admin_user):
    state = _state(db_client, admin_user)
    response = db_client.put(
        f"{ADMIN}/code",
        json={
            "revision": state["revision"],
            "custom_html": "<script>x</script><p>ok</p>",
            "custom_css": ".support-title { color: #fff; }",
        },
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    body = response.json()
    assert "<script" not in body["content"]["code"]["custom_html"]
    assert body["revision"] == state["revision"] + 1

    stale = db_client.put(
        f"{ADMIN}/code",
        json={"revision": state["revision"], "custom_html": "", "custom_css": ""},
        headers=admin_user["headers"],
    )
    assert stale.status_code == 409


# ---------------------------------------------------------------------------
# Preview and history
# ---------------------------------------------------------------------------
def test_preview_shows_draft_only_to_admins(db_client, admin_user, user_account):
    state = _state(db_client, admin_user)
    content = state["content"]
    _title(content)["ru"] = "Только в черновике"
    assert _save(db_client, admin_user, content, state["revision"]).status_code == 200

    preview = db_client.get(f"{ADMIN}/preview", headers=admin_user["headers"])
    assert preview.status_code == 200, preview.text
    assert preview.json()["sections"]["hero"]["title"]["html"] == "Только в черновике"
    assert (
        _public(db_client)["sections"]["hero"]["title"]["html"]
        == "Поддержать Fretfight"
    )

    assert db_client.get(f"{ADMIN}/preview").status_code == 401
    assert (
        db_client.get(f"{ADMIN}/preview", headers=user_account["headers"]).status_code
        == 403
    )


def test_restore_creates_a_draft_without_publishing(db_client, admin_user):
    first = _publish_change(
        db_client, admin_user, lambda content: _title(content).update({"ru": "Первая версия"})
    )
    _publish_change(
        db_client, admin_user, lambda content: _title(content).update({"ru": "Вторая версия"})
    )
    assert _public(db_client)["sections"]["hero"]["title"]["html"] == "Вторая версия"

    versions = db_client.get(f"{ADMIN}/versions", headers=admin_user["headers"]).json()
    target = next(
        item for item in versions["items"] if item["revision"] == first["revision"]
    )

    restored = db_client.post(
        f"{ADMIN}/versions/{target['id']}/restore", headers=admin_user["headers"]
    )
    assert restored.status_code == 200, restored.text
    assert (
        restored.json()["content"]["sections"]["hero"]["title"]["ru"]
        == "Первая версия"
    )
    # The public page still shows the latest published version.
    assert _public(db_client)["sections"]["hero"]["title"]["html"] == "Вторая версия"

    _publish(db_client, admin_user)
    assert _public(db_client)["sections"]["hero"]["title"]["html"] == "Первая версия"


# ---------------------------------------------------------------------------
# Support methods CRUD
# ---------------------------------------------------------------------------
def test_support_methods_crud_and_ordering(db_client, admin_user):
    created = db_client.post(
        f"{ADMIN}/methods",
        json={
            "key": "patreon",
            "title": "Patreon",
            "button_label": "Support on Patreon",
            "url": "https://www.patreon.com/fretfight",
            "sections": ["cta"],
            "style": "link",
            "sort_order": 2,
        },
        headers=admin_user["headers"],
    )
    assert created.status_code == 201, created.text
    method = created.json()
    assert method["sections"] == ["cta"]

    public = _public(db_client)
    assert [item["key"] for item in public["methods"]] == ["boosty", "dalink", "patreon"]
    assert [item["key"] for item in public["sections"]["hero"]["methods"]] == [
        "boosty",
        "dalink",
    ]
    assert [item["key"] for item in public["sections"]["cta"]["methods"]] == [
        "boosty",
        "dalink",
        "patreon",
    ]

    disabled = db_client.put(
        f"{ADMIN}/methods/{method['id']}",
        json={
            "key": "patreon",
            "title": "Patreon",
            "button_label": "Support on Patreon",
            "url": "https://www.patreon.com/fretfight",
            "sections": ["cta"],
            "style": "link",
            "is_active": False,
        },
        headers=admin_user["headers"],
    )
    assert disabled.status_code == 200, disabled.text
    assert "patreon" not in [item["key"] for item in _public(db_client)["methods"]]

    methods = db_client.get(f"{ADMIN}/methods", headers=admin_user["headers"]).json()
    ids = [item["id"] for item in methods["items"]]
    reordered = db_client.put(
        f"{ADMIN}/methods/order",
        json={"ids": list(reversed(ids))},
        headers=admin_user["headers"],
    )
    assert reordered.status_code == 200, reordered.text
    assert [item["id"] for item in reordered.json()["items"]] == list(reversed(ids))

    deleted = db_client.delete(
        f"{ADMIN}/methods/{method['id']}", headers=admin_user["headers"]
    )
    assert deleted.status_code == 204
    assert "patreon" not in [item["key"] for item in _public(db_client)["methods"]]


def test_duplicate_method_key_is_rejected(db_client, admin_user):
    response = db_client.post(
        f"{ADMIN}/methods",
        json={"key": "boosty", "button_label": "Again", "url": "https://boosty.to/x"},
        headers=admin_user["headers"],
    )
    assert response.status_code == 409


def test_deleted_default_method_is_not_resurrected(db_client, admin_user):
    methods = db_client.get(f"{ADMIN}/methods", headers=admin_user["headers"]).json()
    boosty = next(item for item in methods["items"] if item["key"] == "boosty")
    assert (
        db_client.delete(
            f"{ADMIN}/methods/{boosty['id']}", headers=admin_user["headers"]
        ).status_code
        == 204
    )

    assert "boosty" not in [item["key"] for item in _public(db_client)["methods"]]
    # The seed runs once, so a second request must not re-create the method.
    assert "boosty" not in [item["key"] for item in _public(db_client)["methods"]]
    assert "dalink" in [item["key"] for item in _public(db_client)["methods"]]


# ---------------------------------------------------------------------------
# Analytics
# ---------------------------------------------------------------------------
def test_support_events_are_aggregated(db_client, admin_user):
    for _ in range(2):
        assert (
            db_client.post(
                f"{SUPPORT}/events", json={"event": "support_view"}
            ).status_code
            == 204
        )
    assert (
        db_client.post(
            f"{SUPPORT}/events",
            json={"event": "support_click", "method_key": "boosty"},
        ).status_code
        == 204
    )
    assert (
        db_client.post(
            f"{SUPPORT}/events", json={"event": "support_faq_open"}
        ).status_code
        == 204
    )

    stats = db_client.get(f"{ADMIN}/stats?days=30", headers=admin_user["headers"])
    assert stats.status_code == 200, stats.text
    body = stats.json()
    assert body["views"] == 2
    assert body["clicks"] == 1
    assert body["faq_opens"] == 1
    assert body["by_method"][0]["method_key"] == "boosty"
    assert body["by_method"][0]["clicks"] == 1

    assert (
        db_client.post(f"{SUPPORT}/events", json={"event": "nope"}).status_code == 422
    )
    assert (
        db_client.post(
            f"{SUPPORT}/events",
            json={"event": "support_click", "method_key": "bad key!"},
        ).status_code
        == 422
    )


# ---------------------------------------------------------------------------
# Resilience and static page
# ---------------------------------------------------------------------------
def test_public_page_survives_empty_or_partial_content(
    db_client, db_session, admin_user
):
    from app.models.support_page import SupportPage

    # Make sure the row exists, then write a broken document by hand.
    _state(db_client, admin_user)
    row = db_session.get(SupportPage, 1)
    assert row is not None
    row.published_content = {}
    db_session.commit()

    data = _public(db_client)
    assert data["order"] == DEFAULT_ORDER
    assert data["sections"]["hero"]["title"]["html"] == "Поддержать Fretfight"

    row = db_session.get(SupportPage, 1)
    row.published_content = {
        "order": ["hero"],
        "sections": {
            "hero": {
                "visible": True,
                "title": {"ru": "Ручной заголовок", "en": ""},
            }
        },
    }
    db_session.commit()

    data = _public(db_client)
    assert data["order"] == ["hero"]
    assert data["sections"]["hero"]["title"]["html"] == "Ручной заголовок"
    assert data["sections"]["hero"]["kicker"]["html"] == ""

    english = _public(db_client, "en")
    assert english["sections"]["hero"]["title"]["html"] == "Ручной заголовок"


def test_support_routes_and_headers(db_client):
    page = db_client.get("/support")
    assert page.status_code == 200
    assert page.headers["content-type"].startswith("text/html")
    assert "charset=utf-8" in page.headers["content-type"].lower()
    assert page.headers["cache-control"].startswith("no-cache")
    csp = page.headers.get("content-security-policy", "")
    assert "default-src 'self'" in csp
    assert "script-src 'self'" in csp
    assert "frame-src 'self'" in csp
    assert "object-src 'none'" in csp
    assert "support-render.js" in page.text

    donate = db_client.get("/donate", follow_redirects=False)
    assert donate.status_code == 308
    assert donate.headers["location"] == "/support"

    admin_page = db_client.get("/admin/")
    assert admin_page.status_code == 200
    assert "content-security-policy" in admin_page.headers


def test_support_static_page_has_no_inline_scripts():
    html = (STATIC / "app" / "support.html").read_text(encoding="utf-8")
    assert "<script>" not in html
    for attrs in re.findall(r"<script([^>]*)>", html):
        assert "src=" in attrs
    assert 'property="og:title"' in html
    assert 'name="description"' in html
    assert 'rel="canonical" href="https://fretfight.online/support"' in html
