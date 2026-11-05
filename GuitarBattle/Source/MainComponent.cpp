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

        // Guitars only by default: a voice at the chart's pitch would
        // otherwise pass every note. The audio settings dialog can switch the
        // filter off for an unusual tone.
        guitarInputOnly.store(
            settings->getBoolValue("GuitarInputOnly", true),
            std::memory_order_relaxed);

        // Mixer defaults: the generated MIDI guitar and the metronome start
        // silent until the player turns them on. The choice is remembered, so
        // a song never re-enables them behind the player's back.
        midiBackingEnabled.store(
            settings->getBoolValue("MidiGuitarEnabled", false),
            std::memory_order_relaxed);

        metronomeMixerEnabled.store(
            settings->getBoolValue("MetronomeEnabled", false),
            std::memory_order_relaxed);

        // Open-string tuning chosen on the tuner screen. Invalid or missing
        // values fall back to standard tuning; the UI validates the selection.
        // The list length is the slot count, and -1 marks a string the player
        // marked as empty, so a four-string or eight-string tuning round-trips.
        const auto storedTuning = settings->getValue("TuningNotes", juce::String());
        if (storedTuning.isNotEmpty())
        {
            juce::StringArray parts;
            parts.addTokens(storedTuning, ",", "");

            if (parts.size() >= GuitarTuning::minStringCount
                && parts.size() <= GuitarTuning::maxStrings)
            {
                GuitarTuning::Tuning parsed;
                parsed.count = parts.size();
                bool valid = true;

                for (int i = 0; i < parsed.count; ++i)
                {
                    const int note = parts[i].getIntValue();

                    if (note != GuitarTuning::emptyNote
                        && (note < 24 || note > 96))
                    {
                        valid = false;
                        break;
                    }

                    parsed.notes[static_cast<size_t>(i)] = note;
                }

                if (valid && GuitarTuning::activeCount(parsed) > 0)
                    currentTuning = parsed;
            }
        }

        currentTuningName = settings->getValue("TuningName", "STANDARD");

        // Frets on the neck. The parser never places a note above this fret and
        // moves out-of-range notes by whole octaves instead.
        currentTuning.fretCount = juce::jlimit(
            GuitarTuning::minFretCount,
            GuitarTuning::maxFretCount,
            settings->getIntValue("FretCount", GuitarTuning::defaultFretCount));
    }

    inputMonitor = std::make_unique<GuitarInputMonitor>(deviceManager);
    inputMonitor->setSensitivityChangedCallback(
        [this](float normalised) { setInputSensitivity(normalised); });
    inputMonitor->setChannelSelectedCallback(
        [this](int hardwareChannel) { setPreferredInputChannel(hardwareChannel); });
    inputMonitor->setSensitivityNormalised(
        inputSensitivity.load(std::memory_order_relaxed));

    // VST monitor host: settings paths and the cached plugin list are ready
    // before the audio device starts. The saved plugin itself is restored after
    // initialiseAudio(), once the real sample rate and block size are known.
    vstHost.initialise(appProperties.getUserSettings());

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

    // The audio device is running now, so the VST host knows the real sample
    // rate and block size. Loading is asynchronous; the main screen refreshes
    // through vstHost.onStateChanged.
    vstHost.onStateChanged = [this] { updateVstButton(); };
    vstHost.restoreSavedPlugin();
    updateVstButton();

    // Optional global track: absence is a normal state and never blocks startup.
    fetchBackgroundMusic();

    // Restore a saved sign-in (if any) before the player types a password again.
    tryRestoreSavedSession();

    // One update check per launch. Deferred so the main window is already on
    // screen and a restored-session dialog is not overlapped; the request runs
    // on ApiClient's worker pool and the result arrives on the message thread.
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::Timer::callAfterDelay(4000, [safeThis]()
    {
        if (auto* self = safeThis.getComponent())
            self->checkForUpdatesOnLaunch();
    });

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
    // The live guitar monitor is silent on the sign-in / registration screen:
    // only the background music is heard there. The audio callback reads this
    // atomic, so the state is mirrored here on the message thread.
    if (guitarMonitoringAllowed.load(std::memory_order_relaxed) == showAuthScreen)
        guitarMonitoringAllowed.store(! showAuthScreen, std::memory_order_relaxed);

    // A downloaded sound pack is applied only while the audio device is idle.
    if (pendingClientAssetsReload)
        applyPendingClientAssetsReload();

    updateHitFeedbackAnimation();

    // Input monitor: read the audio-thread atomics and advance the meter
    // smoothing / peak hold. Message thread only.
    updateInputMonitor();

    // A build found by the launch check is offered only on the main screen:
    // never over the sign-in card, a song, or another modal dialog.
    if (updatePromptPending
        && gameState == GameState::SongSelect
        && ! showAuthScreen
        && juce::ModalComponentManager::getInstance()->getNumModalComponents() == 0)
    {
        showUpdatePrompt();
    }

    // The live accuracy and the judgement strip are drawn by paintOverlay().
    // Repainting a Label only invalidates its own small area, so the overlay
    // has to be invalidated explicitly whenever a counter changes. Only the
    // two painted stat areas are invalidated: a full overlay repaint would
    // also redraw the tablature on every hit.
    if (gameState == GameState::Playing)
    {
        const int hits = gameplayStats.getHitCount();
        const int misses = gameplayStats.getMissCount();

        if (hits != hudShownHits || misses != hudShownMisses)
        {
            hudShownHits = hits;
            hudShownMisses = misses;

            if (! hudStatsBounds.isEmpty())
                overlayComponent.repaint(hudStatsBounds.expanded(2));

            if (! hudAccuracyBounds.isEmpty())
                overlayComponent.repaint(hudAccuracyBounds.expanded(2));
        }
    }

    // Single place that decides whether preview or background music may play.
    // Idempotent, so screen changes never restart the track.
    updateMusicState();

    // Song asset download: the ApiClient worker stores the overall fraction
    // into a shared atomic. Only this message-thread tick copies it into the
    // value the ProgressBar reads; the network thread never touches the UI.
    if (songDownloadProgressState != nullptr)
    {
        songDownloadProgressValue = juce::jlimit(
            0.0, 1.0,
            static_cast<double>(
                songDownloadProgressState->load(std::memory_order_relaxed)));
    }

    // Practice mode: hold the transport at the target note and evaluate the
    // player's attempts. Message thread only.
    updatePracticeMode();

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

            // A practice run whose first target is on beat zero is already
            // holding the transport at the count-in end. Keep the tablature
            // frozen and the target prompt visible instead of starting the
            // scroll and replacing the prompt with "GO!".
            const bool practiceHeld =
                practiceHoldActive.load(std::memory_order_acquire);

            tablatureComponent.setPlayheadRunning(! practiceHeld);

            if (! practiceHeld)
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
    //
    // Practice mode never scores and holds the transport at the target note,
    // so the wall-clock miss timeout must not run there: it would resolve the
    // target and skip the note that is waiting for the player.

    if (currentTarget >= 0
        && currentTarget <
        static_cast<int>(
            gameSession.getMidiNotes().size())
        && ! practiceActive.load(std::memory_order_relaxed))
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

            // The auto miss must not fire before the (level- and speed-scaled)
            // late hit window closes.
            const double timingWindowScale =
                ScoringSystem::getTimingWindowScale(
                    songLevel.load(std::memory_order_relaxed),
                    speedPercent.load(std::memory_order_relaxed));

            const auto missTimeoutSamples =
                static_cast<uint64_t>(
                    sampleRate
                    * juce::jmax(0.300, 0.150 * timingWindowScale + 0.100));

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
    // The VST host outlives the UI members declared below it; drop the
    // callback before any of them are destroyed.
    vstHost.onStateChanged = nullptr;

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
