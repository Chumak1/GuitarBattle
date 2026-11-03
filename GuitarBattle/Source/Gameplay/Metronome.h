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

    // Fractional length of one quarter note. Keeping the fractional part
    // prevents the metronome from drifting against the MIDI timeline over
    // long songs. The public timeline (song beats, MIDI note positions) is
    // always measured in quarter notes.
    double getBeatLengthSamples() const;

    //======================================================================
    // Meter

    // A song may be written in 3/4, 7/8 or any other meter. The accent falls
    // on the first click of every bar, and the click grid follows the meter's
    // beat unit: a 7/8 bar produces seven eighth-note clicks, not four
    // quarter-note ones. Both values are read from the audio thread, so they
    // are stored in atomics.
    void setTimeSignature(int numerator, int denominator);

    int getBeatsPerBar() const;
    int getBeatUnit() const;

    // Quarter notes per click: 1 for /4 meters, 0.5 for /8 meters.
    double getQuartersPerClick() const;

    // Interval between two clicks of the current meter. This is what the
    // count-in and the scheduler use; getBeatLengthSamples() stays quarter
    // based because the MIDI timeline is.
    double getClickIntervalSamples() const;

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
        uint64_t startSample,
        uint64_t initialBeatCounter = 0);

    void pauseScheduler();

    void resumeScheduler(
        uint64_t currentSample,
        uint64_t songStartSample);

    // Resumes on the exact fractional song beat. Used after tempo changes,
    // where the old song-start reference no longer maps to beat zero.
    // The bar phase is restored from the integer beat index, so the accent
    // keeps landing on the first beat of the bar.
    void resumeAtBeat(
        uint64_t currentSample,
        double currentBeat);

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

    // Meter. Defaults keep the historical 4/4 behaviour for songs that do not
    // declare a time signature.
    std::atomic<int> beatsPerBar{ 4 };
    std::atomic<int> beatUnit{ 4 };

    std::atomic<int> beatInBar{ 0 };

    std::atomic<bool> metronomeTrigger{ false };
    std::atomic<bool> metronomeAccent{ false };
};