"""Chart freezing for Guitar Duels (server-internal timing).

The frozen ``duel_matches.note_times_ms`` must follow the tempo the stems were
recorded at, even when the MIDI has no ``set_tempo`` meta event. These tests
exercise ``duel_service.load_chart`` directly: the public duel contract does
not expose the chart, so only the frozen timings are asserted here.
"""
from __future__ import annotations

from uuid import UUID

import pytest
from sqlalchemy import select

from app.models.song import Song, SongFile
from app.models.song_midi import SongMidiAnalysis, SongMidiNote
from app.services.duel_service import load_chart

from tests import helpers

pytestmark = pytest.mark.db

API = "/api/v1"


def _upload_and_analyse(client, admin_headers, song_id: str, midi_bytes: bytes) -> dict:
    uploaded = client.post(
        f"{API}/admin/songs/{song_id}/files/midi",
        files={"file": ("song.mid", midi_bytes, "audio/midi")},
        headers=admin_headers,
    )
    assert uploaded.status_code == 201, uploaded.text
    file_id = uploaded.json()["id"]

    analysed = client.post(
        f"{API}/admin/songs/{song_id}/files/{file_id}/analyze",
        headers=admin_headers,
    )
    assert analysed.status_code == 200, analysed.text
    return analysed.json()


def _song_row(db_session, song_id: str) -> Song:
    song = db_session.get(Song, UUID(song_id))
    assert song is not None
    return song


def test_chart_uses_song_bpm_when_midi_has_no_tempo_event(
    db_client, admin_user, db_session
):
    """The ``blue-river`` regression: 70 BPM song, tempo-less MIDI.

    At 500 ticks per beat the first note sits on tick 4640 (9.28 beats). The
    MIDI default of 120 BPM freezes it at 4.64 s; the song tempo of 70 BPM
    puts it at ~7.954 s, which is what the stems actually play.
    """
    song = helpers.create_song(db_client, admin_user["headers"], bpm=70)
    helpers.publish_song(db_client, admin_user["headers"], song["id"])

    midi = helpers.build_midi_bytes(
        ticks_per_beat=500,
        start_tick=4640,
        duration_ticks=500,
        include_tempo=False,
    )
    analysis = _upload_and_analyse(
        db_client, admin_user["headers"], song["id"], midi
    )

    # The saved analysis is corrected too, so the admin view and the chart
    # cannot disagree about the tempo.
    assert analysis["bpm"] == pytest.approx(70.0)
    assert analysis["notes"][0]["start_seconds"] == pytest.approx(7.9542857, rel=1e-6)

    note_count, times = load_chart(db_session, _song_row(db_session, song["id"]))

    assert note_count == 1
    assert times == [7954]


def test_legacy_analysis_frozen_at_120_bpm_is_retimed_from_the_file(
    db_client, admin_user, db_session
):
    """Analyses saved before the fix still contain 120-BPM seconds.

    Production rows are not re-analysed, so ``load_chart`` has to re-read the
    MIDI and apply the song tempo itself; existing matches stay untouched
    because they already own a frozen copy.
    """
    song = helpers.create_song(db_client, admin_user["headers"], bpm=70)
    helpers.publish_song(db_client, admin_user["headers"], song["id"])

    midi = helpers.build_midi_bytes(
        ticks_per_beat=500,
        start_tick=4640,
        duration_ticks=500,
        include_tempo=False,
    )
    analysis = _upload_and_analyse(
        db_client, admin_user["headers"], song["id"], midi
    )
    file_id = UUID(analysis["song_file_id"])

    # Simulate the pre-fix analysis: seconds computed at the MIDI default.
    stored = db_session.scalar(
        select(SongMidiAnalysis).where(SongMidiAnalysis.song_file_id == file_id)
    )
    assert stored is not None
    stored.bpm = 120.0
    stored.duration_seconds = 8.81
    for note in db_session.scalars(
        select(SongMidiNote).where(SongMidiNote.song_file_id == file_id)
    ):
        note.start_seconds = 4.64
        note.duration_seconds = 1.0
    db_session.commit()

    note_count, times = load_chart(db_session, _song_row(db_session, song["id"]))

    assert note_count == 1
    assert times == [7954]


def test_explicit_tempo_event_wins_over_song_bpm(db_client, admin_user, db_session):
    """A written-down tempo is respected even when the metadata disagrees.

    The MIDI says 240 BPM, the song row says 70 BPM. The note starts one beat
    in, so the chart must use 250 ms (240 BPM), not 857 ms (70 BPM).
    """
    song = helpers.create_song(db_client, admin_user["headers"], bpm=70)
    helpers.publish_song(db_client, admin_user["headers"], song["id"])

    midi = helpers.build_midi_bytes(
        bpm=240,
        ticks_per_beat=480,
        start_tick=480,
        duration_ticks=480,
    )
    _upload_and_analyse(db_client, admin_user["headers"], song["id"], midi)

    note_count, times = load_chart(db_session, _song_row(db_session, song["id"]))

    assert note_count == 1
    assert times == [250]


def test_missing_midi_file_falls_back_to_saved_analysis(
    db_client, admin_user, db_session
):
    """A deleted file must not break match creation."""
    song = helpers.create_song(db_client, admin_user["headers"], bpm=70)
    helpers.publish_song(db_client, admin_user["headers"], song["id"])

    midi = helpers.build_midi_bytes(
        ticks_per_beat=500,
        start_tick=4640,
        duration_ticks=500,
        include_tempo=False,
    )
    analysis = _upload_and_analyse(
        db_client, admin_user["headers"], song["id"], midi
    )
    file_id = UUID(analysis["song_file_id"])

    # Legacy analysis (120 BPM) plus a file that no longer exists: the tempo
    # mismatch triggers the re-parse, which fails softly.
    stored = db_session.scalar(
        select(SongMidiAnalysis).where(SongMidiAnalysis.song_file_id == file_id)
    )
    assert stored is not None
    stored.bpm = 120.0
    for note in db_session.scalars(
        select(SongMidiNote).where(SongMidiNote.song_file_id == file_id)
    ):
        note.start_seconds = 4.64
        note.duration_seconds = 1.0

    song_file = db_session.scalar(
        select(SongFile).where(
            SongFile.song_id == UUID(song["id"]),
            SongFile.role == "midi",
            SongFile.is_active.is_(True),
        )
    )
    assert song_file is not None
    song_file.object_key = "missing/does-not-exist.mid"
    db_session.commit()

    note_count, times = load_chart(db_session, _song_row(db_session, song["id"]))

    # The saved analysis is used instead of crashing or dropping the chart to
    # the synthetic timing windows.
    assert note_count == 1
    assert times == [4640]
