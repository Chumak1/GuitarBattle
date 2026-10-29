#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include "../MainComponent.h"

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

                    const double silenceThreshold = 0.0035 * thresholdScale;
                    const double onsetThreshold = 0.0080 * thresholdScale;
                    const double noteRmsGate = 0.005 * std::sqrt(thresholdScale);

                    const uint64_t minimumOnsetSamples =
                        static_cast<uint64_t>(
                            sampleRate * 0.060
                            * juce::jlimit(0.5, 2.0, thresholdScale));

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
                    const bool risingAttack =
                        rms >= onsetThreshold
                        && rms > previousRms * 1.20
                        && rms - previousRms > 0.0015;

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

                    if (onset)
                    {
                        owner.noteOn.store(
                            true,
                            std::memory_order_relaxed);

                        owner.lastOnsetSample.store(
                            detectedOnsetSample,
                            std::memory_order_relaxed);

                        owner.lastNoteOnSample.store(
                            analysisSample,
                            std::memory_order_relaxed);
                    }


                    //==========================================================
                    // Practice mode: publish the live pitch on every analysis
                    // window, not only on an onset. While the transport is held
                    // the player usually keeps the target note ringing, so the
                    // hold must be able to release without a fresh attack.
                    if (owner.practiceActive.load(std::memory_order_relaxed)
                        && pitch > MainComponent::minDetectedPitchHz
                        && pitch <= MainComponent::maxDetectedPitchHz
                        && rms >= noteRmsGate)
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



                    if (pitch > MainComponent::minDetectedPitchHz
                        && pitch <= MainComponent::maxDetectedPitchHz
                        && rms >= noteRmsGate
                        && (onset || owner.tunerActive.load(std::memory_order_relaxed)))
                    {
                        owner.lastPitch = pitch;

                        //======================================================
                        // Tuner mode

                        if (owner.tunerActive.load(
                            std::memory_order_relaxed))
                        {
                            const int stringIndex =
                                owner.tuner.detectString(
                                    pitch);

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

                                    juce::String stringText;

                                    switch (stringIndex)
                                    {
                                    case 0:
                                        stringText =
                                            "6th string - E2";
                                        break;

                                    case 1:
                                        stringText =
                                            "5th string - A2";
                                        break;

                                    case 2:
                                        stringText =
                                            "4th string - D3";
                                        break;

                                    case 3:
                                        stringText =
                                            "3rd string - G3";
                                        break;

                                    case 4:
                                        stringText =
                                            "2nd string - B3";
                                        break;

                                    case 5:
                                        stringText =
                                            "1st string - E4";
                                        break;

                                    default:
                                        stringText = "---";
                                        break;
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

                            //==================================================
                            //==========================================================
// Current target

                            int currentTargetIndex =
                                owner.gameSession.getTargetNoteIndex();

                            // Catch up targets whose late-hit window has already
                            // passed before evaluating this detected attack. The
                            // same attack can then be tested against the next
                            // target, but is scored at most once.
                            const auto lateWindowSamples =
                                static_cast<uint64_t>(sampleRate * 0.150);

                            while (currentTargetIndex >= 0
                                && currentTargetIndex < static_cast<int>(
                                    owner.gameSession.getMidiNotes().size()))
                            {
                                const auto targetSample = owner.getTargetSample(
                                    currentTargetIndex, sampleRate);

                                if (detectedOnsetSample <= targetSample + lateWindowSamples)
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
                                        sampleRate * 0.150);

                                const bool staleOnset =
                                    detectedOnsetSample < targetSampleForGuard
                                    && targetSampleForGuard - detectedOnsetSample
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
                                            (static_cast<double>(detectedOnsetSample) - static_cast<double>(chordTargetSample))
                                            * 1000.0 / sampleRate;
                                        const bool chordInsideTimingWindow = std::abs(chordTimingErrorMs) < 150.0;

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
                                                    owner.difficulty, owner.speedPercent);
                                                const int newScore = owner.gameplayStats.recordHit(
                                                    evaluation.scoringResult.score, evaluation.scoringResult.note.quality);
                                                owner.gameplayStats.recordTiming(chordTimingErrorMs);
                                                owner.gameSession.setLastHitSample(detectedOnsetSample);
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
                                            pitch);

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
                                        detectedOnsetSample;

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
                                            pitch,
                                            timingErrorMs,
                                            owner.difficulty,
                                            owner.speedPercent);

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
                        0.005 * std::sqrt(std::pow(
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

                    if (pitch > MainComponent::minDetectedPitchHz
                        && pitch <= MainComponent::maxDetectedPitchHz
                        && rms >= displayNoteGate)
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
};
