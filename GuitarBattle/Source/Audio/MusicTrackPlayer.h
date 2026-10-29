#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <memory>

// Single-purpose player for a fully decoded MP3 (preview or background music).
// The audio callback only reads the atomics and the shared buffer; loading and
// decoding always happen on a worker thread via decodeFile().
class MusicTrackPlayer
{
public:
    MusicTrackPlayer() = default;

    struct Track
    {
        std::shared_ptr<juce::AudioBuffer<float>> audio;
        double sampleRate = 44100.0;
    };

    // Reads and decodes a complete audio file. Safe to call from a worker
    // thread; returns nullptr for missing or undecodable files.
    static std::shared_ptr<Track> decodeFile(const juce::File& file);

    void setTrack(std::shared_ptr<Track> newTrack);
    void clearTrack();
    bool hasTrack() const;

    void setOutputSampleRate(double rate);
    void setLooping(bool shouldLoop) noexcept;
    void setGain(float newGain) noexcept;
    float getGain() const noexcept;

    void playFromStart();
    void stop();
    // Pauses without losing the playback position, so the same track can
    // continue after a higher-priority source (a preview or gameplay) ends.
    void pause();
    // Continues from the stored position; starts from the beginning when the
    // track was never played or has already reached its end.
    void resume();
    bool isPlaying() const noexcept;
    bool isAtEnd() const noexcept;

    // Adds the track into the destination buffer. Realtime-safe: no locks, no
    // allocation, no file access.
    void render(juce::AudioBuffer<float>& destination, int startSample, int numSamples);

private:
    std::shared_ptr<Track> track;
    std::atomic<bool> playing { false };
    std::atomic<bool> looping { false };
    std::atomic<float> gain { 1.0f };
    std::atomic<double> outputRate { 44100.0 };
    std::atomic<juce::int64> position { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MusicTrackPlayer)
};
