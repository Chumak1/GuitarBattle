
#include "PitchDetector.h"
#include <algorithm>
#include <array>

double PitchDetector::detectRms(
    const float* samples,
    int numSamples) const
{
    if (samples == nullptr || numSamples <= 0)
        return 0.0;

    double sum = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const double x =
            static_cast<double>(samples[i]);

        sum += x * x;
    }

    return std::sqrt(
        sum / static_cast<double>(numSamples));
}

double PitchDetector::detectPitch(
    const float* samples,
    int numSamples,
    double sampleRate) const
{
    if (samples == nullptr
        || numSamples <= 0
        || sampleRate <= 0.0)
    {
        return 0.0;
    }

    //==============================================================
    // Remove DC offset

    double mean = 0.0;

    for (int i = 0; i < numSamples; ++i)
        mean += static_cast<double>(samples[i]);

    mean /= static_cast<double>(numSamples);

    //==============================================================
    // Copy signal without DC

    std::vector<double> signal(
        static_cast<size_t>(numSamples));

    double rmsSum = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const double value =
            static_cast<double>(samples[i])
            - mean;

        signal[static_cast<size_t>(i)] =
            value;

        rmsSum += value * value;
    }

    const double rms =
        std::sqrt(
            rmsSum
            / static_cast<double>(numSamples));

    if (rms < 0.005)
        return 0.0;

    //==============================================================
    // Search range
    //
    // Guitar fundamental range:
    //
    // 60 Hz  -> approximately MIDI 35
    // 500 Hz -> approximately MIDI 71
    //
    // We keep the same range as before.

    // Include notes above C5 (MIDI 72 is about 523.25 Hz).
    // Keep this upper limit consistent with the final validation below.
    constexpr double maxFrequency = 700.0;
    constexpr double minFrequency = 60.0;

    const int minLag =
        static_cast<int>(
            sampleRate / maxFrequency);

    const int maxLag =
        static_cast<int>(
            sampleRate / minFrequency);

    if (minLag <= 0
        || maxLag <= minLag
        || maxLag >= numSamples)
    {
        return 0.0;
    }

    //==============================================================
    // Normalized autocorrelation
    //
    // Instead of asking:
    //
    // "Which period has the smallest difference?"
    //
    // ask:
    //
    // "Which period reproduces the waveform most strongly?"
    //
    // This is much more resistant to YIN choosing a subharmonic.

    std::vector<double> correlation(
        static_cast<size_t>(maxLag + 1),
        0.0);

    for (int lag = minLag;
        lag <= maxLag;
        ++lag)
    {
        double numerator = 0.0;
        double energyA = 0.0;
        double energyB = 0.0;

        const int count =
            numSamples - lag;

        for (int i = 0;
            i < count;
            ++i)
        {
            const double a =
                signal[static_cast<size_t>(i)];

            const double b =
                signal[
                    static_cast<size_t>(i + lag)];

            numerator += a * b;
            energyA += a * a;
            energyB += b * b;
        }

        if (energyA > 0.0
            && energyB > 0.0)
        {
            correlation[static_cast<size_t>(lag)] =
                numerator
                / std::sqrt(
                    energyA * energyB);
        }
    }

    //==============================================================
    // Find local correlation peaks
    //
    // A fundamental period should create a strong local maximum.
    //
    // We keep several candidates instead of immediately selecting
    // one global maximum.

    struct Candidate
    {
        int lag = 0;
        double correlation = 0.0;
    };

    std::vector<Candidate> candidates;

    for (int lag = minLag + 1;
        lag < maxLag;
        ++lag)
    {
        const double current =
            correlation[
                static_cast<size_t>(lag)];

        const double previous =
            correlation[
                static_cast<size_t>(lag - 1)];

        const double next =
            correlation[
                static_cast<size_t>(lag + 1)];

        if (current >= previous
            && current >= next
            && current > 0.20)
        {
            candidates.push_back(
                {
                    lag,
                    current
                });
        }
    }

    if (candidates.empty())
        return 0.0;

    //==============================================================
    // Sort strongest candidates first

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Candidate& a,
            const Candidate& b)
        {
            return a.correlation
    > b.correlation;
        });

    //==============================================================
    // Fundamental selection
    //
    // Harmonics often produce strong peaks at:
    //
    //     T
    //     2T
    //     3T
    //
    // If a longer period has a strong correlation but a shorter
    // period is also nearly as strong, prefer the shorter period.
    //
    // This prevents:
    //
    //     392 Hz -> 196 Hz
    //
    // because:
    //
    //     T(392)  ~= 112 samples
    //     T(196)  ~= 225 samples
    //
    // and the shorter period is preferred when it has sufficient
    // correlation support.

    Candidate best =
        candidates.front();

    constexpr double relativeThreshold = 0.88;

    for (const auto& candidate : candidates)
    {
        if (candidate.lag >= best.lag)
            continue;

        if (candidate.correlation
            >= best.correlation
            * relativeThreshold)
        {
            best = candidate;
        }
    }

    //==============================================================
    // Parabolic interpolation
    //
    // Refine the selected lag using the correlation values around
    // the peak. This improves pitch precision.

    double refinedLag =
        static_cast<double>(
            best.lag);

    if (best.lag > minLag
        && best.lag < maxLag)
    {
        const double left =
            correlation[
                static_cast<size_t>(
                    best.lag - 1)];

        const double center =
            correlation[
                static_cast<size_t>(
                    best.lag)];

        const double right =
            correlation[
                static_cast<size_t>(
                    best.lag + 1)];

        const double denominator =
            left
            - 2.0 * center
            + right;

        if (std::abs(denominator) > 1.0e-12)
        {
            const double offset =
                0.5
                * (left - right)
                / denominator;

            if (std::abs(offset) <= 1.0)
            {
                refinedLag += offset;
            }
        }
    }

    if (refinedLag <= 0.0)
        return 0.0;

    //==============================================================
    // Frequency

    const double frequency =
        sampleRate / refinedLag;

    if (frequency < minFrequency
        || frequency > maxFrequency)
    {
        return 0.0;
    }

    return frequency;
}

int PitchDetector::frequencyToMidi(
    double frequency) const
{
    if (frequency <= 0.0)
        return -1;

    const double midi =
        69.0
        + 12.0
        * std::log2(
            frequency / 440.0);

    return static_cast<int>(
        std::round(midi));
}

juce::String PitchDetector::frequencyToNote(
    double frequency) const
{
    if (frequency <= 0.0)
        return "---";

    static constexpr const char* noteNames[] =
    {
        "C",
        "C#",
        "D",
        "D#",
        "E",
        "F",
        "F#",
        "G",
        "G#",
        "A",
        "A#",
        "B"
    };

    const int midiNote =
        frequencyToMidi(frequency);

    if (midiNote < 0)
        return "---";

    const int noteIndex =
        midiNote % 12;

    const int octave =
        midiNote / 12 - 1;

    return juce::String(
        noteNames[noteIndex])
        + juce::String(octave);
}


