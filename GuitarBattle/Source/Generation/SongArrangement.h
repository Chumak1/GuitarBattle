#pragma once

#include <JuceHeader.h>
#include <cstdint>

// Creates deterministic backing MIDI tracks that share the Daily Challenge tempo,
// harmony and duration. The guitar MIDI remains separate so gameplay/scoring keeps
// using exactly the existing guitar-only file.
class SongArrangement
{
public:
    struct Settings
    {
        int seed = 1;
        double bpm = 110.0;
        int bars = 8;
        int ticksPerQuarter = 480;
        int style = 1; // 0-based MidiMelodyGenerator::Style id
    };

    struct Result
    {
        juce::File bassMidi;
        juce::File drumsMidi;
        juce::File pianoMidi;
        juce::File rhythmGuitarMidi;
        juce::File backingMidi; // bass + drums + piano + rhythm guitar, independent tracks
        juce::String progression;
    };

    static bool generateBacking(const Settings& settings,
                                const juce::File& outputDirectory,
                                Result& result,
                                juce::String* errorMessage = nullptr);
};
