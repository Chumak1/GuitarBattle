"""Song asset versions: ``song_files.is_active`` and the 27.10 rollback.

The public API serves exactly one active file per ``(song, role)``.  These
tests cover the migration data repair, the public filter and the admin switch.
"""
from __future__ import annotations

import importlib.util
import uuid
from datetime import datetime, timezone
from pathlib import Path
from uuid import UUID

import pytest

from app.models.song import Song, SongFile
from tests import helpers

pytestmark = pytest.mark.db

MIGRATION_PATH = (
    Path(__file__).resolve().parents[1]
    / "migrations"
    / "versions"
    / "0011_asset_versions.py"
)

FIRST_SONG_ID = UUID("d8d8d7b8-6346-4b2b-85b3-458ff503317e")
FIRST_SONG_SLUG = "pridumay-svetly-mir"
METAL_SONG_ID = UUID("26269652-9c6e-4343-8467-ed49fc9ba190")
METAL_SONG_SLUG = "metal-song"

UTC = timezone.utc


def _load_migration():
    spec = importlib.util.spec_from_file_location(
        "migration_0011_asset_versions", MIGRATION_PATH
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _upload_midi(db_client, admin_user, song_id, bytes_value=b"", note=60):
    if not bytes_value:
        bytes_value = helpers.build_midi_bytes(note=note)
    response = db_client.post(
        f"{helpers.API}/admin/songs/{song_id}/files/midi",
        files={"file": ("song.mid", bytes_value, "audio/midi")},
        headers=admin_user["headers"],
    )
    assert response.status_code == 201, response.text
    return response.json()


def _upload_stem(db_client, admin_user, song_id, role, marker):
    data = b"ID3" + bytes([marker]) * 128
    response = db_client.post(
        f"{helpers.API}/admin/songs/{song_id}/files/audio/{role}",
        files={"file": (f"{role}.mp3", data, "audio/mpeg")},
        headers=admin_user["headers"],
    )
    assert response.status_code == 201, response.text
    return response.json()


def _admin_files(db_client, admin_user, song_id):
    response = db_client.get(
        f"{helpers.API}/admin/songs/{song_id}/files",
        headers=admin_user["headers"],
    )
    assert response.status_code == 200, response.text
    return response.json()


def _public_detail(db_client, slug):
    response = db_client.get(f"{helpers.API}/songs/{slug}")
    assert response.status_code == 200, response.text
    return response.json()


def _seed_file(song, role, size_bytes, created_at, suffix=".mp3"):
    content_type = "audio/midi" if role == "midi" else "audio/mpeg"
    return SongFile(
        id=uuid.uuid4(),
        song_id=song.id,
        role=role,
        object_key=f"{song.id}/{uuid.uuid4()}{suffix}",
        content_type=content_type,
        size_bytes=size_bytes,
        sha256="0" * 64,
        created_at=created_at,
    )


def test_public_song_serves_only_active_files(db_client, db_session, admin_user):
    song = helpers.create_song(db_client, admin_user["headers"])
    helpers.publish_song(db_client, admin_user["headers"], song["id"])

    older = _upload_stem(db_client, admin_user, song["id"], "drums", 1)
    newer = _upload_stem(db_client, admin_user, song["id"], "drums", 2)
    midi = _upload_midi(db_client, admin_user, song["id"])

    detail = _public_detail(db_client, song["slug"])
    assert detail["assets"]["stems"]["drums"]["id"] == newer["id"]
    assert detail["assets"]["midi"]["id"] == midi["id"]

    # Archive the newest stem and re-activate the first upload behind the API's
    # back: the public endpoint must follow the flag, not the timestamp.
    db_session.execute(
        SongFile.__table__.update()
        .where(SongFile.id == UUID(newer["id"]))
        .values(is_active=False)
    )
    db_session.execute(
        SongFile.__table__.update()
        .where(SongFile.id == UUID(older["id"]))
        .values(is_active=True)
    )
    db_session.commit()

    detail = _public_detail(db_client, song["slug"])
    assert detail["assets"]["stems"]["drums"]["id"] == older["id"]
    assert [item["id"] for item in detail["files"]] == [midi["id"], older["id"]]
    assert detail["readiness"]["available_roles"] == ["midi", "drums"]


def test_migration_rolls_back_the_27_10_asset_uploads(db_session, db_engine):
    migration = _load_migration()

    first_song = Song(
        id=FIRST_SONG_ID,
        slug=FIRST_SONG_SLUG,
        title="Придумай светлый мир",
        genre="Rock",
        bpm=100,
        difficulty=2,
        duration_seconds=60,
        version=5,
        is_published=True,
    )
    metal_song = Song(
        id=METAL_SONG_ID,
        slug=METAL_SONG_SLUG,
        title="Metal song",
        genre="Metal",
        bpm=160,
        difficulty=4,
        duration_seconds=287,
        version=4,
        is_published=True,
    )
    db_session.add_all([first_song, metal_song])
    db_session.flush()

    good_midi = _seed_file(
        first_song, "midi", 7223, datetime(2026, 10, 21, 9, 46, tzinfo=UTC), ".mid"
    )
    good_stems = {
        role: _seed_file(
            first_song,
            role,
            960722,
            datetime(2026, 10, 21, 14, 43, tzinfo=UTC),
        )
        for role in ("drums", "bass", "guitar", "others")
    }
    # A later 21.10 re-upload must not become active after the rollback.
    long_stems = [
        _seed_file(first_song, role, 2400129, datetime(2026, 10, 21, 22, 29, tzinfo=UTC))
        for role in ("drums", "bass", "guitar", "others")
    ]
    bad_first = [
        _seed_file(
            first_song, role, 480_000, datetime(2026, 10, 27, 19, 26, minute, tzinfo=UTC)
        )
        for minute, role in enumerate(("drums", "bass", "guitar", "others"))
    ] + [
        _seed_file(
            first_song, "midi", 3000, datetime(2026, 10, 27, 19, 30, tzinfo=UTC), ".mid"
        )
    ]

    metal_midi = _seed_file(
        metal_song, "midi", 6912, datetime(2026, 10, 22, 8, 5, tzinfo=UTC), ".mid"
    )
    metal_stems = {
        role: _seed_file(
            metal_song, role, 11480293, datetime(2026, 10, 22, 8, 5, tzinfo=UTC)
        )
        for role in ("drums", "bass", "guitar", "others")
    }
    bad_metal = [
        _seed_file(
            metal_song, role, 600_000, datetime(2026, 10, 27, 19, 28, minute, tzinfo=UTC)
        )
        for minute, role in enumerate(("drums", "bass", "guitar", "others"))
    ]

    db_session.add_all(
        [
            good_midi,
            *good_stems.values(),
            *long_stems,
            *bad_first,
            metal_midi,
            *metal_stems.values(),
            *bad_metal,
        ]
    )
    db_session.commit()

    with db_engine.begin() as connection:
        migration.rollback_asset_versions(connection)

    db_session.expire_all()

    def active_files(song_id):
        return {
            stored.role: stored
            for stored in db_session.query(SongFile)
            .filter(SongFile.song_id == song_id, SongFile.is_active.is_(True))
            .all()
        }

    first_active = active_files(FIRST_SONG_ID)
    assert set(first_active) == {"midi", "drums", "bass", "guitar", "others"}
    assert first_active["midi"].id == good_midi.id
    assert first_active["midi"].size_bytes == 7223
    for role, stored in good_stems.items():
        assert first_active[role].id == stored.id
        assert first_active[role].size_bytes == 960722

    metal_active = active_files(METAL_SONG_ID)
    assert set(metal_active) == {"midi", "drums", "bass", "guitar", "others"}
    assert metal_active["midi"].id == metal_midi.id
    assert metal_active["midi"].size_bytes == 6912
    for role, stored in metal_stems.items():
        assert metal_active[role].id == stored.id
        assert metal_active[role].size_bytes == 11480293

    # Every upload from the bad 19:26-19:31 batch is archived.
    archived = (
        db_session.query(SongFile)
        .filter(SongFile.created_at >= datetime(2026, 10, 27, 19, 0, tzinfo=UTC))
        .all()
    )
    assert len(archived) == len(bad_first) + len(bad_metal)
    assert all(not stored.is_active for stored in archived)

    # The 21.10 22:29 re-upload is archived, so exactly one version per role wins.
    for stored in long_stems:
        assert stored.is_active is False

    # Clients cache assets by version: both songs must re-download.
    assert db_session.get(Song, FIRST_SONG_ID).version == 6
    assert db_session.get(Song, METAL_SONG_ID).version == 5


def test_activate_switches_public_asset_and_bumps_version(
    db_client, admin_user, published_song
):
    older = _upload_midi(db_client, admin_user, published_song["id"], note=60)
    newer = _upload_midi(db_client, admin_user, published_song["id"], note=64)

    detail = _public_detail(db_client, published_song["slug"])
    assert detail["assets"]["midi"]["id"] == newer["id"]
    version_before = detail["version"]

    admin_detail = db_client.get(
        f"{helpers.API}/admin/songs/{published_song['id']}",
        headers=admin_user["headers"],
    ).json()
    by_id = {item["id"]: item for item in admin_detail["files"]}
    assert by_id[newer["id"]]["is_active"] is True
    assert by_id[older["id"]]["is_active"] is False

    activated = db_client.post(
        f"{helpers.API}/admin/songs/{published_song['id']}/files/{older['id']}/activate",
        headers=admin_user["headers"],
    )
    assert activated.status_code == 200, activated.text
    assert activated.json()["is_active"] is True

    detail = _public_detail(db_client, published_song["slug"])
    assert detail["assets"]["midi"]["id"] == older["id"]
    assert detail["version"] == version_before + 1

    files = _admin_files(db_client, admin_user, published_song["id"])
    assert {item["id"]: item["is_active"] for item in files} == {
        older["id"]: True,
        newer["id"]: False,
    }

    # Activating a file of another song is a 404, not a silent switch.
    other = helpers.create_song(
        db_client, admin_user["headers"], slug=helpers.unique_slug("other")
    )
    missing = db_client.post(
        f"{helpers.API}/admin/songs/{other['id']}/files/{older['id']}/activate",
        headers=admin_user["headers"],
    )
    assert missing.status_code == 404


def test_new_upload_deactivates_previous_versions(
    db_client, admin_user, published_song
):
    first_stem = _upload_stem(db_client, admin_user, published_song["id"], "drums", 1)
    first_midi = _upload_midi(db_client, admin_user, published_song["id"], note=60)
    second_stem = _upload_stem(db_client, admin_user, published_song["id"], "drums", 2)
    second_midi = _upload_midi(db_client, admin_user, published_song["id"], note=62)

    files = _admin_files(db_client, admin_user, published_song["id"])
    active = {item["role"]: item["id"] for item in files if item["is_active"]}
    assert active == {"drums": second_stem["id"], "midi": second_midi["id"]}
    assert sum(1 for item in files if item["is_active"]) == 2
    assert {item["id"] for item in files} == {
        first_stem["id"],
        second_stem["id"],
        first_midi["id"],
        second_midi["id"],
    }

    detail = _public_detail(db_client, published_song["slug"])
    assert detail["assets"]["stems"]["drums"]["id"] == second_stem["id"]
    assert detail["assets"]["midi"]["id"] == second_midi["id"]
