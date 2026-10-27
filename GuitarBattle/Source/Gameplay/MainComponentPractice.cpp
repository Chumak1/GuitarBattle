#include "../MainComponent.h"

#include <algorithm>
#include <cmath>

// Practice mode: a training run that plays the song up to the next target note,
// waits for the player to perform it, and only then continues. It never scores,
// never opens a server session and never submits a result.

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
    practiceLastPitch.store(0.0, std::memory_order_relaxed);
    practiceLastOnsetSample = 0;
    practiceCompleted = false;
}

void MainComponent::freezePracticeTransport()
{
    if (! practiceActive.load(std::memory_order_relaxed)
        || practiceHoldActive.load(std::memory_order_relaxed))
        return;

    practiceHoldStartSample.store(audioSampleCounter.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
    practiceHoldActive.store(true, std::memory_order_release);

    // Everything that follows the musical timeline stops: metronome, stems and
    // the MIDI backing are gated by practiceHoldActive in the audio callback.
    metronome.pauseScheduler();
    metronomeSamplesRemaining = 0;
    metronomePlaybackPosition = 0;

    tablatureComponent.setPlayheadRunning(false);

    hitLabel.setText("WAITING FOR CORRECT NOTE", juce::dontSendNotification);
    repaint();
}

void MainComponent::releasePracticeTransport()
{
    if (! practiceHoldActive.load(std::memory_order_relaxed))
        return;

    const auto now = audioSampleCounter.load(std::memory_order_relaxed);
    const auto holdStart = practiceHoldStartSample.load(std::memory_order_relaxed);

    if (now > holdStart)
    {
        const auto heldSamples = now - holdStart;

        // Shift both anchors by the frozen duration: the musical position does
        // not move, and every component resumes from the same beat.
        gameplayStartSample.fetch_add(heldSamples, std::memory_order_relaxed);
        tempoAnchorSample.fetch_add(heldSamples, std::memory_order_relaxed);
    }

    practiceHoldActive.store(false, std::memory_order_release);
    practiceHoldStartSample.store(0, std::memory_order_relaxed);

    const double resumeBeat = getSongBeatForSample(now);

    if (resumeBeat >= 0.0)
        metronome.resumeAtBeat(now, resumeBeat);
    else
        metronome.resumeScheduler(now, gameplayStartSample.load(std::memory_order_relaxed));

    metronomeSamplesRemaining = 0;
    metronomePlaybackPosition = 0;

    tablatureComponent.setPlayheadBeat(
        currentPlayheadBeat.load(std::memory_order_relaxed));
    tablatureComponent.setPlayheadRunning(true);

    hitLabel.setText("GOOD - NEXT NOTE", juce::dontSendNotification);
}

void MainComponent::evaluatePracticeNote()
{
    const auto notes = gameSession.getMidiNotes();
    const int index = gameSession.getTargetNoteIndex();

    if (index < 0 || index >= static_cast<int>(notes.size()))
        return;

    const double pitch = practiceLastPitch.load(std::memory_order_relaxed);

    if (pitch <= 20.0 || pitch >= 1000.0)
        return;

    const int expectedMidi = notes[static_cast<size_t>(index)].midiNote;
    const int detectedMidi = pitchDetector.frequencyToMidi(pitch);

    // Only judge notes near the current target: noodling during the intro must
    // not freeze the transport before the song reaches the first note.
    const double currentBeat = currentPlayheadBeat.load(std::memory_order_relaxed);
    const double targetBeat = notes[static_cast<size_t>(index)].startBeat;

    if (currentBeat < targetBeat - 2.0)
        return;

    if (detectedMidi == expectedMidi)
    {
        gameSession.resolveTarget(index);
        const int nextIndex = gameSession.advanceTargetIndex();

        releasePracticeTransport();

        if (nextIndex >= static_cast<int>(notes.size()))
        {
            finishPracticeSession();
            return;
        }

        setTargetFromMidiIndex(nextIndex);
        hitLabel.setText("GOOD - NEXT NOTE", juce::dontSendNotification);
        return;
    }

    // Wrong note: keep the current target and freeze everything.
    freezePracticeTransport();
    hitLabel.setText("WRONG NOTE - PLAY THE HIGHLIGHTED NOTE",
                     juce::dontSendNotification);
}

void MainComponent::updatePracticeMode()
{
    if (! practiceActive.load(std::memory_order_relaxed)
        || gameState != GameState::Playing
        || practiceCompleted)
        return;

    const auto notes = gameSession.getMidiNotes();
    const int index = gameSession.getTargetNoteIndex();

    if (index >= static_cast<int>(notes.size()))
    {
        finishPracticeSession();
        return;
    }

    if (index < 0)
        return;

    // One onset = one attempt. A sustained note cannot advance several targets.
    const auto onsetSample = lastNoteOnSample.load(std::memory_order_relaxed);

    if (onsetSample != 0 && onsetSample != practiceLastOnsetSample)
    {
        practiceLastOnsetSample = onsetSample;
        evaluatePracticeNote();

        if (practiceCompleted)
            return;
    }

    // Freeze exactly at the target so the note is played at its own beat.
    const int currentIndex = gameSession.getTargetNoteIndex();
    const double currentBeat = currentPlayheadBeat.load(std::memory_order_relaxed);

    if (currentIndex < 0 || currentIndex >= static_cast<int>(notes.size()))
        return;

    const double targetBeat = notes[static_cast<size_t>(currentIndex)].startBeat;

    if (! practiceHoldActive.load(std::memory_order_relaxed)
        && currentBeat >= targetBeat - 0.02)
    {
        freezePracticeTransport();
        hitLabel.setText("PLAY THE HIGHLIGHTED NOTE", juce::dontSendNotification);
    }
}

void MainComponent::finishPracticeSession()
{
    if (practiceCompleted)
        return;

    practiceCompleted = true;
    practiceActive.store(false, std::memory_order_relaxed);

    if (practiceHoldActive.exchange(false, std::memory_order_relaxed))
        practiceHoldStartSample.store(0, std::memory_order_relaxed);

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
