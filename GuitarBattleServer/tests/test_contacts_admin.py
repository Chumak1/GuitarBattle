"""Admin endpoints for the contacts block (``/admin/site/contacts``).

The values are stored in the single ``landing_settings`` row next to the
landing CMS content, so the tests also pin that saving contacts keeps
``blocks``/``hero_background`` and that saving the landing page keeps contacts.
"""
from __future__ import annotations

import pytest

from tests import helpers

DEFAULT_EMAIL = "chumakevgeny@gmail.com"
DEFAULT_TELEGRAM = "@chumakevgene"
SOCIAL_KEYS = {"youtube", "instagram", "twitter", "vk"}


def _payload(**overrides):
    payload = {
        "name_ru": "Иван Иванов",
        "name_en": "Ivan Ivanov",
        "email": "Ivan@Example.COM",
        "telegram": "ivan_contact",
        "socials": {
            "youtube": "https://www.youtube.com/@guitarbattle",
            "instagram": "",
            "twitter": None,
            "vk": "https://vk.com/guitarbattle",
        },
    }
    payload.update(overrides)
    return payload


def _put_contacts(client, admin_user, payload):
    return client.put(
        f"{helpers.API}/admin/site/contacts",
        json=payload,
        headers=admin_user["headers"],
    )


def _public_contacts(client):
    response = client.get(f"{helpers.API}/site/contacts")
    assert response.status_code == 200, response.text
    return response.json()


def _admin_contacts(client, admin_user):
    response = client.get(
        f"{helpers.API}/admin/site/contacts", headers=admin_user["headers"]
    )
    assert response.status_code == 200, response.text
    return response.json()


def _admin_landing(client, admin_user):
    response = client.get(
        f"{helpers.API}/admin/site/landing", headers=admin_user["headers"]
    )
    assert response.status_code == 200, response.text
    return response.json()


# ---------------------------------------------------------------------------
# Access control
# ---------------------------------------------------------------------------
def test_contacts_admin_requires_admin(db_client, user_account):
    payload = _payload()

    anonymous_get = db_client.get(f"{helpers.API}/admin/site/contacts")
    assert anonymous_get.status_code == 401
    forbidden_get = db_client.get(
        f"{helpers.API}/admin/site/contacts", headers=user_account["headers"]
    )
    assert forbidden_get.status_code == 403

    anonymous_put = db_client.put(f"{helpers.API}/admin/site/contacts", json=payload)
    assert anonymous_put.status_code == 401
    forbidden_put = db_client.put(
        f"{helpers.API}/admin/site/contacts",
        json=payload,
        headers=user_account["headers"],
    )
    assert forbidden_put.status_code == 403

    # The rejected requests changed nothing.
    assert _public_contacts(db_client)["email"] == DEFAULT_EMAIL


# ---------------------------------------------------------------------------
# Defaults and roundtrip
# ---------------------------------------------------------------------------
def test_admin_contacts_defaults(db_client, admin_user):
    body = _admin_contacts(db_client, admin_user)

    assert set(body) == {"name_ru", "name_en", "email", "telegram", "socials"}
    assert body["name_ru"] == "Чумак Евгений Александрович"
    assert body["name_en"] == "Evgeniy Aleksandrovich Chumak"
    assert body["email"] == DEFAULT_EMAIL
    assert body["telegram"] == DEFAULT_TELEGRAM
    assert body["socials"] == {network: None for network in SOCIAL_KEYS}


def test_contacts_roundtrip(db_client, admin_user):
    response = _put_contacts(db_client, admin_user, _payload())

    assert response.status_code == 200, response.text
    saved = response.json()
    assert saved["name_ru"] == "Иван Иванов"
    assert saved["name_en"] == "Ivan Ivanov"
    # Only the domain is lower-cased; the local part keeps its case.
    assert saved["email"] == "Ivan@example.com"
    assert saved["telegram"] == "@ivan_contact"
    assert saved["socials"] == {
        "youtube": "https://www.youtube.com/@guitarbattle",
        "instagram": None,
        "twitter": None,
        "vk": "https://vk.com/guitarbattle",
    }

    public = _public_contacts(db_client)
    assert public["name"] == {"ru": "Иван Иванов", "en": "Ivan Ivanov"}
    assert public["email"] == "Ivan@example.com"
    assert public["telegram"] == "@ivan_contact"
    assert public["telegram_url"] == "https://t.me/ivan_contact"
    assert public["socials"] == {
        "youtube": "https://www.youtube.com/@guitarbattle",
        "instagram": None,
        "twitter": None,
        "vk": "https://vk.com/guitarbattle",
    }

    # The admin endpoint returns exactly the editable fields.
    assert set(_admin_contacts(db_client, admin_user)) == {
        "name_ru",
        "name_en",
        "email",
        "telegram",
        "socials",
    }


@pytest.mark.parametrize(
    "value",
    [
        "chumakevgene",
        "@chumakevgene",
        "https://t.me/chumakevgene",
        "t.me/chumakevgene",
    ],
)
def test_telegram_is_normalized(db_client, admin_user, value):
    response = _put_contacts(db_client, admin_user, _payload(telegram=value))

    assert response.status_code == 200, response.text
    assert response.json()["telegram"] == "@chumakevgene"

    public = _public_contacts(db_client)
    assert public["telegram"] == "@chumakevgene"
    assert public["telegram_url"] == "https://t.me/chumakevgene"


def test_empty_name_and_telegram_are_allowed(db_client, admin_user):
    response = _put_contacts(
        db_client, admin_user, _payload(name_ru="", name_en="", telegram="")
    )

    assert response.status_code == 200, response.text
    public = _public_contacts(db_client)
    # Empty names stay empty: the page keeps its i18n default.
    assert public["name"] == {"ru": "", "en": ""}
    # An empty handle hides the row instead of producing a broken link.
    assert public["telegram"] == ""
    assert public["telegram_url"] == ""


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------
@pytest.mark.parametrize(
    ("case", "overrides", "field"),
    [
        ("email-empty", {"email": ""}, "email"),
        ("email-broken", {"email": "not-an-email"}, "email"),
        ("telegram-short", {"telegram": "@ab"}, "telegram"),
        ("telegram-spaces", {"telegram": "@bad handle!"}, "telegram"),
        (
            "youtube-scheme",
            {"socials": {"youtube": "javascript:alert(1)"}},
            "youtube",
        ),
        (
            "youtube-domain",
            {"socials": {"youtube": "http://evil.example/x"}},
            "youtube",
        ),
        ("name-too-long", {"name_ru": "Я" * 121}, "name_ru"),
        ("unknown-field", {"nickname": "someone"}, "nickname"),
    ],
)
def test_invalid_contacts_are_rejected(db_client, admin_user, case, overrides, field):
    response = _put_contacts(db_client, admin_user, _payload(**overrides))

    assert response.status_code == 422, f"{case}: {response.text}"
    assert field in response.text.lower()
    # A rejected request never partially saves the document.
    assert _public_contacts(db_client)["email"] == DEFAULT_EMAIL


def test_relative_social_url_is_rejected(db_client, admin_user):
    response = _put_contacts(
        db_client, admin_user, _payload(socials={"vk": "/vk/guitarbattle"})
    )

    assert response.status_code == 422, response.text
    assert "vk" in response.text.lower()


def test_other_networks_reject_foreign_hosts(db_client, admin_user):
    for network, url in (
        ("instagram", "https://vk.com/guitarbattle"),
        ("twitter", "https://instagram.com/guitarbattle"),
        ("vk", "https://twitter.com/guitarbattle"),
    ):
        response = _put_contacts(
            db_client, admin_user, _payload(socials={network: url})
        )
        assert response.status_code == 422, f"{network}: {response.text}"
        assert network in response.text.lower()


# ---------------------------------------------------------------------------
# The contacts document shares the row with the landing CMS
# ---------------------------------------------------------------------------
def test_saving_contacts_keeps_landing_blocks(db_client, admin_user):
    blocks = {"hero": {"visible": True, "ru": {"title": "Привет"}, "en": {"title": "Hi"}}}
    hero = {
        "type": "image",
        "url": "/media/landing/hero-test.png",
        "poster_url": None,
    }
    landing = db_client.put(
        f"{helpers.API}/admin/site/landing",
        json={"blocks": blocks, "hero_background": hero},
        headers=admin_user["headers"],
    )
    assert landing.status_code == 200, landing.text

    assert _put_contacts(db_client, admin_user, _payload()).status_code == 200

    saved = _admin_landing(db_client, admin_user)
    assert saved["blocks"] == blocks
    assert saved["hero_background"]["type"] == "image"
    assert saved["hero_background"]["url"] == "/media/landing/hero-test.png"


def test_saving_landing_keeps_contacts(db_client, admin_user):
    assert _put_contacts(db_client, admin_user, _payload()).status_code == 200

    landing = db_client.put(
        f"{helpers.API}/admin/site/landing",
        json={"blocks": {"cta": {"visible": False}}},
        headers=admin_user["headers"],
    )
    assert landing.status_code == 200, landing.text

    saved = _admin_contacts(db_client, admin_user)
    assert saved["name_ru"] == "Иван Иванов"
    assert saved["email"] == "Ivan@example.com"

    public = _public_contacts(db_client)
    assert public["telegram_url"] == "https://t.me/ivan_contact"
    assert public["socials"]["vk"] == "https://vk.com/guitarbattle"
