#pragma once

#include <JuceHeader.h>
#include "TabNote.h"
#include "TabMapper.h"

class MidiParser
{
public:
    //: ``tuning`` decides which strings may receive notes: a slot marked as
    //: empty is skipped, and a note that fits nowhere else is dropped instead
    //: of being forced onto the unused string.
    static bool loadMidi(
        const juce::File& file,
        std::vector<TabNote>& outputNotes,
        const GuitarTuning::Tuning& tuning = GuitarTuning::standardTuning());

private:
    static void processTrack(
        const juce::MidiMessageSequence& sequence,
        double ticksPerQuarterNote,
        std::vector<TabNote>& outputNotes);
};
