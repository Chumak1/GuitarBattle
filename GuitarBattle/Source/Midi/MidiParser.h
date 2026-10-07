#pragma once

#include <JuceHeader.h>
#include "TabNote.h"
#include "TabMapper.h"

class MidiParser
{
public:
    static bool loadMidi(
        const juce::File& file,
        std::vector<TabNote>& outputNotes);

private:
    static void processTrack(
        const juce::MidiMessageSequence& sequence,
        double ticksPerQuarterNote,
        std::vector<TabNote>& outputNotes);
};