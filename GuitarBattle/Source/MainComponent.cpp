#include "MainComponent.h"

#include "Audio/PitchDetectionThread.h"

#include <vector>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <sstream>
#include <string>

#include "Midi/MidiParser.h"

#include "UI/GameStyle.h"



//==============================================================================
// Constructor
//==============================================================================

MainComponent::MainComponent()
    : overlayComponent(*this)
{
    // Apply the admin-panel visual language to every JUCE control, including
    // alert windows, dialog boxes and popup menus.
    juce::LookAndFeel::setDefaultLookAndFeel(&gameLookAndFeel);
    setLookAndFeel(&gameLookAndFeel);

    pitchThread = std::make_unique<PitchDetectionThread>(*this);
    // Configure persistent settings before loading songs or querying best scores.
    juce::PropertiesFile::Options options;
    options.applicationName = "GuitarBattle";
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    options.folderName = "GuitarBattle";
    options.commonToAllUsers = false;
    appProperties.setStorageParameters(options);

    if (auto* settings = appProperties.getUserSettings())
    {
        preferredInputChannel.store(
            settings->getIntValue("InputChannel", -1),
            std::memory_order_relaxed);

        // Neutral default (0.5) keeps the original detector thresholds.
        inputSensitivity.store(
            juce::jlimit(0.0f, 1.0f,
                         static_cast<float>(
                             settings->getDoubleValue("InputSensitivity", 0.5))),
            std::memory_order_relaxed);
    }

    inputMonitor = std::make_unique<GuitarInputMonitor>(deviceManager);
    inputMonitor->setSensitivityChangedCallback(
        [this](float normalised) { setInputSensitivity(normalised); });
    inputMonitor->setChannelSelectedCallback(
        [this](int hardwareChannel) { setPreferredInputChannel(hardwareChannel); });
    inputMonitor->setSensitivityNormalised(
        inputSensitivity.load(std::memory_order_relaxed));

    setSize(
        1000,
        950);

    startTimerHz(30);

    // Each application run starts with a fresh telemetry identity; it is
    // regenerated again on every sign-in.
    telemetryClientSessionId = juce::Uuid().toString();

    overlayComponent.setBounds(getLocalBounds());
    addAndMakeVisible(overlayComponent);

    addAndMakeVisible(
        tablatureComponent);

    initialiseUi();

    // Preview/background music share the existing audio device; settings are
    // restored before the device starts so the first block already has a gain.
    restoreMusicSettings();

    initialiseAudio();

    // Optional global track: absence is a normal state and never blocks startup.
    fetchBackgroundMusic();

    // Added last so pause/count-in cards paint above the HUD and mixer. The
    // component is transparent and does not intercept mouse input.
    addAndMakeVisible(modalOverlayComponent);
    modalOverlayComponent.toFront(false);

    resized();
    repaint();
}

//==============================================================================
// Timer
//==============================================================================

void MainComponent::timerCallback()
{
    // A downloaded sound pack is applied only while the audio device is idle.
    if (pendingClientAssetsReload)
        applyPendingClientAssetsReload();

    updateHitFeedbackAnimation();

    // Input monitor: read the audio-thread atomics and advance the meter
    // smoothing / peak hold. Message thread only.
    updateInputMonitor();

    // Single place that decides whether preview or background music may play.
    // Idempotent, so screen changes never restart the track.
    updateMusicState();

    // Activity heartbeat: every 30 seconds at 30 Hz while signed in. The
    // counter must advance regardless of game state, so it stays above the
    // early return below. The HTTP request runs on a worker thread.
    if (++telemetryTickCounter >= 900)
    {
        telemetryTickCounter = 0;

        if (onlineAccessToken.isNotEmpty())
            sendActivityHeartbeat(false);
    }

    // Smoothly animate the level bar toward the server-side progress. Runs in
    // every state so the bar is already settled when the player returns to the
    // main screen.
    {
        const int xpIntoLevel =
            juce::jmax(0, progressionXpForNext - progressionXpRemaining);

        const float target = progressionXpForNext > 0
            ? juce::jlimit(0.0f, 1.0f,
                           static_cast<float>(xpIntoLevel)
                               / static_cast<float>(progressionXpForNext))
            : 0.0f;

        if (std::abs(target - levelBarShown) > 0.0005f)
        {
            levelBarShown += (target - levelBarShown) * 0.16f;
            overlayComponent.repaint();
        }
        else if (! juce::approximatelyEqual(levelBarShown, target))
        {
            levelBarShown = target;
            overlayComponent.repaint();
        }
    }

    if (gameState != GameState::Playing)
        return;

    // COUNT IN is rendered by MainOverlayComponent across the full window.
    // Do not call TablatureComponent::setCountInDisplay here: the tab view
    // does not own the centered count-in card.
    const bool countInNowActive = countInActive.load(std::memory_order_acquire);

    if (countInNowActive)
    {
        const auto currentSample =
            audioSampleCounter.load(std::memory_order_relaxed);
        const auto startSample =
            gameplayStartSample.load(std::memory_order_relaxed);

        if (currentSample >= startSample)
        {
            countInActive.store(false, std::memory_order_release);
            tablatureComponent.setPlayheadRunning(true);
            hitLabel.setText("GO!", juce::dontSendNotification);
            repaint(); // Remove the large countdown overlay as gameplay begins.
        }
        else
        {
            const int remaining =
                countInRemaining.load(std::memory_order_relaxed);
            hitLabel.setText(
                "COUNT-IN " + juce::String(juce::jmax(1, remaining)),
                juce::dontSendNotification);
            repaint(); // Refresh the custom-painted number on every timer tick.
            return; // Don't resolve misses or advance the playhead during count-in.
        }
    }

    const double beat =
        currentPlayheadBeat.load(
            std::memory_order_relaxed);

    const int currentTarget =
        gameSession.getTargetNoteIndex();

    //==========================================================================
    // Automatic MISS after +150 ms

    if (currentTarget >= 0
        && currentTarget <
        static_cast<int>(
            gameSession.getMidiNotes().size()))
    {
        auto* device =
            deviceManager.getCurrentAudioDevice();

        const double sampleRate =
            device != nullptr
            ? device->getCurrentSampleRate()
            : 0.0;

        if (sampleRate > 0.0)
        {
            const auto currentSample =
                audioSampleCounter.load(
                    std::memory_order_relaxed);

            const auto targetSample =
                getTargetSample(
                    currentTarget,
                    sampleRate);

            const auto missTimeoutSamples =
                static_cast<uint64_t>(
                    sampleRate * 0.300);

            if (currentSample >= targetSample + missTimeoutSamples)
            {
                DBG("AUTO MISS"
                    << " target=" << currentTarget
                    << " currentSample=" << currentSample
                    << " targetSample=" << targetSample
                    << " deltaMs="
                    << ((currentSample - targetSample) * 1000.0 / sampleRate));

                if (gameSession.resolveTarget(currentTarget))
                {
                    gameplayStats.recordMiss();
                    gameSession.setTargetHit(true);

                    juce::MessageManager::callAsync(
                        [this]
                        {
                            showHitFeedback("MISS", 4);
                        });

                    advanceTarget();
                }
            }
        }
    }

    //==========================================================================
    // Playhead

    tablatureComponent.setPlayheadBeat(
        beat);

    beatLabel.setText(
        juce::String(
            static_cast<int>(
                std::floor(beat))
            + 1),
        juce::dontSendNotification);

    const int currentBeatInBar =
        static_cast<int>(
            std::floor(beat))
        % 4;

    barBeatLabel.setText(
        juce::String(
            currentBeatInBar + 1),
        juce::dontSendNotification);

    comboLabel.setText(
        "x" + juce::String(gameplayStats.getCombo()),
        juce::dontSendNotification);
}

//==============================================================================
// Destructor
//==============================================================================

MainComponent::~MainComponent()
{
    // Best-effort: close the activity session while the API client is still
    // alive. The request is queued on the worker pool; the client's destructor
    // waits for the running job, and nothing here touches the UI thread.
    sendActivityHeartbeat(true);

    if (pitchThread != nullptr)
        pitchThread->stopThread(
        1000);

    if (auto* settings =
        appProperties.getUserSettings())
    {
        settings->setValue(
            "AudioDeviceState",
            deviceManager.createStateXml().get());
    }

    deviceManager.removeChangeListener(this);
    shutdownAudio();

    setLookAndFeel(nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
}
