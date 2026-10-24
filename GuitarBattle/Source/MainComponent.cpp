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
        preferredInputChannel.store(
            settings->getIntValue("InputChannel", -1),
            std::memory_order_relaxed);

    setSize(
        1000,
        950);

    startTimerHz(30);

    overlayComponent.setBounds(getLocalBounds());
    addAndMakeVisible(overlayComponent);

    addAndMakeVisible(
        tablatureComponent);

    initialiseUi();

    initialiseAudio();

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
