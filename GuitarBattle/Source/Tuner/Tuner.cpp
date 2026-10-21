#include "Tuner.h"

#include <cmath>

namespace
{
    constexpr double stringFrequencies[6] =
    {
        82.4069,   // 6th string - E2
        110.0000,  // 5th string - A2
        146.8324,  // 4th string - D3
        195.9977,  // 3rd string - G3
        246.9417,  // 2nd string - B3
        329.6276   // 1st string - E4
    };
}

int GuitarTuner::detectString(double frequency) const
{
    if (frequency <= 0.0)
        return -1;

    int bestIndex = -1;
    double bestCents = 1.0e9;

    for (int i = 0; i < 6; ++i)
    {
        const double cents =
            1200.0 * std::log2(frequency / stringFrequencies[i]);

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
    if (stringIndex < 0 || stringIndex >= 6)
        return 0.0;

    return stringFrequencies[stringIndex];
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
