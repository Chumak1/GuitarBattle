#pragma once

#include <atomic>
#include <array>

#include "../Midi/TabMapper.h"

// Tuner domain logic: open-string targets for the selected tuning, string
// recognition, cents error, and per-string tuning progress. The tuning is
// shared with the tab mapper, so the tuner and the tabs always agree.
// UI presentation stays in MainComponent.
class GuitarTuner
{
public:
    GuitarTuner();

    /** Open-string MIDI notes, low string first (6th -> 1st). */
    void setTuning(const std::array<int, 6>& newOpenMidiNotes);
    std::array<int, 6> getTuning() const;
    int getOpenMidiNote(int stringIndex) const;

    int detectString(double frequency) const;
    double getTargetFrequency(int stringIndex) const;
    double getCentsDeviation(double frequency, int stringIndex) const;
    bool isInTune(double frequency, int stringIndex, double toleranceCents = 10.0) const;

    void resetTunedStrings();
    void markStringTuned(int stringIndex);
    void markAllStringsTuned();
    int getTunedMask() const;
    bool isStringTuned(int stringIndex) const;
    bool areAllStringsTuned() const;

private:
    // Written from the message thread when the player picks a tuning; read by
    // the pitch worker on every analysed block. Plain atomics keep the handoff
    // lock-free and safe without touching the audio callback.
    std::atomic<int> openMidiNotes[6];
    std::atomic<int> tunedMask{ 0 };
};
