#include "MidiParser.h"

bool MidiParser::loadMidi(
    const juce::File& file,
    std::vector<TabNote>& outputNotes)



{
    outputNotes.clear();

    if (!file.existsAsFile())
    {
        DBG("MIDI file not found: " + file.getFullPathName());
        return false;
    }

    juce::FileInputStream input(file);

    if (!input.openedOk())
    {
        DBG("Could not open MIDI file: " + file.getFullPathName());
        return false;
    }

    juce::MidiFile midiFile;

    if (!midiFile.readFrom(input))
    {
        DBG("Could not read MIDI file.");
        return false;
    }

    DBG("MIDI loaded: " + file.getFileName());

    DBG("Number of tracks: "
        + juce::String(midiFile.getNumTracks()));

    DBG("Time format: "
        + juce::String(midiFile.getTimeFormat()));

    for (int trackIndex = 0;
        trackIndex < midiFile.getNumTracks();
        ++trackIndex)
    {
        const auto* track =
            midiFile.getTrack(trackIndex);

        if (track == nullptr)
            continue;

        processTrack(
            *track,
            static_cast<double>(
                midiFile.getTimeFormat()),
            outputNotes);
    }

    std::sort(
        outputNotes.begin(),
        outputNotes.end(),
        [](const TabNote& a, const TabNote& b)
        {
            return a.startBeat < b.startBeat;
        });

    DBG("Total guitar notes: "
        + juce::String(
            static_cast<int>(outputNotes.size())));

    for (size_t i = 0;
        i < outputNotes.size() && i < 30;
        ++i)
    {
        const auto& note = outputNotes[i];

        DBG(
            "Note "
            + juce::String(static_cast<int>(i))
            + ": beat="
            + juce::String(note.startBeat, 3)
            + " midi="
            + juce::String(note.midiNote)
            + " string="
            + juce::String(note.stringIndex + 1)
            + " fret="
            + juce::String(note.fret)
            + " duration="
            + juce::String(note.durationBeats, 3));
    }

    return !outputNotes.empty();
}

void MidiParser::processTrack(
    const juce::MidiMessageSequence& sequence,
    double ticksPerQuarterNote,
    std::vector<TabNote>& outputNotes)
{
    struct ActiveNote
    {
        int midiNote = 0;
        double startBeat = 0.0;
    };

    std::vector<ActiveNote> activeNotes;

    const int numEvents =
        sequence.getNumEvents();

    for (int i = 0;
        i < numEvents;
        ++i)
    {
        const auto* event =
            sequence.getEventPointer(i);

        if (event == nullptr)
            continue;

        const auto& message =
            event->message;

        const double time =
            message.getTimeStamp();

        if (message.isNoteOn() &&
            message.getVelocity() > 0)
        {
            ActiveNote active;

            active.midiNote =
                message.getNoteNumber();

            active.startBeat =
                time / ticksPerQuarterNote;

            activeNotes.push_back(active);
        }
        else if (message.isNoteOff())
        {
            const int midiNote =
                message.getNoteNumber();

            for (int j =
                static_cast<int>(activeNotes.size()) - 1;
                j >= 0;
                --j)
            {
                if (activeNotes[j].midiNote == midiNote)
                {
                    const auto active =
                        activeNotes[j];

                    const double durationTicks =
                        juce::jmax(
                            0.0,
                            time -
                            (active.startBeat * ticksPerQuarterNote));

                    const double durationBeats =
                        durationTicks /
                        ticksPerQuarterNote;

                    int stringIndex = 0;
                    int fret = 0;

                    if (TabMapper::midiNoteToPosition(
                        midiNote,
                        stringIndex,
                        fret))
                    {
                        TabNote note;

                        note.midiNote = midiNote;
                        note.stringIndex = stringIndex;
                        note.fret = fret;
                        note.startBeat = active.startBeat;
                        note.durationBeats = durationBeats;

                        outputNotes.push_back(note);
                    }

                    activeNotes.erase(
                        activeNotes.begin() + j);

                    break;
                }
            }
        }
    }
}