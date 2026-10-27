"""Achievement icons: catalog files, API fields, static serving and migration.

Covers the contract added by migration ``0010_achievement_icons``:

* every one of the 27 catalog achievements has an ``icon`` file name and the
  matching SVG exists in ``app/static/media/achievements``;
* ``GET /achievements`` and ``GET /profile/users/{id}`` return ``icon`` and
  ``icon_url`` without breaking the older fields;
* ``/media/achievements/<icon>.svg`` is served as ``image/svg+xml``.
"""
from __future__ import annotations

import uuid
from pathlib import Path

import pytest
import sqlalchemy as sa

from app.services.achievement_catalog import ACHIEVEMENT_CATALOG
from tests import helpers

ICON_DIR = (
    Path(__file__).resolve().parents[1]
    / "app"
    / "static"
    / "media"
    / "achievements"
)
ICON_URL_PREFIX = "/media/achievements"
EXPECTED_ACHIEVEMENTS = 27


def _achievements(client, headers):
    response = client.get(f"{helpers.API}/achievements", headers=headers)
    assert response.status_code == 200, response.text
    return response.json()


def test_catalog_rows_point_to_existing_svg_files():
    """The catalog is the source of truth: every icon must be in the image."""
    assert len(ACHIEVEMENT_CATALOG) == EXPECTED_ACHIEVEMENTS

    used: dict[str, str] = {}
    for item in ACHIEVEMENT_CATALOG:
        icon = item["icon"]
        assert icon, item["code"]
        assert icon.endswith(".svg"), (item["code"], icon)
        path = ICON_DIR / icon
        assert path.is_file(), (item["code"], icon)
        assert 0 < path.stat().st_size < 64 * 1024, (item["code"], icon)
        text = path.read_text(encoding="utf-8", errors="replace")
        assert "<svg" in text, (item["code"], icon)
        used[icon] = item["code"]

    # Twelve to sixteen hand-picked icons are enough for the catalog; a stray
    # download or a temporary file would show up here.
    files = sorted(path.name for path in ICON_DIR.iterdir() if path.is_file())
    assert 12 <= len(files) <= 16, files
    assert set(files) == set(used), files


def test_achievements_api_exposes_icon_url(db_client, user_account):
    payload = _achievements(db_client, user_account["headers"])

    assert payload["total"] == EXPECTED_ACHIEVEMENTS
    assert len(payload["items"]) == EXPECTED_ACHIEVEMENTS

    icons = set()
    for item in payload["items"]:
        assert item["icon"], item["code"]
        assert item["icon_url"] == f"{ICON_URL_PREFIX}/{item['icon']}"
        assert (ICON_DIR / item["icon"]).is_file(), item["code"]
        # The fields the clients already relied on are still present.
        for field in ("title", "description", "category", "xp_bonus", "progress"):
            assert field in item
        assert item["unlocked"] is False
        icons.add(item["icon"])

    assert len(icons) >= 12


def test_public_profile_exposes_icon_url(db_client, user_account, db_session):
    from app.models.progression import UserAchievement

    user_id = uuid.UUID(user_account["id"])
    db_session.execute(
        UserAchievement.__table__.insert().values(
            user_id=user_id, achievement_code="first_notes", progress=1
        )
    )
    db_session.commit()

    response = db_client.get(
        f"{helpers.API}/profile/users/{user_id}",
        headers=user_account["headers"],
    )
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["achievements_unlocked"] == 1

    [achievement] = body["achievements"]
    assert achievement["code"] == "first_notes"
    assert achievement["icon"] == "first_notes.svg"
    assert achievement["icon_url"] == f"{ICON_URL_PREFIX}/first_notes.svg"
    assert achievement["awarded_at"]


def test_achievement_icon_is_served_as_svg(client):
    for icon in ("first_notes.svg", "star.svg", "hard.svg"):
        response = client.get(f"{ICON_URL_PREFIX}/{icon}")
        assert response.status_code == 200, icon
        assert response.headers["content-type"].startswith("image/svg+xml")
        assert b"<svg" in response.content


def test_unknown_achievement_icon_is_404(client):
    assert client.get(f"{ICON_URL_PREFIX}/nope.svg").status_code == 404


@pytest.mark.db
def test_achievement_icons_migration_is_applied(db_session):
    revision = db_session.execute(sa.text("SELECT version_num FROM alembic_version")).scalar()
    assert revision == "0010_achievement_icons"

    rows = db_session.execute(
        sa.text("SELECT code, icon FROM achievements")
    ).all()
    assert len(rows) == EXPECTED_ACHIEVEMENTS
    for code, icon in rows:
        assert icon, code
        assert (ICON_DIR / icon).is_file(), (code, icon)

    by_code = {code: icon for code, icon in rows}
    assert by_code["first_notes"] == "first_notes.svg"
    assert by_code["accuracy_80"] == "target.svg"
    assert by_code["genres_8"] == "genres.svg"
    assert by_code["mastery_5"] == "diploma.svg"
    assert by_code["daily_streak_3"] == "clock.svg"
    assert by_code["daily_streak_7"] == "calendar.svg"
    assert by_code["hard_80"] == "hard.svg"
