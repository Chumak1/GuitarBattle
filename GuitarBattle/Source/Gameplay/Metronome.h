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

    uint64_t getBeatLengthSamples() const;

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
        uint64_t currentSample);

    void processScheduler(
        uint64_t currentSample);

    bool consumeTrigger();

    bool isAccent() const;

private:
    //======================================================================
    // Tempo

    std::atomic<double> bpm{ 120.0 };
    std::atomic<double> sampleRate{ 0.0 };

    //======================================================================
    // Scheduler

    std::atomic<uint64_t> nextBeatSample{ 0 };
    std::atomic<uint64_t> beatCounter{ 0 };

    std::atomic<int> beatInBar{ 0 };

    std::atomic<bool> metronomeTrigger{ false };
    std::atomic<bool> metronomeAccent{ false };
};