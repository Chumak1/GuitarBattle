#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>

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

    //: Lowest and highest slot counts the tuner offers.
    inline constexpr int minStringCount = 1;

    //: Fret range the player can choose from. 22 is a common electric guitar
    //: neck; the parser never places a note above the selected fret.
    inline constexpr int minFretCount = 12;
    inline constexpr int maxFretCount = 36;
    inline constexpr int defaultFretCount = 22;

    //: Sentinel for a slot the player marked as unused.
    inline constexpr int emptyNote = -1;

    struct Tuning
    {
        //: Open MIDI notes, index 0 = lowest string. Entries at or after
        //: ``count`` are ignored; ``emptyNote`` marks an unused string.
        std::array<int, maxStrings> notes{};
        int count = 6;
        //: Frets on the neck. A note that needs a higher fret is moved by
        //: whole octaves until it fits (see TabMapper::fitNoteToPosition).
        int fretCount = defaultFretCount;
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
        tuning.fretCount = defaultFretCount;
        tuning.notes = { 40, 45, 50, 55, 59, 64, emptyNote, emptyNote };
        return tuning;
    }

    /** Frets a note may use: the player's neck, clamped to the UI range. */
    inline int fretLimit(const Tuning& tuning)
    {
        return juce::jlimit(0, maxFretCount, tuning.fretCount);
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

    /** Compares the open strings only: two tunings with the same strings but a
        different neck length are the same preset.
    */
    inline bool sameStrings(const Tuning& left, const Tuning& right)
    {
        if (left.count != right.count)
            return false;

        for (int slot = 0; slot < left.count; ++slot)
            if (left.notes[static_cast<size_t>(slot)]
                != right.notes[static_cast<size_t>(slot)])
                return false;

        return true;
    }

    /** Compares only the slots that are part of the tuning. */
    inline bool sameTuning(const Tuning& left, const Tuning& right)
    {
        if (left.count != right.count || left.fretCount != right.fretCount)
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
                calculatedFret <= GuitarTuning::fretLimit(tuning))
            {
                stringIndex = i;
                fret = calculatedFret;

                return true;
            }
        }

        return false;
    }

    /** Places a written note on the instrument, moving it by whole octaves
        when the selected strings and frets cannot play it.

        A melody written for a guitar stays playable on a bass or a balalaika:
        the note is shifted down or up by one octave at a time (down first when
        both directions work) until it fits the neck. Returns ``false`` only
        when no octave of the note fits at all.
    */
    static bool fitNoteToPosition(
        int midiNote,
        const GuitarTuning::Tuning& tuning,
        int& fittedMidiNote,
        int& stringIndex,
        int& fret)
    {
        if (midiNoteToPosition(midiNote, stringIndex, fret, tuning))
        {
            fittedMidiNote = midiNote;
            return true;
        }

        for (int octave = 1; octave <= 4; ++octave)
        {
            for (const int semitones : { -12, 12 })
            {
                const int shifted = midiNote + semitones * octave;

                if (shifted < 0 || shifted > 127)
                    continue;

                if (midiNoteToPosition(shifted, stringIndex, fret, tuning))
                {
                    fittedMidiNote = shifted;
                    return true;
                }
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

        int fittedNote = midiNote;
        int stringIndex = 0;
        int fret = 0;

        if (fitNoteToPosition(
            midiNote,
            tuning,
            fittedNote,
            stringIndex,
            fret))
        {
            note.midiNote = fittedNote;
            note.stringIndex = stringIndex;
            note.fret = fret;
        }

        return note;
    }

    /** The octave offset (a multiple of 12 semitones) that lets the chart play
        the most notes as written.

        Moving every note by the same number of octaves keeps the melody's up
        and down steps intact; changing notes one by one does not. The parser
        therefore picks the best offset first and only shifts the leftovers
        individually. Ties prefer the smaller shift and, at equal size, the
        lower octave.
    */
    static int bestChartOctaveOffset(
        const std::vector<TabNote>& notes,
        const GuitarTuning::Tuning& tuning)
    {
        int bestOffset = 0;
        int bestPlayable = -1;

        for (const int offset : { 0, -12, 12, -24, 24, -36, 36, -48, 48 })
        {
            int playable = 0;

            for (const auto& note : notes)
            {
                const int shifted = note.midiNote + offset;

                if (shifted < 0 || shifted > 127)
                    continue;

                int stringIndex = 0;
                int fret = 0;

                if (midiNoteToPosition(shifted, stringIndex, fret, tuning))
                    ++playable;
            }

            if (playable > bestPlayable)
            {
                bestPlayable = playable;
                bestOffset = offset;
            }
        }

        return bestOffset;
    }
};
