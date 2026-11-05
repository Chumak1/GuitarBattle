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
//   2. how much of the attack happens inside its steepest 20 ms (a pluck does
//      almost all of it, a vowel only a fraction — this also works when a
//      previous chord or note is still ringing under the new pluck);
//   3. how far the attack rises above the quietest level just before it;
//   4. whether the sound decays in the first half-second (sustain check).
//
// A voice fails at least one of these. A plucked string (including a bass and
// a chord) passes them.
//
// A rejected attack latches the gate only for the sustain display: it does not
// block later onsets, because a single borderline rejection (a soft pluck, a
// quiet room, a slow strum) must never silence a whole playing session. The
// latch is cleared by the next accepted pluck, or by a short silence.
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

        // The rejected sound still counts as non-guitar for the sustain
        // display: do not keep showing it as a plucked note. A later pluck is
        // judged on its own attack and clears the latch.
        bool latched = false;

        // A rejection that is clearly vocal (slow ramp, no fast climb). An
        // ambiguous rejection — a level fluctuation on a ringing string —
        // must not replace the note that is still sounding.
        bool voiceLike = false;

        double attackMs = 0.0;
        // Share of the whole attack completed inside its steepest 20 ms.
        double climbFraction = 0.0;
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

        // Rise time anchored at the steepest 20 ms climb. The 20 ms
        // difference cancels the period ripple of a low string and of a
        // ringing chord, so this still measures the attack when the new pluck
        // lands on top of a loud tail. A pluck finishes the climb within a
        // few milliseconds; a vowel needs tens.
        {
            const std::uint64_t oldestAvailable = oldestBlock();
            const std::uint64_t windowStart =
                onsetBlock > 20 ? onsetBlock - 20 : oldestAvailable;
            const std::uint64_t slopeBlocks = 20;
            float bestSlope = -1.0e9f;
            std::uint64_t fastBlock = onsetBlock;

            for (std::uint64_t block = windowStart + slopeBlocks;
                 block <= peakEnd;
                 ++block)
            {
                float before = 0.0f;
                float after = 0.0f;

                if (! getBlock (block - slopeBlocks, before)
                    || ! getBlock (block, after))
                    continue;

                if (after - before > bestSlope)
                {
                    bestSlope = after - before;
                    fastBlock = block;
                }
            }

            // How much of the whole attack happens inside its steepest 20 ms.
            // A pluck does all of it there, a vowel only a fraction. The
            // 20 ms window also averages out the period ripple, so a loud
            // ringing tail under the new note does not fake a fast climb.
            float before = 0.0f;
            float after = 0.0f;

            if (fastBlock >= slopeBlocks
                && getBlock (fastBlock - slopeBlocks, before)
                && getBlock (fastBlock, after)
                && peak - baseline > 1.0e-9f)
            {
                result.climbFraction =
                    static_cast<double> (after - before)
                    / static_cast<double> (peak - baseline);
            }
        }

        const bool highBaseline = baseline > peak * 0.5f;

        // 10-90% rise time of the attack, measured across the frame
        // boundary. The measurement is anchored at the quietest level just
        // before the onset, so it works the same whether the string started
        // from silence or over a ringing tail.
        {
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
        }

        // A pluck reaches most of its attack peak within a few milliseconds;
        // a sung note ramps up over tens of milliseconds. The climb test
        // additionally accepts a pluck that lands on a loud ringing tail,
        // where the 10-90% rise spans the whole chord and looks slow.
        const bool bassNote = pitchHz > 0.0 && pitchHz < 80.0;
        const bool sharp = result.attackMs
                               <= (bassNote ? lowNoteAttackMs : sharpAttackMs)
                           || result.climbFraction >= minClimbFraction;

        // Over a ringing chord the new pluck may be only a few dB louder than
        // the tail. Requiring the full rise there would reject real playing.
        const bool rising = result.riseDb
            >= (highBaseline ? minRiseDbOverTail : minRiseDb);

        result.pluck = sharp && rising;
        result.decided = true;

        if (! result.pluck)
        {
            // Latch only a clearly voice-like attack: slow ramp that also
            // lacks a fast climb. A borderline rejection (a soft pluck, a
            // slow strum, a level fluctuation on a ringing string) must not
            // block the next real note.
            const bool clearlySlow = result.attackMs > latchAttackMs
                && result.climbFraction < minClimbFraction * 0.7;

            result.voiceLike = clearlySlow;
            result.latched = clearlySlow;
        }

        return result;
    }

    // Called once per analysis frame, after analyseOnset(). `acceptedPluck`
    // is true on the frame that owns a verified pluck attack; `voiceOnset`
    // is true when the rejected attack was clearly vocal. An ambiguous
    // rejection (a level fluctuation on a ringing string, a soft pluck that
    // did not clear the bar) must neither replace the note that is still
    // sounding nor block the next real note.
    void pushFrame (std::uint64_t frameSample,
                    double rms,
                    double rate,
                    bool acceptedPluck,
                    bool voiceOnset,
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

        if (voiceOnset)
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

        // The frame that owns the onset does not always contain the peak:
        // the attack can sit at the frame end, and an acoustic guitar's body
        // resonance keeps building the level for a few tens of milliseconds.
        // Follow the level for 200 ms and use that maximum as the reference.
        if (ageMs >= -20.0 && ageMs <= attackPeakWindowMs)
        {
            pluckPeakRms = std::max (pluckPeakRms, rms);
            pluckPeakEnvelope = std::max (pluckPeakEnvelope, frameMaxBlockEnv);
            return;
        }

        // After the attack window a sound that gets louder than its peak is
        // not the plucked string any more: a voice keeps growing, a string
        // does not. Clear the note (the next real pluck is still judged on
        // its own attack).
        if (ageMs <= 1500.0
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

    // True while the last rejected attack was clearly vocal. The tuner uses
    // this to stop showing a pitch that is not a guitar.
    bool isLatched() const noexcept { return latched; }

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

    // How far after the onset the attack peak is searched for. A new pluck
    // can sit up to one analysis frame after the detected onset when a
    // previous chord keeps the level above the frame threshold, so the window
    // covers a whole frame.
    static constexpr int peakWindowBlocks = 100;

    // How far before the detected onset the true start of the attack may
    // lie. The detector fires at 20% of the frame peak, which on a slow
    // attack can be tens of milliseconds after the start.
    static constexpr int lookbackBlocks = 60;

    // Fewer than this many milliseconds after the onset: the attack cannot
    // be measured in this frame, so the decision is deferred.
    static constexpr int minForwardBlocks = 6;

    // The level is followed for this long after the onset, so the reference
    // peak includes an attack that sits at a frame boundary and an acoustic
    // body resonance that builds up after the pluck.
    static constexpr double attackPeakWindowMs = 200.0;

    // A pluck reaches most of its attack peak within a few milliseconds; a
    // sung note ramps up over tens of milliseconds. The threshold sits well
    // above a pluck, even one recorded through a microphone, and well below a
    // normal vowel.
    static constexpr double sharpAttackMs = 28.0;

    // A bass string's first period is 24 ms at 41 Hz, so the measured rise
    // of its attack is longer even though the pluck itself is sharp.
    static constexpr double lowNoteAttackMs = 45.0;

    // Share of the attack that must happen inside its steepest 20 ms for the
    // attack to count as a pluck. A pluck does almost all of it there; a
    // vowel only a fraction, even when it rides on a ringing chord.
    static constexpr double minClimbFraction = 0.80;

    // A real attack rises at least this far above the quietest level just
    // before it. A vowel fluctuation stays within a few dB of the sustain
    // level.
    static constexpr double minRiseDb = 4.0;

    // Over a ringing chord a real pluck may be only a couple of dB louder
    // than the tail; the step test carries the decision there.
    static constexpr double minRiseDbOverTail = 2.0;

    // A voice has already been growing for 10+ ms before the attack; a
    // string either starts from silence or is decaying there. Reported for
    // diagnostics only.
    static constexpr double preFloor = 0.05;

    // An attack slower than this is clearly a voice and latches the gate.
    // Anything faster is a borderline rejection (soft pluck, slow strum) and
    // must not block the next real note.
    static constexpr double latchAttackMs = 40.0;

    // How long the input must stay below the silence threshold before a
    // rejected voice is forgotten.
    static constexpr double unlatchSilenceMs = 120.0;

    // A rejected voice is also forgotten when the level drops this far below
    // the rejected attack: a noisy room never reaches the absolute silence
    // threshold, but it is far below a voice.
    static constexpr double unlatchDropRatio = 0.20;

    // How long one pluck keeps the tuner alive. A ringing string is replaced
    // by a new pluck long before this on a real guitar.
    static constexpr double sustainWindowMs = 4000.0;

    // The level may dip and come back a little (beating, room) but a note
    // that gets 45% louder than its attack is a new, unplucked sound.
    static constexpr double maxSustainGain = 1.45;

    // A plucked string has decayed noticeably by this age; a voice holds its
    // level. Only the sustained-note path is affected, a new pluck is still
    // judged by its own attack. The window is long enough for an acoustic
    // guitar, whose body keeps the note ringing well past half a second.
    static constexpr double decayCheckMs = 500.0;
    static constexpr double maxFlatRatio = 0.97;

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
