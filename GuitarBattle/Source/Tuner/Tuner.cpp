#include "Tuner.h"

#include <cmath>

namespace
{
    double midiToFrequency(int midiNote)
    {
        return 440.0 * std::pow(2.0,
                                (static_cast<double>(midiNote) - 69.0) / 12.0);
    }
}

GuitarTuner::GuitarTuner()
{
    for (int i = 0; i < GuitarTuning::maxStrings; ++i)
        openMidiNotes[i].store(GuitarTuning::standardTuning().notes[static_cast<size_t>(i)],
                               std::memory_order_relaxed);
}

void GuitarTuner::setTuning(const GuitarTuning::Tuning& tuning)
{
    const int count = juce::jlimit(1, GuitarTuning::maxStrings, tuning.count);

    for (int i = 0; i < GuitarTuning::maxStrings; ++i)
    {
        int note = i < count ? tuning.notes[static_cast<size_t>(i)]
                             : GuitarTuning::emptyNote;

        if (note != GuitarTuning::emptyNote)
            note = juce::jlimit(0, 127, note);

        openMidiNotes[i].store(note, std::memory_order_relaxed);
    }

    stringCount.store(count, std::memory_order_relaxed);

    // The previous green lights referred to different target pitches.
    resetTunedStrings();
}

GuitarTuning::Tuning GuitarTuner::getTuning() const
{
    GuitarTuning::Tuning result;

    result.count = getStringCount();

    for (int i = 0; i < result.count; ++i)
        result.notes[static_cast<size_t>(i)] = getOpenMidiNote(i);

    return result;
}

int GuitarTuner::getStringCount() const
{
    return juce::jlimit(1, GuitarTuning::maxStrings,
                        stringCount.load(std::memory_order_relaxed));
}

int GuitarTuner::getActiveStringCount() const
{
    int result = 0;

    for (int i = 0; i < getStringCount(); ++i)
        if (isActiveSlot(i))
            ++result;

    return result;
}

int GuitarTuner::getOpenMidiNote(int stringIndex) const
{
    if (stringIndex < 0 || stringIndex >= getStringCount())
        return GuitarTuning::emptyNote;

    return openMidiNotes[stringIndex].load(std::memory_order_relaxed);
}

bool GuitarTuner::isActiveSlot(int stringIndex) const
{
    return getOpenMidiNote(stringIndex) != GuitarTuning::emptyNote;
}

int GuitarTuner::detectString(double frequency) const
{
    if (frequency <= 0.0)
        return -1;

    int bestIndex = -1;
    double bestCents = 1.0e9;

    for (int i = 0; i < getStringCount(); ++i)
    {
        // An empty slot has no target pitch and must not attract the note.
        if (! isActiveSlot(i))
            continue;

        const double target = getTargetFrequency(i);
        if (target <= 0.0)
            continue;

        const double cents =
            1200.0 * std::log2(frequency / target);

        const double absoluteCents = std::abs(cents);

        if (absoluteCents < bestCents)
        {
            bestCents = absoluteCents;
            bestIndex = i;
        }
    }

    // Струна определяется только в пределах ±100 cents.
    if (bestCents > 100.0)
        return -1;

    return bestIndex;
}

double GuitarTuner::getTargetFrequency(int stringIndex) const
{
    const int midiNote = getOpenMidiNote(stringIndex);
    if (midiNote < 0)
        return 0.0;

    return midiToFrequency(midiNote);
}

double GuitarTuner::getCentsDeviation(double frequency, int stringIndex) const
{
    const double target = getTargetFrequency(stringIndex);
    if (frequency <= 0.0 || target <= 0.0)
        return 0.0;

    return 1200.0 * std::log2(frequency / target);
}

bool GuitarTuner::isInTune(double frequency, int stringIndex, double toleranceCents) const
{
    if (frequency <= 0.0 || getTargetFrequency(stringIndex) <= 0.0)
        return false;

    return std::abs(getCentsDeviation(frequency, stringIndex)) <= std::abs(toleranceCents);
}

void GuitarTuner::resetTunedStrings()
{
    tunedMask.store(0, std::memory_order_relaxed);
}

void GuitarTuner::markStringTuned(int stringIndex)
{
    if (isActiveSlot(stringIndex))
        tunedMask.fetch_or(1 << stringIndex, std::memory_order_relaxed);
}

void GuitarTuner::markAllStringsTuned()
{
    int mask = 0;

    for (int i = 0; i < getStringCount(); ++i)
        if (isActiveSlot(i))
            mask |= 1 << i;

    tunedMask.store(mask, std::memory_order_relaxed);
}

int GuitarTuner::getTunedMask() const
{
    return tunedMask.load(std::memory_order_relaxed)
        & ((1 << GuitarTuning::maxStrings) - 1);
}

bool GuitarTuner::isStringTuned(int stringIndex) const
{
    return isActiveSlot(stringIndex)
        && (getTunedMask() & (1 << stringIndex)) != 0;
}

bool GuitarTuner::areAllStringsTuned() const
{
    const int active = getActiveStringCount();

    if (active == 0)
        return false;

    const int mask = getTunedMask();
    int tuned = 0;

    for (int i = 0; i < getStringCount(); ++i)
        if (isActiveSlot(i) && (mask & (1 << i)) != 0)
            ++tuned;

    return tuned == active;
}
