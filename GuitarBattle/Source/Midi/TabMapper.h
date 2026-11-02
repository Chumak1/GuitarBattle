#pragma once

#include <JuceHeader.h>
#include <array>

#include "TabNote.h"

/** Open-string MIDI notes, low string first (6th -> 1st). */
namespace GuitarTuning
{
    inline constexpr std::array<int, 6> standard
    {
        40,  // 6th string E2
        45,  // 5th string A2
        50,  // 4th string D3
        55,  // 3rd string G3
        59,  // 2nd string B3
        64   // 1st string E4
    };

    inline juce::String noteName(int midiNote, bool withOctave)
    {
        return juce::MidiMessage::getMidiNoteName(midiNote, true, withOctave, 3);
    }

    inline juce::String label(const std::array<int, 6>& openMidiNotes,
                              bool withOctave = false)
    {
        juce::StringArray names;
        for (int note : openMidiNotes)
            names.add(noteName(note, withOctave));
        return names.joinIntoString("  ");
    }
}

class TabMapper
{
public:
    static bool midiNoteToPosition(
        int midiNote,
        int& stringIndex,
        int& fret,
        const std::array<int, 6>& openMidiNotes = GuitarTuning::standard)
    {
        // Сначала пробуем самые тонкие струны.
        // Это даёт более естественную позицию для мелодии.
        for (int i = 5; i >= 0; --i)
        {
            const int calculatedFret =
                midiNote - openMidiNotes[static_cast<size_t>(i)];

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
        double durationBeats,
        const std::array<int, 6>& openMidiNotes = GuitarTuning::standard)
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
            fret,
            openMidiNotes))
        {
            note.stringIndex = stringIndex;
            note.fret = fret;
        }

        return note;
    }
};