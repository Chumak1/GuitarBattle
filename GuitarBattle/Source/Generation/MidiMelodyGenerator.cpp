#include "MidiMelodyGenerator.h"
#include "DailyHarmony.h"
#include "../Midi/TabMapper.h"
#include "../Midi/MidiParser.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <random>

namespace
{
    struct RhythmStep
    {
        double durationBeats;
        int velocity;
    };

    int chooseSeed(int requestedSeed)
    {
        if (requestedSeed != 0)
            return requestedSeed;

        const auto now = static_cast<std::uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        std::random_device randomDevice;
        return static_cast<int>((now ^ (static_cast<std::uint64_t>(randomDevice()) << 16))
                                & 0x7fffffffU) + 1;
    }

    void setError(juce::String* destination, const juce::String& message)
    {
        if (destination != nullptr)
            *destination = message;
    }
}

bool MidiMelodyGenerator::generate(const Settings& requestedSettings,
                                   const juce::File& outputMidiFile,
                                   Result& result,
                                   juce::String* errorMessage)
{
    result = {};
    if (outputMidiFile.getFullPathName().isEmpty())
    {
        setError(errorMessage, "Output MIDI file path is empty.");
        return false;
    }

    Settings settings = requestedSettings;
    settings.durationSeconds = juce::jlimit(15, 30, settings.durationSeconds);
    settings.bpm = juce::jlimit(170.0, 200.0, settings.bpm);
    settings.difficulty = juce::jlimit(0, 2, settings.difficulty);
    settings.style = Style::PowerMetal; // Single supported style during Power Rock tuning.
    settings.seed = chooseSeed(settings.seed);

    const bool vaiStyle = settings.style == Style::SteveVai;
    const bool powerMetalStyle = settings.style == Style::PowerMetal;
    const bool progressiveStyle = settings.style == Style::ProgressiveMetal;
    const bool bluesStyle = settings.style == Style::BluesRock;
    const bool hardRockStyle = settings.style == Style::HardRock;
    const bool polyphiaStyle = settings.style == Style::PolyphiaInspired;
    const bool triviumStyle = settings.style == Style::TriviumInspired;
    const bool djentStyle = settings.style == Style::DjentModernProg;
    const bool neoclassicalStyle = settings.style == Style::NeoclassicalMetal
        || settings.style == Style::SymphonicMetal;
    const bool classicRockStyle = settings.style == Style::ClassicRock
        || settings.style == Style::AlternativeIndie || settings.style == Style::CountryAmericana;
    const bool heavyMetalStyle = settings.style == Style::HeavyMetal
        || settings.style == Style::TechnicalMetal || settings.style == Style::ModernMetalcore;
    const bool fusionStyle = settings.style == Style::GuitarFusion
        || settings.style == Style::JazzFusion || settings.style == Style::FunkSoul;
    const bool cinematicStyle = settings.style == Style::CinematicExperimental;

    std::mt19937 rng(static_cast<std::uint32_t>(settings.seed));
    auto randomInt = [&rng](int low, int high)
    {
        return std::uniform_int_distribution<int>(low, high)(rng);
    };

    // Adaptive rock phrase generator. The contour provides a recognisable hook,
    // while strong beats are selected from the chord active at that moment.
    // Rhythmic accents and chord targets follow the selected daily progression.
    // Build a genre-specific scale in the same playable register. The old
    // generator always used the A-minor/C-major pitch collection, which made
    // almost every genre sound major-ish regardless of its style selection.
    int tonicPc = powerMetalStyle ? DailyHarmony::powerRockTonicPc(settings.seed) : 4;
    std::array<int, 7> intervals = {{0, 2, 3, 5, 7, 8, 10}}; // natural minor
    switch (settings.style)
    {
        case Style::BluesRock: case Style::Blues:
            tonicPc = 4; intervals = {{0, 3, 5, 6, 7, 10, 12}}; break; // E blues
        case Style::FunkSoul: case Style::JazzFusion: case Style::GuitarFusion:
            tonicPc = 2; intervals = {{0, 2, 3, 5, 7, 9, 10}}; break; // D Dorian
        case Style::ClassicRock: case Style::HardRock: case Style::CountryAmericana:
            tonicPc = 9; intervals = {{0, 2, 4, 5, 7, 9, 11}}; break; // A major
        case Style::PolyphiaInspired:
            tonicPc = 4; intervals = {{0, 2, 4, 6, 7, 9, 11}}; break; // Lydian colour
        case Style::CinematicExperimental:
            tonicPc = 2; intervals = {{0, 1, 3, 5, 7, 8, 11}}; break; // harmonic minor colour
        case Style::SteveVai:
            tonicPc = 4; intervals = {{0, 2, 4, 6, 7, 9, 10}}; break; // Mixolydian
        case Style::PowerMetal:
            tonicPc = DailyHarmony::powerRockTonicPc(settings.seed);
            // Use natural minor as the stable melodic collection. The raised
            // leading tone (e.g. D# over Em) is added only when it belongs to
            // the active V7 chord, avoiding D/D# clashes over VII major chords.
            intervals = {{0, 2, 3, 5, 7, 8, 10}}; break; // transposable natural minor
        case Style::NeoclassicalMetal: case Style::SymphonicMetal:
            tonicPc = 4; intervals = {{0, 2, 3, 5, 7, 8, 11}}; break; // harmonic minor
        case Style::ProgressiveMetal: case Style::TriviumInspired:
        case Style::DjentModernProg: case Style::HeavyMetal: case Style::TechnicalMetal:
        case Style::ModernMetalcore:
            tonicPc = 4; intervals = {{0, 1, 3, 5, 7, 8, 10}}; break; // Phrygian
        case Style::AlternativeIndie:
            tonicPc = 9; intervals = {{0, 2, 3, 5, 7, 9, 10}}; break; // Dorian/minor rock
        default: break;
    }
    std::vector<int> scale;
    const int scaleLow = powerMetalStyle ? 60 : 55;
    const int scaleHigh = powerMetalStyle ? 88 : 84;
    if (powerMetalStyle)
    {
        // For Power Rock, scale indices are true scale degrees rooted at the tonic.
        // A pitch-class-sorted list beginning at MIDI 60 made the same contour mean
        // different degrees in different keys (e.g. G could begin on D, its fifth).
        // Anchor degree 0 on the tonic in the C4..B4 octave and then extend upward.
        const int tonicMidi = 60 + tonicPc;
        for (int degree = 0; ; ++degree)
        {
            const int midi = tonicMidi
                + (degree / static_cast<int>(intervals.size())) * 12
                + intervals[static_cast<size_t>(degree) % intervals.size()];
            if (midi > scaleHigh)
                break;
            if (midi >= scaleLow)
                scale.push_back(midi);
        }
    }
    else
    {
        for (int midi = scaleLow; midi <= scaleHigh; ++midi)
        {
            const int relativePc = (midi - tonicPc + 120) % 12;
            bool inScale = false;
            for (const int interval : intervals)
                if (relativePc == interval % 12) { inScale = true; break; }
            if (inScale) scale.push_back(midi);
        }
    }
    if (scale.empty()) scale = {55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 77, 79, 81, 83, 84};
    // Four phrase archetypes give each seed a different musical identity while
    // preserving a singable 2-bar shape. Values are scale degrees, not semitones.
    static constexpr std::array<std::array<int, 16>, 4> powerContours = {{
        {{0, 2, 3, 2, 5, 3, 2, 0, 0, 2, 5, 7, 5, 3, 2, 0}}, // rise and resolve
        {{0, 1, 3, 4, 3, 5, 4, 2, 0, 2, 4, 6, 5, 4, 2, 0}}, // stepwise climb
        {{0, 3, 2, 4, 6, 4, 3, 1, 0, 2, 5, 6, 7, 5, 3, 0}}, // leap then answer
        {{0, 2, 4, 3, 2, 5, 7, 5, 3, 2, 4, 6, 4, 3, 1, 0}}  // high peak, falling tail
    }};
    const auto& contour = powerContours[static_cast<size_t>(static_cast<std::uint32_t>(settings.seed) & 3U)];

    const double beatsPerSecond = settings.bpm / 60.0;
    const double targetBeats = settings.durationSeconds * beatsPerSecond;
    const double totalBeats = std::ceil(targetBeats / 4.0) * 4.0;
    // Keep every generated event on the eighth-note grid. The requested duration
    // may end between grid points, so round the final boundary down to 1/2 beat.
    const double endBeat = std::floor(std::min(totalBeats, targetBeats) * 2.0) / 2.0;

    auto pitchClass = [](int midi) { return midi % 12; };
    auto chordTone = [&](int midi, const DailyHarmony::Chord& chord)
    {
        const int pc = pitchClass(midi);
        return pc == chord.tonesPc[0] || pc == chord.tonesPc[1] || pc == chord.tonesPc[2];
    };

    // Candidate scoring balances the intended contour with singable voice-leading.
    // Large leaps are welcome at marked peaks, but are discouraged elsewhere;
    // repeated pitches are useful as accents, not as the default motion.
    auto melodicMotionCost = [&](int pitch, int target, int previous, bool peak, bool cadence)
    {
        const int leap = std::abs(pitch - previous);
        int cost = std::abs(pitch - target) * 2;
        if (leap > 7) cost += (leap - 7) * (peak ? 1 : 3);
        if (leap == 0) cost += 2;
        if (peak && pitch > previous && pitch >= target - 2) cost -= 2;
        if (cadence && pitch % 12 == tonicPc) cost -= 5; // tonic-resolution anchor.
        return cost;
    };

    std::vector<double> starts;
    std::vector<double> durations;
    double beat = 0.0;
    int stepIndex = 0;
    int previousMidi = 69;
    while (beat < endBeat - 0.001)
    {
        const double beatInBar = std::fmod(beat, 4.0);
        const int bar = static_cast<int>(beat / 4.0);
        const auto chord = DailyHarmony::forSection(settings.seed, static_cast<int>(settings.style), bar / 4)[static_cast<size_t>(bar % 4)];

        double duration = 0.5;
        const bool onQuarter = std::abs(beatInBar - std::round(beatInBar)) < 0.001;
        const bool onBackbeat = std::abs(beatInBar - 1.0) < 0.001
                             || std::abs(beatInBar - 3.0) < 0.001;

        const int estimatedBars = juce::jmax(1, static_cast<int>(std::ceil(endBeat / 4.0)));
        const double sectionProgress = estimatedBars <= 1 ? 0.0
            : static_cast<double>(bar) / static_cast<double>(estimatedBars - 1);
        const bool verseSection = sectionProgress < 0.25 && bar > 0;
        const bool buildSection = sectionProgress >= 0.25 && sectionProgress < 0.40;
        const bool chorusSection = sectionProgress >= 0.40 && sectionProgress < 0.70;
        const bool bridgeSection = sectionProgress >= 0.70 && bar < estimatedBars - 1;
        const bool finalSection = bar == estimatedBars - 1;

        // Song-section phrasing: verse leaves more air, the build adds pickups,
        // and the chorus uses a tighter eighth-note pulse. The same note events
        // still drive both MIDI playback and generated tab/scoring data.
        if (onQuarter)
        {
            if (settings.difficulty == 0)
                duration = (stepIndex % 3 == 0 || onBackbeat) ? 1.0 : 0.5;
            else if (settings.difficulty == 1)
                duration = (onBackbeat && randomInt(0, 2) == 0) ? 1.0
                         : (randomInt(0, 4) == 0 ? 1.0 : 0.5);
            else
                duration = (randomInt(0, 4) == 0) ? 0.5
                         : (randomInt(0, 3) == 0 ? 1.0 : 0.5);

            if (verseSection && (beatInBar == 0.0 || beatInBar == 2.0))
                duration = std::max(duration, 1.0);
            // The middle section alternates longer statements with short
            // pickups; the chorus stays more rhythmically active.
            if (buildSection && (stepIndex % 4 == 2) && onQuarter)
                duration = std::min(duration, 0.5);
            if (chorusSection && (stepIndex % 6 == 4) && onQuarter
                && randomInt(0, 2) == 0)
                duration = 0.5;
            if (buildSection && (beatInBar == 1.0 || beatInBar == 3.0))
                duration = std::min(duration, 0.5);
            if (chorusSection && !onBackbeat && beatInBar != 0.0)
                duration = std::min(duration, 0.5);
            if (bridgeSection && beatInBar == 0.0)
                duration = std::max(duration, 1.0);
            if (finalSection && beatInBar == 0.0)
                duration = 1.0;
        }
        else if ((settings.difficulty == 2 || buildSection || chorusSection)
                 && (std::abs(beatInBar - 1.5) < 0.001
                     || std::abs(beatInBar - 3.5) < 0.001)
                 && randomInt(0, chorusSection ? 1 : 2) == 0)
        {
            duration = 0.5;
        }
        // Long-note phrasing: leave room for expressive, singing lead-guitar
        // statements instead of keeping every phrase on a short eighth-note pulse.
        // The MIDI holds the note; bends/vibrato remain performance articulations.
        if (onQuarter && std::abs(beatInBar) < 0.001)
        {
            if (finalSection)
                duration = 1.5;
            else if ((verseSection || bridgeSection) && randomInt(0, 2) == 0)
                duration = 1.5;
            else if (chorusSection && randomInt(0, 4) == 0)
                duration = 1.5;
        }
        // Give the middle of some chorus bars a sustained peak, then return to
        // shorter notes so the melody breathes and the fast passages have contrast.
        if (onQuarter && chorusSection && std::abs(beatInBar - 2.0) < 0.001
            && randomInt(0, 3) == 0)
            duration = 1.5;

        // Style profiles change phrasing, rhythmic density and note contour.
        // Polyphia-inspired = syncopated, angular instrumental prog; Trivium-inspired
        // = tight melodic metalcore; djent = stop/start modern prog; neoclassical
        // = quick scalar runs and larger melodic leaps.
        if (onQuarter)
        {
            if (vaiStyle && (beatInBar == 0.0 || beatInBar == 2.0)
                && randomInt(0, 3) == 0)
                duration = 2.0;
            else if (powerMetalStyle && !finalSection && beatInBar != 0.0
                     && randomInt(0, 3) != 0)
                duration = 0.5;
            else if (progressiveStyle && (stepIndex % 5 == 2 || stepIndex % 7 == 4))
                duration = (duration < 1.0 ? 1.5 : 0.5);
            else if (bluesStyle && (beatInBar == 1.0 || beatInBar == 3.0)
                     && randomInt(0, 2) == 0)
                duration = 1.5;
            else if (hardRockStyle && beatInBar != 0.0 && randomInt(0, 3) != 0)
                duration = 0.5;
            else if (polyphiaStyle)
                duration = (randomInt(0, 4) == 0) ? 1.0 : 0.5;
            else if (triviumStyle)
                duration = (finalSection && beatInBar == 0.0) ? 1.0 : 0.5;
            else if (djentStyle)
                duration = (beatInBar == 0.0 || beatInBar == 2.0) ? 1.0 : 0.5;
            else if (neoclassicalStyle)
                duration = (stepIndex % 8 == 7) ? 1.0 : 0.5;
        }

        // Fast-run articulation: create real sixteenth-note start positions for
        // styles that depend on tight, technical bursts. These short runs are
        // deliberately intermittent so the whole melody does not become a scale drill.
        const int runPosition = stepIndex % 8;
        if (polyphiaStyle && runPosition >= 2 && runPosition <= 5)
            duration = 0.25;
        else if (triviumStyle && (buildSection || chorusSection || finalSection)
                 && runPosition >= 3 && runPosition <= 6)
            duration = 0.25;
        else if (neoclassicalStyle && runPosition >= 4 && runPosition <= 7)
            duration = 0.25;
        else if (djentStyle && settings.difficulty == 2
                 && (runPosition == 2 || runPosition == 3))
            duration = 0.25;

        // Power Rock phrasing uses contrast rather than an unbroken stream of
        // eighth notes: let the hook breathe, then make the chorus more active.
        if (powerMetalStyle)
        {
            const int phraseSlot = static_cast<int>(std::fmod(beat, 8.0) * 2.0);
            if (!chorusSection && !finalSection && (phraseSlot == 6 || phraseSlot == 14)
                && duration < 1.0)
                duration = 1.0;
            if ((chorusSection || finalSection) && (phraseSlot == 4 || phraseSlot == 12)
                && settings.difficulty > 0)
                duration = 0.5;
            // Hold the final cadence long enough to register as an ending.
            if (finalSection && beatInBar >= 2.0 && beat + duration >= endBeat - 0.001)
                duration = std::max(duration, std::min(1.5, endBeat - beat));
        }

        // Power-metal lead needs occasional real sixteenth-note runs, not just
        // eighth-note contour changes. Put short ascending/descending bursts in
        // the later sections; the pitch pass below shapes each run as a scale run.
        const bool powerMetalRunWindow = powerMetalStyle
            && (chorusSection || bridgeSection || finalSection)
            && (bar % 2 == 1 || finalSection)
            && beatInBar >= 2.0 && beatInBar < 3.0;
        if (powerMetalRunWindow)
            duration = 0.25;

        duration = std::min(duration, endBeat - beat);
        if (duration < 0.20)
            break;

        starts.push_back(beat);
        durations.push_back(duration);
        beat += duration;

        // Random rests for other profiles are tied to their legacy phrasing.
        // Power Rock deliberately avoids rests based on stepIndex: sixteenth-note
        // solo runs change the number of note events per bar, so an event counter
        // made gaps drift to different musical positions. Its breathing room is
        // created by sustained notes and phrase durations above instead.
        if (!powerMetalStyle)
        {
            if (settings.difficulty > 0 && beat < endBeat - 0.5
                && (stepIndex % 8 == 6) && randomInt(0, 2) == 0)
                beat += 0.5;
            // Syncopated gaps and stop-start accents are part of the groove, not errors.
            if (beat < endBeat - 0.5 && polyphiaStyle
                && (std::abs(std::fmod(beat, 4.0) - 1.5) < 0.001
                    || std::abs(std::fmod(beat, 4.0) - 3.5) < 0.001)
                && randomInt(0, 3) == 0)
                beat += 0.5;
            if (beat < endBeat - 0.5 && djentStyle
                && (std::abs(std::fmod(beat, 4.0) - 1.5) < 0.001
                    || std::abs(std::fmod(beat, 4.0) - 3.5) < 0.001)
                && randomInt(0, 2) == 0)
                beat += 0.5;
            else if (verseSection && beat < endBeat - 0.5
                     && (stepIndex % 8 == 5) && randomInt(0, 2) == 0)
                beat += 0.5; // extra breathing room in verse phrases
            else if (chorusSection && beat < endBeat - 0.5
                     && (stepIndex % 8 == 3) && randomInt(0, 3) == 0)
                beat += 0.5; // occasional eighth-note gap
        }
        ++stepIndex;
    }

    result.notes.reserve(starts.size());
    for (size_t i = 0; i < starts.size(); ++i)
    {
        const double startBeat = starts[i];
        const double beatInBar = std::fmod(startBeat, 4.0);
        const int bar = static_cast<int>(startBeat / 4.0);
        const auto chord = DailyHarmony::forSection(settings.seed, static_cast<int>(settings.style), bar / 4)[static_cast<size_t>(bar % 4)];

        // Candidate notes are the key scale plus active-chord tones. That lets
        // V7 chords supply their raised third (e.g. D# in B7) without forcing
        // that chromatic leading tone over unrelated chords such as D major.
        std::vector<int> noteCandidates = scale;
        for (int pitch = scale.front(); pitch <= scaleHigh; ++pitch)
        {
            if (chordTone(pitch, chord)
                && std::find(noteCandidates.begin(), noteCandidates.end(), pitch) == noteCandidates.end())
                noteCandidates.push_back(pitch);
        }
        std::sort(noteCandidates.begin(), noteCandidates.end());

        // Shape the challenge like a short song: a restrained opening, a
        // developing middle, a higher-energy climax, then a clear final resolve.
        // The contour is indexed by musical position rather than note count, so
        // rests and difficulty-dependent rhythms do not scramble the phrase.
        const int phraseStep = (bar % 2) * 8
            + juce::jlimit(0, 7, static_cast<int>(beatInBar * 2.0));
        int desiredIndex = contour[static_cast<size_t>(phraseStep)];

        // Keep a recognisable hook, but let each two-bar phrase answer the
        // previous one with small, deliberate melodic changes. The variation
        // works in scale degrees (not raw semitones), so it stays diatonic.
        const int phraseVariant = bar % 4;
        if (phraseVariant == 1 && (phraseStep == 2 || phraseStep == 10))
            desiredIndex += 1;
        else if (phraseVariant == 2 && (phraseStep == 3 || phraseStep == 11))
            desiredIndex -= 1;
        else if (phraseVariant == 3 && (phraseStep == 5 || phraseStep == 13))
            desiredIndex += 2;

        // A compact four-note identity anchors the piece. It recurs at the
        // same phrase positions, while the ending and selected scale degrees
        // change so the return feels familiar rather than mechanically copied.
        // Values are scale-degree offsets and never alter MIDI/tab event timing.
        static constexpr std::array<std::array<int, 8>, 4> motifShapes = {{
            {{0, 2, 3, 2, 5, 3, 2, 0}},
            {{0, 1, 3, 4, 3, 2, 1, 0}},
            {{0, 3, 2, 4, 6, 4, 2, 0}},
            {{0, 2, 4, 3, 5, 4, 1, 0}}
        }};
        const auto& motifShape = motifShapes[static_cast<size_t>(static_cast<std::uint32_t>(settings.seed / 7) & 3U)];
        const int motifPosition = phraseStep % 8;
        const int motifAnchor = motifShape[static_cast<size_t>(motifPosition)];
        // Blend the hook into the existing contour, with a stronger influence
        // on the first half of each two-bar phrase and a lighter response later.
        if (motifPosition == 1 || motifPosition == 2 || motifPosition == 4)
            desiredIndex = (desiredIndex + motifAnchor + 1) / 2;
        else if (motifPosition == 0 || motifPosition == 7)
            desiredIndex = (desiredIndex + motifAnchor) / 2;

        // Each two-bar response gets a seed-dependent shape. This keeps the
        // challenge reproducible for a given seed while avoiding the same
        // answer phrase on every day. Offsets are scale degrees, not semitones.
        const int responseShape = (bar / 2 + std::abs(settings.seed % 3)) % 3;
        if (responseShape == 1 && (phraseStep == 1 || phraseStep == 9))
            desiredIndex += 1;
        else if (responseShape == 2 && (phraseStep == 6 || phraseStep == 14))
            desiredIndex -= 1;
        // On later returns, vary just the motif tail to create a call/answer
        // relationship while preserving the main contour and daily harmony.
        const int motifReturn = (bar / 2 + std::abs(settings.seed % 4)) % 4;
        if (motifReturn == 1 && (motifPosition == 6 || motifPosition == 7))
            desiredIndex += (motifPosition == 6 ? 1 : 0);
        else if (motifReturn == 2 && (motifPosition == 4 || motifPosition == 5))
            desiredIndex -= (motifPosition == 4 ? 1 : 0);
        else if (motifReturn == 3 && motifPosition == 7)
            desiredIndex += 1; // altered cadence before the final resolve

        // Song position is needed by style rules below, so calculate it before
        // applying the style-specific contour adjustments.
        const int totalBars = juce::jmax(1, static_cast<int>(std::ceil(endBeat / 4.0)));
        const double barProgress = totalBars <= 1 ? 0.0
            : static_cast<double>(bar) / static_cast<double>(totalBars - 1);

        // The chorus restates the hook with a higher emotional ceiling. Keep
        // its rhythmic identity, but lift the motif's anchor notes; the bridge
        // deliberately drops back before the final return.
        if (powerMetalStyle && barProgress >= 0.40 && barProgress < 0.70
            && (motifPosition == 2 || motifPosition == 4 || motifPosition == 5))
            desiredIndex += (motifPosition == 4 ? 2 : 1);
        if (powerMetalStyle && barProgress >= 0.70 && barProgress < 0.88
            && (motifPosition == 0 || motifPosition == 1 || motifPosition == 7))
            desiredIndex = std::max(0, desiredIndex - 1);

        // Power Rock: bring the main hook back in the chorus, then develop it
        // into a rising climax instead of generating unrelated scale fragments.
        // All values are scale indices; MIDI timing and scoring events are unchanged.
        if (powerMetalStyle)
        {
            const bool mainHookPosition = motifPosition == 0 || motifPosition == 1
                || motifPosition == 2 || motifPosition == 4 || motifPosition == 7;

            // Chorus: recognisable return of the opening idea, lifted at its peak.
            if (barProgress >= 0.40 && barProgress < 0.70 && mainHookPosition)
            {
                if (motifPosition == 0 || motifPosition == 7)
                    desiredIndex = (desiredIndex + motifAnchor) / 2;
                else if (motifPosition == 2 || motifPosition == 4)
                    desiredIndex += (motifPosition == 4 ? 2 : 1);
            }

            // Late solo: sequence the same motif upward in small steps, with a
            // descending answer on the second half of alternating two-bar phrases.
            if (barProgress >= 0.70 && barProgress < 0.94)
            {
                const bool ascendingAnswer = ((bar / 2) % 2) == 0;
                if (motifPosition == 1 || motifPosition == 2)
                    desiredIndex += ascendingAnswer ? 2 : 1;
                else if (motifPosition == 4 || motifPosition == 5)
                    desiredIndex += ascendingAnswer ? 3 : 1;
                else if (motifPosition == 6)
                    desiredIndex += ascendingAnswer ? 1 : -1;
            }

            // Final bars: leave the peak behind and prepare a clear cadence.
            if (barProgress >= 0.94 && (motifPosition == 4 || motifPosition == 5))
                desiredIndex = std::max(0, desiredIndex - 1);
        }

        // Style-specific melodic vocabulary. These shifts are scale-degree
        // changes, so every profile remains compatible with the chosen scale.
        switch (settings.style)
        {
            case Style::SteveVai:
                if (phraseStep == 4 || phraseStep == 5 || phraseStep == 11)
                    desiredIndex += 2;
                if (phraseStep == 7 || phraseStep == 15)
                    desiredIndex -= 1; // leave a breath after the high statement
                break;
            case Style::PowerMetal:
                // Melodic power-metal arc: restrained opening, stronger chorus lift,
                // a short high-register statement, then a cadence with room to breathe.
                if (barProgress >= 0.22 && barProgress < 0.68
                    && (phraseStep == 3 || phraseStep == 5 || phraseStep == 11))
                    desiredIndex += 1;
                if (barProgress >= 0.40 && barProgress < 0.72
                    && (phraseStep == 4 || phraseStep == 5 || phraseStep == 12))
                    desiredIndex += 1;
                if (barProgress >= 0.68 && bar < totalBars - 1
                    && (phraseStep == 0 || phraseStep == 8))
                    desiredIndex = std::max(0, desiredIndex - 1); // bridge pulls back
                if (phraseStep == 0 || phraseStep == 8)
                    desiredIndex = std::max(desiredIndex, 2); // firm phrase starts
                if (bar == totalBars - 1 && (phraseStep == 6 || phraseStep == 14))
                    desiredIndex = std::max(0, desiredIndex - 1); // prepare final resolve
                break;
            case Style::ProgressiveMetal:
                if ((bar % 2 == 1) && (phraseStep == 2 || phraseStep == 6 || phraseStep == 10))
                    desiredIndex += (phraseStep == 6 ? 2 : -1);
                if (phraseStep == 7 || phraseStep == 15)
                    desiredIndex += (bar % 2 == 0 ? 1 : -1);
                break;
            case Style::BluesRock:
                desiredIndex = juce::jlimit(0, 11, desiredIndex);
                if (phraseStep == 2 || phraseStep == 6 || phraseStep == 10 || phraseStep == 14)
                    desiredIndex = std::max(0, desiredIndex - 1);
                if (phraseStep == 4 || phraseStep == 12)
                    desiredIndex = std::min(11, desiredIndex + 1);
                break;
            case Style::HardRock:
                if (phraseStep == 0 || phraseStep == 8)
                    desiredIndex = std::max(0, desiredIndex - 1);
                if (phraseStep == 4 || phraseStep == 12)
                    desiredIndex += 1;
                break;
            case Style::PolyphiaInspired:
                // Angular skips, syncopated-feeling contour and a wider lead register.
                if (phraseStep == 1 || phraseStep == 5 || phraseStep == 9 || phraseStep == 13)
                    desiredIndex += (phraseStep == 5 || phraseStep == 13) ? 3 : -2;
                if (phraseStep == 3 || phraseStep == 11)
                    desiredIndex += 2;
                if (phraseStep == 7 || phraseStep == 15)
                    desiredIndex -= 1;
                break;
            case Style::TriviumInspired:
                // Tight melodic-metalcore phrasing with minor-leaning scale-degree motion.
                if (phraseStep == 0 || phraseStep == 8)
                    desiredIndex = std::max(0, desiredIndex - 2);
                if (phraseStep == 3 || phraseStep == 7 || phraseStep == 11 || phraseStep == 15)
                    desiredIndex += 1;
                if (phraseStep == 5 || phraseStep == 13)
                    desiredIndex += 2;
                break;
            case Style::DjentModernProg:
                if (phraseStep == 0 || phraseStep == 8)
                    desiredIndex = std::max(0, desiredIndex - 2);
                if (phraseStep == 3 || phraseStep == 11)
                    desiredIndex += 2;
                if (phraseStep == 6 || phraseStep == 14)
                    desiredIndex -= 1;
                break;
            case Style::NeoclassicalMetal:
                if (phraseStep == 2 || phraseStep == 6 || phraseStep == 10 || phraseStep == 14)
                    desiredIndex += 2;
                if (phraseStep == 4 || phraseStep == 12)
                    desiredIndex -= 1;
                if (phraseStep == 7 || phraseStep == 15)
                    desiredIndex += 1;
                break;
            case Style::ClassicRock:
            case Style::HeavyMetal:
            case Style::SymphonicMetal:
            case Style::TechnicalMetal:
            case Style::ModernMetalcore:
            case Style::GuitarFusion:
            case Style::Blues:
            case Style::FunkSoul:
            case Style::JazzFusion:
            case Style::AlternativeIndie:
            case Style::CinematicExperimental:
            case Style::CountryAmericana:
                if (fusionStyle)
                {
                    if (phraseStep == 3 || phraseStep == 11) desiredIndex += 2;
                    if (phraseStep == 7 || phraseStep == 15) desiredIndex -= 1;
                }
                else if (heavyMetalStyle)
                {
                    if (phraseStep == 0 || phraseStep == 8) desiredIndex = std::max(0, desiredIndex - 2);
                    if (phraseStep == 5 || phraseStep == 13) desiredIndex += 2;
                }
                else if (cinematicStyle)
                {
                    if (phraseStep == 4 || phraseStep == 12) desiredIndex += 2;
                    if (phraseStep == 7 || phraseStep == 15) desiredIndex -= 2;
                }
                else if (classicRockStyle)
                {
                    if (phraseStep == 0 || phraseStep == 8) desiredIndex = std::max(0, desiredIndex - 1);
                    if (phraseStep == 4 || phraseStep == 12) desiredIndex += 1;
                }
                break;
        }
        desiredIndex = juce::jlimit(0, static_cast<int>(scale.size()) - 1,
                                    desiredIndex);

        const bool nextBarStarts = i + 1 < starts.size()
            && static_cast<int>(starts[i + 1] / 4.0) != bar;
        const bool phraseEnding = powerMetalStyle
            ? ((nextBarStarts && (bar % 2 == 1)) || (i + 1 == starts.size()))
            : (nextBarStarts || (i + 1 == starts.size()));
        if (bar == 0)
        {
            // Opening: stay in the lower/middle register and establish the hook.
            desiredIndex = juce::jmin(desiredIndex, 5);
        }
        else if (barProgress >= 0.35 && barProgress < 0.65)
        {
            // Development: lift selected contour points to avoid a flat loop.
            if (phraseStep == 3 || phraseStep == 5 || phraseStep == 11)
                desiredIndex = juce::jmin(static_cast<int>(scale.size()) - 1, desiredIndex + 1);
        }
        else if (barProgress >= 0.65 && bar < totalBars - 1)
        {
            // Bridge: briefly lower the contour before the final lift.
            if (phraseStep == 0 || phraseStep == 7 || phraseStep == 8)
                desiredIndex = juce::jmax(0, desiredIndex - 1);
            else if (phraseStep == 4 || phraseStep == 5 || phraseStep == 11)
                desiredIndex = juce::jmin(static_cast<int>(scale.size()) - 1, desiredIndex + 2);
        }
        if (bar == totalBars - 1 && !phraseEnding)
        {
            // Final phrase rises toward its last chord tone, then resolves on it.
            if (phraseStep == 4 || phraseStep == 5 || phraseStep == 6)
                desiredIndex = juce::jmin(static_cast<int>(scale.size()) - 1, desiredIndex + 1);
        }
        // Solo-like bridge: shape a compact scale run into a peak, then leave
        // space for the cadence. This remains diatonic and uses the same scale
        // and chord-aware resolution as every other generated note.
        if (barProgress >= 0.65 && bar < totalBars - 1)
        {
            if (phraseStep == 2 || phraseStep == 3)
                desiredIndex = juce::jmin(static_cast<int>(scale.size()) - 1, desiredIndex + 1);
            else if (phraseStep == 6)
                desiredIndex = juce::jmax(0, desiredIndex - 1);
        }

        // Raise selected late-song peaks into the expanded upper register.
        // These are sparse melodic highlights, not a blanket octave shift.
        if (barProgress >= 0.65 && phraseStep == 5)
            desiredIndex = juce::jmin(static_cast<int>(scale.size()) - 1,
                                      desiredIndex + 2);

        // Match the sixteenth-note burst to a deliberate scale-degree direction.
        // Alternate ascending and descending runs so the climax does not sound
        // like the same copied lick every time.
        const bool powerMetalRun = powerMetalStyle
            && barProgress >= 0.40
            && (bar % 2 == 1 || barProgress >= 0.88)
            && beatInBar >= 2.0 && beatInBar < 3.0;
        if (powerMetalRun)
        {
            const int runStep = juce::jlimit(0, 3,
                static_cast<int>(std::floor((beatInBar - 2.0) * 4.0 + 0.001)));
            const bool descendingRun = ((bar / 2) % 2) != 0;
            desiredIndex += descendingRun ? (3 - runStep) : runStep;
        }
        desiredIndex = juce::jlimit(0, static_cast<int>(scale.size()) - 1,
                                    desiredIndex);

        // Resolve at the end of a two-bar melodic phrase and at the end of the
        // song. Phrase endings are chosen from the harmony active at that exact
        // position, so each cadence remains compatible with today's progression.
        const bool barDownbeat = std::abs(beatInBar) < 0.001;
        const bool beatThree = std::abs(beatInBar - 2.0) < 0.001;
        int candidate = scale[static_cast<size_t>(desiredIndex)];
        if (phraseEnding)
        {
            // The final note resolves to the selected tonic; earlier phrase endings
            // land on a chord tone from the active harmony.
            const bool finalCadence = (i + 1 == starts.size());
            const int targetPc = finalCadence ? tonicPc : chord.tonesPc[0];
            int best = candidate;
            int bestCost = 10000;
            for (const int pitch : noteCandidates)
            {
                if (pitch % 12 != targetPc && (finalCadence || !chordTone(pitch, chord)))
                    continue;
                const int cost = melodicMotionCost(pitch, candidate, previousMidi,
                                                   false, finalCadence);
                if (cost < bestCost) { bestCost = cost; best = pitch; }
            }
            candidate = best;
        }

        const bool strongBeat = std::abs(beatInBar - std::round(beatInBar)) < 0.001
                             && (static_cast<int>(std::round(beatInBar)) % 2 == 0);
        if (strongBeat && !phraseEnding)
        {
            int best = candidate;
            int bestCost = 10000;
            for (const int pitch : noteCandidates)
            {
                if (!chordTone(pitch, chord))
                    continue;
                const bool melodicPeak = powerMetalStyle
                    && barProgress >= 0.40 && barProgress < 0.70
                    && (phraseStep == 4 || phraseStep == 5 || phraseStep == 12);
                int cost = melodicMotionCost(pitch, candidate, previousMidi,
                                             melodicPeak, false);
                // Prefer the active chord root on beat 1. Beat 3 remains chordal
                // but can use the third or fifth to keep the line moving.
                if (barDownbeat && pitchClass(pitch) != chord.tonesPc[0])
                    cost += 5;
                if (beatThree && pitchClass(pitch) == chord.tonesPc[0])
                    cost += 2;
                if (cost < bestCost)
                {
                    bestCost = cost;
                    best = pitch;
                }
            }
            candidate = best;
        }
        else if (!phraseEnding)
        {
            // Keep ordinary motion stepwise; allow a little lift at phrase peaks.
            int best = candidate;
            int bestCost = 10000;
            for (const int pitch : noteCandidates)
            {
                const bool melodicPeak = powerMetalStyle
                    && barProgress >= 0.40 && barProgress < 0.70
                    && (phraseStep == 4 || phraseStep == 5 || phraseStep == 12);
                const int cost = melodicMotionCost(pitch, candidate, previousMidi,
                                                   melodicPeak, false);
                if (cost < bestCost)
                {
                    bestCost = cost;
                    best = pitch;
                }
            }
            candidate = best;
        }

        // Power Rock lead vocabulary: shape selected climax bursts as chord-tone
        // arpeggios rather than four arbitrary neighbouring scale notes. The run
        // still follows the current harmony and the existing tab/scoring events.
        if (powerMetalRun && !phraseEnding)
        {
            const int runStep = juce::jlimit(0, 3,
                static_cast<int>(std::floor((beatInBar - 2.0) * 4.0 + 0.001)));
            const bool descendingRun = ((bar / 2) % 2) != 0;
            const int chordToneIndex = descendingRun
                ? (runStep == 0 ? 2 : (runStep == 1 ? 1 : (runStep == 2 ? 0 : 1)))
                : (runStep == 0 ? 0 : (runStep == 1 ? 1 : (runStep == 2 ? 2 : 0)));
            const int targetPc = chord.tonesPc[chordToneIndex];
            int bestPitch = candidate;
            int bestCost = 100000;
            for (const int pitch : noteCandidates)
            {
                if (pitchClass(pitch) != targetPc)
                    continue;

                int cost = std::abs(pitch - candidate) * 2
                         + std::abs(pitch - previousMidi);
                if (!descendingRun && runStep > 0 && pitch < previousMidi)
                    cost += 18;
                if (descendingRun && runStep > 0 && pitch > previousMidi)
                    cost += 18;
                // Keep the arpeggio in a practical lead register and avoid
                // unexpectedly jumping an octave when a nearer chord tone exists.
                if (pitch < 60 || pitch > 88)
                    cost += 12;
                if (cost < bestCost)
                {
                    bestCost = cost;
                    bestPitch = pitch;
                }
            }
            if (bestCost < 100000)
                candidate = bestPitch;
        }

        if (settings.difficulty == 2 && !phraseEnding && !strongBeat
            && randomInt(0, 7) == 0)
        {
            // Hard mode may add a neighbouring scale tone as a pickup. Select
            // by scale index rather than +/- semitones to avoid chromatic notes
            // that would clash with some Daily Harmony progressions.
            int nearestScaleIndex = 0;
            int nearestDistance = 1000;
            for (size_t scaleIndex = 0; scaleIndex < scale.size(); ++scaleIndex)
            {
                const int distance = std::abs(scale[scaleIndex] - candidate);
                if (distance < nearestDistance)
                {
                    nearestDistance = distance;
                    nearestScaleIndex = static_cast<int>(scaleIndex);
                }
            }
            const int direction = randomInt(0, 1) == 0 ? -1 : 1;
            const int pickupIndex = juce::jlimit(0, static_cast<int>(scale.size()) - 1,
                                                  nearestScaleIndex + direction);
            candidate = scale[static_cast<size_t>(pickupIndex)];
        }
        previousMidi = candidate;
        result.notes.push_back(TabMapper::createTabNote(candidate, startBeat, durations[i]));
    }

    if (result.notes.empty())
    {
        setError(errorMessage, "Generator produced no notes.");
        return false;
    }

    constexpr int ticksPerQuarter = 480;
    juce::MidiMessageSequence track;
    track.addEvent(juce::MidiMessage::tempoMetaEvent(
        static_cast<int>(std::round(60000000.0 / settings.bpm))), 0.0);
    track.addEvent(juce::MidiMessage::timeSignatureMetaEvent(4, 4), 0.0);

    for (const auto& note : result.notes)
    {
        const double startTick = note.startBeat * ticksPerQuarter;
        const double endTick = (note.startBeat + note.durationBeats) * ticksPerQuarter;
        const double beatInBar = std::fmod(note.startBeat, 4.0);
        const bool downbeat = std::abs(beatInBar) < 0.001;
        const bool backbeat = std::abs(beatInBar - 1.0) < 0.001
                           || std::abs(beatInBar - 3.0) < 0.001;
        const bool quarterBeat = std::abs(beatInBar - std::round(beatInBar)) < 0.001;
        const int bar = static_cast<int>(note.startBeat / 4.0);
        const auto activeChord = DailyHarmony::forSection(settings.seed, static_cast<int>(settings.style), bar / 4)[static_cast<size_t>(bar % 4)];
        int velocity = settings.difficulty == 2 ? randomInt(88, 108) : randomInt(72, 94);
        if (vaiStyle)
            velocity = randomInt(66, 102); // expressive dynamic range
        else if (powerMetalStyle)
            velocity = randomInt(86, 106); // consistently driving attack
        else if (progressiveStyle)
            velocity = randomInt(74, 108); // wider dynamic contrast
        else if (bluesStyle)
            velocity = randomInt(64, 94);  // more relaxed dynamics
        else if (hardRockStyle)
            velocity = randomInt(82, 104); // punchy riff accents
        else if (polyphiaStyle)
            velocity = randomInt(70, 104); // alternating light taps and lead accents
        else if (triviumStyle)
            velocity = randomInt(88, 110); // aggressive, consistent attack
        else if (djentStyle)
            velocity = randomInt(84, 108); // tight modern-prog pulse
        else if (neoclassicalStyle)
            velocity = randomInt(78, 106); // clear fast-run articulation
        if (downbeat)
            velocity += 16;
        else if (backbeat)
            velocity += 12;
        else if (quarterBeat)
            velocity += 5;
        else
            velocity -= 4; // lighter eighth-note offbeats

        // Accentuate the start of each chord bar and selected offbeat pickups.
        // This makes harmony changes audible in the guitar rhythm as well.
        const auto previousChord = bar > 0
            ? DailyHarmony::forSection(settings.seed, static_cast<int>(settings.style), (bar - 1) / 4)[static_cast<size_t>((bar - 1) % 4)]
            : activeChord;
        const bool chordChangeAccent = downbeat && bar > 0
            && previousChord.rootMidi != activeChord.rootMidi;
        const bool offbeatPickup = std::abs(beatInBar - 1.5) < 0.001
                                || std::abs(beatInBar - 3.5) < 0.001;
        if (chordChangeAccent)
            velocity += 5;
        if (settings.difficulty > 0 && offbeatPickup)
            velocity += 3;
        if (hardRockStyle && downbeat)
            velocity += 7;
        if (powerMetalStyle && quarterBeat)
            velocity += 3;
        if ((triviumStyle || djentStyle) && downbeat)
            velocity += 7;
        if (polyphiaStyle && !quarterBeat)
            velocity -= 2;
        if (neoclassicalStyle && quarterBeat)
            velocity += 2;
        if (bluesStyle && !quarterBeat)
            velocity -= 3;

        // Arrangement dynamics follow the same opening/development/climax arc
        // as the note choices above. The last bar relaxes slightly for closure.
        const int totalBars = juce::jmax(1, static_cast<int>(std::ceil(endBeat / 4.0)));
        const double barProgress = totalBars <= 1 ? 0.0
            : static_cast<double>(bar) / static_cast<double>(totalBars - 1);
        if (bar == 0)
            velocity -= 7;
        else if (barProgress >= 0.65 && bar < totalBars - 1)
            velocity += 5;
        // A clear but not overpowering dynamic arc: build and chorus lift,
        // while the last bar eases slightly into the tonic resolution.
        if (powerMetalStyle && barProgress >= 0.25 && barProgress < 0.40)
            velocity += 2;
        if (powerMetalStyle && barProgress >= 0.40 && barProgress < 0.70)
            velocity += 4;
        // Phrase lift and answer accents: strengthen phrase starts and selected
        // offbeat pickups, while keeping the ending slightly more controlled.
        if (bar > 0 && downbeat)
            velocity += 2;
        if (settings.difficulty > 0 && offbeatPickup && barProgress >= 0.25
            && barProgress < 0.70)
            velocity += 2;
        if (bar == totalBars - 1)
            velocity -= 3;
        velocity = juce::jlimit(1, 127, velocity);
        track.addEvent(juce::MidiMessage::noteOn(1, note.midiNote,
                         static_cast<juce::uint8>(velocity)), startTick);

        // Expressive MIDI articulation for sustained lead notes. Pitch-wheel
        // data is optional for the receiving instrument; it does not alter the
        // note number used by the tab/scoring parser. Keep bends subtle and reset
        // the wheel before the note ends so the next note starts in tune.
        const bool expressiveLead = (vaiStyle || polyphiaStyle || bluesStyle);
        const bool canBend = expressiveLead && note.durationBeats >= 1.0
            && (barProgress >= 0.20 || bluesStyle)
            && (static_cast<int>(note.startBeat * 4.0) % 3 != 1);
        if (canBend)
        {
            const int bendAmount = bluesStyle ? 1550 : (vaiStyle ? 1800 : 1050);
            const double bendTick = startTick + (endTick - startTick) * 0.62;
            const double resetTick = startTick + (endTick - startTick) * 0.86;
            track.addEvent(juce::MidiMessage::pitchWheel(1,
                juce::jlimit(0, 16383, 8192 + bendAmount)), bendTick);
            track.addEvent(juce::MidiMessage::pitchWheel(1, 8192), resetTick);
        }
        else if (vaiStyle && note.durationBeats >= 1.5
                 && (static_cast<int>(note.startBeat * 2.0) % 2 == 0))
        {
            // Small pitch-wheel oscillation gives long Vai-style notes a gentle
            // vibrato-like motion when the selected MIDI instrument supports it.
            const double span = endTick - startTick;
            track.addEvent(juce::MidiMessage::pitchWheel(1, 8600), startTick + span * 0.55);
            track.addEvent(juce::MidiMessage::pitchWheel(1, 7850), startTick + span * 0.66);
            track.addEvent(juce::MidiMessage::pitchWheel(1, 8500), startTick + span * 0.77);
            track.addEvent(juce::MidiMessage::pitchWheel(1, 8192), startTick + span * 0.88);
        }
        track.addEvent(juce::MidiMessage::noteOff(1, note.midiNote), endTick);
    }
    track.sort();

    juce::MidiFile midi;
    midi.setTicksPerQuarterNote(ticksPerQuarter);
    midi.addTrack(track);

    if (outputMidiFile.getParentDirectory().createDirectory().failed())
    {
        setError(errorMessage, "Could not create the MIDI output folder.");
        return false;
    }

    {
    juce::FileOutputStream stream(outputMidiFile);
    if (!stream.openedOk())
    {
        setError(errorMessage, "Could not open the MIDI output file for writing.");
        return false;
    }
    stream.setPosition(0);
    stream.truncate();
    if (!midi.writeTo(stream))
    {
        setError(errorMessage, "JUCE could not write the MIDI file.");
        return false;
    }
    stream.flush();
    } // Close the file before the parser reopens it (required on Windows).

    // Run the exact same phrase-wide fingering optimiser used by imported MIDI.
    // This chooses practical string/fret positions and fingers across the phrase,
    // instead of leaving every generated note on the first locally valid string.
    std::vector<TabNote> playableNotes;
    if (MidiParser::loadMidi(outputMidiFile, playableNotes) && !playableNotes.empty())
        result.notes = std::move(playableNotes);

    result.midiFile = outputMidiFile;
    result.seed = settings.seed;
    result.durationSeconds = settings.durationSeconds;
    result.bpm = settings.bpm;
    switch (settings.style)
    {
        case Style::SteveVai:       result.keyName = "Vai-inspired melodic lead"; break;
        case Style::PowerMetal:
        {
            static constexpr const char* noteNames[12] = {
                "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
            };
            result.keyName = juce::String(noteNames[tonicPc]) + " harmonic minor / Power Rock";
            break;
        }
        case Style::ProgressiveMetal: result.keyName = "Progressive metal"; break;
        case Style::BluesRock:     result.keyName = "Blues rock"; break;
        case Style::HardRock:      result.keyName = "Hard rock"; break;
        case Style::PolyphiaInspired: result.keyName = "Polyphia-inspired instrumental prog"; break;
        case Style::TriviumInspired: result.keyName = "Trivium-inspired melodic metalcore"; break;
        case Style::DjentModernProg: result.keyName = "Djent / modern progressive metal"; break;
        case Style::NeoclassicalMetal: result.keyName = "Neoclassical metal"; break;
        case Style::ClassicRock: result.keyName = "Classic Rock"; break;
        case Style::HeavyMetal: result.keyName = "Heavy Metal"; break;
        case Style::SymphonicMetal: result.keyName = "Symphonic Metal"; break;
        case Style::TechnicalMetal: result.keyName = "Technical Metal"; break;
        case Style::ModernMetalcore: result.keyName = "Modern Metalcore"; break;
        case Style::GuitarFusion: result.keyName = "Guitar Fusion"; break;
        case Style::Blues: result.keyName = "Blues"; break;
        case Style::FunkSoul: result.keyName = "Funk / Soul / R&B"; break;
        case Style::JazzFusion: result.keyName = "Jazz Fusion"; break;
        case Style::AlternativeIndie: result.keyName = "Alternative / Indie"; break;
        case Style::CinematicExperimental: result.keyName = "Cinematic / Experimental"; break;
        case Style::CountryAmericana: result.keyName = "Country / Americana"; break;
    }
    setError(errorMessage, {});
    return true;
}
