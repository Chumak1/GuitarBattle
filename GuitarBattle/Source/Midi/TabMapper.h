#pragma once

#include <JuceHeader.h>
#include "TabNote.h"

class TabMapper
{
public:
    static bool midiNoteToPosition(
        int midiNote,
        int& stringIndex,
        int& fret)
    {
        // Standard guitar tuning:
        //
        // 6th string E2 = MIDI 40
        // 5th string A2 = MIDI 45
        // 4th string D3 = MIDI 50
        // 3rd string G3 = MIDI 55
        // 2nd string B3 = MIDI 59
        // 1st string E4 = MIDI 64

        static constexpr int openStrings[6] =
        {
            40,  // 6th string
            45,  // 5th string
            50,  // 4th string
            55,  // 3rd string
            59,  // 2nd string
            64   // 1st string
        };

        // Сначала пробуем самые тонкие струны.
        // Это даёт более естественную позицию для мелодии.
        for (int i = 5; i >= 0; --i)
        {
            const int calculatedFret =
                midiNote - openStrings[i];

            if (calculatedFret >= 0 &&
                calculatedFret <= 24)
            {
                stringIndex = i;
                fret = calculatedFret;

                return true;
            }
        }

        return false;
    }

    static TabNote createTabNote(
        int midiNote,
        double startBeat,
        double durationBeats)
    {
        TabNote note;

        note.midiNote = midiNote;
        note.startBeat = startBeat;
        note.durationBeats = durationBeats;

        int stringIndex = 0;
        int fret = 0;

        if (midiNoteToPosition(
            midiNote,
            stringIndex,
            fret))
        {
            note.stringIndex = stringIndex;
            note.fret = fret;
        }

        return note;
    }
};