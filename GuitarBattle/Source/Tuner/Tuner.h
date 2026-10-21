#pragma once

#include <atomic>

// Tuner domain logic: standard tuning targets, string recognition, cents error,
// and persistent per-string tuning progress. UI presentation stays in MainComponent.
class GuitarTuner
{
public:
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
    std::atomic<int> tunedMask{ 0 };
};
