"""Campaigns ("Tower riff"): admin CRUD, ordered stages and the public view.

The product rules pinned here:

* an administrator names a campaign, chooses one bot difficulty and adds an
  ordered list of songs; the number of stages equals the number of songs;
* there is no campaign-count limit;
* the public endpoint exposes only published songs and omits a campaign that
  has no playable stage;
* deleting a campaign removes its stages, and permanently deleting a song
  removes its stage from every campaign (``ON DELETE CASCADE``).
"""
from __future__ import annotations

import uuid
from pathlib import Path

from sqlalchemy import delete, func, select, update

from app.models.campaign import Campaign, CampaignSong
from app.models.song import Song
from tests import helpers

ADMIN = f"{helpers.API}/admin/campaigns"
PUBLIC = f"{helpers.API}/campaigns"

CAMPAIGN_KEYS = {
    "id",
    "name",
    "bot_level",
    "song_count",
    "songs",
    "reward_character_id",
    "reward_character_name",
    "reward_condition",
    "cover_image_url",
    "description",
    "created_at",
    "updated_at",
}
SONG_KEYS = {
    "song_id",
    "slug",
    "title",
    "difficulty",
    "is_published",
    "position",
    "bot_level",
}


def _create(client, admin_user, *, name: str = "Tower riff", bot_level: int = 1):
    return client.post(
        ADMIN,
        json={"name": name, "bot_level": bot_level},
        headers=admin_user["headers"],
    )


def _put_songs(client, admin_user, campaign_id: str, song_ids: list[str]):
    return client.put(
        f"{ADMIN}/{campaign_id}/songs",
        json={"song_ids": song_ids},
        headers=admin_user["headers"],
    )


def _put_levels(client, admin_user, campaign_id: str, songs: list[dict]):
    return client.put(
        f"{ADMIN}/{campaign_id}/songs",
        json={"songs": songs},
        headers=admin_user["headers"],
    )


def _get_admin(client, admin_user, campaign_id: str):
    return client.get(f"{ADMIN}/{campaign_id}", headers=admin_user["headers"])


def _public(client):
    response = client.get(PUBLIC)
    assert response.status_code == 200, response.text
    return response.json()["items"]


def _songs(client, admin_user, count: int):
    return [
        helpers.create_song(client, admin_user["headers"], title=f"Campaign Song {index}")
        for index in range(count)
    ]


# ---------------------------------------------------------------------------
# Access control
# ---------------------------------------------------------------------------
def test_campaigns_require_admin(db_client, user_account):
    payload = {"name": "Forbidden tower", "bot_level": 0}

    assert db_client.get(ADMIN).status_code == 401
    assert db_client.post(ADMIN, json=payload).status_code == 401
    assert (
        db_client.get(ADMIN, headers=user_account["headers"]).status_code == 403
    )
    assert (
        db_client.post(
            ADMIN, json=payload, headers=user_account["headers"]
        ).status_code
        == 403
    )
    # The public list stays anonymous.
    public = db_client.get(PUBLIC)
    assert public.status_code == 200
    assert public.json() == {"items": []}


# ---------------------------------------------------------------------------
# CRUD
# ---------------------------------------------------------------------------
def test_campaign_crud_roundtrip(db_client, admin_user):
    created = _create(db_client, admin_user, name="  Tower riff  ", bot_level=2)

    assert created.status_code == 201, created.text
    campaign = created.json()
    assert set(campaign) == CAMPAIGN_KEYS
    assert campaign["name"] == "Tower riff"  # trimmed
    assert campaign["bot_level"] == 2
    assert campaign["song_count"] == 0
    assert campaign["songs"] == []
    assert campaign["created_at"]
    assert campaign["updated_at"]

    campaign_id = campaign["id"]
    single = _get_admin(db_client, admin_user, campaign_id)
    assert single.status_code == 200, single.text
    assert single.json()["id"] == campaign_id

    listed = db_client.get(ADMIN, headers=admin_user["headers"])
    assert listed.status_code == 200, listed.text
    assert [item["id"] for item in listed.json()["items"]] == [campaign_id]

    patched = db_client.patch(
        f"{ADMIN}/{campaign_id}",
        json={"name": "Tower riff II", "bot_level": 0},
        headers=admin_user["headers"],
    )
    assert patched.status_code == 200, patched.text
    assert patched.json()["name"] == "Tower riff II"
    assert patched.json()["bot_level"] == 0

    deleted = db_client.delete(f"{ADMIN}/{campaign_id}", headers=admin_user["headers"])
    assert deleted.status_code == 204
    assert _get_admin(db_client, admin_user, campaign_id).status_code == 404
    assert db_client.get(ADMIN, headers=admin_user["headers"]).json()["items"] == []


def test_campaign_validation_and_duplicate_name(db_client, admin_user):
    assert (
        db_client.post(
            ADMIN,
            json={"name": "", "bot_level": 1},
            headers=admin_user["headers"],
        ).status_code
        == 422
    )
    assert (
        db_client.post(
            ADMIN,
            json={"name": "Bad bot", "bot_level": 3},
            headers=admin_user["headers"],
        ).status_code
        == 422
    )

    assert _create(db_client, admin_user, name="Tower riff").status_code == 201
    # Names are unique case-insensitively.
    duplicate = _create(db_client, admin_user, name="tower RIFF")
    assert duplicate.status_code == 409, duplicate.text

    second = _create(db_client, admin_user, name="Second tower")
    assert second.status_code == 201
    other = _create(db_client, admin_user, name="Third tower").json()
    rename = db_client.patch(
        f"{ADMIN}/{other['id']}",
        json={"name": "Tower RIFF"},
        headers=admin_user["headers"],
    )
    assert rename.status_code == 409, rename.text


def test_admin_list_is_stable_by_name(db_client, admin_user):
    _create(db_client, admin_user, name="Zeta")
    _create(db_client, admin_user, name="Alpha")
    _create(db_client, admin_user, name="Beta")

    items = db_client.get(ADMIN, headers=admin_user["headers"]).json()["items"]
    assert [item["name"] for item in items] == ["Alpha", "Beta", "Zeta"]


# ---------------------------------------------------------------------------
# Ordered songs
# ---------------------------------------------------------------------------
def test_ordered_songs_are_stored_by_array_position(db_client, admin_user):
    campaign = _create(db_client, admin_user).json()
    songs = _songs(db_client, admin_user, 3)
    order = [songs[2]["id"], songs[0]["id"], songs[1]["id"]]

    response = _put_songs(db_client, admin_user, campaign["id"], order)
    assert response.status_code == 200, response.text
    body = response.json()
    assert body["song_count"] == 3
    assert [song["song_id"] for song in body["songs"]] == order
    assert [song["position"] for song in body["songs"]] == [0, 1, 2]
    assert set(body["songs"][0]) == SONG_KEYS
    assert body["songs"][0]["slug"] == songs[2]["slug"]
    assert body["songs"][0]["title"] == songs[2]["title"]
    assert body["songs"][0]["difficulty"] == songs[2]["difficulty"]
    assert body["songs"][0]["is_published"] is False

    # The same order comes back from the detail endpoint.
    detail = _get_admin(db_client, admin_user, campaign["id"]).json()
    assert [song["song_id"] for song in detail["songs"]] == order

    # Replacement shrinks the list; an empty array clears it.
    replaced = _put_songs(db_client, admin_user, campaign["id"], [songs[1]["id"]])
    assert replaced.status_code == 200, replaced.text
    assert [song["position"] for song in replaced.json()["songs"]] == [0]

    cleared = _put_songs(db_client, admin_user, campaign["id"], [])
    assert cleared.status_code == 200, cleared.text
    assert cleared.json()["song_count"] == 0
    assert cleared.json()["songs"] == []


def test_duplicate_song_ids_are_rejected_without_changing_the_list(
    db_client, admin_user
):
    campaign = _create(db_client, admin_user).json()
    songs = _songs(db_client, admin_user, 2)
    assert (
        _put_songs(db_client, admin_user, campaign["id"], [songs[0]["id"]]).status_code
        == 200
    )

    duplicate = _put_songs(
        db_client,
        admin_user,
        campaign["id"],
        [songs[1]["id"], songs[1]["id"]],
    )
    assert duplicate.status_code == 422, duplicate.text
    assert "duplicate" in duplicate.text.lower()

    after = _get_admin(db_client, admin_user, campaign["id"]).json()
    assert [song["song_id"] for song in after["songs"]] == [songs[0]["id"]]


def test_unknown_song_is_rejected_without_changing_the_list(db_client, admin_user):
    campaign = _create(db_client, admin_user).json()
    songs = _songs(db_client, admin_user, 1)
    assert (
        _put_songs(db_client, admin_user, campaign["id"], [songs[0]["id"]]).status_code
        == 200
    )

    missing = _put_songs(
        db_client,
        admin_user,
        campaign["id"],
        [songs[0]["id"], str(uuid.uuid4())],
    )
    assert missing.status_code == 404, missing.text
    assert "unknown song" in missing.text.lower()

    after = _get_admin(db_client, admin_user, campaign["id"]).json()
    assert [song["song_id"] for song in after["songs"]] == [songs[0]["id"]]


def test_unknown_campaign_returns_404(db_client, admin_user):
    unknown = str(uuid.uuid4())
    assert _get_admin(db_client, admin_user, unknown).status_code == 404
    assert (
        db_client.patch(
            f"{ADMIN}/{unknown}",
            json={"name": "Ghost"},
            headers=admin_user["headers"],
        ).status_code
        == 404
    )
    assert (
        db_client.delete(f"{ADMIN}/{unknown}", headers=admin_user["headers"]).status_code
        == 404
    )
    assert _put_songs(db_client, admin_user, unknown, []).status_code == 404


# ---------------------------------------------------------------------------
# Per-stage bot difficulty
# ---------------------------------------------------------------------------
def test_per_stage_levels_round_trip_through_admin_and_public(
    db_client, admin_user
):
    campaign = _create(db_client, admin_user, bot_level=1).json()
    songs = _songs(db_client, admin_user, 3)
    levels = [0, 2, 1]

    response = _put_levels(
        db_client,
        admin_user,
        campaign["id"],
        [
            {"song_id": song["id"], "bot_level": level}
            for song, level in zip(songs, levels)
        ],
    )
    assert response.status_code == 200, response.text
    body = response.json()
    # The campaign-wide value stays the default; stages carry their own level.
    assert body["bot_level"] == 1
    assert [song["bot_level"] for song in body["songs"]] == levels
    assert set(body["songs"][0]) == SONG_KEYS

    # Reopening the editor (GET) shows exactly the saved values.
    detail = _get_admin(db_client, admin_user, campaign["id"]).json()
    assert [song["bot_level"] for song in detail["songs"]] == levels

    for song in songs:
        helpers.publish_song(db_client, admin_user["headers"], song["id"])
    items = _public(db_client)
    assert [song["bot_level"] for song in items[0]["songs"]] == levels


def test_stage_level_is_independent_of_the_campaign_default(
    db_client, admin_user
):
    campaign = _create(db_client, admin_user, bot_level=1).json()
    first, second = _songs(db_client, admin_user, 2)

    # The second entry omits bot_level: it follows the campaign default.
    response = _put_levels(
        db_client,
        admin_user,
        campaign["id"],
        [
            {"song_id": first["id"], "bot_level": 0},
            {"song_id": second["id"]},
        ],
    )
    assert response.status_code == 200, response.text
    assert [song["bot_level"] for song in response.json()["songs"]] == [0, 1]

    # Changing the campaign default moves the fallback stage only.
    patched = db_client.patch(
        f"{ADMIN}/{campaign['id']}",
        json={"bot_level": 2},
        headers=admin_user["headers"],
    )
    assert patched.status_code == 200, patched.text
    assert [song["bot_level"] for song in patched.json()["songs"]] == [0, 2]


def test_legacy_song_ids_preserve_levels_and_default_new_songs(
    db_client, admin_user
):
    campaign = _create(db_client, admin_user, bot_level=1).json()
    first, second, third = _songs(db_client, admin_user, 3)
    assert (
        _put_levels(
            db_client,
            admin_user,
            campaign["id"],
            [
                {"song_id": first["id"], "bot_level": 0},
                {"song_id": second["id"], "bot_level": 2},
            ],
        ).status_code
        == 200
    )

    # Legacy reorder keeps the per-stage levels of the songs that stay.
    reordered = _put_songs(
        db_client, admin_user, campaign["id"], [second["id"], first["id"]]
    )
    assert reordered.status_code == 200, reordered.text
    assert [song["bot_level"] for song in reordered.json()["songs"]] == [2, 0]

    # A song added through the legacy shape gets the campaign default.
    replaced = _put_songs(
        db_client, admin_user, campaign["id"], [second["id"], third["id"]]
    )
    assert replaced.status_code == 200, replaced.text
    assert [song["bot_level"] for song in replaced.json()["songs"]] == [2, 1]


def test_null_stage_level_falls_back_to_campaign_default(
    db_client, admin_user, db_session
):
    """Rows written before migration 0028 (NULL) keep the old behaviour."""
    campaign = _create(db_client, admin_user, bot_level=2).json()
    song = _songs(db_client, admin_user, 1)[0]
    assert (
        _put_levels(
            db_client,
            admin_user,
            campaign["id"],
            [{"song_id": song["id"], "bot_level": 0}],
        ).status_code
        == 200
    )

    db_session.execute(
        update(CampaignSong)
        .where(CampaignSong.campaign_id == uuid.UUID(campaign["id"]))
        .values(bot_level=None)
    )
    db_session.commit()

    detail = _get_admin(db_client, admin_user, campaign["id"]).json()
    assert detail["songs"][0]["bot_level"] == 2


def test_invalid_stage_level_is_rejected(db_client, admin_user):
    campaign = _create(db_client, admin_user).json()
    song = _songs(db_client, admin_user, 1)[0]

    for level in (-1, 3, 99):
        response = _put_levels(
            db_client,
            admin_user,
            campaign["id"],
            [{"song_id": song["id"], "bot_level": level}],
        )
        assert response.status_code == 422, response.text

    # The rejected requests never touched the stored list.
    assert _get_admin(db_client, admin_user, campaign["id"]).json()["songs"] == []


def test_patch_single_stage_level_keeps_the_rest_intact(db_client, admin_user):
    campaign = _create(db_client, admin_user, bot_level=1).json()
    first, second = _songs(db_client, admin_user, 2)
    assert (
        _put_levels(
            db_client,
            admin_user,
            campaign["id"],
            [
                {"song_id": first["id"], "bot_level": 0},
                {"song_id": second["id"], "bot_level": 0},
            ],
        ).status_code
        == 200
    )

    url = f"{ADMIN}/{campaign['id']}/songs/{second['id']}"
    patched = db_client.patch(
        url, json={"bot_level": 2}, headers=admin_user["headers"]
    )
    assert patched.status_code == 200, patched.text
    body = patched.json()
    assert [song["song_id"] for song in body["songs"]] == [
        first["id"],
        second["id"],
    ]
    assert [song["position"] for song in body["songs"]] == [0, 1]
    assert [song["bot_level"] for song in body["songs"]] == [0, 2]

    # A song that is not a stage of this campaign is a 404, not a new stage.
    other = _songs(db_client, admin_user, 1)[0]
    missing = db_client.patch(
        f"{ADMIN}/{campaign['id']}/songs/{other['id']}",
        json={"bot_level": 1},
        headers=admin_user["headers"],
    )
    assert missing.status_code == 404, missing.text

    unknown_campaign = db_client.patch(
        f"{ADMIN}/{uuid.uuid4()}/songs/{first['id']}",
        json={"bot_level": 1},
        headers=admin_user["headers"],
    )
    assert unknown_campaign.status_code == 404

    invalid = db_client.patch(
        url, json={"bot_level": 3}, headers=admin_user["headers"]
    )
    assert invalid.status_code == 422, invalid.text


def test_songs_shape_requires_exactly_one_of_two_forms(db_client, admin_user):
    campaign = _create(db_client, admin_user).json()
    song = _songs(db_client, admin_user, 1)[0]
    url = f"{ADMIN}/{campaign['id']}/songs"

    both = db_client.put(
        url,
        json={
            "song_ids": [song["id"]],
            "songs": [{"song_id": song["id"], "bot_level": 1}],
        },
        headers=admin_user["headers"],
    )
    assert both.status_code == 422, both.text

    neither = db_client.put(url, json={}, headers=admin_user["headers"])
    assert neither.status_code == 422, neither.text

    unknown_field = db_client.put(
        url,
        json={"songs": [{"song_id": song["id"], "level": 1}]},
        headers=admin_user["headers"],
    )
    assert unknown_field.status_code == 422, unknown_field.text

    # The empty per-song form clears the campaign, exactly like song_ids: [].
    assert (
        _put_levels(db_client, admin_user, campaign["id"], []).status_code == 200
    )


# ---------------------------------------------------------------------------
# Public view
# ---------------------------------------------------------------------------
def test_public_endpoint_hides_unpublished_songs(db_client, admin_user):
    campaign = _create(db_client, admin_user, name="Public tower", bot_level=1).json()
    published, draft = _songs(db_client, admin_user, 2)
    helpers.publish_song(db_client, admin_user["headers"], published["id"])

    response = _put_songs(
        db_client, admin_user, campaign["id"], [published["id"], draft["id"]]
    )
    assert response.status_code == 200, response.text

    items = _public(db_client)
    assert len(items) == 1
    item = items[0]
    assert set(item) == CAMPAIGN_KEYS
    assert item["name"] == "Public tower"
    assert item["bot_level"] == 1
    # The draft stage exists in the admin list but not in the public payload.
    assert item["song_count"] == 1
    assert [song["song_id"] for song in item["songs"]] == [published["id"]]
    assert item["songs"][0]["is_published"] is True
    assert item["songs"][0]["position"] == 0


def test_public_endpoint_omits_campaigns_without_published_songs(
    db_client, admin_user
):
    campaign = _create(db_client, admin_user, name="Draft only").json()
    draft = _songs(db_client, admin_user, 1)[0]
    assert (
        _put_songs(db_client, admin_user, campaign["id"], [draft["id"]]).status_code
        == 200
    )

    assert _public(db_client) == []
    # The admin still sees it with its full stage list.
    admin_items = db_client.get(ADMIN, headers=admin_user["headers"]).json()["items"]
    assert admin_items[0]["song_count"] == 1
    assert admin_items[0]["songs"][0]["is_published"] is False

    helpers.publish_song(db_client, admin_user["headers"], draft["id"])
    public = _public(db_client)
    assert len(public) == 1
    assert public[0]["song_count"] == 1


def test_public_songs_are_sorted_by_position_and_campaigns_by_name(
    db_client, admin_user
):
    zeta = _create(db_client, admin_user, name="Zeta tower").json()
    alpha = _create(db_client, admin_user, name="Alpha tower").json()
    first, second = _songs(db_client, admin_user, 2)
    for song in (first, second):
        helpers.publish_song(db_client, admin_user["headers"], song["id"])

    assert (
        _put_songs(db_client, admin_user, zeta["id"], [second["id"], first["id"]]).status_code
        == 200
    )
    assert (
        _put_songs(db_client, admin_user, alpha["id"], [first["id"]]).status_code
        == 200
    )

    items = _public(db_client)
    assert [item["name"] for item in items] == ["Alpha tower", "Zeta tower"]
    zeta_item = items[1]
    assert [song["song_id"] for song in zeta_item["songs"]] == [
        second["id"],
        first["id"],
    ]
    assert [song["position"] for song in zeta_item["songs"]] == [0, 1]


# ---------------------------------------------------------------------------
# Cascades
# ---------------------------------------------------------------------------
def test_deleting_a_campaign_removes_its_stages(db_client, admin_user, db_session):
    campaign = _create(db_client, admin_user).json()
    song = _songs(db_client, admin_user, 1)[0]
    assert (
        _put_songs(db_client, admin_user, campaign["id"], [song["id"]]).status_code
        == 200
    )

    assert (
        db_client.delete(
            f"{ADMIN}/{campaign['id']}", headers=admin_user["headers"]
        ).status_code
        == 204
    )
    remaining = db_session.scalar(
        select(func.count())
        .select_from(CampaignSong)
        .where(CampaignSong.campaign_id == uuid.UUID(campaign["id"]))
    )
    assert remaining == 0


def test_deleting_a_song_removes_its_stage_from_every_campaign(
    db_client, admin_user, db_session
):
    first = _create(db_client, admin_user, name="First tower").json()
    second = _create(db_client, admin_user, name="Second tower").json()
    song = _songs(db_client, admin_user, 1)[0]
    for campaign in (first, second):
        assert (
            _put_songs(
                db_client, admin_user, campaign["id"], [song["id"]]
            ).status_code
            == 200
        )

    # Permanently deleting a song that was never played is allowed; the FK
    # cascade removes its campaign stages instead of blocking the delete.
    db_session.execute(delete(Song).where(Song.id == uuid.UUID(song["id"])))
    db_session.commit()

    assert db_session.scalar(select(func.count()).select_from(CampaignSong)) == 0
    assert db_session.scalar(select(func.count()).select_from(Campaign)) == 2
    assert _get_admin(db_client, admin_user, first["id"]).json()["song_count"] == 0


# ---------------------------------------------------------------------------
# Admin markup
# ---------------------------------------------------------------------------
def test_admin_campaigns_tab_markup():
    admin_dir = Path(__file__).resolve().parents[1] / "app" / "static" / "admin"
    html = (admin_dir / "index.html").read_text(encoding="utf-8")

    assert 'data-admin-section="campaigns"' in html
    assert 'data-admin-panel="campaigns"' in html
    assert 'id="sectionCampaigns"' in html
    assert "КАМПАНИИ" in html
    assert "TOWER RIFF" in html
    for element_id in (
        "campaignsBody",
        "campaignCreateForm",
        "campaignName",
        "campaignBotLevel",
        "campaignEditName",
        "campaignEditBotLevel",
        "campaignEditDescription",
        "campaignCoverPreview",
        "campaignCoverFile",
        "campaignCoverUpload",
        "campaignCoverRemove",
        "campaignSongSelect",
        "campaignSongAdd",
        "campaignSongsList",
        "campaignSave",
        "campaignDelete",
    ):
        assert f'id="{element_id}"' in html, element_id
    assert "EASY" in html and "NORMAL" in html and "HARD" in html
    assert "admin-campaigns.js" in html

    script = (admin_dir / "admin-campaigns.js").read_text(encoding="utf-8")
    assert "/admin/campaigns" in script
    assert "SONG_PAGE_LIMIT = 200" in script
    assert "bot_level" in script
    assert "data-stage-level" in script
    assert 'method: "PUT"' in script
    assert 'method: "PATCH"' in script
    assert "/cover" in script
    assert "MAX_COVER_BYTES" in script

    router_script = (admin_dir / "admin-site.js").read_text(encoding="utf-8")
    assert 'FFAdminCampaigns' in router_script
    assert 'state.section === "campaigns"' in router_script
    # Contacts are loaded with the landing tab, not as a separate section.
    assert 'data-admin-section="contacts"' not in html
