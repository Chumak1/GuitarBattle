"""Public catalog and admin song workflow tests."""
import pytest

from tests import helpers

pytestmark = pytest.mark.db


def test_public_catalog_starts_empty(db_client):
    response = db_client.get(f"{helpers.API}/songs")
    assert response.status_code == 200
    assert response.json() == {"items": [], "total": 0, "limit": 20, "offset": 0}


def test_draft_song_is_not_public(db_client, draft_song, admin_user):
    listing = db_client.get(f"{helpers.API}/songs")
    assert listing.status_code == 200
    assert listing.json()["total"] == 0

    detail = db_client.get(f"{helpers.API}/songs/{draft_song['slug']}")
    assert detail.status_code == 404

    admin_list = db_client.get(
        f"{helpers.API}/admin/songs", headers=admin_user["headers"]
    )
    assert admin_list.status_code == 200
    assert admin_list.json()["total"] == 1
    assert admin_list.json()["items"][0]["is_published"] is False


def test_publish_makes_song_visible(db_client, draft_song, admin_user):
    published = helpers.publish_song(
        db_client, admin_user["headers"], draft_song["id"]
    )
    assert published["is_published"] is True
    assert published["version"] == 1

    listing = db_client.get(f"{helpers.API}/songs").json()
    assert listing["total"] == 1
    item = listing["items"][0]
    assert item["slug"] == draft_song["slug"]
    assert item["title"] == draft_song["title"]
    assert item["artist"] == draft_song["artist"]
    assert item["genre"] == draft_song["genre"]
    assert item["bpm"] == draft_song["bpm"]
    assert item["difficulty"] == draft_song["difficulty"]
    assert "is_published" not in item

    detail = db_client.get(f"{helpers.API}/songs/{draft_song['slug']}")
    assert detail.status_code == 200
    assert detail.json()["files"] == []
    assert detail.json()["id"] == draft_song["id"]


def test_catalog_filters(db_client, admin_user):
    rock = helpers.create_song(
        db_client,
        admin_user["headers"],
        slug=helpers.unique_slug("rock"),
        title="Rock Anthem",
        artist="The Tests",
        genre="Rock",
        difficulty=2,
    )
    jazz = helpers.create_song(
        db_client,
        admin_user["headers"],
        slug=helpers.unique_slug("jazz"),
        title="Jazz Night",
        artist="Unit Players",
        genre="Jazz",
        difficulty=7,
    )
    helpers.publish_song(db_client, admin_user["headers"], rock["id"])
    helpers.publish_song(db_client, admin_user["headers"], jazz["id"])

    by_genre = db_client.get(f"{helpers.API}/songs", params={"genre": "rock"})
    assert by_genre.status_code == 200
    assert [item["slug"] for item in by_genre.json()["items"]] == [rock["slug"]]

    by_difficulty = db_client.get(f"{helpers.API}/songs", params={"difficulty": 7})
    assert [item["slug"] for item in by_difficulty.json()["items"]] == [jazz["slug"]]

    by_search = db_client.get(
        f"{helpers.API}/songs", params={"search": rock["slug"]}
    )
    assert [item["slug"] for item in by_search.json()["items"]] == [rock["slug"]]

    page = db_client.get(f"{helpers.API}/songs", params={"limit": 1, "offset": 1})
    assert page.json()["total"] == 2
    assert len(page.json()["items"]) == 1

    invalid = db_client.get(f"{helpers.API}/songs", params={"difficulty": 11})
    assert invalid.status_code == 422


def test_admin_song_creation_requires_admin(db_client, user_account):
    payload = {"slug": helpers.unique_slug(), "title": "Forbidden", "bpm": 120}
    anonymous = db_client.post(f"{helpers.API}/admin/songs", json=payload)
    assert anonymous.status_code == 401

    regular = db_client.post(
        f"{helpers.API}/admin/songs", json=payload, headers=user_account["headers"]
    )
    assert regular.status_code == 403


@pytest.mark.parametrize(
    "overrides",
    [
        {"slug": "Bad Slug"},
        {"slug": "double--dash"},
        {"title": "   "},
        {"bpm": 0},
        {"bpm": 401},
        {"difficulty": 0},
        {"duration_seconds": 0},
    ],
)
def test_admin_song_creation_validates_payload(db_client, admin_user, overrides):
    payload = {"slug": helpers.unique_slug(), "title": "Valid", "bpm": 120, **overrides}
    response = db_client.post(
        f"{helpers.API}/admin/songs", json=payload, headers=admin_user["headers"]
    )
    assert response.status_code == 422, response.text


def test_duplicate_song_slug_is_rejected(db_client, admin_user, draft_song):
    response = db_client.post(
        f"{helpers.API}/admin/songs",
        json={"slug": draft_song["slug"], "title": "Copy", "bpm": 100},
        headers=admin_user["headers"],
    )
    assert response.status_code == 409


def test_admin_can_delete_song(db_client, admin_user, draft_song):
    response = db_client.delete(
        f"{helpers.API}/admin/songs/{draft_song['id']}", headers=admin_user["headers"]
    )
    assert response.status_code == 204

    missing = db_client.get(
        f"{helpers.API}/admin/songs/{draft_song['id']}", headers=admin_user["headers"]
    )
    assert missing.status_code == 404


def test_midi_upload_analysis_and_public_download(
    db_client, admin_user, published_song, song_storage
):
    midi_bytes = helpers.build_midi_bytes(bpm=120, note=64, duration_ticks=480)

    upload = db_client.post(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/midi",
        files={"file": ("tiny.mid", midi_bytes, "audio/midi")},
        headers=admin_user["headers"],
    )
    assert upload.status_code == 201, upload.text
    stored = upload.json()
    assert stored["role"] == "midi"
    assert stored["content_type"] == "audio/midi"
    assert stored["size_bytes"] == len(midi_bytes)
    assert len(stored["sha256"]) == 64
    assert (song_storage / stored["object_key"]).is_file()

    detail = db_client.get(f"{helpers.API}/songs/{published_song['slug']}")
    assert detail.status_code == 200
    files = detail.json()["files"]
    assert len(files) == 1
    assert files[0]["role"] == "midi"
    assert files[0]["size_bytes"] == len(midi_bytes)
    assert files[0]["download_url"] == (
        f"{helpers.API}/songs/{published_song['slug']}/files/{stored['id']}/download"
    )
    assert "object_key" not in files[0]

    download = db_client.get(files[0]["download_url"])
    assert download.status_code == 200
    assert download.content == midi_bytes

    analysis = db_client.post(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/{stored['id']}/analyze",
        headers=admin_user["headers"],
    )
    assert analysis.status_code == 200, analysis.text
    body = analysis.json()
    assert body["song_file_id"] == stored["id"]
    assert body["note_count"] == 1
    assert body["notes_returned"] == 1
    assert body["notes_truncated"] is False
    assert body["bpm"] == pytest.approx(120.0)
    assert body["ticks_per_beat"] == 480
    assert body["duration_seconds"] == pytest.approx(0.5)
    assert body["notes"][0]["midi_note"] == 64
    assert body["notes"][0]["start_tick"] == 0
    assert body["notes"][0]["end_tick"] == 480
    assert body["tempo_changes"][0]["tick"] == 0

    saved = db_client.get(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/{stored['id']}/analysis",
        headers=admin_user["headers"],
    )
    assert saved.status_code == 200
    assert saved.json()["note_count"] == 1
    assert saved.json()["notes"][0]["midi_note"] == 64

    admin_files = db_client.get(
        f"{helpers.API}/admin/songs/{published_song['id']}/files",
        headers=admin_user["headers"],
    )
    assert admin_files.status_code == 200
    assert [item["id"] for item in admin_files.json()] == [stored["id"]]

    admin_download = db_client.get(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/{stored['id']}/download",
        headers=admin_user["headers"],
    )
    assert admin_download.status_code == 200
    assert admin_download.content == midi_bytes


def test_midi_upload_rejects_wrong_extension_or_content(
    db_client, admin_user, published_song
):
    wrong_extension = db_client.post(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/midi",
        files={"file": ("notes.txt", b"hello", "audio/midi")},
        headers=admin_user["headers"],
    )
    assert wrong_extension.status_code == 415

    fake_midi = db_client.post(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/midi",
        files={"file": ("fake.mid", b"not a midi file", "audio/midi")},
        headers=admin_user["headers"],
    )
    assert fake_midi.status_code == 422


def test_unpublish_hides_song_from_catalog(db_client, admin_user, published_song):
    response = db_client.post(
        f"{helpers.API}/admin/songs/{published_song['id']}/unpublish",
        headers=admin_user["headers"],
    )
    assert response.status_code == 200
    assert response.json()["is_published"] is False

    listing = db_client.get(f"{helpers.API}/songs").json()
    assert listing["total"] == 0
    detail = db_client.get(f"{helpers.API}/songs/{published_song['slug']}")
    assert detail.status_code == 404
