#pragma once

#include <atomic>
#include <cstdint>

class Metronome
{
public:
    //======================================================================
    // Tempo

    void setBpm(double newBpm);
    void setSampleRate(double newSampleRate);

    double getBpm() const;
    double getSampleRate() const;

    // Fractional beat length. Keeping the fractional part prevents the
    // metronome from drifting against the MIDI timeline over long songs.
    double getBeatLengthSamples() const;

    //======================================================================
    // Timeline

    uint64_t getBeatSample(
        uint64_t startSample,
        double beat) const;

    double getBeatFromSamples(
        uint64_t startSample,
        uint64_t currentSample) const;

    //======================================================================
    // Scheduler

    void resetScheduler(
        uint64_t startSample);

    void pauseScheduler();

    void resumeScheduler(
        uint64_t currentSample,
        uint64_t songStartSample);

    // Returns the sample offset of the first beat inside this block, or -1
    // when no beat starts in the block. This lets the caller place the click
    // at the exact beat sample instead of quantising it to the block start.
    int processScheduler(
        uint64_t currentSample,
        int numSamples);

    bool consumeTrigger();

    bool isAccent() const;

private:
    //======================================================================
    // Tempo

    std::atomic<double> bpm{ 120.0 };
    std::atomic<double> sampleRate{ 0.0 };

    //======================================================================
    // Scheduler

    std::atomic<double> nextBeatSample{ 0.0 };
    std::atomic<uint64_t> beatCounter{ 0 };

    std::atomic<int> beatInBar{ 0 };

    std::atomic<bool> metronomeTrigger{ false };
    std::atomic<bool> metronomeAccent{ false };
};