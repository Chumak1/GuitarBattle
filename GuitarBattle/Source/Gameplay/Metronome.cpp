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
// Meter
//==============================================================================

namespace
{
// MIDI only allows power-of-two denominators; anything else is a caller bug
// and falls back to a quarter note instead of producing a broken grid.
int normaliseBeatUnit(int denominator)
{
    switch (denominator)
    {
        case 1:
        case 2:
        case 4:
        case 8:
        case 16:
        case 32:
            return denominator;

        default:
            return 4;
    }
}
}

//==============================================================================

void Metronome::setTimeSignature(
    int numerator,
    int denominator)
{
    beatsPerBar.store(
        std::clamp(
            numerator,
            1,
            32),
        std::memory_order_relaxed);

    beatUnit.store(
        normaliseBeatUnit(
            denominator),
        std::memory_order_relaxed);
}

//==============================================================================

int Metronome::getBeatsPerBar() const
{
    return beatsPerBar.load(
        std::memory_order_relaxed);
}

//==============================================================================

int Metronome::getBeatUnit() const
{
    return beatUnit.load(
        std::memory_order_relaxed);
}

//==============================================================================

double Metronome::getQuartersPerClick() const
{
    return 4.0
        / static_cast<double>(
            beatUnit.load(
                std::memory_order_relaxed));
}

//==============================================================================

double Metronome::getClickIntervalSamples() const
{
    const double quarterLength =
        getBeatLengthSamples();

    if (quarterLength <= 0.0)
        return 0.0;

    return quarterLength
        * getQuartersPerClick();
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
    uint64_t startSample,
    uint64_t initialBeatCounter)
{
    // ``initialBeatCounter`` is the click index of the first scheduled click:
    // 0 accents the downbeat, which is what the count-in needs.
    const auto barLength =
        static_cast<uint64_t>(
            beatsPerBar.load(
                std::memory_order_relaxed));

    beatCounter.store(
        initialBeatCounter,
        std::memory_order_relaxed);

    beatInBar.store(
        static_cast<int>(initialBeatCounter % barLength),
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
    const auto clickLength =
        getClickIntervalSamples();

    metronomeTrigger.store(
        false,
        std::memory_order_relaxed);

    metronomeAccent.store(
        false,
        std::memory_order_relaxed);

    if (clickLength <= 0.0)
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
    //
    // The song timeline is measured in quarter notes, while the grid counts
    // clicks of the current meter, so the elapsed quarters are converted to
    // click positions first: a 7/8 song clicks on every eighth note.
    const double elapsedQuarters =
        (static_cast<double>(currentSample)
         - static_cast<double>(songStartSample))
        / getBeatLengthSamples();

    const double clickPosition =
        elapsedQuarters
        / getQuartersPerClick();

    const double nextClickIndex =
        std::ceil(clickPosition + 1.0e-9);

    nextBeatSample.store(
        static_cast<double>(songStartSample)
            + nextClickIndex * clickLength,
        std::memory_order_relaxed);
}

//==============================================================================

void Metronome::resumeAtBeat(
    uint64_t currentSample,
    double currentBeat)
{
    const auto clickLength = getClickIntervalSamples();

    metronomeTrigger.store(false, std::memory_order_relaxed);
    metronomeAccent.store(false, std::memory_order_relaxed);

    if (clickLength <= 0.0)
    {
        nextBeatSample.store(static_cast<double>(currentSample),
                             std::memory_order_relaxed);
        return;
    }

    // ``currentBeat`` is a position on the quarter-note song timeline; the
    // scheduler counts clicks of the current meter, so convert it first.
    const double clickPosition =
        currentBeat / getQuartersPerClick();

    // Next whole click strictly after the current fractional position. The
    // epsilon keeps an exact integer click from being played twice.
    const double nextClickIndex =
        std::ceil(clickPosition + 1.0e-6);

    const auto clickIndex = nextClickIndex > 0.0
        ? static_cast<uint64_t>(std::llround(nextClickIndex))
        : 0;

    // beatCounter stores how many clicks have already been scheduled. A
    // trigger at click index n produces beatInBar = n % beatsPerBar, so the
    // accented first click of every bar survives tempo changes.
    const auto barLength =
        static_cast<uint64_t>(
            beatsPerBar.load(std::memory_order_relaxed));

    beatCounter.store(clickIndex, std::memory_order_relaxed);
    beatInBar.store(static_cast<int>(clickIndex % barLength),
                    std::memory_order_relaxed);

    nextBeatSample.store(
        static_cast<double>(currentSample)
            + (nextClickIndex - clickPosition) * clickLength,
        std::memory_order_relaxed);
}

//==============================================================================

int Metronome::processScheduler(
    uint64_t currentSample,
    int numSamples)
{
    const auto clickLength =
        getClickIntervalSamples();

    if (clickLength <= 0.0 || numSamples <= 0)
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

    // Advance to the first click after this block. Keeping the fractional
    // click length here prevents cumulative drift against the MIDI timeline.
    const double delta =
        blockEnd - nextBeat;

    const auto clicksToSkip =
        static_cast<uint64_t>(
            std::floor(delta / clickLength) + 1.0);

    nextBeatSample.store(
        nextBeat + static_cast<double>(clicksToSkip) * clickLength,
        std::memory_order_relaxed);

    const auto click =
        beatCounter.fetch_add(
            clicksToSkip,
            std::memory_order_relaxed)
        + 1;

    const auto barLength =
        static_cast<uint64_t>(
            beatsPerBar.load(std::memory_order_relaxed));

    const int currentBeatInBar =
        static_cast<int>(
            (click - 1) % barLength);

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