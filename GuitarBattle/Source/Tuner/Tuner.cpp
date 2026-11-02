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
    for (int i = 0; i < 6; ++i)
        openMidiNotes[i].store(GuitarTuning::standard[static_cast<size_t>(i)],
                               std::memory_order_relaxed);
}

void GuitarTuner::setTuning(const std::array<int, 6>& newOpenMidiNotes)
{
    for (int i = 0; i < 6; ++i)
        openMidiNotes[i].store(
            juce::jlimit(0, 127, newOpenMidiNotes[static_cast<size_t>(i)]),
            std::memory_order_relaxed);

    // The previous green lights referred to different target pitches.
    resetTunedStrings();
}

std::array<int, 6> GuitarTuner::getTuning() const
{
    std::array<int, 6> result {};

    for (int i = 0; i < 6; ++i)
        result[static_cast<size_t>(i)] =
            openMidiNotes[i].load(std::memory_order_relaxed);

    return result;
}

int GuitarTuner::getOpenMidiNote(int stringIndex) const
{
    if (stringIndex < 0 || stringIndex >= 6)
        return -1;

    return openMidiNotes[stringIndex].load(std::memory_order_relaxed);
}

int GuitarTuner::detectString(double frequency) const
{
    if (frequency <= 0.0)
        return -1;

    int bestIndex = -1;
    double bestCents = 1.0e9;

    for (int i = 0; i < 6; ++i)
    {
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
    if (stringIndex >= 0 && stringIndex < 6)
        tunedMask.fetch_or(1 << stringIndex, std::memory_order_relaxed);
}

void GuitarTuner::markAllStringsTuned()
{
    tunedMask.store(0x3F, std::memory_order_relaxed);
}

int GuitarTuner::getTunedMask() const
{
    return tunedMask.load(std::memory_order_relaxed) & 0x3F;
}

bool GuitarTuner::isStringTuned(int stringIndex) const
{
    return stringIndex >= 0 && stringIndex < 6
        && (getTunedMask() & (1 << stringIndex)) != 0;
}

bool GuitarTuner::areAllStringsTuned() const
{
    return getTunedMask() == 0x3F;
}
