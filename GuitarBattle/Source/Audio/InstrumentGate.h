#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Rejects input that does not look like a plucked guitar string.
//
// The game listens through a microphone or an audio interface, and the pitch
// detector cannot tell a guitar from a voice: a sung note at the chart's pitch
// is detected exactly like a fretted one. What differs is how the note starts:
//
//   * a pluck reaches most of its level within one or two periods (a few
//     milliseconds) and is loudest right at the attack;
//   * a sung note ramps up over tens of milliseconds, so by the time the
//     detector notices it the level has been growing for a while, and it
//     keeps growing afterwards.
//
// The gate therefore measures two things in the analysis frame that owns the
// attack: how quickly the level reached its peak after the detected onset,
// and whether the level was already climbing during the 20 ms before it. A
// voice fails at least one of the two; a plucked string (including a bass and
// a chord) passes both.
//
// The gate runs on the pitch-analysis thread (never in the audio callback).
// It is deliberately conservative: whenever the frame is too short to judge,
// the note is accepted, because a wrong rejection breaks real playing. Set
// GuitarInputOnly to false to bypass it entirely.
class InstrumentGate
{
public:
    struct OnsetAnalysis
    {
        bool pluck = false;
        double attackMs = 0.0;
        // Level trend during the 20 ms before the detected onset, in dB.
        // Large positive values mean the note was already growing (voice).
        double preRiseDb = -99.0;
    };

    // `onsetSample` is the first sample of the attack inside `samples`; the
    // thread already estimates it for timing and passes it here.
    OnsetAnalysis analyseOnset (const float* samples,
                                int numSamples,
                                double rate,
                                int onsetSample,
                                double onsetRms) noexcept
    {
        OnsetAnalysis result;

        if (samples == nullptr || numSamples <= 0 || rate <= 0.0
            || onsetRms <= 0.0)
            return result;

        const int start = juce::jlimit (0, numSamples - 1, onsetSample);
        int blockSize = juce::jmax (1, (int) std::lround (rate * 0.001));
        int blockCount = numSamples / blockSize;

        // Very low sample rates would need more blocks than the fixed array
        // holds; grow the block instead of the array.
        if (blockCount > maxEnvelopeBlocks)
        {
            blockSize = juce::jmax (1, numSamples / maxEnvelopeBlocks);
            blockCount = numSamples / blockSize;
        }

        if (blockCount < 8)
        {
            result.pluck = true;
            return result;
        }

        std::array<double, maxEnvelopeBlocks> envelope {};
        double framePeak = 0.0;

        for (int block = 0; block < blockCount; ++block)
        {
            const int first = block * blockSize;
            double sum = 0.0;

            for (int i = 0; i < blockSize; ++i)
            {
                const double value = samples[first + i];
                sum += value * value;
            }

            const double rms = std::sqrt (sum / static_cast<double> (blockSize));
            envelope[static_cast<size_t> (block)] = rms;
            framePeak = std::max (framePeak, rms);
        }

        if (framePeak <= 1.0e-9)
        {
            result.pluck = true;
            return result;
        }

        const double msPerBlock =
            static_cast<double> (blockSize) * 1000.0 / rate;
        const int onsetBlock = juce::jlimit (0, blockCount - 1,
                                             (int) std::lround (start / (double) blockSize));

        // The attack sits at the very end of the buffer: nothing after it to
        // measure. Accept it; the sustain check of the tuner path still
        // applies, and the next attack has a full frame again.
        if ((blockCount - onsetBlock) * msPerBlock < minForwardMs)
        {
            result.pluck = true;
            return result;
        }

        // Peak of the attack region (not the whole frame: a chord's beating or
        // a later re-pluck must not move the reference).
        const int peakEnd = juce::jmin (blockCount,
                                        onsetBlock + (int) std::lround (peakWindowMs / msPerBlock));
        double postPeak = 0.0;

        for (int block = onsetBlock; block < peakEnd; ++block)
            postPeak = std::max (postPeak, envelope[static_cast<size_t> (block)]);

        if (postPeak <= 1.0e-9)
        {
            result.pluck = true;
            return result;
        }

        int blockAtNinety = onsetBlock;

        for (int block = onsetBlock; block < peakEnd; ++block)
        {
            if (envelope[static_cast<size_t> (block)] >= postPeak * 0.9)
            {
                blockAtNinety = block;
                break;
            }
        }

        result.attackMs = static_cast<double> (blockAtNinety - onsetBlock)
                          * msPerBlock;

        // A loud previous note can mask the attack: the detector then locks
        // onto its tail and the "attack" would look slow. When the detected
        // onset is already a third of the attack peak, the measurement is not
        // trustworthy, so do not reject on it.
        if (envelope[static_cast<size_t> (onsetBlock)] >= postPeak * 0.32)
            result.attackMs = 0.0;

        // Level trend just before the detected onset. A voice has been
        // growing there; a plucked string has not (silence before the pluck,
        // or a decaying tail from the previous note).
        const int preShortBlocks = juce::jmax (2, (int) std::lround (10.0 / msPerBlock));
        const int preLongBlocks = juce::jmax (3, (int) std::lround (30.0 / msPerBlock));

        if (onsetBlock >= preLongBlocks)
        {
            double recent = 0.0;
            double earlier = 0.0;

            for (int block = onsetBlock - preShortBlocks; block < onsetBlock; ++block)
            {
                const double value = envelope[static_cast<size_t> (block)];
                recent += value * value;
            }

            for (int block = onsetBlock - preLongBlocks;
                 block < onsetBlock - preShortBlocks; ++block)
            {
                const double value = envelope[static_cast<size_t> (block)];
                earlier += value * value;
            }

            recent = std::sqrt (recent / static_cast<double> (preShortBlocks));
            earlier = std::sqrt (earlier / static_cast<double> (preLongBlocks - preShortBlocks));

            // Below the floor the "rise" is the noise floor, not an attack.
            if (recent > postPeak * preFloor && earlier > 1.0e-9)
                result.preRiseDb = 20.0 * std::log10 (recent / earlier);
        }

        const bool sharp = result.attackMs <= sharpAttackMs;
        const bool risingBefore = result.preRiseDb > maxPreRiseDb;

        result.pluck = sharp && ! risingBefore;

        return result;
    }

    // Called once per analysis frame. `acceptedPluck` is true on the frame
    // that owns a verified pluck attack; `onsetSample` is that attack's
    // position in the audio timeline.
    void pushFrame (std::uint64_t frameSample,
                    double rms,
                    double rate,
                    bool acceptedPluck,
                    std::uint64_t onsetSample) noexcept
    {
        if (rate > 0.0)
            this->sampleRate = rate;

        if (acceptedPluck)
        {
            hasPluck = true;
            this->pluckSample = onsetSample;
            pluckPeakRms = rms;
            return;
        }

        if (! hasPluck || this->sampleRate <= 0.0)
            return;

        const double ageMs = sampleAgeMs (frameSample);

        // The frame that owns the onset does not always contain the peak
        // (the attack can sit at its end), so follow the level for a moment.
        if (ageMs >= -20.0 && ageMs <= 500.0)
            pluckPeakRms = std::max (pluckPeakRms, rms);
    }

    // True while the current sound can still be the string that was plucked:
    // the pluck is recent and the level is not climbing. A sustained vowel
    // keeps growing after its onset and fails the second test.
    bool acceptsSustain (std::uint64_t frameSample, double rms) const noexcept
    {
        if (! hasPluck || sampleRate <= 0.0)
            return false;

        const double ageMs = sampleAgeMs (frameSample);

        if (ageMs < -20.0 || ageMs > sustainWindowMs)
            return false;

        return rms <= pluckPeakRms * maxSustainGain;
    }

    void reset() noexcept
    {
        hasPluck = false;
        pluckSample = 0;
        pluckPeakRms = 0.0;
    }

private:
    double sampleAgeMs (std::uint64_t frameSample) const noexcept
    {
        const double delta = static_cast<double> (frameSample)
                           - static_cast<double> (pluckSample);

        return delta * 1000.0 / sampleRate;
    }

    static constexpr int maxEnvelopeBlocks = 256;

    // Fewer than this many milliseconds after the onset: the attack cannot be
    // measured, so the note is accepted rather than guessed.
    static constexpr double minForwardMs = 12.0;

    // How far after the onset the attack peak is searched for.
    static constexpr double peakWindowMs = 40.0;

    // A plucked string (including a bass) reaches its peak within a few
    // milliseconds. A vowel needs tens of milliseconds.
    static constexpr double sharpAttackMs = 25.0;

    // A voice has already been growing for 10+ ms before the detector's
    // onset; a string either starts from silence or is decaying there.
    static constexpr double maxPreRiseDb = 2.0;
    static constexpr double preFloor = 0.05;

    // How long one pluck keeps the tuner alive. A ringing string is replaced
    // by a new pluck long before this on a real guitar.
    static constexpr double sustainWindowMs = 4000.0;

    // The level may dip and come back a little (beating, room) but a note
    // that gets 45% louder than its attack is a new, unplucked sound.
    static constexpr double maxSustainGain = 1.45;

    bool hasPluck = false;
    std::uint64_t pluckSample = 0;
    double pluckPeakRms = 0.0;
    double sampleRate = 0.0;
};
