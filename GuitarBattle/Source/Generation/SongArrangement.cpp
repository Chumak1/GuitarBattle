#include "SongArrangement.h"
#include "DailyHarmony.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace
{
    using Track = juce::MidiMessageSequence;

    void addNote(Track& track, int channel, int pitch, int velocity,
                 double startBeat, double durationBeats, int ticksPerQuarter = 480)
    {
        const auto startTick = startBeat * ticksPerQuarter;
        const auto endTick = (startBeat + durationBeats) * ticksPerQuarter;
        track.addEvent(juce::MidiMessage::noteOn(channel, pitch,
            static_cast<juce::uint8>(juce::jlimit(1, 127, velocity))), startTick);
        track.addEvent(juce::MidiMessage::noteOff(channel, pitch), endTick);
    }

    bool writeMidi(const juce::File& file, Track track, int ticksPerQuarter,
                   double bpm, bool addTempo)
    {
        track.sort();
        juce::MidiFile midi;
        midi.setTicksPerQuarterNote(ticksPerQuarter);
        if (addTempo)
        {
            track.addEvent(juce::MidiMessage::tempoMetaEvent(
                static_cast<int>(std::round(60000000.0 / bpm))), 0.0);
            track.addEvent(juce::MidiMessage::timeSignatureMetaEvent(4, 4), 0.0);
            track.sort();
        }
        midi.addTrack(track);
        if (file.getParentDirectory().createDirectory().failed())
            return false;
        juce::FileOutputStream stream(file);
        if (!stream.openedOk())
            return false;
        stream.setPosition(0);
        stream.truncate();
        const bool ok = midi.writeTo(stream);
        stream.flush();
        return ok;
    }

    bool writeMultiTrack(const juce::File& file, Track bass, Track drums, Track piano, Track rhythmGuitar,
                         int ticksPerQuarter, double bpm)
    {
        bass.addEvent(juce::MidiMessage::tempoMetaEvent(
            static_cast<int>(std::round(60000000.0 / bpm))), 0.0);
        bass.addEvent(juce::MidiMessage::timeSignatureMetaEvent(4, 4), 0.0);
        bass.sort(); drums.sort(); piano.sort(); rhythmGuitar.sort();
        juce::MidiFile midi;
        midi.setTicksPerQuarterNote(ticksPerQuarter);
        midi.addTrack(bass);
        midi.addTrack(drums);
        midi.addTrack(piano);
    midi.addTrack(rhythmGuitar);
        if (file.getParentDirectory().createDirectory().failed())
            return false;
        juce::FileOutputStream stream(file);
        if (!stream.openedOk())
            return false;
        stream.setPosition(0);
        stream.truncate();
        const bool ok = midi.writeTo(stream);
        stream.flush();
        return ok;
    }
}

bool SongArrangement::generateBacking(const Settings& requested,
                                      const juce::File& outputDirectory,
                                      Result& result,
                                      juce::String* errorMessage)
{
    result = {};
    const int seed = requested.seed == 0 ? 1 : requested.seed;
    const double bpm = juce::jlimit(60.0, 210.0, requested.bpm);
    const int style = juce::jlimit(0, 20, requested.style);
    const int bars = juce::jlimit(4, 32, requested.bars);
    const int tpq = juce::jlimit(96, 960, requested.ticksPerQuarter);
    if (outputDirectory.getFullPathName().isEmpty())
    {
        if (errorMessage != nullptr) *errorMessage = "Backing MIDI output directory is empty.";
        return false;
    }

    std::mt19937 rng(static_cast<std::uint32_t>(seed));
    auto variation = [&rng](int low, int high)
    { return std::uniform_int_distribution<int>(low, high)(rng); };

    // Lead and backing use the same seed-derived progression per four-bar section.

    Track bass, drums, piano, rhythmGuitar;
    bass.addEvent(juce::MidiMessage::programChange(2, 33), 0.0); // fingered bass
    piano.addEvent(juce::MidiMessage::programChange(3, 0), 0.0); // acoustic grand
    rhythmGuitar.addEvent(juce::MidiMessage::programChange(1, 29), 0.0); // overdriven guitar hint for external MIDI players

    for (int bar = 0; bar < bars; ++bar)
    {
        const double barBeat = static_cast<double>(bar * 4);
        const auto chord = DailyHarmony::forSection(seed, style, bar / 4)[static_cast<size_t>(bar % 4)];
        const int sectionBar = bar % 8;
        const bool phraseEnding = sectionBar == 3 || sectionBar == 7;
        const double songProgress = bars <= 1 ? 0.0
            : static_cast<double>(bar) / static_cast<double>(bars - 1);
        // Four broad energy zones make the backing follow a song-like arc:
        // restrained opening, build, full chorus, then a short bridge/ending.
        const bool openingBar = (bar == 0);
        const bool buildBar = !openingBar && songProgress >= 0.18 && songProgress < 0.38;
        const bool chorusBar = songProgress >= 0.38 && songProgress < 0.68;
        const bool bridgeBar = songProgress >= 0.68 && bar < bars - 1;
        const bool finaleBar = (bar == bars - 1);
        const int energy = openingBar ? 0 : buildBar ? 1 : chorusBar ? 3
            : bridgeBar ? 2 : finaleBar ? 4 : 2;
        const int thirdInterval = (chord.tonesPc[1] - chord.tonesPc[0] + 12) % 12;
        const int upperInterval = (chord.tonesPc[2] - chord.tonesPc[0] + 12) % 12;
        const bool diminishedChord = thirdInterval == 3 && upperInterval == 6;

        // Power Rock gets a deliberately non-overlapping eighth-note bass riff.
        // Earlier generic bass sustains overlapped the later root/fifth notes,
        // creating simultaneous pitches that fought the guitar riff.
        if (style == 1)
        {
            // Power Rock bass follows a long-short-short gallop on a sixteenth grid.
            // Keep a stable tonic pedal through the minor-key chords, but use the
            // dominant root on V7 bars so the tonic does not rub against its raised
            // leading tone. Selected offbeats use the active chord's fifth as a pickup.
            static constexpr std::array<int, 12> gallopSteps =
                {{ 0, 2, 3, 4, 6, 7, 8, 10, 11, 12, 14, 15 }};
            const int tonicPc = DailyHarmony::powerRockTonicPc(seed);
            const int chordRootPc = (chord.rootMidi % 12 + 12) % 12;
            const int dominantPc = (tonicPc + 7) % 12;
            const int pedalPitch = (chordRootPc == dominantPc)
                ? chord.rootMidi : (24 + tonicPc);
            for (size_t i = 0; i < gallopSteps.size(); ++i)
            {
                const int step = gallopSteps[i];
                const int nextStep = (i + 1 < gallopSteps.size())
                    ? gallopSteps[i + 1] : 16;
                const int spacing = nextStep - step;
                const double duration = spacing >= 2 ? 0.42 : 0.17;
                const bool beatAccent = (step % 4 == 0);
                const bool fifthPickup = (step % 4 == 2 && (step / 4 == 1 || step / 4 == 3));
                const int pitch = fifthPickup ? chord.rootMidi + 7 : pedalPitch;
                const int velocity = beatAccent ? variation(84, 100) : variation(58, 76);
                addNote(bass, 2, pitch, velocity,
                        barBeat + step * 0.25, duration, tpq);
            }
        }
        else
        {
        // Other legacy profiles retain their existing bass vocabulary.
        const int rootVelocity = juce::jlimit(1, 127,
            (openingBar ? variation(67, 78) : variation(79, 96)) + energy * 3);
        addNote(bass, 2, chord.rootMidi, rootVelocity, barBeat, 1.5, tpq);

        // Genre-specific bass vocabulary: galloping/pulsed metal, walking fusion,
        // and syncopated funk. These notes use the current chord rather than a
        // fixed major-scale riff, so the bass follows the chosen harmony.
        const bool metalBass = style == 1 || style == 2 || style == 6 || style == 7
            || style == 8 || style == 10 || style == 11 || style == 12 || style == 13;
        const bool walkingBass = style == 14 || style == 16 || style == 17 || style == 18;
        if (metalBass)
        {
            addNote(bass, 2, chord.rootMidi + 12, variation(66, 82), barBeat + 1.5, 0.35, tpq);
            addNote(bass, 2, chord.rootMidi, variation(64, 80), barBeat + 2.5, 0.35, tpq);
            if (style == 1 || style == 2 || style == 8 || style == 11)
            {
                const int bassThirdInterval = (chord.tonesPc[1] - chord.tonesPc[0] + 12) % 12;
                const int bassUpperInterval = (chord.tonesPc[2] - chord.tonesPc[0] + 12) % 12;
                const bool diminished = bassThirdInterval == 3 && bassUpperInterval == 6;
                addNote(bass, 2, chord.rootMidi + (diminished ? 6 : 7),
                        variation(58, 74), barBeat + 3.0, 0.4, tpq);
            }
        }
        else if (walkingBass)
        {
            addNote(bass, 2, chord.rootMidi + 3, variation(56, 72), barBeat + 1.0, 0.8, tpq);
            addNote(bass, 2, chord.rootMidi + 7, variation(58, 74), barBeat + 2.0, 0.8, tpq);
            const auto nextChord = DailyHarmony::forSection(seed, style, (bar + 1) / 4)[static_cast<size_t>((bar + 1) % 4)];
            addNote(bass, 2, nextChord.rootMidi, variation(54, 70), barBeat + 3.0, 0.8, tpq);
        }
        else if (style == 4 || style == 9 || style == 20)
        {
            // Classic rock/country: root-fifth movement rather than constant roots.
            addNote(bass, 2, chord.rootMidi + 7, variation(61, 77), barBeat + 1.0, 0.75, tpq);
            addNote(bass, 2, chord.rootMidi + 12, variation(58, 73), barBeat + 3.0, 0.75, tpq);
        }

        int secondPitch = chord.rootMidi;
        if ((bar % 2 == 0 && variation(0, 2) == 0) || buildBar || chorusBar)
            secondPitch = chord.rootMidi + (diminishedChord ? 6 : 7);
        addNote(bass, 2, secondPitch, juce::jlimit(1, 127, variation(72, 90) + energy * 2),
                barBeat + 2.0, chorusBar ? 1.5 : 1.0, tpq);
        // In the louder sections, a light fifth-octave pulse adds forward motion.
        if ((chorusBar || finaleBar) && !phraseEnding)
            addNote(bass, 2, chord.rootMidi + 12, variation(57, 70), barBeat + 3.0, 0.5, tpq);

        // Eighth-note approach to the next bar: use the next chord's root,
        // so the fill points toward the next harmony rather than a fixed riff.
        if (!phraseEnding && bar + 1 < bars && (bar % 2 == 1 || buildBar))
        {
            const auto nextChord = DailyHarmony::forSection(seed, style, (bar + 1) / 4)[static_cast<size_t>((bar + 1) % 4)];
            addNote(bass, 2, nextChord.rootMidi, variation(61, 75), barBeat + 3.5, 0.5, tpq);
        }
        else if (phraseEnding)
        {
            addNote(bass, 2, chord.rootMidi + (diminishedChord ? 6 : 7), variation(62, 76), barBeat + 3.5, 0.5, tpq);
        }

        } // End non-Power-Rock bass patterns.

        // Rhythm guitar uses chord tones from the same genre-specific progression.
        // The app renders these MIDI notes with its existing recorded guitar sample bank.
        // Heavy styles use short palm-muted-style root/fifth pulses; rock uses strummed
        // triads/power chords; jazz/fusion uses compact chord stabs; country uses open voicings.
        const bool heavyRhythm = style == 1 || style == 2 || style == 6 || style == 7
            || style == 8 || style == 10 || style == 11 || style == 12 || style == 13;
        const bool syncRhythm = style == 5 || style == 7 || style == 12 || style == 13
            || style == 16 || style == 17 || style == 18;
        const int guitarRoot = 36 + ((chord.rootMidi % 12 + 12) % 12); // Guitar low register, C2..B2
        auto addChord = [&](double beat, double duration, int velocity, bool powerOnly)
        {
            std::array<int, 4> pitches{};
            int count = 0;
            pitches[static_cast<size_t>(count++)] = guitarRoot;
            if (powerOnly)
            {
                const int bassThirdInterval = (chord.tonesPc[1] - chord.tonesPc[0] + 12) % 12;
                const int bassUpperInterval = (chord.tonesPc[2] - chord.tonesPc[0] + 12) % 12;
                const bool diminished = bassThirdInterval == 3 && bassUpperInterval == 6;
                // Power Rock rhythm guitar stays on a stable fifth. A diminished
                // triad's tritone is too exposed in a distorted riff; preserve
                // the old diminished voicing for other styles only.
                const int fifthInterval = (style == 1) ? 7 : (diminished ? 6 : 7);
                pitches[static_cast<size_t>(count++)] = guitarRoot + fifthInterval;
                pitches[static_cast<size_t>(count++)] = guitarRoot + 12;
            }
            else
            {
                for (int tone = 1; tone < 3; ++tone)
                {
                    const int interval = (chord.tonesPc[static_cast<size_t>(tone)]
                        - chord.tonesPc[0] + 12) % 12;
                    const int pitch = guitarRoot + interval;
                    bool duplicate = false;
                    for (int n = 0; n < count; ++n) if (pitches[static_cast<size_t>(n)] == pitch) duplicate = true;
                    if (!duplicate && count < static_cast<int>(pitches.size())) pitches[static_cast<size_t>(count++)] = pitch;
                }
                pitches[static_cast<size_t>(count++)] = guitarRoot + 12;
            }
            for (int n = 0; n < count; ++n)
                addNote(rhythmGuitar, 1, pitches[static_cast<size_t>(n)],
                        juce::jlimit(28, 112, velocity - n * 2), beat, duration, tpq);
        };
        if (style == 1)
        {
            // Power Rock riff engine: low pedal-note gallop plus occasional open
            // high-string power-chord flashes. MIDI cannot encode a guitar's actual
            // palm-mute technique, so clipped note lengths and lower velocities
            // approximate muted pedal strokes; accent chords are higher and louder.
            const int riff = finaleBar ? 3 : chorusBar ? (bar % 2 == 0 ? 1 : 2)
                : buildBar ? 2 : (bar % 4 == 0 ? 0 : bar % 4 == 1 ? 2 : 0);
            static constexpr std::array<std::array<int, 16>, 4> riffCells = {{
                {{1,0,1,1, 1,0,1,1, 1,0,1,1, 1,0,1,1}}, // eighth + two sixteenths
                {{1,0,1,0, 1,0,1,0, 1,0,1,0, 1,0,1,0}}, // open eighth-note chorus
                {{1,0,0,1, 1,0,1,0, 1,0,0,1, 1,0,1,0}}, // syncopated answer
                {{1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,1,1}}  // final accents + pickup
            }};
            const int tonicPc = DailyHarmony::powerRockTonicPc(seed);
            const int chordRootPc = (chord.rootMidi % 12 + 12) % 12;
            const int dominantPc = (tonicPc + 7) % 12;
            // Pedal on the song tonic where it belongs to the harmony; on V7,
            // change the pedal to the dominant root to avoid a semitone rub.
            const int pedalPitch = (chordRootPc == dominantPc)
                ? guitarRoot : (36 + tonicPc);
            auto addOpenPowerChord = [&](double beat, int velocity, double duration)
            {
                addNote(rhythmGuitar, 1, guitarRoot + 24, velocity, beat, duration, tpq);
                addNote(rhythmGuitar, 1, guitarRoot + 31, velocity - 3, beat, duration, tpq);
                addNote(rhythmGuitar, 1, guitarRoot + 36, velocity - 6, beat, duration, tpq);
            };
            const bool melodicFlashBar = (riff == 2 && !openingBar);
            for (int sixteenth = 0; sixteenth < 16; ++sixteenth)
            {
                if (riffCells[static_cast<size_t>(riff)][static_cast<size_t>(sixteenth)] == 0)
                    continue;
                // Leave air in the intro and the last half of phrase-ending bars.
                if (openingBar && sixteenth != 0 && sixteenth != 8)
                    continue;
                if (phraseEnding && !finaleBar && sixteenth > 11)
                    continue;

                // Find the next active hit (after applying the phrase-space rules)
                // so the note-off always precedes the next pedal attack.
                int nextStep = sixteenth + 1;
                while (nextStep < 16)
                {
                    const bool active = riffCells[static_cast<size_t>(riff)][static_cast<size_t>(nextStep)] != 0;
                    const bool introSpace = openingBar && nextStep != 0 && nextStep != 8;
                    const bool endingSpace = phraseEnding && !finaleBar && nextStep > 11;
                    if (active && !introSpace && !endingSpace)
                        break;
                    ++nextStep;
                }
                const int spacing = nextStep < 16 ? nextStep - sixteenth : 16 - sixteenth;
                const double duration = spacing >= 2 ? 0.42 : 0.17;
                const bool beatAccent = (sixteenth % 4 == 0);
                const int pedalVelocity = beatAccent ? 76 : (chorusBar ? 59 : 48);
                addNote(rhythmGuitar, 1, pedalPitch,
                        juce::jlimit(1, 110, pedalVelocity + energy * 2),
                        barBeat + sixteenth * 0.25, duration, tpq);

                // High power-chord flashes mimic notes played open on the thinner
                // strings. Verses use two per bar; chorus beats are more expansive.
                const bool lickWindow = melodicFlashBar && sixteenth >= 8 && sixteenth <= 11;
                const bool downbeatFlash = beatAccent
                    && (chorusBar || finaleBar || sixteenth == 0 || sixteenth == 8);
                const bool syncFlash = riff == 2 && (sixteenth == 3 || sixteenth == 11);
                if (!lickWindow && (downbeatFlash || syncFlash))
                {
                    const int flashVelocity = syncFlash ? 67 : (chorusBar || finaleBar ? 91 : 81);
                    addOpenPowerChord(barBeat + sixteenth * 0.25,
                        juce::jlimit(45, 105, flashVelocity + energy), 0.21);
                }
            }
            // Every few bars, replace the upper chord hit with a short four-note
            // chord-tone burst (root, third, fifth, octave) over the low pedal.
            // It fills the sixteenth-grid gaps without changing the scoring lead.
            if (melodicFlashBar)
            {
                const int lickThirdInterval = (chord.tonesPc[1] - chord.tonesPc[0] + 12) % 12;
                const std::array<int, 4> lickIntervals = {{ 0, lickThirdInterval, 7, 12 }};
                for (int i = 0; i < static_cast<int>(lickIntervals.size()); ++i)
                {
                    const int pitch = guitarRoot + 24 + lickIntervals[static_cast<size_t>(i)];
                    const int velocity = juce::jlimit(52, 100, 76 + energy * 3 - i * 2);
                    addNote(rhythmGuitar, 1, pitch, velocity,
                            barBeat + 2.0 + i * 0.25, 0.17, tpq);
                }
            }
            // Land phrase endings on a clear chord rather than leaving a rhythmic gap.
            if (phraseEnding && !finaleBar)
                addOpenPowerChord(barBeat + 3.0, 88, 0.42);
        }
        else if (heavyRhythm)
        {
            // Other heavy profiles retain their established eighth-note pattern.
            const bool fastStraightMetal = style == 8 || style == 11;
            for (int eighth = 0; eighth < 8; ++eighth)
            {
                const double pulse = eighth * 0.5;
                const bool beatAccent = (eighth == 0 || eighth == 4);
                if (!fastStraightMetal && syncRhythm
                    && (eighth == 3 || eighth == 7) && (bar % 2 == 0))
                    continue;
                const double duration = beatAccent ? 0.36 : 0.28;
                const int low = beatAccent ? 80 : (fastStraightMetal ? 62 : 50);
                const int high = beatAccent ? 101 : (fastStraightMetal ? 84 : 76);
                addChord(barBeat + pulse, duration,
                         variation(low, high) + energy * 2, true);
            }
        }
        else if (style == 17 || style == 18)
        {
            // Jazz/fusion comping: clipped chord stabs on offbeats.
            for (const double pulse : { 0.5, 1.5, 2.5, 3.5 })
                addChord(barBeat + pulse, 0.22, variation(42, 68), false);
        }
        else if (style == 20)
        {
            // Country/Americana: alternating bass-note chord strums with space between hits.
            addChord(barBeat, 0.65, variation(62, 82), false);
            addChord(barBeat + 2.0, 0.55, variation(55, 75), false);
            if (phraseEnding) addChord(barBeat + 3.0, 0.35, variation(48, 67), false);
        }
        else if (style == 16)
        {
            // Funk: tight muted-sounding chord chops on the offbeats.
            for (const double pulse : { 0.5, 1.5, 2.5, 3.5 })
                addChord(barBeat + pulse, 0.16, variation(39, 61), false);
        }
        else
        {
            // General rock: clear downbeat chord and backbeat accents, with phrase variation.
            addChord(barBeat, 0.72, variation(65, 86) + energy * 2, heavyRhythm);
            addChord(barBeat + 2.0, 0.42, variation(54, 74) + energy * 2, heavyRhythm);
            if (chorusBar || finaleBar) addChord(barBeat + 3.0, 0.32, variation(48, 69), heavyRhythm);
        }

        // Power Rock reference arrangements are driven by guitar, bass and drums.
        // Keep piano silent in this mode so block chords/arpeggios do not compete
        // with the distorted rhythm guitar and lead. The MIDI track remains present.
        if (style != 1)
        {
        // Piano alternates sustained chord support and short stabs. The first
        // bar stays sparse; phrase-ending bars leave space for the drum fill.
        for (int beatInBar : {0, 2})
        {
            if (openingBar && beatInBar == 2)
                continue;
            const double start = barBeat + beatInBar;
            // Piano attacks only on quarter-note beats, with note lengths on
            // the eighth-note grid. Avoid fractional lengths that can feel late.
            const double duration = (beatInBar == 0 && !phraseEnding)
                ? (chorusBar ? 1.5 : 1.0) : 1.0;
            int velocity = variation(46, 64) + (beatInBar == 2 ? 8 : 0);
            velocity += energy * 5;
            if (phraseEnding) velocity -= 5;

            // Heavy styles avoid the bright, constant major-triad piano sound:
            // use root/fifth/octave support instead of spelling the chord's third.
            const bool heavyPiano = style == 1 || style == 2 || style == 6 || style == 7
                || style == 8 || style == 10 || style == 11 || style == 12 || style == 13;
            if (heavyPiano)
            {
                addNote(piano, 3, chord.pianoMidi[0] - 12, juce::jlimit(1, 100, velocity - 7), start, duration, tpq);
                addNote(piano, 3, chord.pianoMidi[0] + (diminishedChord ? 6 : 7),
                        juce::jlimit(1, 100, velocity - 3), start, duration, tpq);
                if (chorusBar || finaleBar)
                    addNote(piano, 3, chord.pianoMidi[0] + 12, juce::jlimit(1, 100, velocity - 12), start, duration, tpq);
            }
            else
            {
                for (const int note : chord.pianoMidi)
                    addNote(piano, 3, note, juce::jlimit(1, 120, velocity), start, duration, tpq);
            }
        }
        // Genre-specific piano vocabulary. Metal/prog and cinematic profiles get
        // broken-chord movement; jazz/fusion gets light offbeat comping; hard rock
        // stays sparse. The pitches come from the active chord voicing.
        const bool pianoArpeggio = style == 0 || style == 1 || style == 2 || style == 5
            || style == 6 || style == 8 || style == 11 || style == 14 || style == 18 || style == 19;
        if (pianoArpeggio && !openingBar)
        {
            const auto& tones = chord.pianoMidi;
            const double arpDuration = style == 18 ? 0.45 : 0.35;
            for (int eighth = 0; eighth < 8; ++eighth)
            {
                const int toneIndex = (eighth / 2 + (style == 5 ? bar % 3 : 0)) % 3;
                const double arpBeat = barBeat + eighth * 0.5;
                const int arpVelocity = juce::jlimit(1, 94,
                    (style == 19 ? 34 : 43) + energy * 4 + (eighth % 2 == 0 ? 5 : 0));
                addNote(piano, 3, tones[static_cast<size_t>(toneIndex)], arpVelocity,
                        arpBeat, arpDuration, tpq);
            }
        }
        else if (style == 17 || style == 18)
        {
            // Funk/jazz comping uses offbeat chord stabs, not continuous block chords.
            for (const int beatOffset : {1, 3})
                for (const int note : chord.pianoMidi)
                    addNote(piano, 3, note, variation(35, 53), barBeat + beatOffset + 0.5, 0.35, tpq);
        }

        } // End non-Power-Rock piano arrangement.

        // Rock drum groove: kick on 1/3, snare on 2/4, with occasional extra
        // kick syncopation. Hats vary between straight eighths and lighter offbeats.
        const int drumLift = energy * 2;
        // Genre-dependent groove: metal drives with extra kick pulses; funk/jazz
        // place the backbeat differently; country/blues leave more breathing room.
        addNote(drums, 10, 36, juce::jlimit(1, 127, variation(83, 101) + drumLift), barBeat, 0.12, tpq);
        const bool syncopated = style == 5 || style == 6 || style == 7 || style == 8 || style == 12 || style == 13;
        const bool heavy = style == 1 || style == 2 || style == 3 || style == 6 || style == 7 || style == 8 || style == 10 || style == 11 || style == 12 || style == 13;
        if (style == 13) // metalcore half-time backbeat
        {
            addNote(drums, 10, 38, variation(92, 108) + drumLift, barBeat + 2.0, 0.14, tpq);
        }
        else if (style == 16 || style == 17 || style == 18) // blues, funk/soul, jazz fusion
        {
            addNote(drums, 10, 38, variation(76, 94) + drumLift, barBeat + 1.0, 0.12, tpq);
            addNote(drums, 10, 38, variation(80, 98) + drumLift, barBeat + 3.0, 0.12, tpq);
        }
        else
        {
            addNote(drums, 10, 38, juce::jlimit(1, 127, variation(78, 96) + drumLift), barBeat + 1.0, 0.12, tpq);
            addNote(drums, 10, 38, juce::jlimit(1, 127, variation(80, 99) + drumLift), barBeat + 3.0, 0.12, tpq);
        }
        if ((heavy && style != 1) || style == 0 || style == 9)
            addNote(drums, 10, 36, juce::jlimit(1, 127, variation(80, 99) + drumLift), barBeat + 2.0, 0.12, tpq);
        if (style == 1)
        {
            // Kick follows the changing guitar cells instead of filling every
            // offbeat: stable drive in verses, extra pushes in chorus/build.
            addNote(drums, 10, 36, variation(76, 91), barBeat + 2.0, 0.10, tpq);
            if (chorusBar || finaleBar)
            {
                addNote(drums, 10, 36, variation(74, 88), barBeat + 1.5, 0.09, tpq);
                addNote(drums, 10, 36, variation(78, 92), barBeat + 3.0, 0.09, tpq);
            }
            else if (buildBar || bar % 4 == 2)
                addNote(drums, 10, 36, variation(64, 80), barBeat + 2.5, 0.09, tpq);

            // Add two restrained sixteenth-grid kick pickups only where the riff
            // cell contains corresponding syncopated strokes (build / sync chorus).
            if (buildBar || (chorusBar && bar % 2 == 1))
            {
                addNote(drums, 10, 36, variation(59, 74), barBeat + 0.75, 0.09, tpq);
                addNote(drums, 10, 36, variation(59, 74), barBeat + 2.75, 0.09, tpq);
            }
        }
        else if (syncopated)
            addNote(drums, 10, 36, variation(66, 82), barBeat + 2.5, 0.10, tpq);
        else
            addNote(drums, 10, 36, juce::jlimit(1, 127, variation(70, 88) + drumLift), barBeat + 2.0, 0.12, tpq);

        if (!openingBar && (bar % 2 == 1 || buildBar))
            addNote(drums, 10, 36, variation(58, 76), barBeat + 2.5, 0.10, tpq);
        if (buildBar)
            addNote(drums, 10, 46, variation(54, 70), barBeat + 3.5, 0.10, tpq); // open hat

        // Double-kick / gallop vocabulary separates metal from the default rock beat.
        // Use eighth-note kick pulses for fast metal and syncopated low accents for djent.
        if (heavy && style != 1)
        {
            for (const int eighth : { 1, 3, 5, 7 })
            {
                const bool djentAccent = (style == 7 || style == 12 || style == 13);
                const bool fastMetal = (style == 1 || style == 8 || style == 11);
                if (!djentAccent || eighth == 3 || eighth == 7 || variation(0, 2) == 0)
                    addNote(drums, 10, 36,
                            juce::jlimit(1, 127, variation(fastMetal ? 67 : 52,
                                                          fastMetal ? 88 : 76) + drumLift),
                            barBeat + eighth * 0.5, 0.09, tpq);
            }
        }
        // Funk emphasizes offbeat kick/snare interplay; half-time metalcore leaves
        // a wide space between backbeats, creating a heavier groove at the same BPM.
        if (style == 16)
        {
            addNote(drums, 10, 36, variation(66, 82), barBeat + 1.5, 0.10, tpq);
            addNote(drums, 10, 38, variation(72, 88), barBeat + 2.5, 0.10, tpq);
        }
        for (int eighth = 0; eighth < 8; ++eighth)
        {
            // Slightly stronger hats on beats, quieter in-between strokes.
            int hatVelocity = (eighth % 2 == 0) ? variation(49, 67) : variation(32, 49);
            if (openingBar) hatVelocity -= 10;
            if (buildBar && eighth >= 4) hatVelocity += 7;
            if (chorusBar || finaleBar) hatVelocity += 8;
            if (bridgeBar) hatVelocity -= 4;
            if ((style == 16 || style == 17 || style == 18) && eighth % 2 == 1) hatVelocity += 5;
            if ((style == 1 || style == 8 || style == 11) && eighth % 2 == 1) hatVelocity += 5;
            if ((style == 4 || style == 5 || style == 19 || style == 20) && eighth % 2 == 1) hatVelocity -= 8;
            int hatNote = 42; // closed hi-hat
            if (style == 17 || style == 18) hatNote = 51; // ride cymbal for jazz/fusion
            else if (style == 16) hatNote = (eighth % 2 == 0) ? 42 : 46; // funk open-hat accents
            else if (style == 20) hatNote = 51; // country ride pattern
            else if ((style == 7 || style == 12 || style == 13) && eighth % 2 == 1) hatNote = 44; // pedal hat
            addNote(drums, 10, hatNote, juce::jlimit(1, 110, hatVelocity),
                    barBeat + eighth * 0.5, 0.10, tpq);
        }

        // Short tom fill at phrase endings; crash only on the next phrase downbeat
        // (except the first bar), keeping fills predictable and musically placed.
        if (phraseEnding)
        {
            // Fill lands with the guitar phrase boundary: two toms lead into
            // the next bar, with a slightly stronger final tom in the finale.
            addNote(drums, 10, 45, variation(62, 78), barBeat + 3.0, 0.16, tpq);
            addNote(drums, 10, 47, variation(66, 82) + (finaleBar ? 4 : 0), barBeat + 3.5, 0.16, tpq);
            if (style == 1)
            {
                // A compact sixteenth-note snare/tom pickup adds a metal-style
                // lead-in without changing the existing drum sample mapping.
                addNote(drums, 10, 38, variation(64, 78), barBeat + 3.25, 0.09, tpq);
                addNote(drums, 10, 45, variation(68, 82), barBeat + 3.75, 0.09, tpq);
            }
        }
        if ((bar > 0 && bar % 4 == 0) || (chorusBar && !openingBar) || finaleBar)
            addNote(drums, 10, 49, variation(78, 96) + (finaleBar ? 5 : 0), barBeat, 0.22, tpq);
        // Chorus and final section get a coordinated extra kick on the last
        // eighth when the phrase is not ending, reinforcing the guitar pickup.
        if ((chorusBar || finaleBar) && !phraseEnding && bar + 1 < bars)
            addNote(drums, 10, 36, variation(61, 76), barBeat + 3.5, 0.10, tpq);
        // Stronger closing fill, resolving on the final downbeat with a crash.
        if (finaleBar)
        {
            addNote(drums, 10, 45, variation(67, 80), barBeat + 2.5, 0.16, tpq);
            addNote(drums, 10, 47, variation(71, 86), barBeat + 3.0, 0.16, tpq);
            addNote(drums, 10, 50, variation(76, 91), barBeat + 3.5, 0.16, tpq);
        }
    }

    result.bassMidi = outputDirectory.getChildFile("DailyChallenge_Bass.mid");
    result.drumsMidi = outputDirectory.getChildFile("DailyChallenge_Drums.mid");
    result.pianoMidi = outputDirectory.getChildFile("DailyChallenge_Piano.mid");
    result.rhythmGuitarMidi = outputDirectory.getChildFile("DailyChallenge_RhythmGuitar.mid");
    result.backingMidi = outputDirectory.getChildFile("DailyChallenge_Backing.mid");
    result.progression.clear();
    const auto summaryChords = DailyHarmony::forSection(seed, style, 0);
    for (size_t i = 0; i < summaryChords.size(); ++i)
    {
        if (i != 0)
            result.progression += " - ";
        result.progression += juce::String(summaryChords[i].name.c_str());
    }

    const bool bassOk = writeMidi(result.bassMidi, bass, tpq, bpm, true);
    const bool drumsOk = writeMidi(result.drumsMidi, drums, tpq, bpm, true);
    const bool pianoOk = writeMidi(result.pianoMidi, piano, tpq, bpm, true);
    const bool rhythmOk = writeMidi(result.rhythmGuitarMidi, rhythmGuitar, tpq, bpm, true);
    const bool combinedOk = writeMultiTrack(result.backingMidi, bass, drums, piano, rhythmGuitar, tpq, bpm);
    if (!(bassOk && drumsOk && pianoOk && rhythmOk && combinedOk))
    {
        if (errorMessage != nullptr) *errorMessage = "Could not write one or more backing MIDI files.";
        return false;
    }
    if (errorMessage != nullptr) *errorMessage = {};
    return true;
}
