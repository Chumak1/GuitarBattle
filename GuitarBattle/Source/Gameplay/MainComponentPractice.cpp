#include "../MainComponent.h"

#include <algorithm>
#include <cmath>

// Practice mode: a training run that plays the song up to the next target note,
// waits for the player to perform it, and only then continues. It never scores,
// never opens a server session and never submits a result.
//
// Timing is sample-accurate: the message thread schedules the target sample in
// practiceGateSample, and the audio callback stops the stems, the metronome,
// the MIDI backing and the playhead on that exact sample. The 30 Hz UI timer
// only schedules the gate and reacts to the hold, so neither timer jitter nor
// the device output latency can stop the music before the target is heard.

void MainComponent::setPracticeMode(bool enabled)
{
    if (practiceMode == enabled)
        return;

    practiceMode = enabled;
    practiceModeButton.setButtonText(practiceMode ? "PRACTICE: ON"
                                                  : "PRACTICE: OFF");
    practiceModeButton.setColour(juce::TextButton::buttonColourId,
                                 practiceMode ? UiColors::accentStrong
                                              : UiColors::surface2);
    practiceModeButton.setColour(juce::TextButton::textColourOffId,
                                 practiceMode ? UiColors::onAccent
                                              : UiColors::text);
    repaint();
}

void MainComponent::beginPracticeRun()
{
    practiceActive.store(practiceMode, std::memory_order_relaxed);
    practiceHoldActive.store(false, std::memory_order_relaxed);
    practiceHoldStartSample.store(0, std::memory_order_relaxed);
    practiceGateSample.store(0, std::memory_order_relaxed);
    practiceGateArmed.store(false, std::memory_order_relaxed);
    practiceLastPitch.store(0.0, std::memory_order_relaxed);
    practicePitchSample.store(0, std::memory_order_relaxed);

    // A tuning note may still be the last detected onset. Skip it so the first
    // tick of the run does not judge a note played before the count-in.
    practiceLastOnsetSample = lastNoteOnSample.load(std::memory_order_relaxed);
    practiceResumeSample = 0;
    practiceChordHold = false;
    practiceHoldUiApplied = false;
    practiceCompleted = false;
}

void MainComponent::schedulePracticeFreeze()
{
    if (! practiceActive.load(std::memory_order_relaxed)
        || practiceHoldActive.load(std::memory_order_acquire)
        || practiceCompleted)
        return;

    const auto& notes = gameSession.getMidiNotes();
    const int index = gameSession.getTargetNoteIndex();

    if (index < 0 || index >= static_cast<int>(notes.size()))
        return;

    const auto currentSample =
        audioSampleCounter.load(std::memory_order_relaxed);

    auto gateSample =
        getSongSampleForBeat(notes[static_cast<size_t>(index)].startBeat);

    // A first note on beat zero, a chord note or a late timer tick can leave
    // the target behind the rendered position. Freeze on the next sample
    // instead of scheduling a gate in the past.
    if (gateSample <= currentSample)
        gateSample = currentSample + 1;

    practiceGateSample.store(gateSample, std::memory_order_relaxed);
    practiceGateArmed.store(true, std::memory_order_release);
}

void MainComponent::applyPracticeHoldUi()
{
    if (! practiceActive.load(std::memory_order_relaxed)
        || practiceHoldUiApplied)
        return;

    practiceHoldUiApplied = true;

    // The audio callback has already stopped the musical timeline on the
    // target sample. Everything here is UI state only.
    practiceGateArmed.store(false, std::memory_order_relaxed);
    practiceGateSample.store(0, std::memory_order_relaxed);

    metronome.pauseScheduler();
    metronomeSamplesRemaining = 0;
    metronomePlaybackPosition = 0;

    tablatureComponent.setPlayheadBeat(
        currentPlayheadBeat.load(std::memory_order_relaxed));
    tablatureComponent.setPlayheadRunning(false);

    hitLabel.setText("PLAY THE HIGHLIGHTED NOTE", juce::dontSendNotification);
    repaint();
}

void MainComponent::releasePracticeTransport()
{
    if (! practiceHoldActive.load(std::memory_order_acquire))
        return;

    // Disarm first: the audio thread must not re-freeze on the gate that just
    // ended when the transport resumes.
    practiceGateArmed.store(false, std::memory_order_relaxed);
    practiceGateSample.store(0, std::memory_order_relaxed);

    const auto now =
        audioSampleCounter.load(std::memory_order_relaxed);
    const auto holdStart =
        practiceHoldStartSample.load(std::memory_order_relaxed);

    if (now > holdStart)
    {
        const auto heldSamples = now - holdStart;

        // Shift both anchors by the frozen duration: the musical position does
        // not move, and every component resumes from the same beat.
        gameplayStartSample.fetch_add(heldSamples, std::memory_order_relaxed);
        tempoAnchorSample.fetch_add(heldSamples, std::memory_order_relaxed);
    }

    const double resumeBeat = getSongBeatForSample(now);

    // Prepare the metronome before the audio thread can render the next block.
    if (resumeBeat >= 0.0)
        metronome.resumeAtBeat(now, resumeBeat);
    else
        metronome.resumeScheduler(now, gameplayStartSample.load(std::memory_order_relaxed));

    metronomeSamplesRemaining = 0;
    metronomePlaybackPosition = 0;

    practiceHoldStartSample.store(0, std::memory_order_relaxed);
    practiceResumeSample = now;
    practiceChordHold = false;
    practiceHoldUiApplied = false;

    practiceHoldActive.store(false, std::memory_order_release);

    tablatureComponent.setPlayheadBeat(
        currentPlayheadBeat.load(std::memory_order_relaxed));
    tablatureComponent.setPlayheadRunning(true);

    hitLabel.setText("GOOD - NEXT NOTE", juce::dontSendNotification);
}

void MainComponent::evaluatePracticeNote()
{
    // Only the note played at a held target is judged. A stray note while the
    // transport is still running must not stop the song before its beat.
    if (! practiceHoldActive.load(std::memory_order_acquire))
        return;

    const auto& notes = gameSession.getMidiNotes();
    const int index = gameSession.getTargetNoteIndex();

    if (index < 0 || index >= static_cast<int>(notes.size()))
        return;

    const auto onsetSample =
        lastNoteOnSample.load(std::memory_order_relaxed);
    const auto pitchSample =
        practicePitchSample.load(std::memory_order_relaxed);

    // The pitch and the onset are published by the analysis thread one after
    // another. Requiring the pitch window to reach the end of the onset
    // window keeps a tick that lands between the two stores from judging the
    // new attack with the previous pitch.
    if (pitchSample
        < onsetSample + static_cast<uint64_t>(pitchAnalysisSize))
        return;

    const double pitch = practiceLastPitch.load(std::memory_order_relaxed);

    if (pitch <= minDetectedPitchHz || pitch > maxDetectedPitchHz)
        return;

    const int expectedMidi = notes[static_cast<size_t>(index)].midiNote;
    const int detectedMidi = pitchDetector.frequencyToMidi(pitch);

    if (detectedMidi == expectedMidi)
    {
        advancePracticeTarget(index);
        return;
    }

    // Wrong note: keep the target and the whole mix stopped.
    hitLabel.setText("WRONG NOTE - PLAY THE HIGHLIGHTED NOTE",
                     juce::dontSendNotification);
}

void MainComponent::advancePracticeTarget(int index)
{
    const auto& notes = gameSession.getMidiNotes();

    if (index < 0 || index >= static_cast<int>(notes.size()))
        return;

    const double resolvedBeat = notes[static_cast<size_t>(index)].startBeat;

    gameSession.resolveTarget(index);
    const int nextIndex = gameSession.advanceTargetIndex();

    if (nextIndex >= static_cast<int>(notes.size()))
    {
        finishPracticeSession();
        return;
    }

    setTargetFromMidiIndex(nextIndex);

    const double nextBeat = notes[static_cast<size_t>(nextIndex)].startBeat;

    if (std::abs(nextBeat - resolvedBeat) < 1.0e-4
        && practiceHoldActive.load(std::memory_order_acquire))
    {
        // Chord: the next note starts on the same beat, so the transport stays
        // stopped until the whole shape has been played. A fresh attack is
        // required for each chord note; the ringing note from the previous
        // target must not skip it.
        practiceResumeSample =
            lastNoteOnSample.load(std::memory_order_relaxed);
        practiceChordHold = true;

        hitLabel.setText("PLAY THE HIGHLIGHTED NOTE",
                         juce::dontSendNotification);
        return;
    }

    releasePracticeTransport();
    schedulePracticeFreeze();
}

void MainComponent::updatePracticeMode()
{
    if (! practiceActive.load(std::memory_order_relaxed)
        || gameState != GameState::Playing
        || practiceCompleted)
        return;

    const auto& notes = gameSession.getMidiNotes();
    const int index = gameSession.getTargetNoteIndex();

    if (index >= static_cast<int>(notes.size()))
    {
        finishPracticeSession();
        return;
    }

    if (index < 0)
        return;

    if (! practiceHoldActive.load(std::memory_order_acquire))
    {
        // The transport is running: schedule the sample-accurate stop for the
        // current target. Re-scheduling every tick keeps the gate correct
        // after a tempo change or a pause.
        schedulePracticeFreeze();
        return;
    }

    applyPracticeHoldUi();

    const auto holdStart =
        practiceHoldStartSample.load(std::memory_order_relaxed);

    // One onset = one attempt. A sustained note cannot advance several targets.
    const auto onsetSample =
        lastNoteOnSample.load(std::memory_order_relaxed);

    if (onsetSample != 0 && onsetSample != practiceLastOnsetSample)
    {
        practiceLastOnsetSample = onsetSample;

        // Only an onset whose analysis window reaches the freeze belongs to
        // the held target. A strum during the count-in, or a note played long
        // before the target, must not skip it.
        const bool nearHold =
            onsetSample + static_cast<uint64_t>(pitchAnalysisSize) >= holdStart;

        if (nearHold)
        {
            evaluatePracticeNote();

            if (practiceCompleted)
                return;

            if (! practiceHoldActive.load(std::memory_order_acquire))
                return; // target advanced and the transport resumed
        }
    }

    // While the transport is held, watch the live pitch as well. A note that
    // was attacked just before the freeze, or that the player keeps ringing,
    // produces no new onset; without this poll the hold would never release.
    const int heldIndex = gameSession.getTargetNoteIndex();

    if (heldIndex < 0 || heldIndex >= static_cast<int>(notes.size()))
        return;

    const auto pitchSample =
        practicePitchSample.load(std::memory_order_relaxed);
    const auto lastOnset =
        lastOnsetSample.load(std::memory_order_relaxed);
    const double sampleRate =
        cachedSampleRate.load(std::memory_order_relaxed);

    // Ignore the analysis window that still contains audio from before the
    // freeze, so a ringing previous note cannot release the hold.
    const auto settleSamples = static_cast<uint64_t>(
        juce::jmax(1.0, sampleRate * 0.12));

    // A note left ringing from before the resume belongs to the previous
    // target. Requiring an onset after the resume keeps repeated notes from
    // skipping the next target. A chord is the exception: its notes are
    // strummed together and share one onset, but each has a different pitch.
    //
    // The attack must also be close to the freeze: a tuner note or a strum
    // from the count-in is seconds older and must not release the hold.
    const auto liveWindowSamples = static_cast<uint64_t>(
        juce::jmax(1.0, sampleRate * 0.25));

    const bool onsetNearHold =
        lastOnset > 0
        && lastOnset + liveWindowSamples >= holdStart;

    const bool onsetBelongsToHold =
        practiceChordHold
        || (lastOnset > practiceResumeSample && onsetNearHold);

    if (! onsetBelongsToHold || pitchSample <= holdStart + settleSamples)
        return;

    const double pitch = practiceLastPitch.load(std::memory_order_relaxed);

    if (pitch > minDetectedPitchHz && pitch <= maxDetectedPitchHz
        && pitchDetector.frequencyToMidi(pitch)
            == notes[static_cast<size_t>(heldIndex)].midiNote)
    {
        advancePracticeTarget(heldIndex);
    }
}

void MainComponent::finishPracticeSession()
{
    if (practiceCompleted)
        return;

    practiceCompleted = true;
    practiceActive.store(false, std::memory_order_relaxed);

    // Keep the timeline frozen until returnToSongSelect() clears the hold:
    // releasing it here would let the stems play the last note again while the
    // completion dialog is being prepared.
    practiceGateArmed.store(false, std::memory_order_relaxed);
    practiceGateSample.store(0, std::memory_order_relaxed);

    metronome.pauseScheduler();
    metronomeSamplesRemaining = 0;
    metronomePlaybackPosition = 0;
    tablatureComponent.setPlayheadRunning(false);

    // No scoring, no best score, no server submission: nothing competitive is
    // touched by a training run.
    gameplayStats.reset();
    gameSession.setTargetHit(false);

    hitLabel.setText("PRACTICE COMPLETE", juce::dontSendNotification);

    juce::Component::SafePointer<MainComponent> safeThis(this);

    juce::MessageManager::callAsync([safeThis]()
    {
        auto* self = safeThis.getComponent();

        if (self == nullptr)
            return;

        self->returnToSongSelect();

        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::NoIcon,
            "Practice complete",
            "You played every note. No score was saved and your rating is unchanged.",
            "OK",
            self);
    });
}
