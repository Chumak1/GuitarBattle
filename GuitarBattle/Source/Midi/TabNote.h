#pragma once

#include <JuceHeader.h>

struct TabNote
{
    int midiNote = 0;

    // 0 = 6-я струна (низкая E)
    // 5 = 1-я струна (высокая E)
    int stringIndex = 0;

    int fret = 0;

    double startBeat = 0.0;
    double durationBeats = 0.0;
};