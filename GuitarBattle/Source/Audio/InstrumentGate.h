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
// is detected exactly like a fretted one. What differs is how the note starts
// and how it behaves afterwards:
//
//   * a pluck jumps from silence (or from a decaying tail) to most of its
//     level within a few milliseconds, then decays;
//   * a sung note ramps up over tens of milliseconds, holds its level and
//     often keeps growing after the detector notices it.
//
// The gate therefore keeps a continuous 1 ms envelope history across analysis
// frames, finds the real start of the attack (which may lie in the previous
// frame), and measures:
//
//   1. the 10-90% rise time of the attack;
//   2. how far the attack rises above the quietest level just before it;
//   3. whether the level was already climbing before the attack;
//   4. whether the sound decays in the first half-second (sustain check).
//
// A voice fails at least one of these. A plucked string (including a bass and
// a chord) passes them. A rejected attack latches the gate: no note is
// accepted until the input has been silent for a moment, so a sustained vowel
// cannot sneak in through a later level fluctuation.
//
// The gate runs on the pitch-analysis thread (never in the audio callback).
// It is deliberately conservative about real playing: when the analysis frame
// is too short to judge an attack that sits at its very end, the decision is
// deferred to the next frame instead of accepting the note. Set
// GuitarInputOnly to false to bypass it entirely.
class InstrumentGate
{
public:
    struct OnsetAnalysis
    {
        // A verified pluck attack: score/tune this note.
        bool pluck = false;

        // The frame did not contain enough data after the onset (the attack
        // sits at the very end of the analysis buffer) or the envelope
        // history is too short. Do not score it and do not latch: the next
        // frame re-detects the same attack with the full envelope available.
        bool decided = false;

        // The input is considered non-guitar until it has been silent for
        // unlatchSilenceMs.
        bool latched = false;

        double attackMs = 0.0;
        double riseDb = 0.0;
        // Level trend during the 20 ms before the attack, in dB. Large
        // positive values mean the note was already growing (voice).
        double preRiseDb = -99.0;
    };

    // Call once per analysis frame, before analyseOnset(). Appends the frame
    // to the 1 ms envelope history, follows the silence/latch state and
    // remembers the frame's loudest block for the sustain checks.
    void processFrame (const float* samples,
                       int numSamples,
                       double rate,
                       std::uint64_t frameStartSample,
                       double frameRms,
                       double silenceThreshold) noexcept
    {
        if (samples == nullptr || numSamples <= 0 || rate <= 0.0)
            return;

        const int newBlockSize =
            juce::jmax (1, (int) std::lround (rate * 0.001));

        // A gap means the analysis thread missed a buffer; the old envelope
        // no longer touches the new frame, so do not use it as context.
        if (newBlockSize != blockSamples
            || ! hasLastFrameEnd
            || frameStartSample != lastFrameEnd)
        {
            resetHistory (newBlockSize);
        }

        sampleRate = rate;
        blockSamples = newBlockSize;

        // Peak envelope with a fast attack and a slow release. A plain 1 ms
        // RMS block is phase-sensitive on low strings: a 41 Hz period is
        // 24 ms, so a block can sit on a zero crossing and the measured
        // "attack" stretches over several periods. The follower tracks the
        // waveform peak, so a bass attack is still a few milliseconds.
        const double releaseCoeff =
            std::exp (-1.0 / (envelopeReleaseMs * rate / 1000.0));

        double maxBlockEnv = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            const std::uint64_t absolute =
                frameStartSample + static_cast<std::uint64_t> (i);
            const std::uint64_t block =
                absolute / static_cast<std::uint64_t> (blockSamples);

            if (hasPartial && block != partialBlock)
            {
                storeBlock (partialBlock, (float) partialMax);
                maxBlockEnv = std::max (maxBlockEnv, partialMax);
                partialMax = 0.0;
                hasPartial = false;
            }

            if (! hasPartial)
            {
                partialBlock = block;
                partialMax = 0.0;
                hasPartial = true;
            }

            const double value =
                std::abs (static_cast<double> (samples[i]));

            envelopeState = std::max (value, envelopeState * releaseCoeff);
            partialMax = std::max (partialMax, envelopeState);
        }

        const std::uint64_t endSample =
            frameStartSample + static_cast<std::uint64_t> (numSamples);

        if (hasPartial
            && endSample % static_cast<std::uint64_t> (blockSamples) == 0)
        {
            storeBlock (partialBlock, (float) partialMax);
            maxBlockEnv = std::max (maxBlockEnv, partialMax);
            partialMax = 0.0;
            hasPartial = false;
        }

        lastFrameEnd = endSample;
        hasLastFrameEnd = true;
        frameMaxBlockEnv = maxBlockEnv;

        // Silence clears both the voice latch and the current pluck. A real
        // pluck is replaced by a new attack long before this, and a voice
        // must stop before the next note can be accepted. The absolute
        // threshold alone is not enough: a room with a fan or a noisy input
        // can stay above it forever, so a large drop from the rejected
        // attack also counts as silence.
        const bool silent =
            frameRms < silenceThreshold
            || (rejectedPeakEnvelope > 0.0
                && frameMaxBlockEnv < rejectedPeakEnvelope * unlatchDropRatio);

        if (silent)
        {
            silentMs += static_cast<double> (numSamples) * 1000.0 / rate;

            if (silentMs >= unlatchSilenceMs)
            {
                latched = false;
                hasPluck = false;
                sustainRejected = false;
                rejectedPeakEnvelope = 0.0;
            }
        }
        else
        {
            silentMs = 0.0;
        }

        // A plucked string decays after the attack; a voice holds its level.
        // If the sound is still as loud as the attack half a second later,
        // do not keep showing it as the plucked note. This is a sustain
        // check only: a new real pluck is still judged by its own attack.
        if (hasPluck && ! decayChecked && sampleRate > 0.0)
        {
            const double ageMs = sampleAgeMs (lastFrameEnd);

            if (ageMs >= decayCheckMs)
            {
                decayChecked = true;

                if (frameMaxBlockEnv >= pluckPeakEnvelope * maxFlatRatio)
                    sustainRejected = true;
            }
        }
    }

    // Decides whether the attack that starts at `onsetSample` (an absolute
    // position in the audio timeline) is a pluck. Must be called after
    // processFrame() for the frame that owns the onset. `pitchHz` is the
    // detected fundamental, used only to relax the attack-time limit for
    // bass strings, whose first period is longer than a guitar one.
    OnsetAnalysis analyseOnset (std::uint64_t onsetSample,
                                double pitchHz = 0.0) noexcept
    {
        OnsetAnalysis result;

        if (latched)
        {
            result.decided = true;
            result.latched = true;
            return result;
        }

        if (! hasLatestBlock || blockSamples <= 0 || sampleRate <= 0.0)
        {
            result.decided = false;
            return result;
        }

        const std::uint64_t onsetBlock =
            onsetSample / static_cast<std::uint64_t> (blockSamples);

        if (onsetBlock > latestBlock)
        {
            result.decided = false;
            return result;
        }

        // Not enough time after the onset to see the attack peak. The next
        // frame will contain the rest of it; re-detect it there.
        if (latestBlock - onsetBlock
            < static_cast<std::uint64_t> (minForwardBlocks))
        {
            result.decided = false;
            return result;
        }

        // The true start of the attack can lie before the detected onset.
        // Without a full pre-onset window the measurement is not reliable;
        // defer to the next frame instead of guessing.
        const std::uint64_t oldest = oldestBlock();

        if (onsetBlock < oldest
            || onsetBlock - oldest
                   < static_cast<std::uint64_t> (lookbackBlocks))
        {
            result.decided = false;
            return result;
        }

        const std::uint64_t peakEnd = juce::jmin (
            latestBlock,
            onsetBlock + static_cast<std::uint64_t> (peakWindowBlocks));

        float peak = 0.0f;
        std::uint64_t peakBlock = onsetBlock;
        bool havePeak = false;

        for (std::uint64_t block = onsetBlock; block <= peakEnd; ++block)
        {
            float value = 0.0f;

            if (getBlock (block, value) && value > peak)
            {
                peak = value;
                peakBlock = block;
                havePeak = true;
            }
        }

        if (! havePeak || peak <= 1.0e-9f)
        {
            result.decided = false;
            return result;
        }

        // The detector fires when the level first crosses a fraction of the
        // frame peak, so the real start of the attack can be up to
        // lookbackBlocks before it. The quietest block in that window is the
        // baseline the attack started from.
        float baseline = peak;
        std::uint64_t baselineBlock = onsetBlock;

        for (std::uint64_t block = onsetBlock - static_cast<std::uint64_t> (lookbackBlocks);
             block <= onsetBlock;
             ++block)
        {
            float value = 0.0f;

            if (getBlock (block, value) && value < baseline)
            {
                baseline = value;
                baselineBlock = block;
            }
        }

        result.riseDb = 20.0 * std::log10 (
            static_cast<double> (peak)
            / juce::jmax (static_cast<double> (baseline), 1.0e-9));

        // Level trend just before the attack. A voice has usually been
        // growing there; a string starts from silence or from a decaying
        // tail.
        const int preShortBlocks = juce::jmax (
            2, (int) std::lround (10.0 * sampleRate / (1000.0 * blockSamples)));
        const int preLongBlocks = juce::jmax (
            3, (int) std::lround (30.0 * sampleRate / (1000.0 * blockSamples)));

        if (baselineBlock >= static_cast<std::uint64_t> (preLongBlocks))
        {
            double recent = 0.0;
            double earlier = 0.0;
            int recentCount = 0;
            int earlierCount = 0;

            for (int i = preShortBlocks; i > 0; --i)
            {
                float value = 0.0f;

                if (getBlock (baselineBlock - static_cast<std::uint64_t> (i),
                              value))
                {
                    recent += static_cast<double> (value) * value;
                    ++recentCount;
                }
            }

            for (int i = preLongBlocks; i > preShortBlocks; --i)
            {
                float value = 0.0f;

                if (getBlock (baselineBlock - static_cast<std::uint64_t> (i),
                              value))
                {
                    earlier += static_cast<double> (value) * value;
                    ++earlierCount;
                }
            }

            if (recentCount > 0 && earlierCount > 0)
            {
                recent = std::sqrt (recent / static_cast<double> (recentCount));
                earlier = std::sqrt (earlier / static_cast<double> (earlierCount));

                if (recent > static_cast<double> (peak) * preFloor
                    && earlier > 1.0e-9)
                {
                    result.preRiseDb = 20.0 * std::log10 (recent / earlier);
                }
            }
        }

        const bool quietBefore = result.preRiseDb <= maxPreRiseDb;

        // A steady sound has no attack at all: the level at the detected
        // onset is already close to the peak. This is exactly what a vowel
        // fluctuation looks like. A pluck, even over a decaying tail, always
        // rises well above the level just before it.
        //
        // A fluctuation on a decaying string has the same shape, so it is
        // rejected without latching: otherwise a real note played after it
        // would be blocked until silence. Latch only when the level was
        // already growing, which a voice does and a decaying string does not.
        if (baseline > peak * 0.5f)
        {
            result.decided = true;
            result.pluck = false;
            result.latched = ! quietBefore;
            return result;
        }

        // 10-90% rise time of the attack, measured across the frame boundary.
        const float low = baseline + (peak - baseline) * 0.1f;
        const float high = baseline + (peak - baseline) * 0.9f;
        std::uint64_t block10 = peakBlock;
        std::uint64_t block90 = peakBlock;

        for (std::uint64_t block = baselineBlock; block <= peakBlock; ++block)
        {
            float value = 0.0f;

            if (! getBlock (block, value))
                continue;

            if (block10 == peakBlock && value >= low)
                block10 = block;

            if (value >= high)
            {
                block90 = block;
                break;
            }
        }

        result.attackMs = static_cast<double> (block90 - block10)
                          * static_cast<double> (blockSamples)
                          * 1000.0 / sampleRate;

        const bool sharp = result.attackMs
            <= ((pitchHz > 0.0 && pitchHz < 80.0) ? lowNoteAttackMs
                                                  : sharpAttackMs);
        const bool rising = result.riseDb >= minRiseDb;

        result.pluck = sharp && rising && quietBefore;
        result.decided = true;

        if (! result.pluck)
        {
            // A slow attack with a real rise is a voice; a small level
            // fluctuation on a decaying string is not. Latch only the voice
            // (and a level that was already climbing), so a quiet re-pluck
            // after a fluctuation is still judged by its own attack.
            result.latched = ! sharp || ! quietBefore;
        }

        return result;
    }

    // Called once per analysis frame, after analyseOnset(). `acceptedPluck`
    // is true on the frame that owns a verified pluck attack; `rejectedOnset`
    // is true when the gate decided to latch on a voice-like attack. A level
    // fluctuation that was rejected without latching (see analyseOnset) must
    // not block the next real note.
    void pushFrame (std::uint64_t frameSample,
                    double rms,
                    double rate,
                    bool acceptedPluck,
                    bool rejectedOnset,
                    std::uint64_t onsetSample) noexcept
    {
        if (rate > 0.0)
            sampleRate = rate;

        if (acceptedPluck)
        {
            hasPluck = true;
            latched = false;
            sustainRejected = false;
            decayChecked = false;
            rejectedPeakEnvelope = 0.0;
            this->pluckSample = onsetSample;
            pluckPeakRms = rms;
            pluckPeakEnvelope = frameMaxBlockEnv;
            return;
        }

        if (rejectedOnset)
        {
            hasPluck = false;
            sustainRejected = false;
            latched = true;
            rejectedPeakEnvelope = frameMaxBlockEnv;
            return;
        }

        if (! hasPluck || sampleRate <= 0.0)
            return;

        const double ageMs = sampleAgeMs (frameSample);

        // The frame that owns the onset does not always contain the peak
        // (the attack can sit at its end), so follow the level for a moment.
        if (ageMs >= -20.0 && ageMs <= 500.0)
            pluckPeakRms = std::max (pluckPeakRms, rms);

        // A sound that gets louder than its attack is not the plucked string
        // any more: a voice keeps growing, a string does not. Latch until
        // silence instead of accepting the next fluctuation.
        if (ageMs >= -20.0 && ageMs <= 1000.0
            && frameMaxBlockEnv > pluckPeakEnvelope * maxSustainGain)
        {
            hasPluck = false;
            latched = true;
            rejectedPeakEnvelope = frameMaxBlockEnv;
        }
    }

    // True while the current sound can still be the string that was plucked:
    // the pluck is recent, the level is not climbing, and the sound decays.
    // A sustained vowel fails at least one of these.
    bool acceptsSustain (std::uint64_t frameSample, double rms) const noexcept
    {
        if (latched || sustainRejected || ! hasPluck || sampleRate <= 0.0)
            return false;

        const double ageMs = sampleAgeMs (frameSample);

        if (ageMs < -20.0 || ageMs > sustainWindowMs)
            return false;

        return rms <= pluckPeakRms * maxSustainGain;
    }

    void reset() noexcept
    {
        hasPluck = false;
        latched = false;
        sustainRejected = false;
        decayChecked = false;
        pluckSample = 0;
        pluckPeakRms = 0.0;
        pluckPeakEnvelope = 0.0;
        rejectedPeakEnvelope = 0.0;
        silentMs = 0.0;
        resetHistory (blockSamples);
    }

private:
    double sampleAgeMs (std::uint64_t frameSample) const noexcept
    {
        const double delta = static_cast<double> (frameSample)
                           - static_cast<double> (pluckSample);

        return delta * 1000.0 / sampleRate;
    }

    void storeBlock (std::uint64_t index, float envelope) noexcept
    {
        blockEnv[static_cast<size_t> (
            index % static_cast<std::uint64_t> (maxHistoryBlocks))] = envelope;

        latestBlock = index;
        hasLatestBlock = true;
    }

    bool getBlock (std::uint64_t index, float& value) const noexcept
    {
        if (! hasLatestBlock || index > latestBlock)
            return false;

        if (latestBlock - index
            >= static_cast<std::uint64_t> (maxHistoryBlocks))
        {
            return false;
        }

        value = blockEnv[static_cast<size_t> (
            index % static_cast<std::uint64_t> (maxHistoryBlocks))];

        return true;
    }

    std::uint64_t oldestBlock() const noexcept
    {
        if (! hasLatestBlock)
            return 0;

        const std::uint64_t available = juce::jmin<std::uint64_t> (
            latestBlock + 1, static_cast<std::uint64_t> (maxHistoryBlocks));

        return latestBlock + 1 - available;
    }

    void resetHistory (int newBlockSize) noexcept
    {
        blockSamples = juce::jmax (1, newBlockSize);
        blockEnv.fill (0.0f);
        latestBlock = 0;
        hasLatestBlock = false;
        partialBlock = 0;
        partialMax = 0.0;
        hasPartial = false;
        envelopeState = 0.0;
        lastFrameEnd = 0;
        hasLastFrameEnd = false;
        frameMaxBlockEnv = 0.0;
    }

    // Peak follower release. Long enough to bridge the period of a low
    // string, short enough to keep the attack edge.
    static constexpr double envelopeReleaseMs = 40.0;

    // 1 ms envelope, kept across analysis frames. 512 blocks cover half a
    // second, enough for the attack lookback and the sustain checks.
    static constexpr int maxHistoryBlocks = 512;

    // How far after the onset the attack peak is searched for.
    static constexpr int peakWindowBlocks = 40;

    // How far before the detected onset the true start of the attack may
    // lie. The detector fires at 20% of the frame peak, which on a slow
    // attack can be tens of milliseconds after the start.
    static constexpr int lookbackBlocks = 60;

    // Fewer than this many milliseconds after the onset: the attack cannot
    // be measured in this frame, so the decision is deferred.
    static constexpr int minForwardBlocks = 6;

    // A plucked string reaches 90% of its attack peak within a few
    // milliseconds. A vowel needs tens of milliseconds. The threshold is
    // deliberately above a bass attack and below a sung one.
    static constexpr double sharpAttackMs = 15.0;

    // A bass string's first period is 24 ms at 41 Hz, so the measured rise
    // of its attack is longer even though the pluck itself is sharp.
    static constexpr double lowNoteAttackMs = 25.0;

    // A real attack rises at least 6 dB above the quietest level just before
    // it. A vowel fluctuation stays within a few dB of the sustain level.
    static constexpr double minRiseDb = 6.0;

    // A voice has already been growing for 10+ ms before the attack; a
    // string either starts from silence or is decaying there.
    static constexpr double maxPreRiseDb = 2.0;
    static constexpr double preFloor = 0.05;

    // How long the input must stay below the silence threshold before a
    // rejected voice is forgotten.
    static constexpr double unlatchSilenceMs = 150.0;

    // A rejected voice is also forgotten when the level drops this far below
    // the rejected attack: a noisy room never reaches the absolute silence
    // threshold, but it is far below a voice.
    static constexpr double unlatchDropRatio = 0.10;

    // How long one pluck keeps the tuner alive. A ringing string is replaced
    // by a new pluck long before this on a real guitar.
    static constexpr double sustainWindowMs = 4000.0;

    // The level may dip and come back a little (beating, room) but a note
    // that gets 45% louder than its attack is a new, unplucked sound.
    static constexpr double maxSustainGain = 1.45;

    // A plucked string has decayed noticeably by this age; a voice holds its
    // level. Only the sustained-note path is affected, a new pluck is still
    // judged by its own attack.
    static constexpr double decayCheckMs = 400.0;
    static constexpr double maxFlatRatio = 0.90;

    std::array<float, maxHistoryBlocks> blockEnv {};

    std::uint64_t latestBlock = 0;
    bool hasLatestBlock = false;

    std::uint64_t partialBlock = 0;
    double partialMax = 0.0;
    bool hasPartial = false;

    std::uint64_t lastFrameEnd = 0;
    bool hasLastFrameEnd = false;

    double frameMaxBlockEnv = 0.0;
    double envelopeState = 0.0;

    int blockSamples = 1;
    double sampleRate = 0.0;

    bool hasPluck = false;
    bool latched = false;
    bool sustainRejected = false;
    bool decayChecked = false;
    std::uint64_t pluckSample = 0;
    double pluckPeakRms = 0.0;
    double pluckPeakEnvelope = 0.0;
    double rejectedPeakEnvelope = 0.0;
    double silentMs = 0.0;
};
