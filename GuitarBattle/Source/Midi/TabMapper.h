#pragma once

#include <JuceHeader.h>
#include <array>

#include "TabNote.h"

/** Open-string MIDI notes, low string first (6th -> 1st).
 *
 *  A chart is playable on instruments other than a six-string guitar, and a
 *  slot may be marked as empty when the player does not want the MIDI parser
 *  to place notes on that string. ``Tuning`` keeps the slot order (index 0 is
 *  the lowest string) and the displayed count, so the tuner, the parser and
 *  the tablature always agree on which string a note belongs to.
 */
namespace GuitarTuning
{
    //: Upper bound of the editable slot count. Eight covers four-, five- and
    //: six-string basses and six-, seven- and eight-string guitars.
    inline constexpr int maxStrings = 8;

    //: Sentinel for a slot the player marked as unused.
    inline constexpr int emptyNote = -1;

    //: Lowest and highest slot counts the tuner offers.
    inline constexpr int minStringCount = 4;

    struct Tuning
    {
        //: Open MIDI notes, index 0 = lowest string. Entries at or after
        //: ``count`` are ignored; ``emptyNote`` marks an unused string.
        std::array<int, maxStrings> notes{};
        int count = 6;
    };

    inline constexpr std::array<int, 6> standard
    {
        40,  // 6th string E2
        45,  // 5th string A2
        50,  // 4th string D3
        55,  // 3rd string G3
        59,  // 2nd string B3
        64   // 1st string E4
    };

    inline Tuning standardTuning()
    {
        Tuning tuning;
        tuning.count = 6;
        tuning.notes = { 40, 45, 50, 55, 59, 64, emptyNote, emptyNote };
        return tuning;
    }

    /** A slot that exists in the tuning and is not marked empty. */
    inline bool isActive(const Tuning& tuning, int slot)
    {
        return slot >= 0
            && slot < tuning.count
            && tuning.notes[static_cast<size_t>(slot)] != emptyNote;
    }

    /** Number of strings the player actually tunes and plays. */
    inline int activeCount(const Tuning& tuning)
    {
        int result = 0;

        for (int slot = 0; slot < tuning.count; ++slot)
            if (isActive(tuning, slot))
                ++result;

        return result;
    }

    /** The number shown in the UI: the highest string is 1. */
    inline int stringNumber(const Tuning& tuning, int slot)
    {
        return tuning.count - slot;
    }

    inline juce::String noteName(int midiNote, bool withOctave)
    {
        return juce::MidiMessage::getMidiNoteName(midiNote, true, withOctave, 3);
    }

    /** Note names of the active strings, low string first; empty slots are
        skipped so "DROP D / D A D G B E" never shows a placeholder.
    */
    inline juce::String label(const Tuning& tuning,
                              bool withOctave = false)
    {
        juce::StringArray names;

        for (int slot = 0; slot < tuning.count; ++slot)
            if (isActive(tuning, slot))
                names.add(noteName(tuning.notes[static_cast<size_t>(slot)],
                                   withOctave));

        return names.joinIntoString("  ");
    }

    /** Compares only the slots that are part of the tuning. */
    inline bool sameTuning(const Tuning& left, const Tuning& right)
    {
        if (left.count != right.count)
            return false;

        for (int slot = 0; slot < left.count; ++slot)
            if (left.notes[static_cast<size_t>(slot)]
                != right.notes[static_cast<size_t>(slot)])
                return false;

        return true;
    }
}

class TabMapper
{
public:
    static bool midiNoteToPosition(
        int midiNote,
        int& stringIndex,
        int& fret,
        const GuitarTuning::Tuning& tuning = GuitarTuning::standardTuning())
    {
        // Сначала пробуем самые тонкие струны.
        // Это даёт более естественную позицию для мелодии.
        for (int i = tuning.count - 1; i >= 0; --i)
        {
            // A string the player marked as empty must never receive a note.
            if (! GuitarTuning::isActive(tuning, i))
                continue;

            const int calculatedFret =
                midiNote - tuning.notes[static_cast<size_t>(i)];

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
        const GuitarTuning::Tuning& tuning = GuitarTuning::standardTuning())
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
            tuning))
        {
            note.stringIndex = stringIndex;
            note.fret = fret;
        }

        return note;
    }
};
