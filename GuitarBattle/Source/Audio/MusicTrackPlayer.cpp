#include "MusicTrackPlayer.h"

#include <cmath>
#include <limits>

std::shared_ptr<MusicTrackPlayer::Track> MusicTrackPlayer::decodeFile(const juce::File& file)
{
    if (! file.existsAsFile())
        return nullptr;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels <= 0)
        return nullptr;

    auto track = std::make_shared<Track>();
    track->sampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;

    const int channels = juce::jlimit (1, 2, static_cast<int> (reader->numChannels));
    const auto totalSamples = static_cast<int> (
        juce::jmin<juce::int64> (reader->lengthInSamples,
                                 static_cast<juce::int64> (std::numeric_limits<int>::max())));

    if (totalSamples <= 0)
        return nullptr;

    track->audio = std::make_shared<juce::AudioBuffer<float>> (channels, totalSamples);
    track->audio->clear();

    // Read in blocks and keep everything decoded before the first failure.
    // MP3 duration is only an estimate, and a VBR stream with a truncated or
    // unusual tail can fail the single full-length read even though the vast
    // majority of the file is perfectly decodable.
    constexpr int blockSize = 4096;
    int decodedSamples = 0;

    while (decodedSamples < totalSamples)
    {
        const int count = juce::jmin(blockSize, totalSamples - decodedSamples);

        if (! reader->read (track->audio.get(), decodedSamples, count,
                            decodedSamples, true, true))
            break;

        decodedSamples += count;
    }

    if (decodedSamples <= 0)
        return nullptr;

    if (decodedSamples < totalSamples)
        track->audio->setSize (channels, decodedSamples, true, true, true);

    return track;
}

void MusicTrackPlayer::setTrack(std::shared_ptr<Track> newTrack)
{
    stop();
    std::atomic_store (&track, std::move (newTrack));
    position.store (0, std::memory_order_relaxed);
}

void MusicTrackPlayer::clearTrack()
{
    stop();
    std::atomic_store (&track, std::shared_ptr<Track> {});
    position.store (0, std::memory_order_relaxed);
}

bool MusicTrackPlayer::hasTrack() const
{
    return std::atomic_load (&track) != nullptr;
}

void MusicTrackPlayer::setOutputSampleRate(double rate)
{
    if (rate > 0.0)
        outputRate.store (rate, std::memory_order_relaxed);
}

void MusicTrackPlayer::setLooping(bool shouldLoop) noexcept
{
    looping.store (shouldLoop, std::memory_order_relaxed);
}

void MusicTrackPlayer::setGain(float newGain) noexcept
{
    gain.store (juce::jlimit (0.0f, 4.0f, newGain), std::memory_order_relaxed);
}

float MusicTrackPlayer::getGain() const noexcept
{
    return gain.load (std::memory_order_relaxed);
}

void MusicTrackPlayer::playFromStart()
{
    position.store (0, std::memory_order_relaxed);
    playing.store (hasTrack(), std::memory_order_relaxed);
}

void MusicTrackPlayer::stop()
{
    playing.store (false, std::memory_order_relaxed);
    position.store (0, std::memory_order_relaxed);
}

void MusicTrackPlayer::pause()
{
    playing.store (false, std::memory_order_relaxed);
}

void MusicTrackPlayer::resume()
{
    auto current = std::atomic_load (&track);

    if (current == nullptr || current->audio == nullptr
        || current->audio->getNumSamples() <= 1)
        return;

    // Reaching the end stops a non-looping track; a looping one wraps inside
    // render(). Either way, resuming from the final sample would be silent, so
    // start over in that case.
    if (position.load (std::memory_order_relaxed)
        >= current->audio->getNumSamples() - 1)
        position.store (0, std::memory_order_relaxed);

    playing.store (true, std::memory_order_relaxed);
}

bool MusicTrackPlayer::isPlaying() const noexcept
{
    return playing.load (std::memory_order_relaxed) && hasTrack();
}

bool MusicTrackPlayer::isAtEnd() const noexcept
{
    auto current = std::atomic_load (&track);

    if (current == nullptr || current->audio == nullptr)
        return true;

    return position.load (std::memory_order_relaxed)
        >= current->audio->getNumSamples() - 1;
}

void MusicTrackPlayer::render(juce::AudioBuffer<float>& destination,
                              int startSample,
                              int numSamples)
{
    auto current = std::atomic_load (&track);

    if (current == nullptr || current->audio == nullptr
        || current->audio->getNumSamples() <= 1
        || ! playing.load (std::memory_order_relaxed))
        return;

    const float level = gain.load (std::memory_order_relaxed);

    if (level <= 0.0001f)
        return;

    const double rate = outputRate.load (std::memory_order_relaxed);
    const double sourceRate = current->sampleRate > 0.0 ? current->sampleRate : 44100.0;
    const double step = sourceRate / (rate > 0.0 ? rate : sourceRate);

    const int sourceSamples = current->audio->getNumSamples();
    const int sourceChannels = current->audio->getNumChannels();
    const int outputChannels = destination.getNumChannels();
    const bool shouldLoop = looping.load (std::memory_order_relaxed);
    juce::int64 pos = position.load (std::memory_order_relaxed);

    for (int i = 0; i < numSamples; ++i)
    {
        if (pos >= sourceSamples - 1)
        {
            if (! shouldLoop)
            {
                playing.store (false, std::memory_order_relaxed);
                pos = sourceSamples;
                break;
            }

            pos = 0;
        }

        const auto index0 = static_cast<int> (pos);
        const int index1 = juce::jmin (index0 + 1, sourceSamples - 1);
        const float fraction = static_cast<float> (pos - index0);

        for (int channel = 0; channel < outputChannels; ++channel)
        {
            const int sourceChannel = juce::jmin (channel, sourceChannels - 1);
            const float a = current->audio->getSample (sourceChannel, index0);
            const float b = current->audio->getSample (sourceChannel, index1);

            destination.addSample (channel, startSample + i,
                                   (a + (b - a) * fraction) * level);
        }

        pos += step;
    }

    position.store (pos, std::memory_order_relaxed);
}
