#include <functional>
#include "MidiParser.h"

bool MidiParser::loadMidi(
    const juce::File& file,
    std::vector<TabNote>& outputNotes,
    const std::array<int, 6>& openMidiNotes)
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
            outputNotes,
            openMidiNotes);
    }

    std::sort(
        outputNotes.begin(),
        outputNotes.end(),
        [](const TabNote& a, const TabNote& b)
        {
            return a.startBeat < b.startBeat;
        });

    // Optimise complete phrases for a stable left-hand position and practical
    // fingering. Each candidate includes a string, fret, hand position (index
    // finger fret), and the finger used. A hand position spans four frets.
    struct PositionCandidate
    {
        int stringIndex = 0; // 0 = 6th (low E), 5 = 1st (high E)
        int fret = 0;
        int handPosition = 1;
        int finger = 0; // 0=open string, 1=index, 2=middle, 3=ring, 4=pinky
        int preferenceCost = 0;
    };

    const size_t noteCount = outputNotes.size();
    std::vector<std::vector<PositionCandidate>> candidates(noteCount);

    for (size_t i = 0; i < noteCount; ++i)
    {
        const auto& note = outputNotes[i];
        auto addCandidatesForMaxFret = [&](int maxFret)
        {
            for (int stringIndex = 0; stringIndex < 6; ++stringIndex)
            {
                const int fret = note.midiNote
                    - openMidiNotes[static_cast<size_t>(stringIndex)];
                if (fret < 0 || fret > maxFret)
                    continue;

                for (int handPosition = 1; handPosition <= (maxFret > 15 ? 21 : 12); ++handPosition)
            {
                int finger = 0;
                if (fret > 0)
                {
                    // Four-fret hand frame: index, middle, ring, pinky.
                    if (fret < handPosition || fret > handPosition + 3)
                        continue;
                    finger = fret - handPosition + 1;
                }

                    // Prefer positions at or below fret 12. Frets 13-15 remain
                    // available when needed, but are less desirable than a lower
                    // playable position on another string.
                    const int highFretPenalty = std::max(0, fret - 12) * 18;
                    const int stringPenalty = stringIndex == note.stringIndex ? 0 : 3;
                    candidates[i].push_back({ stringIndex, fret, handPosition, finger,
                                              stringPenalty + highFretPenalty });
                }
            }
        };

        // First try a comfortable range (0-15). Only notes that cannot be played
        // in that range get a wider fallback, so unusually high notes still work.
        addCandidatesForMaxFret(15);
        if (candidates[i].empty())
            addCandidatesForMaxFret(24);
    }

    if (noteCount > 0)
    {
        constexpr int impossibleCost = 100000000;
        std::vector<std::vector<int>> costs(noteCount);
        std::vector<std::vector<int>> previousCandidate(noteCount);

        for (size_t i = 0; i < noteCount; ++i)
        {
            costs[i].assign(candidates[i].size(), impossibleCost);
            previousCandidate[i].assign(candidates[i].size(), -1);

            for (size_t currentIndex = 0; currentIndex < candidates[i].size(); ++currentIndex)
            {
                const auto& current = candidates[i][currentIndex];
                if (i == 0)
                {
                    costs[i][currentIndex] = current.preferenceCost;
                    continue;
                }

                for (size_t previousIndex = 0; previousIndex < candidates[i - 1].size(); ++previousIndex)
                {
                    if (costs[i - 1][previousIndex] >= impossibleCost)
                        continue;

                    const auto& previous = candidates[i - 1][previousIndex];
                    const int handShift = std::abs(current.handPosition - previous.handPosition);
                    const int stringShift = std::abs(current.stringIndex - previous.stringIndex);
                    int transitionCost = handShift * 24 + stringShift * 2 + current.preferenceCost;

                    // Avoid repeated long runs that force the hand to crawl up/down the neck.
                    if (handShift > 1)
                        transitionCost += 30 + handShift * 12;

                    // Crossing strings on the same fret with the same finger is
                    // possible, but usually less fluid than an alternate fingering.
                    if (current.stringIndex != previous.stringIndex &&
                        current.fret == previous.fret && current.finger != 0 &&
                        current.finger == previous.finger)
                        transitionCost += 14;

                    // Avoid using the same fretting finger on consecutive notes
                    // when another practical fingering is available. Repeating a
                    // finger can be valid, so this is a preference rather than a
                    // hard constraint.
                    if (previous.finger != 0 && current.finger != 0 &&
                        previous.finger == current.finger)
                    {
                        transitionCost += 13;

                        // Repeating the same finger while crossing strings is
                        // especially awkward unless the phrase clearly benefits
                        // from a small barre-like movement.
                        if (previous.stringIndex != current.stringIndex)
                            transitionCost += 9;

                        // Repeating a finger on the same string for a different
                        // fret often means sliding it; prefer another finger when
                        // the hand can remain in the same four-fret frame.
                        if (previous.stringIndex == current.stringIndex &&
                            previous.fret != current.fret && handShift == 0)
                            transitionCost += 5;
                    }

                    // Prefer changing fingers for adjacent notes when the hand
                    // remains in position. This avoids long runs such as index,
                    // index, index, index when a nearby finger can take over.
                    if (previous.finger != 0 && current.finger != 0 &&
                        previous.finger != current.finger && handShift <= 1)
                        transitionCost -= 2;

                    // After the pinky, favour returning toward the index or middle
                    // finger if that does not cause a large hand shift.
                    if (previous.finger == 4 &&
                        (current.finger == 1 || current.finger == 2) &&
                        handShift <= 1)
                        transitionCost -= 4;

                    const int totalCost = costs[i - 1][previousIndex] + transitionCost;
                    if (totalCost < costs[i][currentIndex])
                    {
                        costs[i][currentIndex] = totalCost;
                        previousCandidate[i][currentIndex] = static_cast<int>(previousIndex);
                    }
                }
            }
        }

        size_t bestIndex = 0;
        for (size_t i = 1; i < costs.back().size(); ++i)
            if (costs.back()[i] < costs.back()[bestIndex])
                bestIndex = i;

        for (size_t reverseIndex = noteCount; reverseIndex-- > 0;)
        {
            const auto& selected = candidates[reverseIndex][bestIndex];
            outputNotes[reverseIndex].stringIndex = selected.stringIndex;
            outputNotes[reverseIndex].fret = selected.fret;
            outputNotes[reverseIndex].finger = selected.finger;

            if (reverseIndex > 0)
            {
                const int previousIndex = previousCandidate[reverseIndex][bestIndex];
                bestIndex = previousIndex >= 0 ? static_cast<size_t>(previousIndex) : 0;
            }
        }
    }

    // Chord-song voicing: treat notes at the same beat as one shape instead
    // of optimising every note independently. This branch is intentionally
    // limited to Chords.mid; the existing solo fingering optimiser is untouched.
    if (file.getFileName().equalsIgnoreCase("Chords.mid"))
    {
        const std::array<int, 4> chordOpenStrings
        {
            openMidiNotes[0],
            openMidiNotes[1],
            openMidiNotes[2],
            openMidiNotes[3]
        };
        double previousRootFret = -1.0;
        size_t groupBegin = 0;

        while (groupBegin < outputNotes.size())
        {
            size_t groupEnd = groupBegin + 1;
            while (groupEnd < outputNotes.size() &&
                   std::abs(outputNotes[groupEnd].startBeat - outputNotes[groupBegin].startBeat) < 0.001)
                ++groupEnd;

            const size_t groupSize = groupEnd - groupBegin;
            if (groupSize >= 2 && groupSize <= 4)
            {
                std::vector<size_t> order;
                for (size_t i = groupBegin; i < groupEnd; ++i)
                    order.push_back(i);
                std::sort(order.begin(), order.end(), [&](size_t a, size_t b)
                {
                    return outputNotes[a].midiNote < outputNotes[b].midiNote;
                });

                struct ChordChoice { int stringIndex = 0; int fret = 0; };
                std::vector<ChordChoice> current(groupSize), best(groupSize);
                int bestCost = 100000000;

                std::function<void(size_t, int)> search = [&](size_t noteOffset, int cost)
                {
                    if (cost >= bestCost)
                        return;
                    if (noteOffset == groupSize)
                    {
                        const int rootFret = current[0].fret;
                        int total = cost;
                        for (size_t j = 0; j < groupSize; ++j)
                        {
                            // Prefer a contiguous bass-to-treble shape beginning on string 6.
                            total += std::abs(current[j].stringIndex - static_cast<int>(j)) * 80;
                            total += std::max(0, current[j].fret - rootFret - 4) * 25;
                        }
                        if (previousRootFret >= 0.0)
                            total += static_cast<int>(std::abs(rootFret - previousRootFret) * 12.0);
                        if (total < bestCost)
                        {
                            bestCost = total;
                            best = current;
                        }
                        return;
                    }

                    const auto& note = outputNotes[order[noteOffset]];
                    const int lastString = noteOffset == 0 ? -1 : current[noteOffset - 1].stringIndex;
                    for (int stringIndex = lastString + 1; stringIndex < 4; ++stringIndex)
                    {
                        const int fret = note.midiNote - chordOpenStrings[stringIndex];
                        if (fret < 0 || fret > 22)
                            continue;
                        // The index-finger barre/root is the lowest fretted point of this shape.
                        if (noteOffset > 0 && fret < current[0].fret)
                            continue;
                        current[noteOffset] = { stringIndex, fret };
                        const int localCost = std::abs(stringIndex - static_cast<int>(noteOffset)) * 80
                                            + (fret == 0 ? 3 : 0);
                        search(noteOffset + 1, cost + localCost);
                    }
                };
                search(0, 0);

                if (bestCost < 100000000)
                {
                    const int rootFret = best[0].fret;
                    std::vector<int> fingers(groupSize, 1);
                    for (size_t j = 0; j < groupSize; ++j)
                    {
                        if (best[j].fret == rootFret)
                        {
                            // Only the index finger may barre, and no chord note sits below it.
                            fingers[j] = 1;
                            continue;
                        }

                        int sameFretRank = 0;
                        for (size_t k = 0; k < j; ++k)
                            if (best[k].fret == best[j].fret && best[k].fret > rootFret)
                                ++sameFretRank;

                        if (sameFretRank > 0)
                            fingers[j] = std::min(4, 3 + sameFretRank); // separate ring/pinky fingers, not a barre
                        else
                            fingers[j] = juce::jlimit(2, 4, best[j].fret - rootFret + 1);
                    }

                    for (size_t j = 0; j < groupSize; ++j)
                    {
                        auto& note = outputNotes[order[j]];
                        note.stringIndex = best[j].stringIndex;
                        note.fret = best[j].fret;
                        note.finger = fingers[j];
                    }
                    previousRootFret = static_cast<double>(rootFret);
                }
            }
            groupBegin = groupEnd;
        }
    }

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
    std::vector<TabNote>& outputNotes,
    const std::array<int, 6>& openMidiNotes)
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
                        fret,
                        openMidiNotes))
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