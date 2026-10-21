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
    assert parsed["bpm"] == pytest.approx(240.0)
    # One beat at 240 BPM is 0.25 s.
    assert parsed["duration_seconds"] == pytest.approx(0.25)
    assert parsed["notes"][0]["duration_seconds"] == pytest.approx(0.25)


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
