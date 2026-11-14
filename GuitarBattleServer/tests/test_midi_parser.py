"""MIDI parser unit tests (no database required)."""
from io import BytesIO

import mido
import pytest

from app.services.midi_parser import MidiParseError, parse_midi
from tests import helpers


def test_parse_tiny_midi_file():
    data = helpers.build_midi_bytes(bpm=120, note=60, velocity=100, duration_ticks=480)
    parsed = parse_midi(data)

    assert parsed["midi_format"] == 0
    assert parsed["track_count"] == 1
    assert parsed["ticks_per_beat"] == 480
    assert parsed["bpm"] == pytest.approx(120.0)
    assert parsed["has_tempo"] is True
    assert parsed["duration_seconds"] == pytest.approx(0.5)
    assert parsed["note_count"] == 1
    assert parsed["tempo_changes"] == [{"tick": 0, "bpm": pytest.approx(120.0)}]

    note = parsed["notes"][0]
    assert note["track_index"] == 0
    assert note["channel"] == 0
    assert note["midi_note"] == 60
    assert note["velocity"] == 100
    assert note["start_tick"] == 0
    assert note["end_tick"] == 480
    assert note["start_seconds"] == pytest.approx(0.0)
    assert note["duration_seconds"] == pytest.approx(0.5)


def test_parse_respects_tempo_meta_event():
    data = helpers.build_midi_bytes(bpm=240, duration_ticks=480)
    parsed = parse_midi(data)
    assert parsed["has_tempo"] is True
    assert parsed["bpm"] == pytest.approx(240.0)
    # One beat at 240 BPM is 0.25 s.
    assert parsed["duration_seconds"] == pytest.approx(0.25)
    assert parsed["notes"][0]["duration_seconds"] == pytest.approx(0.25)


def test_explicit_tempo_event_wins_over_fallback_bpm():
    """A written-down tempo is never replaced by the caller's fallback."""
    data = helpers.build_midi_bytes(bpm=240, duration_ticks=480)
    parsed = parse_midi(data, fallback_bpm=70)

    assert parsed["has_tempo"] is True
    assert parsed["bpm"] == pytest.approx(240.0)
    assert parsed["duration_seconds"] == pytest.approx(0.25)
    assert parsed["tempo_changes"] == [{"tick": 0, "bpm": pytest.approx(240.0)}]


def test_missing_tempo_event_uses_fallback_bpm():
    """Files exported without set_tempo follow the real song tempo.

    At 500 ticks per beat, tick 4640 is 9.28 beats: 4.64 s at the MIDI default
    of 120 BPM, but 7.954 s at the song tempo of 70 BPM (the ``blue-river``
    case: the chart used to be frozen 1.714x too slow).
    """
    data = helpers.build_midi_bytes(
        ticks_per_beat=500,
        start_tick=4640,
        duration_ticks=500,
        include_tempo=False,
    )

    default = parse_midi(data)
    assert default["has_tempo"] is False
    assert default["bpm"] == pytest.approx(120.0)
    assert default["notes"][0]["start_seconds"] == pytest.approx(4.64)

    fallback = parse_midi(data, fallback_bpm=70)
    assert fallback["has_tempo"] is False
    assert fallback["bpm"] == pytest.approx(70.0)
    assert fallback["notes"][0]["start_seconds"] == pytest.approx(7.9542857, rel=1e-6)
    assert fallback["duration_seconds"] == pytest.approx(10.28 * 60 / 70, rel=1e-6)
    assert fallback["tempo_changes"] == [{"tick": 0, "bpm": pytest.approx(70.0)}]


@pytest.mark.parametrize("fallback", [None, 0, -70, float("nan"), "fast"])
def test_unusable_fallback_bpm_keeps_midi_default(fallback):
    data = helpers.build_midi_bytes(
        ticks_per_beat=500,
        start_tick=4640,
        duration_ticks=500,
        include_tempo=False,
    )
    parsed = parse_midi(data, fallback_bpm=fallback)
    assert parsed["bpm"] == pytest.approx(120.0)
    assert parsed["notes"][0]["start_seconds"] == pytest.approx(4.64)


def test_parse_multiple_notes_on_one_track():
    midi = mido.MidiFile(type=0, ticks_per_beat=480)
    track = mido.MidiTrack()
    midi.tracks.append(track)
    track.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(120), time=0))
    track.append(mido.Message("note_on", note=60, velocity=90, time=0))
    track.append(mido.Message("note_off", note=60, velocity=0, time=240))
    track.append(mido.Message("note_on", note=64, velocity=80, time=0))
    track.append(mido.Message("note_off", note=64, velocity=0, time=240))
    buffer = BytesIO()
    midi.save(file=buffer)

    parsed = parse_midi(buffer.getvalue())
    assert parsed["note_count"] == 2
    assert [note["midi_note"] for note in parsed["notes"]] == [60, 64]
    assert parsed["notes"][0]["duration_seconds"] == pytest.approx(0.25)
    assert parsed["notes"][1]["start_tick"] == 240
    assert parsed["duration_seconds"] == pytest.approx(0.5)


def test_dangling_note_on_is_closed_at_track_end():
    midi = mido.MidiFile(type=0, ticks_per_beat=480)
    track = mido.MidiTrack()
    midi.tracks.append(track)
    track.append(mido.Message("note_on", note=67, velocity=70, time=0))
    track.append(mido.MetaMessage("end_of_track", time=480))
    buffer = BytesIO()
    midi.save(file=buffer)

    parsed = parse_midi(buffer.getvalue())
    assert parsed["note_count"] == 1
    assert parsed["notes"][0]["end_tick"] == 480


@pytest.mark.parametrize("payload", [b"", b"not a midi file", b"MThd\x00\x00\x00\x06\x00\x00"])
def test_malformed_midi_is_rejected(payload):
    with pytest.raises(MidiParseError):
        parse_midi(payload)
