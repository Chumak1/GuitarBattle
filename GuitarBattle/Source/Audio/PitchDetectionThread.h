#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include "../MainComponent.h"
#include "InstrumentGate.h"

// Dedicated audio-analysis and gameplay worker, extracted from MainComponent.
class PitchDetectionThread : public juce::Thread
{
public:
    explicit PitchDetectionThread(MainComponent& ownerToUse)
        : juce::Thread("Pitch Detection Thread"), owner(ownerToUse)
    {
    }

    void run() override
    {
            while (!threadShouldExit())
            {
                //==============================================================
                // First analysis buffer
                //
                // This buffer is responsible for:
                // - pitch detection
                // - onset detection
                // - tuner
                // - game scoring
                //

                if (owner.pitchBufferReady.exchange(
                    false,
                    std::memory_order_relaxed))
                {
                    auto* device =
                        owner.deviceManager
                        .getCurrentAudioDevice();

                    if (device == nullptr)
                        continue;

                    const double sampleRate =
                        device->getCurrentSampleRate();

                    if (sampleRate <= 0.0)
                        continue;

                    // Frequency window for the analysis: during gameplay it
                    // follows the current target, so high frets above 700 Hz
                    // are detected without octave errors on low strings.
                    const auto pitchRange =
                        owner.getPitchAnalysisRange();

                    const double pitch =
                        owner.pitchDetector.detectPitch(
                            owner.pitchAnalysisBuffer
                            .getReadPointer(0),
                            owner.pitchAnalysisBuffer.getNumSamples(),
                            sampleRate,
                            pitchRange.getStart(),
                            pitchRange.getEnd());

                    const double rms =
                        owner.pitchDetector.detectRms(
                            owner.pitchAnalysisBuffer
                            .getReadPointer(0),
                            owner.pitchAnalysisBuffer.getNumSamples());

                    const auto analysisSample =
                        owner.pitchAnalysisStartSample.load(
                            std::memory_order_relaxed);

                    // Expose what the detector sees, for the diagnostics
                    // readout in the audio settings.
                    owner.analysisRms.store(
                        static_cast<float>(rms),
                        std::memory_order_relaxed);
                    owner.analysisPitch.store(
                        static_cast<float>(pitch),
                        std::memory_order_relaxed);

                    const auto* blockSamples =
                        owner.pitchAnalysisBuffer
                        .getReadPointer(0);

                    //==========================================================
                    // Estimate actual onset inside analysis block

                    int onsetOffset = 0;

                    double maxSample = 0.0;

                    const int numAnalysisSamples =
                        owner.pitchAnalysisBuffer.getNumSamples();

                    for (int i = 0;
                        i < numAnalysisSamples;
                        ++i)
                    {
                        maxSample =
                            std::max(
                                maxSample,
                                std::abs(
                                    static_cast<double>(
                                        blockSamples[i])));
                    }

                    const double threshold =
                        maxSample * 0.2;

                    if (threshold > 0.0)
                    {
                        const int thresholdSampleCount =
                            owner.pitchAnalysisBuffer.getNumSamples();

                        for (int i = 0;
                            i < thresholdSampleCount;
                            ++i)
                        {
                            if (std::abs(
                                static_cast<double>(
                                    blockSamples[i]))
                                >= threshold)
                            {
                                onsetOffset = i;
                                break;
                            }
                        }
                    }

                    const auto detectedOnsetSample =
                        analysisSample
                        + static_cast<uint64_t>(
                            onsetOffset);



                    //==========================================================
                    // Silence
                    //
                    // A new onset is allowed only after the signal has returned
                    // to silence.

                    //==============================================================
// Onset detection
//
// Use hysteresis instead of a single RMS threshold.
//
// This prevents small level fluctuations inside a sustained
// guitar note from being interpreted as a new note.



                    // Detector sensitivity scales the thresholds around the
                    // historical defaults (0.5 = neutral, factor 1.0):
                    // 100% halves them, 0% doubles them. Only the detector is
                    // affected; the input meter always shows the real level.
                    const double sensitivity = juce::jlimit(
                        0.0,
                        1.0,
                        static_cast<double>(
                            owner.inputSensitivity.load(std::memory_order_relaxed)));

                    const double thresholdScale =
                        std::pow(2.0, (0.5 - sensitivity) * 2.0);

                    const double silenceThreshold = 0.0020 * thresholdScale;
                    const double onsetThreshold = 0.0030 * thresholdScale;
                    const double noteRmsGate = 0.0030 * std::sqrt(thresholdScale);

                    const uint64_t minimumOnsetSamples =
                        static_cast<uint64_t>(
                            sampleRate * 0.060
                            * juce::jlimit(0.5, 2.0, thresholdScale));

                    // Feed the guitar-only gate with every frame, not only
                    // with onsets: the attack may start in the previous
                    // frame, and a sustained vowel must be recognised as a
                    // continuous sound. Runs on this thread only.
                    instrumentGate.processFrame(
                        blockSamples,
                        numAnalysisSamples,
                        sampleRate,
                        analysisSample,
                        rms,
                        silenceThreshold);

                    // Сравниваем громкость с предыдущим анализируемым блоком.
                    const double previousRms = owner.previousRmsValue;
                    owner.previousRmsValue = rms;

                    const bool isSilent = rms < silenceThreshold;

                    if (isSilent)
                    {
                        owner.noteOn.store(
                            false,
                            std::memory_order_relaxed);

                        juce::MessageManager::callAsync(
                            [this]
                            {
                                owner.pitchLabel.setText(
                                    "---",
                                    juce::dontSendNotification);
                            });
                    }

                    const bool aboveOnsetThreshold =
                        rms >= onsetThreshold;

                    // Новая атака: громкость заметно выросла относительно
                    // предыдущего блока. Не ждём полного затухания струны.
                    // Абсолютный прирост мал, чтобы тихий микрофон тоже
                    // работал; ложные срабатывания отсекает гитарный фильтр.
                    const bool risingAttack =
                        rms >= onsetThreshold
                        && rms > previousRms * 1.20
                        && rms - previousRms > 0.0008;

                    const auto previousOnsetSample =
                        owner.lastOnsetSample.load(
                            std::memory_order_relaxed);

                    const bool enoughTimeSincePreviousOnset =
                        previousOnsetSample == 0
                        || detectedOnsetSample >
                        previousOnsetSample + minimumOnsetSamples;

                    const bool onset =
                        aboveOnsetThreshold
                        && risingAttack
                        && enoughTimeSincePreviousOnset;

                    //==========================================================
                    // Guitar-only input filter
                    //
                    // A voice at the chart's pitch is detected exactly like a
                    // guitar note. The gate measures the attack across the
                    // whole envelope history (the attack may start in the
                    // previous frame) and latches on a voice-like attack. It
                    // runs on this thread only; the audio callback never
                    // touches it.
                    const bool guitarOnly =
                        owner.guitarInputOnly.load(std::memory_order_relaxed);

                    InstrumentGate::OnsetAnalysis onsetAnalysis;

                    if (guitarOnly && onset)
                    {
                        onsetAnalysis =
                            instrumentGate.analyseOnset(detectedOnsetSample,
                                                        pitch);

                        // Publish the decision for the input monitor so a
                        // player can report which test failed.
                        owner.gateDecision.store(
                            onsetAnalysis.decided
                                ? (onsetAnalysis.pluck ? 1 : 2)
                                : 3,
                            std::memory_order_relaxed);
                        owner.gateAttackMs.store(
                            static_cast<float>(onsetAnalysis.attackMs),
                            std::memory_order_relaxed);
                        owner.gateClimbFraction.store(
                            static_cast<float>(onsetAnalysis.climbFraction),
                            std::memory_order_relaxed);
                        owner.gateRiseDb.store(
                            static_cast<float>(onsetAnalysis.riseDb),
                            std::memory_order_relaxed);
                    }

                    // A voice-like attack must not score, move the tuner or
                    // advance practice. An attack that sits at the very end of
                    // the frame is undecided: the next frame re-detects it
                    // with the full envelope and must not be blocked by the
                    // refractory window, so it is not accepted now.
                    const bool pluckAccepted =
                        ! guitarOnly || ! onset
                        || (onsetAnalysis.decided && onsetAnalysis.pluck);

                    const auto frameEndSample =
                        analysisSample
                        + static_cast<uint64_t>(numAnalysisSamples);

                    instrumentGate.pushFrame(
                        frameEndSample,
                        rms,
                        sampleRate,
                        onset && pluckAccepted,
                        onset && onsetAnalysis.voiceLike,
                        detectedOnsetSample);

                    const bool sustainAccepted =
                        ! guitarOnly
                        || instrumentGate.acceptsSustain(frameEndSample, rms);

                    if (! owner.tunerActive.load(std::memory_order_relaxed))
                        tunerRejectedShown = false;

                    if (onset)
                    {
                        owner.noteOn.store(
                            pluckAccepted,
                            std::memory_order_relaxed);

                        // Only an accepted pluck starts the refractory
                        // window. A deferred attack must be re-detected on the
                        // next frame; a rejected voice is blocked by the gate
                        // latch instead of the 60 ms onset spacing.
                        if (pluckAccepted)
                        {
                            owner.lastOnsetSample.store(
                                detectedOnsetSample,
                                std::memory_order_relaxed);

                            // Practice timing must only follow a real pluck:
                            // a rejected vocal attack must not schedule the
                            // hold.
                            owner.lastNoteOnSample.store(
                                analysisSample,
                                std::memory_order_relaxed);
                        }
                    }


                    //==========================================================
                    // Practice mode: publish the live pitch on every analysis
                    // window, not only on an onset. While the transport is held
                    // the player usually keeps the target note ringing, so the
                    // hold must be able to release without a fresh attack.
                    if (owner.practiceActive.load(std::memory_order_relaxed)
                        && pitch > MainComponent::minDetectedPitchHz
                        && pitch <= MainComponent::maxDetectedPitchHz
                        && rms >= noteRmsGate
                        && sustainAccepted)
                    {
                        owner.practiceLastPitch.store(
                            pitch, std::memory_order_relaxed);

                        owner.practicePitchSample.store(
                            analysisSample
                                + static_cast<uint64_t>(numAnalysisSamples),
                            std::memory_order_relaxed);
                    }

                    //==========================================================
                    // Valid detected guitar note



                    const bool tunerActive =
                        owner.tunerActive.load(std::memory_order_relaxed);

                    // An attack is judged by how it starts; a sustained frame
                    // (the tuner) by the recent pluck and the level trend.
                    const bool inputAccepted =
                        onset ? pluckAccepted : sustainAccepted;

                    // The tuner is a live instrument: once a pluck has been
                    // accepted, follow the ringing string and update the
                    // reading on every frame. Only a clearly vocal attack
                    // stops it, so turning the peg changes the value instead
                    // of freezing the last one.
                    const bool tunerHoldAccepted =
                        ! guitarOnly || ! instrumentGate.isLatched();

                    const bool noteLevelValid =
                        pitch > MainComponent::minDetectedPitchHz
                        && pitch <= MainComponent::maxDetectedPitchHz
                        && rms >= noteRmsGate;

                    // Which open string the pitch belongs to. Far from every
                    // string the tuner has nothing to show, so the reading is
                    // treated as stale instead of freezing the old value.
                    const int tunerStringIndex =
                        tunerActive ? owner.tuner.detectString(pitch) : -1;

                    //==========================================================
                    // Note stability
                    //
                    // A plucked string holds its pitch; speech jumps between
                    // notes every syllable. A reading counts only once the same
                    // note has been seen on minStableFrames frames in a row.

                    if (! noteLevelValid)
                    {
                        stableFrames = 0;
                    }
                    else
                    {
                        const bool sameNote = stableFrames > 0
                            && std::abs(1200.0 * std::log2(
                                   static_cast<double>(pitch) / stablePitch)) < 60.0;

                        stableFrames = sameNote ? stableFrames + 1 : 1;
                        stablePitch = pitch;
                    }

                    const bool noteStable = stableFrames >= minStableFrames;

                    //==========================================================
                    // Attack confirmation
                    //
                    // A plucked string keeps its pitch after the attack; a
                    // spoken syllable jumps to the next note right away. The
                    // rest of the same analysis buffer is checked as soon as
                    // the attack is found, so a real note is scored on its own
                    // frame with its original timing instead of waiting for the
                    // next one. Only an attack too close to the buffer end is
                    // confirmed on the next frame.

                    const auto sameDetectedNote = [this](double first,
                                                         double second)
                    {
                        if (first <= 0.0 || second <= 0.0)
                            return false;

                        const double cents = std::abs(1200.0 * std::log2(
                            first / second));

                        if (cents < 150.0)
                            return true;

                        // An octave slip in the detector must not cancel a real
                        // note: the game compares exact MIDI notes anyway.
                        const int firstMidi =
                            owner.pitchDetector.frequencyToMidi(first);
                        const int secondMidi =
                            owner.pitchDetector.frequencyToMidi(second);

                        return firstMidi >= 0 && secondMidi >= 0
                            && std::abs(firstMidi - secondMidi) % 12 == 0;
                    };

                    const int confirmDelaySamples =
                        (int) std::lround(sampleRate * 0.010);
                    const int confirmWindowSamples =
                        (int) std::lround(sampleRate * 0.060);
                    const int confirmMinSamples =
                        (int) std::lround(sampleRate * 0.045);

                    bool scoreReady = false;
                    double scorePitch = pitch;
                    std::uint64_t scoreOnset = detectedOnsetSample;

                    // Finish a pending note first, so a fast next attack does
                    // not cancel it.
                    if (pendingScore)
                    {
                        if (sameDetectedNote(pitch, pendingScorePitch))
                        {
                            scoreReady = true;
                            scorePitch = pendingScorePitch;
                            scoreOnset = pendingScoreOnset;
                        }

                        pendingScore = false;
                    }

                    if (onset && pluckAccepted && noteLevelValid && ! tunerActive)
                    {
                        const int confirmStart =
                            onsetOffset + confirmDelaySamples;
                        const int confirmAvailable =
                            numAnalysisSamples - confirmStart;

                        if (confirmAvailable >= confirmMinSamples)
                        {
                            const int confirmLength = juce::jmin(
                                confirmWindowSamples, confirmAvailable);

                            const double confirmPitch =
                                owner.pitchDetector.detectPitch(
                                    blockSamples + confirmStart,
                                    confirmLength,
                                    sampleRate,
                                    pitchRange.getStart(),
                                    pitchRange.getEnd());

                            scoreReady = sameDetectedNote(pitch, confirmPitch);
                        }
                        else
                        {
                            pendingScore = true;
                            pendingScoreOnset = detectedOnsetSample;
                            pendingScorePitch = pitch;
                        }
                    }

                    const bool tunerUsable =
                        tunerActive && noteLevelValid && tunerStringIndex >= 0
                        && tunerHoldAccepted && noteStable;

                    const bool displayAccepted =
                        inputAccepted || tunerUsable || scoreReady;

                    // A tuner without a usable reading must not keep the last
                    // value on screen: after ~190 ms clear it and ask for a
                    // pluck. This covers a decayed string, a missing pitch and
                    // a rejected attack alike.
                    if (tunerActive)
                    {
                        pendingScore = false;

                        if (tunerUsable)
                        {
                            tunerStaleFrames = 0;
                        }
                        else if (++tunerStaleFrames >= 2 && ! tunerRejectedShown)
                        {
                            tunerRejectedShown = true;

                            juce::MessageManager::callAsync([this]
                            {
                                owner.tunerDetectedString = -1;
                                owner.tunerFrequency = 0.0;
                                owner.tunerCents = 0.0;
                                owner.tunerNoteLabel.setText("---", juce::dontSendNotification);
                                owner.tunerFrequencyLabel.setText("--- Hz", juce::dontSendNotification);
                                owner.tunerStringLabel.setText("---", juce::dontSendNotification);
                                owner.tunerCentsLabel.setText("--- cents", juce::dontSendNotification);
                                owner.tunerStatusLabel.setText("PLUCK A STRING", juce::dontSendNotification);
                                owner.tunerStatusLabel.setColour(juce::Label::textColourId, UiColors::blue);
                                owner.repaint();
                                owner.overlayComponent.repaint();
                            });
                        }
                    }

                    if (noteLevelValid
                        && (onset || tunerUsable || scoreReady)
                        && displayAccepted)
                    {
                        owner.lastPitch = pitch;
                        tunerRejectedShown = false;

                        //======================================================
                        // Tuner mode

                        if (tunerActive)
                        {
                            const int stringIndex = tunerStringIndex;

                            if (stringIndex >= 0)
                            {
                                const double targetFrequency =
                                    owner.tuner.getTargetFrequency(
                                        stringIndex);

                                if (targetFrequency > 0.0)
                                {
                                    const double cents =
                                        owner.tuner.getCentsDeviation(pitch, stringIndex);

                                    const bool inTune =
                                        owner.tuner.isInTune(pitch, stringIndex, 10.0);

                                    if (inTune)
                                        owner.tuner.markStringTuned(stringIndex);

                                    const bool allStringsTuned =
                                        owner.tuner.areAllStringsTuned();

                                    juce::String stringText = "---";
                                    const int stringCount = owner.tuner.getStringCount();

                                    if (stringIndex >= 0 && stringIndex < stringCount)
                                    {
                                        const int stringNumber = stringCount - stringIndex;
                                        const juce::String ordinal =
                                            stringNumber == 1 ? "1st"
                                            : stringNumber == 2 ? "2nd"
                                            : stringNumber == 3 ? "3rd"
                                            : juce::String(stringNumber) + "th";

                                        stringText = ordinal + " string - "
                                            + GuitarTuning::noteName(
                                                owner.tuner.getOpenMidiNote(stringIndex),
                                                true);
                                    }

                                    const auto note =
                                        owner.pitchDetector.frequencyToNote(
                                            pitch);

                                    const auto frequencyText =
                                        juce::String(
                                            pitch,
                                            2)
                                        + " Hz";

                                    const auto centsText =
                                        (cents >= 0.0
                                            ? "+"
                                            : "")
                                        + juce::String(
                                            cents,
                                            1)
                                        + " cents";

                                    juce::String statusText;

                                    if (inTune)
                                        statusText = "IN TUNE";
                                    else if (cents < 0.0)
                                        statusText = "TOO LOW";
                                    else
                                        statusText = "TOO HIGH";

                                    juce::MessageManager::callAsync(
                                        [this,
                                        stringIndex,
                                        pitch,
                                        note,
                                        frequencyText,
                                        stringText,
                                        centsText,
                                        statusText,
                                        allStringsTuned]
                                        {
                                            owner.tunerDetectedString =
                                                stringIndex;

                                            owner.tunerFrequency =
                                                pitch;

                                            owner.tunerCents =
                                                1200.0 * std::log2(
                                                    pitch / owner.tuner.getTargetFrequency(stringIndex));

                                            owner.tunerNoteLabel.setText(
                                                note,
                                                juce::dontSendNotification);

                                            owner.tunerFrequencyLabel.setText(
                                                frequencyText,
                                                juce::dontSendNotification);

                                            owner.tunerStringLabel.setText(
                                                stringText,
                                                juce::dontSendNotification);

                                            owner.tunerCentsLabel.setText(
                                                centsText,
                                                juce::dontSendNotification);

                                            owner.tunerStatusLabel.setText(
                                                statusText,
                                                juce::dontSendNotification);

                                            owner.tunerStartButton.setEnabled(
                                                allStringsTuned);

                                            // The string labels are painted in MainComponent::paint.
                                            // Request a repaint so newly tuned strings turn green immediately.
                                            owner.repaint();
                                            owner.overlayComponent.repaint();
                                        });
                                }
                            }
                        }
                        else
                        {
                            //==================================================
                            // Game mode
                            //
                            // No scoring during tuner / pause / menu /
                            // finished.

                            const auto currentState =
                                owner.gameState.load(
                                    std::memory_order_relaxed);

                            if (currentState
                                != MainComponent::GameState::Playing
                                || owner.countInActive.load(std::memory_order_acquire))
                            {
                                juce::MessageManager::callAsync(
                                    [this, pitch]
                                    {
                                        const auto detectedNote =
                                            owner.pitchDetector.frequencyToNote(
                                                pitch);

                                        const auto text =
                                            detectedNote
                                            + "  |  "
                                            + juce::String(
                                                pitch,
                                                2)
                                            + " Hz";

                                        owner.pitchLabel.setText(
                                            text,
                                            juce::dontSendNotification);
                                    });

                                // Do NOT score.
                                continue;
                            }

                            //==================================================================
                            // Practice mode
                            //
                            // The message thread confirms the target note, freezes
                            // and resumes the transport. The competitive scoring
                            // path below must never run: it would consume targets
                            // and submit a result.
                            if (owner.practiceActive.load(std::memory_order_relaxed))
                            {
                                owner.practiceLastPitch.store(pitch, std::memory_order_relaxed);
                                continue;
                            }

                            // Only a note that proved it lasts is scored.
                            if (! scoreReady)
                                continue;

                            const auto detectedNote =
                                owner.pitchDetector.frequencyToNote(
                                    scorePitch);

                            const auto text =
                                detectedNote
                                + "  |  "
                                + juce::String(
                                    scorePitch,
                                    2)
                                + " Hz";

                            //==================================================
                            //==========================================================
// Current target

                            int currentTargetIndex =
                                owner.gameSession.getTargetNoteIndex();

                            // Easy charts and slower playback widen the timing
                            // window (see ScoringSystem::getTimingWindowScale).
                            // The miss and stale-onset guards below must use the
                            // same value as the scoring path: a note inside the
                            // widened window must reach evaluateNote() instead
                            // of being skipped as late.
                            const double timingWindowScale =
                                ScoringSystem::getTimingWindowScale(
                                    owner.songLevel.load(std::memory_order_relaxed),
                                    owner.speedPercent.load(std::memory_order_relaxed));

                            // Catch up targets whose late-hit window has already
                            // passed before evaluating this detected attack. The
                            // same attack can then be tested against the next
                            // target, but is scored at most once.
                            const auto lateWindowSamples =
                                static_cast<uint64_t>(sampleRate * 0.150 * timingWindowScale);

                            while (currentTargetIndex >= 0
                                && currentTargetIndex < static_cast<int>(
                                    owner.gameSession.getMidiNotes().size()))
                            {
                                const auto targetSample = owner.getTargetSample(
                                    currentTargetIndex, sampleRate);

                                if (scoreOnset <= targetSample + lateWindowSamples)
                                    break;



                                if (!owner.gameSession.resolveTarget(currentTargetIndex))
                                    break;

                                owner.gameplayStats.recordMiss();
                                owner.gameSession.setTargetHit(true);
                                juce::MessageManager::callAsync(
                                    [this]
                                    {
                                        owner.showHitFeedback("MISS", 4);
                                    });
                                owner.advanceTarget();

                                currentTargetIndex =
                                    owner.gameSession.getTargetNoteIndex();
                            }



                            if (currentTargetIndex >= 0
                                && currentTargetIndex
                                < static_cast<int>(
                                    owner.gameSession.getMidiNotes().size()))
                            {
                                // Ignore an onset that belongs to a time before
                                // the current target's early-hit window. Analysis
                                // buffers can be processed late; such an onset
                                // must not be reused to score a later target.
                                const auto targetSampleForGuard =
                                    owner.getTargetSample(
                                        currentTargetIndex,
                                        sampleRate);

                                const auto earlyWindowSamples =
                                    static_cast<uint64_t>(
                                        sampleRate * 0.150 * timingWindowScale);

                                const bool staleOnset =
                                    scoreOnset < targetSampleForGuard
                                    && targetSampleForGuard - scoreOnset
                                    > earlyWindowSamples;

                                if (staleOnset)
                                {

                                }
                                else
                                {
                                    // Group MIDI notes that start at the same beat.
                                    // If the current target is a chord, analyze the full
                                    // audio buffer and score each chord tone independently.
                                    const auto& midiNotes = owner.gameSession.getMidiNotes();
                                    const double chordStartBeat = midiNotes[static_cast<size_t>(currentTargetIndex)].startBeat;
                                    int chordCount = 1;
                                    while (currentTargetIndex + chordCount < static_cast<int>(midiNotes.size())
                                        && std::abs(midiNotes[static_cast<size_t>(currentTargetIndex + chordCount)].startBeat - chordStartBeat) < 0.001)
                                    {
                                        ++chordCount;
                                    }

                                    if (chordCount > 1)
                                    {
                                        const auto chordDetectedNotes = owner.pitchDetector.detectMultipleMidiNotes(
                                            owner.pitchAnalysisBuffer.getReadPointer(0),
                                            owner.pitchAnalysisBuffer.getNumSamples(),
                                            sampleRate,
                                            6);

                                        const auto chordTargetSample = owner.getTargetSample(currentTargetIndex, sampleRate);
                                        const double chordTimingErrorMs =
                                            (static_cast<double>(scoreOnset) - static_cast<double>(chordTargetSample))
                                            * 1000.0 / sampleRate;
                                        const bool chordInsideTimingWindow =
                                            std::abs(chordTimingErrorMs) < 150.0 * timingWindowScale;

                                        for (int chordOffset = 0; chordOffset < chordCount; ++chordOffset)
                                        {
                                            const int targetIndex = currentTargetIndex + chordOffset;
                                            const int targetMidi = midiNotes[static_cast<size_t>(targetIndex)].midiNote;
                                            const bool noteWasDetected = std::find(chordDetectedNotes.begin(), chordDetectedNotes.end(), targetMidi)
                                                != chordDetectedNotes.end();
                                            if (!owner.gameSession.resolveTarget(targetIndex))
                                                continue;

                                            if (noteWasDetected && chordInsideTimingWindow)
                                            {
                                                const double targetFrequency = 440.0 * std::pow(2.0, (targetMidi - 69.0) / 12.0);
                                                const auto evaluation = GameplayEngine::evaluateNote(
                                                    targetMidi, targetMidi, targetFrequency, chordTimingErrorMs,
                                                    owner.difficulty, owner.speedPercent,
                                                    owner.songLevel.load(std::memory_order_relaxed));
                                                const int newScore = owner.gameplayStats.recordHit(
                                                    evaluation.scoringResult.score, evaluation.scoringResult.note.quality);
                                                owner.gameplayStats.recordTiming(chordTimingErrorMs);
                                                owner.gameSession.setLastHitSample(scoreOnset);
                                                juce::MessageManager::callAsync([this, newScore]
                                                {
                                                    owner.gameSessionLabel.setText(juce::String(newScore), juce::dontSendNotification);
                                                    owner.showHitFeedback("CHORD NOTE HIT", 2);
                                                });

                                            }
                                            else
                                            {
                                                owner.gameplayStats.recordMiss();
                                                juce::MessageManager::callAsync([this]
                                                {
                                                    owner.showHitFeedback("PARTIAL CHORD", 4);
                                                });

                                            }

                                            owner.gameSession.setTargetHit(true);
                                            owner.advanceTarget();
                                        }
                                    }
                                    else
                                    {
                                    const auto& target =
                                        owner.gameSession.getMidiNotes()[
                                            currentTargetIndex];

                                    const int targetMidi =
                                        target.midiNote;

                                    const int detectedMidi =
                                        owner.pitchDetector.frequencyToMidi(
                                            scorePitch);

                                    // IMPORTANT:
                                    // Target timing is based on the original
                                    // MIDI startBeat and gameplayStartSample.
                                    const auto targetStartSample =
                                        owner.getTargetSample(
                                            currentTargetIndex,
                                            sampleRate);

                                    // IMPORTANT:
                                    // Use the estimated actual onset.
                                    const auto hitSample =
                                        scoreOnset;

                                    const auto timingErrorSamples =
                                        static_cast<long long>(
                                            hitSample)
                                        - static_cast<long long>(
                                            targetStartSample);

                                    const double timingErrorMs =
                                        static_cast<double>(
                                            timingErrorSamples)
                                        * 1000.0
                                        / sampleRate;


                                    const auto noteEvaluation =
                                        GameplayEngine::evaluateNote(
                                            targetMidi,
                                            detectedMidi,
                                            scorePitch,
                                            timingErrorMs,
                                            owner.difficulty,
                                            owner.speedPercent,
                                            owner.songLevel.load(std::memory_order_relaxed));

                                    const bool correctPitch =
                                        noteEvaluation.correctPitch;

                                    const bool insideTimingWindow =
                                        noteEvaluation.insideTimingWindow;


























                                    //======================================================
                                    // WRONG NOTE OR TIMING MISS

                                    if (!correctPitch
                                        || !insideTimingWindow)
                                    {
                                        if (owner.gameSession.resolveTarget(
                                            currentTargetIndex))
                                        {
                                            owner.gameplayStats.recordMiss();
                                            owner.gameSession.setTargetHit(
                                                true);

                                            juce::MessageManager::callAsync(
                                                [this]
                                                {
                                                    owner.showHitFeedback("MISS", 4);
                                                });

                                            owner.advanceTarget();
                                        }
                                    }
                                    else
                                    {
                                        //==================================================
                                        // CORRECT NOTE + CORRECT TIMING

                                        if (owner.gameSession.resolveTarget(
                                            currentTargetIndex))
                                        {
                                            //==============================================
                                            // Score calculated by GameplayEngine.

                                            const auto& scoringResult =
                                                noteEvaluation.scoringResult;

                                            const int scoreToAdd =
                                                scoringResult.score;

                                            const int newScore =
                                                owner.gameplayStats.recordHit(
                                                    scoreToAdd,
                                                    scoringResult.note.quality);

                                            owner.gameplayStats.recordTiming(
                                                timingErrorMs);

                                            //==============================================
                                            // Successful hit state

                                            owner.gameSession.setTargetHit(
                                                true);

                                            owner.gameSession.setLastHitSample(
                                                hitSample);

                                            //==============================================
                                            // Hit quality UI

                                            juce::MessageManager::callAsync(
                                                [this,
                                                quality = scoringResult.note.quality,
                                                timingErrorMs,
                                                currentCombo = owner.gameplayStats.getCombo()]
                                                {
                                                    juce::String qualityText;

                                                    switch (quality)
                                                    {
                                                    case HitQuality::Perfect:
                                                        qualityText = "PERFECT";
                                                        break;

                                                    case HitQuality::Great:
                                                        qualityText = "GREAT";
                                                        break;

                                                    case HitQuality::Good:
                                                        qualityText = "GOOD";
                                                        break;

                                                    case HitQuality::Miss:
                                                        qualityText = "MISS";
                                                        break;

                                                    default:
                                                        qualityText = "MISS";
                                                        break;
                                                    }

                                                    const int roundedTiming =
                                                        static_cast<int>(
                                                            std::round(
                                                                std::abs(
                                                                    timingErrorMs)));

                                                    const juce::String timingText =
                                                        timingErrorMs < 0.0
                                                        ? "-"
                                                        + juce::String(
                                                            roundedTiming)
                                                        + " ms EARLY"
                                                        : "+"
                                                        + juce::String(
                                                            roundedTiming)
                                                        + " ms LATE";

                                                    juce::String resultText = qualityText
                                                        + "\n" + timingText;
                                                    if (currentCombo >= 2)
                                                        resultText += "\nCOMBO x" + juce::String(currentCombo);
                                                    int feedback = 4;
                                                    switch (quality)
                                                    {
                                                    case HitQuality::Perfect: feedback = 1; break;
                                                    case HitQuality::Great:  feedback = 2; break;
                                                    case HitQuality::Good:   feedback = 3; break;
                                                    case HitQuality::Miss:   feedback = 4; break;
                                                    }

                                                    // Highlight every ten-hit milestone without
                                                    // disturbing the quality/timing information.
                                                    if (currentCombo >= 10 && currentCombo % 10 == 0)
                                                    {
                                                        resultText += "\nCOMBO MILESTONE!";
                                                        feedback = 5;
                                                    }

                                                    owner.showHitFeedback(resultText, feedback);
                                                });

                                            //==============================================
                                            // Score UI

                                            juce::MessageManager::callAsync(
                                                [this, newScore]
                                                {
                                                    owner.gameSessionLabel.setText(
                                                        juce::String(
                                                            newScore),
                                                        juce::dontSendNotification);
                                                });

                                            //==============================================
                                            // Next target

                                            owner.advanceTarget();
                                        }
                                    }
                                    } // end: single-note scoring path

                                } // end: non-stale onset

                                //======================================================
                                // Pitch UI

                                juce::MessageManager::callAsync(
                                    [this, text]
                                    {
                                        owner.pitchLabel.setText(
                                            text,
                                            juce::dontSendNotification);
                                    });
                            }
                        }
                    }
                }

                //==============================================================
                // Second analysis buffer
                //
                // DISPLAY ONLY.
                //
                // IMPORTANT:
                // This buffer does NOT:
                // - score
                // - change target
                // - change lastOnsetSample
                // - change lastNoteOnSample
                //
                // Therefore it cannot corrupt the timing used by scoring.

                if (owner.pitchBufferReady2.exchange(
                    false,
                    std::memory_order_relaxed))
                {
                    auto* device =
                        owner.deviceManager
                        .getCurrentAudioDevice();

                    if (device == nullptr)
                        continue;

                    const double sampleRate =
                        device->getCurrentSampleRate();

                    if (sampleRate <= 0.0)
                        continue;

                    const auto displayPitchRange =
                        owner.getPitchAnalysisRange();

                    const double pitch =
                        owner.pitchDetector.detectPitch(
                            owner.pitchAnalysisBuffer2
                            .getReadPointer(0),
                            owner.pitchAnalysisBuffer2.getNumSamples(),
                            sampleRate,
                            displayPitchRange.getStart(),
                            displayPitchRange.getEnd());

                    const double rms =
                        owner.pitchDetector.detectRms(
                            owner.pitchAnalysisBuffer2
                            .getReadPointer(0),
                            owner.pitchAnalysisBuffer2.getNumSamples());

                    const double previousRms =
                        owner.previousRmsValue2;

                    owner.previousRmsValue2 = rms;

                    const double displayNoteGate =
                        0.0030 * std::sqrt(std::pow(
                            2.0,
                            (0.5 - juce::jlimit(
                                 0.0,
                                 1.0,
                                 static_cast<double>(
                                     owner.inputSensitivity.load(
                                         std::memory_order_relaxed))))
                                * 2.0));

                    if (rms < displayNoteGate)
                    {
                        juce::MessageManager::callAsync(
                            [this]
                            {
                                owner.pitchLabel.setText(
                                    "---",
                                    juce::dontSendNotification);
                            });
                    }

                    const bool onset =
                        rms > displayNoteGate
                        && rms > previousRms * 1.02;

                    juce::ignoreUnused(onset);

                    const bool displayAccepted =
                        ! owner.guitarInputOnly.load(std::memory_order_relaxed)
                        || instrumentGate.acceptsSustain(
                               owner.pitchAnalysisStartSample2.load(
                                   std::memory_order_relaxed)
                                   + static_cast<uint64_t>(
                                       owner.pitchAnalysisBuffer2.getNumSamples()),
                               rms);

                    // Loud input that the gate rejects must not leave the
                    // last accepted note on screen (a voice would otherwise
                    // look like a held guitar note).
                    if (rms >= displayNoteGate && ! displayAccepted)
                    {
                        if (! displayRejectedShown)
                        {
                            displayRejectedShown = true;

                            juce::MessageManager::callAsync(
                                [this]
                                {
                                    owner.pitchLabel.setText(
                                        "---",
                                        juce::dontSendNotification);
                                });
                        }
                    }
                    else if (displayAccepted)
                    {
                        displayRejectedShown = false;
                    }

                    if (pitch > MainComponent::minDetectedPitchHz
                        && pitch <= MainComponent::maxDetectedPitchHz
                        && rms >= displayNoteGate
                        && displayAccepted)
                    {
                        const auto detectedNote =
                            owner.pitchDetector.frequencyToNote(
                                pitch);

                        const auto text =
                            detectedNote
                            + "  |  "
                            + juce::String(
                                pitch,
                                2)
                            + " Hz";

                        juce::MessageManager::callAsync(
                            [this, text]
                            {
                                owner.pitchLabel.setText(
                                    text,
                                    juce::dontSendNotification);
                            });
                    }
                }

                //==============================================================
                // Keep original worker cadence.

                wait(1);
            }
        

    }

private:
    MainComponent& owner;
    // Voice / non-guitar rejection for the note being analysed. Only touched
    // from run(); the audio callback never sees it.
    InstrumentGate instrumentGate;
    bool tunerRejectedShown = false;
    bool displayRejectedShown = false;
    // Frames without a usable tuner reading. After two (about 190 ms) the
    // tuner is cleared instead of keeping the last value on screen.
    int tunerStaleFrames = 0;

    // Pitch stability: how many consecutive analysis frames carried the same
    // note (within 60 cents). Speech jumps between notes every syllable; a
    // plucked string holds its pitch. Two frames is about 190 ms.
    static constexpr int minStableFrames = 2;
    double stablePitch = 0.0;
    int stableFrames = 0;

    // An accepted attack is scored only after the note has been stable for
    // minStableFrames frames. The onset sample is kept, so the timing of a
    // real pluck is unchanged.
    bool pendingScore = false;
    std::uint64_t pendingScoreOnset = 0;
    double pendingScorePitch = 0.0;
};
