#include "Metronome.h"

#include <algorithm>
#include <cmath>

//==============================================================================
// Tempo
//==============================================================================

void Metronome::setBpm(
    double newBpm)
{
    bpm.store(
        std::max(
            1.0,
            newBpm),
        std::memory_order_relaxed);
}

//==============================================================================

void Metronome::setSampleRate(
    double newSampleRate)
{
    sampleRate.store(
        std::max(
            0.0,
            newSampleRate),
        std::memory_order_relaxed);
}

//==============================================================================

double Metronome::getBpm() const
{
    return bpm.load(
        std::memory_order_relaxed);
}

//==============================================================================

double Metronome::getSampleRate() const
{
    return sampleRate.load(
        std::memory_order_relaxed);
}

//==============================================================================

uint64_t Metronome::getBeatLengthSamples() const
{
    const double currentBpm =
        bpm.load(
            std::memory_order_relaxed);

    const double currentSampleRate =
        sampleRate.load(
            std::memory_order_relaxed);

    if (currentBpm <= 0.0
        || currentSampleRate <= 0.0)
    {
        return 0;
    }

    return static_cast<uint64_t>(
        currentSampleRate
        * 60.0
        / currentBpm);
}

//==============================================================================
// Timeline
//==============================================================================

uint64_t Metronome::getBeatSample(
    uint64_t startSample,
    double beat) const
{
    if (beat < 0.0)
        return startSample;

    const auto beatLength =
        getBeatLengthSamples();

    if (beatLength == 0)
        return startSample;

    return startSample
        + static_cast<uint64_t>(
            std::llround(
                beat
                * static_cast<double>(
                    beatLength)));
}

//==============================================================================

double Metronome::getBeatFromSamples(
    uint64_t startSample,
    uint64_t currentSample) const
{
    const auto beatLength =
        getBeatLengthSamples();

    if (beatLength == 0
        || currentSample < startSample)
    {
        return 0.0;
    }

    return static_cast<double>(
        currentSample - startSample)
        / static_cast<double>(
            beatLength);
}

//==============================================================================
// Scheduler
//==============================================================================

void Metronome::resetScheduler(
    uint64_t startSample)
{
    beatCounter.store(
        0,
        std::memory_order_relaxed);

    beatInBar.store(
        0,
        std::memory_order_relaxed);

    nextBeatSample.store(
        startSample,
        std::memory_order_relaxed);

    metronomeTrigger.store(
        false,
        std::memory_order_relaxed);

    metronomeAccent.store(
        false,
        std::memory_order_relaxed);
}

//==============================================================================

void Metronome::pauseScheduler()
{
    metronomeTrigger.store(
        false,
        std::memory_order_relaxed);

    metronomeAccent.store(
        false,
        std::memory_order_relaxed);
}

//==============================================================================

void Metronome::resumeScheduler(
    uint64_t currentSample)
{
    const auto beatLength =
        getBeatLengthSamples();

    metronomeTrigger.store(
        false,
        std::memory_order_relaxed);

    metronomeAccent.store(
        false,
        std::memory_order_relaxed);

    if (beatLength == 0)
    {
        nextBeatSample.store(
            currentSample,
            std::memory_order_relaxed);

        return;
    }

    // Первый click после RESUME — через один полный beat.
    nextBeatSample.store(
        currentSample + beatLength,
        std::memory_order_relaxed);
}

//==============================================================================

void Metronome::processScheduler(
    uint64_t currentSample)
{
    const auto beatLength =
        getBeatLengthSamples();

    if (beatLength == 0)
        return;

    auto nextBeat =
        nextBeatSample.load(
            std::memory_order_relaxed);

    if (currentSample < nextBeat)
        return;

    do
    {
        nextBeat += beatLength;

    } while (currentSample >= nextBeat);

    nextBeatSample.store(
        nextBeat,
        std::memory_order_relaxed);

    const auto beat =
        beatCounter.fetch_add(
            1,
            std::memory_order_relaxed)
        + 1;

    const int currentBeatInBar =
        static_cast<int>(
            (beat - 1) % 4);

    beatInBar.store(
        currentBeatInBar,
        std::memory_order_relaxed);

    metronomeAccent.store(
        currentBeatInBar == 0,
        std::memory_order_relaxed);

    metronomeTrigger.store(
        true,
        std::memory_order_release);
}

//==============================================================================

bool Metronome::consumeTrigger()
{
    return metronomeTrigger.exchange(
        false,
        std::memory_order_acq_rel);
}

//==============================================================================

bool Metronome::isAccent() const
{
    return metronomeAccent.load(
        std::memory_order_relaxed);
}