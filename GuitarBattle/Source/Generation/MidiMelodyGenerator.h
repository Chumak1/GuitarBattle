#pragma once

#include <JuceHeader.h>
#include <cstdint>
#include <vector>
#include "../Midi/TabNote.h"

// Deterministic, server-ready first version of the short guitar challenge generator.
// Supplying the same seed and settings produces the same melody and MIDI file.
class MidiMelodyGenerator
{
public:
    enum class Style
    {
        SteveVai = 0,
        PowerMetal,
        ProgressiveMetal,
        BluesRock,
        HardRock,
        PolyphiaInspired,
        TriviumInspired,
        DjentModernProg,
        NeoclassicalMetal,
        ClassicRock,
        HeavyMetal,
        SymphonicMetal,
        TechnicalMetal,
        ModernMetalcore,
        GuitarFusion,
        Blues,
        FunkSoul,
        JazzFusion,
        AlternativeIndie,
        CinematicExperimental,
        CountryAmericana
    };

    struct Settings
    {
        int durationSeconds = 20; // clamped to the duel range: 15..30 seconds
        double bpm = 110.0;
        int seed = 0;             // 0 = choose a new seed; non-zero = reproducible
        int difficulty = 1;       // 0 = easy, 1 = normal, 2 = hard
        Style style = Style::PowerMetal;
    };

    struct Result
    {
        std::vector<TabNote> notes;
        juce::File midiFile;
        int seed = 0;
        int durationSeconds = 20;
        double bpm = 110.0;
        juce::String keyName;
    };

    // Writes a standard MIDI file and returns matching tab/gameplay note events.
    static bool generate(const Settings& settings,
                         const juce::File& outputMidiFile,
                         Result& result,
                         juce::String* errorMessage = nullptr);
};
