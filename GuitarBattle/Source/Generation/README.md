# Daily Challenge MIDI generation

`MidiMelodyGenerator` generates the guitar-only MIDI used by the existing tablature and scoring pipeline. Keep this file separate: the guitar lead remains the scoring source of truth.

`SongArrangement` generates independent backing MIDI parts from the same deterministic seed, tempo and genre profile:

- `DailyChallenge_Bass.mid` — genre-shaped bass line.
- `DailyChallenge_Drums.mid` — General MIDI drum channel 10.
- `DailyChallenge_Piano.mid` — piano harmony/accent part.
- `DailyChallenge_RhythmGuitar.mid` — chord voicings and genre-shaped rhythm-guitar hits.
- `DailyChallenge_Backing.mid` — multi-track MIDI with separate backing parts.

## Harmony profiles

`DailyHarmony.h` now provides four seed-selectable progressions for each of the 21 style IDs. The same seed-selected progression is held consistently through the song and shared by the lead and backing arranger. This avoids abrupt harmonic resets every four bars. This expands variation without changing the gameplay note/timing interface.

Genre arrangement now differentiates rhythm as well as harmony: power/neoclassical/symphonic metal use eighth-note rhythm-guitar attacks and double-kick eighth-note drive; djent/metalcore use more syncopation and half-time accents. The rhythm-guitar root pitch class is derived directly from the chord root to avoid transposed, dissonant voicings. The profiles separate:
- harmonic-minor cadences and diminished passing chords for power/neoclassical/symphonic metal;
- Phrygian bII motion and power-chord loops for progressive, technical and modern metal;
- dominant-seventh language for blues;
- Dorian/seventh-chord vamps for funk and fusion;
- major-family progressions for classic rock and country;
- Lydian/major-colour harmony for Polyphia-inspired instrumental prog;
- darker minor progressions for cinematic and alternative styles.

Some chromatic and dominant chords are intentional genre colour. The scale is not meant to forbid every non-diatonic note; the next improvement can make the melody scale adapt to the current chord/section more explicitly.

## Important limits

The MIDI lead remains separate from backing MIDI so `MidiParser`, tablature, pitch detection and scoring continue using the existing gameplay source. The audio playback engine's instrument routing is independent of this harmony change; the presence of MIDI files alone does not imply that every backing part is routed to audio on every device.

A future server-rendered audio workflow can use these same MIDI parts and metadata as its input, render them through licensed VST/SFZ instruments, then cache the rendered mix and stems.


Harmony consistency pass (v8): genre progressions are aligned more closely to their melody scale.
Power Metal stays in E harmonic minor; Phrygian metal styles avoid out-of-scale B5/Eb5/Bb5 power chords;
Dorian fusion uses diatonic voicings; and diminished chords keep their diminished fifth in rhythm guitar,
bass and heavy-style piano instead of being silently changed into perfect-fifth power chords. Blues
dominants remain intentionally bluesy.


## Power Rock reference tuning (2026-10-14)

The supplied Guitar Pro MIDI exports were inspected as multi-track MIDI. Across the
examples, the dominant meter is 4/4; the main reference tempos include 170, 180,
190 and 200 BPM, with some songs using tempo changes. Rhythm-guitar attacks are
commonly on an eighth-note grid (0.5 beat), while the drum parts frequently include
sixteenth-note activity (0.25 beat) and simultaneous kick/cymbal events.

The current app is intentionally restricted to one selectable style, shown as
"Power Rock". The generator uses a stable E-minor / harmonic-minor chord loop
(Em-C-Am-B7) to keep the lead and backing compatible while the rhythm, contour and
dynamics vary. Power Rock's piano part is left silent for now, so the guitar, bass,
and drums form the main arrangement without a competing block-chord/arpeggio layer.
This is a first controlled baseline, not a note-for-note copy of the reference songs.


## Power Rock pass (v10)

- Power Rock bass uses a dedicated eighth-note root/fifth riff with short note lengths, preventing sustained bass notes from overlapping later riff notes.
- Rhythm guitar keeps a coherent root/fifth/octave power-chord voicing and adds sparse verse rests while remaining straight in the chorus/finale.
- Fast sections add a small amount of extra kick support; the lead uses a more guitar-like register and selected phrase gaps.
- The scoring path, tab parser, WAV drum assets, and playback instrument loading are unchanged.


## Power Rock refinement v11

- Power Rock rhythm guitar now rotates four riff cells: gallop, straight eighth-note chorus, syncopated variation, and phrase-ending pickup. Guitar pitches remain tied to the active chord progression.
- Power Rock kick accents are less mechanically continuous and respond to build/chorus/finale energy. Phrase endings add a short snare/tom pickup.
- Other genre branches, gameplay scoring, tab generation, MIDI parsing, and drum WAV asset loading are unchanged.


## Melodic phrase refinement (v12)

- Power Rock now selects one of four melodic contour archetypes and one of four compact motif shapes from the deterministic seed. This increases identity without changing MIDI timing or gameplay note interfaces.
- Power Rock harmony now has four distinct E-minor progressions: classic epic cadence, descending drive, build-to-cadence, and melodic lift. Each remains compatible with the E harmonic-minor lead vocabulary and the B7 dominant cadence.
- The Power Metal phrase arc now makes a clearer opening/build/chorus/bridge/finale distinction: chorus peaks rise, the bridge briefly pulls back, and the final phrase prepares its resolution.
- MIDI writing, TabMapper, MidiParser, scoring, and backing-part interfaces are unchanged.

Validation note: this source-only patch was reviewed for local consistency, but a full JUCE build and audio/MIDI audition were not available in this environment. Please compile and audition several seeds in the project before shipping.


## Power Rock song-level harmony (v14 refinement)

Power Rock now chooses from 16 minor-key progression templates across seven
tonal centres (E, F#, G, A, B, C and D). The deterministic seed selects both
the key and the starting progression. Four-bar sections evolve through
different progression variants while staying in the same tonal centre, so
the backing and lead follow the same section harmony. Lead-scale construction
uses the selected tonic and transposable harmonic-minor collection. This is
intended to make each seed feel like a different composition rather than a
fixed E-minor loop with only a changed lead contour. No second harmonized lead
guitar is added.


## Power Rock melodic correction (v15)

- The Power Rock scale is now indexed from the selected tonic, so motif degrees transpose consistently across all seven keys instead of depending on which scale pitch happens to occur first above MIDI 60.
- Later sections include intermittent sixteenth-note lead runs with alternating ascending/descending shapes.
- Power Rock phrase resolutions now happen at two-bar boundaries rather than forcing a root resolution at every single bar, allowing the line to move through a longer phrase.
- Other styles and the gameplay note interface are unchanged.

Validation: ZIP/source structure and local static consistency checks only; a full JUCE build and audio audition are still required in Visual Studio.


## Power Rock melody refinement (v16)

Selected late-song sixteenth-note lead runs now target chord tones in an alternating ascending/descending arpeggio shape. The selection is constrained to the active DailyHarmony chord and the existing generated note events, so tab timing and scoring data remain unchanged. This is a general stylistic vocabulary improvement, not a transcription of any reference song.


## v17 — main hook and climax development

- Repeats recognisable hook positions in the chorus with a controlled lift.
- Develops the late solo using alternating rising sequences and descending answers.
- Pulls selected notes back before the final cadence.
- Changes only lead melody pitch-choice logic; MIDI event timing, tab mapping, scoring,
  audio engine, harmony definitions, arrangement, and drum WAV assets are untouched.
- Static checks only; build and listen in Visual Studio to verify the result.


## v18 - harmony and rhythm stabilization

- Power Rock melody now uses natural minor as its baseline, with active-chord tones added to the candidate pool. This keeps the raised leading tone available over V7 while preventing it from clashing with natural-minor VII-major harmony.
- Power Rock distorted rhythm-guitar voicings use a stable perfect fifth even on diminished harmony, reducing exposed tritones in the main riff. Other styles keep their prior diminished voicing behavior.
- Sixteenth-grid rhythm-guitar note durations are shortened so note-off events do not overlap the next possible hit on the same pitch.
- Scoring, tablature mapping, gameplay timing code, and drum WAV assets are unchanged.


## v19 Power Rock riff refinement

- Rhythm guitar now alternates a clipped low pedal tone with open higher-register power-chord accents and periodic four-note chord-tone flashes.
- Power Rock bass uses the same long-short-short gallop vocabulary on a sixteenth-note grid, with note-offs kept before the next attack.
- The tonic pedal yields to the dominant root on V7 bars to avoid rubbing against the raised leading tone.
- Light kick pickups are aligned with syncopated riff positions in build/chorus sections.
- Only `SongArrangement.cpp` changes the backing MIDI; lead scoring, tablature mapping, pitch detection, audio engine, and drum WAV assets are untouched.
- Standard MIDI does not store palm-muting articulation. Short note durations and velocity differences are used as an approximation; the actual articulation depends on the playback instrument.
