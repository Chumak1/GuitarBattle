#pragma once

#include <atomic>
#include <array>

#include "../Midi/TabMapper.h"

// Tuner domain logic: open-string targets for the selected tuning, string
// recognition, cents error, and per-string tuning progress. The tuning is
// shared with the tab mapper, so the tuner and the tabs always agree.
// UI presentation stays in MainComponent.
//
// The instrument may have more or fewer than six strings, and a slot may be
// marked as empty (``GuitarTuning::emptyNote``): such a slot has no target, is
// never detected and does not block "all strings tuned".
class GuitarTuner
{
public:
    GuitarTuner();

    void setTuning(const GuitarTuning::Tuning& tuning);
    GuitarTuning::Tuning getTuning() const;

    /** Slot count, including the slots marked as empty. */
    int getStringCount() const;

    /** Strings the player actually tunes and plays. */
    int getActiveStringCount() const;

    /** Open MIDI note of a slot, or ``GuitarTuning::emptyNote``. */
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
    bool isActiveSlot(int stringIndex) const;

    // Written from the message thread when the player picks a tuning; read by
    // the pitch worker on every analysed block. Plain atomics keep the handoff
    // lock-free and safe without touching the audio callback.
    std::atomic<int> openMidiNotes[GuitarTuning::maxStrings];
    std::atomic<int> stringCount{ 6 };
    std::atomic<int> tunedMask{ 0 };
};
