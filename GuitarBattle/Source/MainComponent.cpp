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

    // UI sounds: preferences + manifest. The manager is safe before the audio
    // device opens (it just queues triggers until the callback runs).
    loadUiSoundPreferences();
    uiSounds.setApiClient(&apiClient);
    uiSounds.refreshManifest({});

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

        // Manual timing calibration in milliseconds. 0 = trust the device's
        // reported output latency; positive values compensate hits that still
        // register late (hidden buffers, Bluetooth, slow input drivers).
        timingOffsetMs.store(
            juce::jlimit(-200, 200,
                         settings->getIntValue("TimingOffsetMs", 0)),
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

                    // -1 is an empty slot; every other value must be a valid
                    // MIDI note. The old 24..96 window rejected a five-string
                    // bass low B (23) and silently reset the tuning.
                    if (note != GuitarTuning::emptyNote
                        && (note < 0 || note > 127))
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

    // The duel backdrop sits below the overlay and the tablature. It is
    // hidden until a duel is active, so the normal screens are untouched.
    addChildComponent (duelStage);

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

    // Restore the profile character picked from the admin UI levels.
    restoreSelectedCharacter();

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

    // Social notifications: a stack of pop-up cards in the corner of the window
    // and, from the sidebar bell, a small history pop-up. Both live above every
    // section so they never get hidden by the screen that happens to be open.
    notificationToasts = std::make_unique<NotificationToasts>();
    notificationToasts->onOpenProfile =
        [safeThis](const juce::String& userId)
        {
            if (auto* self = safeThis.getComponent())
                self->openPublicProfile(userId);
        };
    notificationToasts->onSizeChanged = [safeThis]
    {
        if (auto* self = safeThis.getComponent())
        {
            self->resized();

            if (self->notificationToasts != nullptr)
                self->notificationToasts->toFront(false);

            if (self->notificationPopup != nullptr)
                self->notificationPopup->toFront(false);
        }
    };
    addAndMakeVisible(*notificationToasts);

    resized();
    repaint();
}

//==============================================================================
// Timer
//==============================================================================

void MainComponent::loadUiSoundPreferences()
{
    auto* settings = appProperties.getUserSettings();

    const bool enabled = settings != nullptr
                             ? settings->getBoolValue ("UiSoundsEnabled", true)
                             : true;
    const float volume = settings != nullptr
                             ? static_cast<float> (settings->getDoubleValue (
                                   "UiSoundsVolume", 0.8))
                             : 0.8f;
    const bool navigation = settings != nullptr
                                ? settings->getBoolValue ("UiSoundsNavigation", true)
                                : true;
    const bool actions = settings != nullptr
                             ? settings->getBoolValue ("UiSoundsActions", true)
                             : true;
    const bool system = settings != nullptr
                            ? settings->getBoolValue ("UiSoundsSystem", true)
                            : true;

    uiSounds.setPreferences (enabled, volume, navigation, actions, system);
}

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

    // Unread notification badge for the sidebar bell. A 20 s poll is enough for
    // social events; the request itself runs on the network pool.
    {
        const auto now = juce::Time::getMillisecondCounter();

        if (lastNotificationPollMs == 0 || now - lastNotificationPollMs > 20000)
        {
            lastNotificationPollMs = now;
            refreshSocialNotifications();
            refreshMessageUnread();
        }
    }

    // Keep the display awake while a song is loaded: playing the guitar produces
    // no input events, so the OS would otherwise dim the screen mid-session.
    updateScreenSaverForSession();

    // The fullscreen state can also change from the native window controls
    // (green button on macOS, maximize on Windows); keep the label honest.
    if (fullScreenButton.isVisible())
        updateFullScreenToggle();

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

        // Campaign: the next opponent's stems/chart may still be downloading
        // behind the victory card. That overlay is custom-painted from the
        // value copied above, so it has to be repainted on every tick while
        // the worker thread advances the shared fraction.
        if (gameState.load (std::memory_order_relaxed) == GameState::Finished
            && campaignFightActive)
        {
            overlayComponent.repaint();
        }
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

    // Online duel: poll the authoritative match state and drain resolved
    // notes. Both run through ApiClient's worker pool, so this tick only
    // schedules work and applies results on the message thread.
    updateOnlineDuel();

    // Show/hide the pixel-art stage as the duel state changes. setActive() is
    // idempotent; the check keeps the 30 Hz timer free of layout work. The
    // venue/opponent follow the current song's genre.
    const bool stageActive = isDuelStageActive();

    if (duelStage.isActive() != stageActive)
        duelStage.setActive (stageActive);

    // The duel strip changes on every exchange and its damage label fades, so
    // it needs a repaint while a duel is running. Only the strip region is
    // invalidated: a full overlay repaint would also repaint the tablature.
    if (duelActive.load (std::memory_order_relaxed)
        || duelOnlineActive.load (std::memory_order_relaxed)
        || duelOnlinePhase == DuelOnlinePhase::Playing
        || duelOnlinePhase == DuelOnlinePhase::Finishing
        || duelOnlinePhase == DuelOnlinePhase::Finished)
        repaintDuelHud();

    // Open duel rooms stay fresh while the song screen is visible: the host's
    // room appears for everyone within a few seconds without pressing REFRESH.
    if (duelLobby != nullptr
        && currentSection == AppSection::OnlineDuel
        && gameState.load (std::memory_order_relaxed) == GameState::SongSelect
        && onlineAccessToken.isNotEmpty()
        && ! showAuthScreen)
    {
        if (--duelLobbyRefreshTicks <= 0)
        {
            // The Online Duel section polls faster (~2 s); the Home/Songs hub
            // keeps the old ~5 s cadence.
            duelLobbyRefreshTicks = currentSection == AppSection::OnlineDuel ? 60 : 150;
            refreshDuelLobby();
        }
    }
    else
    {
        duelLobbyRefreshTicks = 150;
    }

    // Campaigns can be created or reordered in the admin panel while the
    // player keeps the CAMPAIGN section open. Re-read the public list every
    // ~10 seconds (silently: the visible list stays interactive).
    if (campaignPanel != nullptr
        && currentSection == AppSection::Campaign
        && gameState.load (std::memory_order_relaxed) == GameState::SongSelect
        && ! showAuthScreen)
    {
        if (--campaignRefreshTicks <= 0)
        {
            campaignRefreshTicks = 300;
            refreshServerCampaigns (true);
        }
    }
    else
    {
        campaignRefreshTicks = 300;
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
        audiblePlayheadBeat.load(
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

            // The auto miss must not fire before the (level- and speed-scaled)
            // late hit window closes. The extra allowance covers the pitch
            // analysis lag: a valid attack can be resolved a few buffers after
            // the note time, and auto-missing it first counted a real hit as a
            // miss (visible in online duels).
            const double timingWindowScale =
                ScoringSystem::getTimingWindowScale(
                    songLevel.load(std::memory_order_relaxed),
                    speedPercent.load(std::memory_order_relaxed));

            const auto missTimeoutSamples =
                static_cast<uint64_t>(
                    sampleRate
                    * juce::jmax(0.500, 0.150 * timingWindowScale + 0.300))
                + getScoringLatencySamples(sampleRate);

            // Resolve every target whose window has already passed, not just
            // one per tick: after the last note (or a knockout) the remaining
            // targets are overdue at once, and resolving one per 33 ms tick
            // delayed the finish screen by seconds.
            for (int guard = 0; guard < 64; ++guard)
            {
                // Online: once the server has decided the match (or this side
                // already asked to finish), the remaining chart notes are not
                // part of the duel and must not be counted as misses.
                if (duelOnlineActive.load (std::memory_order_relaxed)
                    && (duelOnlinePhase == DuelOnlinePhase::Finishing
                        || duelOnlinePhase == DuelOnlinePhase::Finished))
                    break;

                const int target =
                    gameSession.getTargetNoteIndex();

                if (target < 0
                    || target >= static_cast<int>(
                        gameSession.getMidiNotes().size()))
                    break;

                const auto targetSample =
                    getTargetSample(target, sampleRate);

                if (currentSample < targetSample + missTimeoutSamples)
                    break;

                if (! gameSession.resolveTarget(target))
                    break;

                onDuelNoteResolved(target,
                                   static_cast<int>(HitQuality::Miss));

                gameplayStats.recordMiss();
                gameSession.setTargetHit(true);

                juce::MessageManager::callAsync(
                    [this]
                    {
                        showHitFeedback("MISS", 4);
                    });

                advanceTarget();

                // The chart ended: advanceTarget switched to Finished and
                // asked the duel to finish.
                if (gameState.load(std::memory_order_relaxed)
                    != GameState::Playing)
                    break;
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

    // Never leave the display-sleep assertion behind for the next app.
    if (screenSaverSuppressed)
    {
        screenSaverSuppressed = false;
        juce::Desktop::setScreenSaverEnabled (true);
    }

    // An online duel that is still waiting or playing is resigned on the
    // server. Fire-and-forget: the API client outlives this destructor body.
    if (duelOnlineMatchId.isNotEmpty()
        && onlineAccessToken.isNotEmpty()
        && duelOnlinePhase != DuelOnlinePhase::None
        && duelOnlinePhase != DuelOnlinePhase::Finished)
        duelClient.leave (duelOnlineMatchId, onlineAccessToken, nullptr);

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
