#pragma once

#include <JuceHeader.h>

struct TabNote
{
    int midiNote = 0;

    // 0 = 6-я струна (низкая E)
    // 5 = 1-я струна (высокая E)
    int stringIndex = 0;

    int fret = 0;

    // Suggested fretting-hand finger: 0=open string, 1=index, 2=middle, 3=ring, 4=pinky.
    int finger = 0;

    double startBeat = 0.0;
    double durationBeats = 0.0;
};