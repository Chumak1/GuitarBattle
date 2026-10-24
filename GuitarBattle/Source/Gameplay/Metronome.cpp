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

double Metronome::getBeatLengthSamples() const
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
        return 0.0;
    }

    return currentSampleRate
        * 60.0
        / currentBpm;
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

    if (beatLength <= 0.0)
        return startSample;

    return startSample
        + static_cast<uint64_t>(
            std::llround(
                beat
                * beatLength));
}

//==============================================================================

double Metronome::getBeatFromSamples(
    uint64_t startSample,
    uint64_t currentSample) const
{
    const auto beatLength =
        getBeatLengthSamples();

    if (beatLength <= 0.0
        || currentSample < startSample)
    {
        return 0.0;
    }

    return static_cast<double>(
        currentSample - startSample)
        / beatLength;
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
        static_cast<double>(startSample),
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
    uint64_t currentSample,
    uint64_t songStartSample)
{
    const auto beatLength =
        getBeatLengthSamples();

    metronomeTrigger.store(
        false,
        std::memory_order_relaxed);

    metronomeAccent.store(
        false,
        std::memory_order_relaxed);

    if (beatLength <= 0.0)
    {
        nextBeatSample.store(
            static_cast<double>(currentSample),
            std::memory_order_relaxed);

        return;
    }

    // Resume on the song's beat grid. The old implementation always waited
    // exactly one beat from the resume sample, which shifted the metronome
    // relative to the MIDI/audio timeline when the pause happened between
    // beats. ceil() gives the next grid beat strictly after currentSample.
    const double elapsed =
        static_cast<double>(currentSample)
        - static_cast<double>(songStartSample);

    const double nextBeatIndex =
        std::ceil(elapsed / beatLength + 1.0e-9);

    nextBeatSample.store(
        static_cast<double>(songStartSample)
            + nextBeatIndex * beatLength,
        std::memory_order_relaxed);
}

//==============================================================================

int Metronome::processScheduler(
    uint64_t currentSample,
    int numSamples)
{
    const auto beatLength =
        getBeatLengthSamples();

    if (beatLength <= 0.0 || numSamples <= 0)
        return -1;

    const double blockStart =
        static_cast<double>(currentSample);

    const double blockEnd =
        blockStart + static_cast<double>(numSamples);

    const double nextBeat =
        nextBeatSample.load(
            std::memory_order_relaxed);

    // A beat exactly on the block end belongs to the next block. This avoids
    // playing the same click twice at a block boundary.
    if (nextBeat >= blockEnd)
        return -1;

    int triggerOffset = 0;

    if (nextBeat > blockStart)
        triggerOffset = static_cast<int>(
            std::llround(nextBeat - blockStart));

    triggerOffset = std::max(
        0,
        std::min(triggerOffset, numSamples - 1));

    // Advance to the first beat after this block. Keeping the fractional
    // beat length here prevents cumulative drift against the MIDI timeline.
    const double delta =
        blockEnd - nextBeat;

    const auto beatsToSkip =
        static_cast<uint64_t>(
            std::floor(delta / beatLength) + 1.0);

    nextBeatSample.store(
        nextBeat + static_cast<double>(beatsToSkip) * beatLength,
        std::memory_order_relaxed);

    const auto beat =
        beatCounter.fetch_add(
            beatsToSkip,
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

    return triggerOffset;
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