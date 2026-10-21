from collections import defaultdict, deque
from io import BytesIO

import mido


class MidiParseError(ValueError):
    pass


def parse_midi(data: bytes) -> dict:
    try:
        midi = mido.MidiFile(file=BytesIO(data))
    except Exception as exc:
        raise MidiParseError("Could not parse MIDI data") from exc

    if midi.ticks_per_beat <= 0:
        raise MidiParseError("MIDI ticks_per_beat must be positive")

    # Collect absolute-tick events and a global tempo map.
    tempo_events = [(0, 0, 0, 500000)]
    raw_notes = []
    max_tick = 0

    for track_index, track in enumerate(midi.tracks):
        tick = 0
        order = 0
        active = defaultdict(deque)
        for message in track:
            tick += message.time
            max_tick = max(max_tick, tick)
            if message.type == "set_tempo":
                tempo_events.append((tick, track_index, order, message.tempo))
            elif message.type == "note_on" and message.velocity > 0:
                key = (message.channel, message.note)
                active[key].append((tick, message.velocity))
            elif message.type == "note_off" or (message.type == "note_on" and message.velocity == 0):
                key = (message.channel, message.note)
                if active[key]:
                    start_tick, velocity = active[key].popleft()
                    raw_notes.append({
                        "track_index": track_index,
                        "channel": message.channel,
                        "midi_note": message.note,
                        "velocity": velocity,
                        "start_tick": start_tick,
                        "end_tick": max(tick, start_tick),
                    })
            order += 1

        # Close dangling note-ons at the end of their track instead of losing them.
        for (channel, note), starts in active.items():
            while starts:
                start_tick, velocity = starts.popleft()
                raw_notes.append({
                    "track_index": track_index,
                    "channel": channel,
                    "midi_note": note,
                    "velocity": velocity,
                    "start_tick": start_tick,
                    "end_tick": max(tick, start_tick),
                })

    tempo_events.sort(key=lambda event: (event[0], event[1], event[2]))
    # At the same tick, the last encountered tempo wins.
    compact_tempos = []
    for tick, track_index, order, tempo in tempo_events:
        if compact_tempos and compact_tempos[-1][0] == tick:
            compact_tempos[-1] = (tick, tempo)
        else:
            compact_tempos.append((tick, tempo))
    if not compact_tempos or compact_tempos[0][0] != 0:
        compact_tempos.insert(0, (0, 500000))

    def ticks_to_seconds(target_tick: int) -> float:
        seconds = 0.0
        for i, (tempo_tick, tempo) in enumerate(compact_tempos):
            if target_tick <= tempo_tick:
                break
            next_tick = compact_tempos[i + 1][0] if i + 1 < len(compact_tempos) else target_tick
            segment_end = min(target_tick, next_tick)
            if segment_end > tempo_tick:
                seconds += mido.tick2second(segment_end - tempo_tick, midi.ticks_per_beat, tempo)
            if segment_end >= target_tick:
                break
        return seconds

    notes = []
    for note in raw_notes:
        start = ticks_to_seconds(note["start_tick"])
        end = ticks_to_seconds(note["end_tick"])
        notes.append({
            **note,
            "start_seconds": start,
            "duration_seconds": max(0.0, end - start),
        })
    notes.sort(key=lambda n: (n["start_tick"], n["track_index"], n["channel"], n["midi_note"]))

    initial_tempo = next((tempo for tick, tempo in compact_tempos if tick == 0), 500000)
    tempo_changes = [
        {"tick": tick, "bpm": round(60_000_000 / tempo, 6)}
        for tick, tempo in compact_tempos
    ]
    return {
        "midi_format": midi.type,
        "track_count": len(midi.tracks),
        "ticks_per_beat": midi.ticks_per_beat,
        "bpm": 60_000_000 / initial_tempo,
        "duration_seconds": ticks_to_seconds(max_tick),
        "note_count": len(notes),
        "tempo_changes": tempo_changes,
        "notes": notes,
    }
