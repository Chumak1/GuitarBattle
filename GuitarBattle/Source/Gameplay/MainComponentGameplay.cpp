#include "../MainComponent.h"

// These two implementations are compiled through this translation unit. Do not
// add them to GuitarBattle.jucer as separate compile units, and do not include
// them from any other .cpp file.
#include "../Generation/MidiMelodyGenerator.cpp"
#include "../Generation/SongArrangement.cpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

// Gameplay-only implementation for MainComponent: session lifecycle, tuning,
// MIDI targets and scoring.

void MainComponent::applySongTimeSignature(int numerator, int denominator)
{
    // 0 means "the server did not publish a meter": 4/4 is the historical
    // default and the value the admin form starts with.
    const int safeNumerator = numerator > 0 ? numerator : 4;
    const int safeDenominator = denominator > 0 ? denominator : 4;

    metronome.setTimeSignature(safeNumerator, safeDenominator);

    // The metronome normalises both values; everything else follows its copy
    // so the tablature, the header and the count-in cannot disagree.
    songSignatureNumerator = metronome.getBeatsPerBar();
    songSignatureDenominator = metronome.getBeatUnit();

    tablatureComponent.setTimeSignature(songSignatureNumerator,
                                        songSignatureDenominator);
}

//==============================================================================

juce::String MainComponent::formatTimeSignature(int numerator, int denominator)
{
    return juce::String(numerator > 0 ? numerator : 4)
        + "/"
        + juce::String(denominator > 0 ? denominator : 4);
}

//==============================================================================

double MainComponent::getSongBeatForSample(uint64_t sample) const
{
    const double sampleRate = cachedSampleRate.load(std::memory_order_relaxed);
    const double currentBpm = bpm.load(std::memory_order_relaxed);

    if (sampleRate <= 0.0 || currentBpm <= 0.0)
        return 0.0;

    const auto anchorSample = tempoAnchorSample.load(std::memory_order_relaxed);
    const double anchorBeat = tempoAnchorBeat.load(std::memory_order_relaxed);

    // Signed difference: samples before the anchor (count-in) yield a
    // negative beat instead of wrapping around.
    const double deltaSamples =
        static_cast<double>(static_cast<int64_t>(sample)
                            - static_cast<int64_t>(anchorSample));

    return anchorBeat + deltaSamples * currentBpm / (sampleRate * 60.0);
}

uint64_t MainComponent::getSongSampleForBeat(double beat) const
{
    const double sampleRate = cachedSampleRate.load(std::memory_order_relaxed);
    const double currentBpm = bpm.load(std::memory_order_relaxed);

    if (sampleRate <= 0.0 || currentBpm <= 0.0)
        return tempoAnchorSample.load(std::memory_order_relaxed);

    const auto anchorSample = tempoAnchorSample.load(std::memory_order_relaxed);
    const double anchorBeat = tempoAnchorBeat.load(std::memory_order_relaxed);

    const double deltaSamples = (beat - anchorBeat)
        * sampleRate * 60.0 / currentBpm;

    const auto result =
        static_cast<int64_t>(anchorSample)
        + static_cast<int64_t>(std::llround(deltaSamples));

    return result > 0 ? static_cast<uint64_t>(result) : 0;
}

void MainComponent::applyTempoPercent(double percent)
{
    if (tempoChangeInProgress.load(std::memory_order_relaxed))
    {
        // A change is already being prepared: remember the latest request
        // instead of silently dropping the click.
        tempoChangeQueued = true;
        queuedTempoPercent = percent;
        return;
    }

    const bool countingIn = countInActive.load(std::memory_order_acquire);

    const bool paused = gameState == GameState::Paused;

    if ((gameState == GameState::Playing && ! countingIn) || paused)
    {
        beginManagedTempoChange(percent);
        return;
    }

    // Before the song starts (or during the count-in) there is nothing to
    // freeze: re-render the stems and apply the new tempo directly.
    const double newBpm = baseBpm * percent / 100.0;

    prepareExternalAudioForSpeed(percent);

    speedPercent.store(percent, std::memory_order_relaxed);
    bpm.store(newBpm, std::memory_order_relaxed);
    audioPlaybackRate.store(percent / 100.0, std::memory_order_relaxed);

    metronome.setBpm(newBpm);
    gameSession.setBaseBpm(baseBpm);
    gameSession.setSpeedPercent(percent);
    gameSession.setBpm(newBpm);

    bpmLabel.setText(juce::String(static_cast<int>(std::round(newBpm))),
                     juce::dontSendNotification);
    tablatureComponent.setBpm(newBpm);
}

void MainComponent::beginManagedTempoChange(double percent)
{
    tempoChangeInProgress.store(true, std::memory_order_relaxed);

    tempoResumeState = gameState;
    pendingTempoPercent = percent;

    if (gameState == GameState::Paused)
    {
        // The transport is already frozen: anchor on the pause position so
        // the normal pause/resume shift lands on the same beat later.
        const auto pauseStart = pauseStartSample.load(std::memory_order_relaxed);
        pendingTempoSample = pauseStart;
        pendingTempoBeat = getSongBeatForSample(pauseStart);
    }
    else
    {
        pendingTempoSample = audioSampleCounter.load(std::memory_order_relaxed);
        pendingTempoBeat = getSongBeatForSample(pendingTempoSample);
    }

    // Freeze the whole transport before the heavy re-render starts: the player
    // must see a stable "preparing tempo" card, not a frozen window.
    tablatureComponent.setPlayheadRunning(false);
    metronome.pauseScheduler();
    metronomeSamplesRemaining = 0;
    metronomePlaybackPosition = 0;

    // The tempo change re-anchors the beat timeline. A gate scheduled for the
    // old anchor would stop the song at the wrong sample.
    practiceGateArmed.store(false, std::memory_order_relaxed);
    practiceGateSample.store(0, std::memory_order_relaxed);

    gameState = GameState::Preparing;

    juce::Component::SafePointer<MainComponent> safeThis(this);

    juce::Thread::launch([safeThis, percent]()
    {
        if (auto* self = safeThis.getComponent())
            self->prepareExternalAudioForSpeed(percent);

        juce::MessageManager::callAsync([safeThis]()
        {
            if (auto* self = safeThis.getComponent())
                self->finishManagedTempoChange();
        });
    });

    resized();
    repaint();
}

void MainComponent::finishManagedTempoChange()
{
    if (! tempoChangeInProgress.load(std::memory_order_relaxed))
        return;

    // The player may have left the game while the stems were rendering.
    if (gameState != GameState::Preparing)
    {
        tempoChangeInProgress.store(false, std::memory_order_relaxed);
        return;
    }

    // Playback stood still while the stems were rendered.
    const auto now = audioSampleCounter.load(std::memory_order_relaxed);
    const bool resumeToPlaying = tempoResumeState == GameState::Playing;

    if (resumeToPlaying)
    {
        const auto prepDuration = now > pendingTempoSample
            ? now - pendingTempoSample
            : 0;

        if (prepDuration > 0)
            gameplayStartSample.fetch_add(prepDuration, std::memory_order_relaxed);

        tempoAnchorBeat.store(pendingTempoBeat, std::memory_order_relaxed);
        tempoAnchorSample.store(now, std::memory_order_relaxed);

        // A practice hold stays frozen across the tempo change. The prep time
        // is already accounted for by the anchor move above, so the hold must
        // restart from the resume sample to avoid shifting the timeline twice.
        if (practiceHoldActive.load(std::memory_order_acquire))
            practiceHoldStartSample.store(now, std::memory_order_relaxed);
    }
    else
    {
        // Still paused: keep the anchor on the pause position, the regular
        // pause/resume shift will move it forward with the whole pause.
        tempoAnchorBeat.store(pendingTempoBeat, std::memory_order_relaxed);
        tempoAnchorSample.store(pendingTempoSample, std::memory_order_relaxed);
    }

    const double newBpm = baseBpm * pendingTempoPercent / 100.0;

    speedPercent.store(pendingTempoPercent, std::memory_order_relaxed);
    bpm.store(newBpm, std::memory_order_relaxed);
    audioPlaybackRate.store(pendingTempoPercent / 100.0, std::memory_order_relaxed);

    metronome.setBpm(newBpm);
    gameSession.setBaseBpm(baseBpm);
    gameSession.setSpeedPercent(pendingTempoPercent);
    gameSession.setBpm(newBpm);

    metronomeSamplesRemaining = 0;

    if (resumeToPlaying)
    {
        // Resume on the exact fractional beat with the new tempo: the old
        // song-start reference no longer maps to beat zero after an anchor
        // move, which used to shift the bar accent by one beat.
        metronome.resumeAtBeat(now, pendingTempoBeat);

        tablatureComponent.setPlayheadBeat(pendingTempoBeat);
        tablatureComponent.setPlayheadRunning(true);

        gameState = GameState::Playing;
        hitLabel.setText("READY", juce::dontSendNotification);
    }
    else
    {
        // Stay paused; metronome and tablature remain frozen.
        gameState = GameState::Paused;
        tablatureComponent.setPlayheadRunning(false);
        hitLabel.setText("PAUSED", juce::dontSendNotification);
    }

    bpmLabel.setText(juce::String(static_cast<int>(std::round(newBpm))),
                     juce::dontSendNotification);
    tablatureComponent.setBpm(newBpm);

    tempoChangeInProgress.store(false, std::memory_order_relaxed);

    resized();
    repaint();

    // The player may have picked another tempo while this one was rendering.
    if (tempoChangeQueued)
    {
        const double queued = queuedTempoPercent;
        tempoChangeQueued = false;

        if (! juce::approximatelyEqual(queued, pendingTempoPercent))
            applyTempoPercent(queued);
    }
}

void MainComponent::beginGameplayCountIn()
{
    if (gameState != GameState::Ready)
        return;

    // A new run invalidates the progression summary of the previous one.
    lastProgressionValid = false;
    lastAchievementsText.clear();

    {
        juce::String stemState;

        for (size_t i = 0; i < externalAudioTracks.size(); ++i)
        {
            const auto& track = externalAudioTracks[i];
            auto playback = std::atomic_load(&track.playbackAudio);
            stemState << " [" << static_cast<int>(i)
                      << " loaded=" << (track.loaded.load() ? 1 : 0)
                      << " samples=" << track.audio.getNumSamples()
                      << " playback=" << (playback != nullptr ? playback->getNumSamples() : -1)
                      << "]";
        }

        appendAudioDebugLog("gameplay start:" + stemState);
    }

    // The launch is counted and the server session is opened only now: the
    // Ready screen itself is not gameplay. A training run never opens one.
    if (! practiceMode)
        startOnlineGameSession();

    const auto currentSample =
        audioSampleCounter.load(
            std::memory_order_relaxed);

    const double clickIntervalSamples =
        metronome.getClickIntervalSamples();

    // One full bar of the current meter: 4/4 counts four clicks, 3/4 counts
    // three and 7/8 counts seven. The first click is the bar's downbeat and
    // the song starts on the next one, which is why the scheduler is reset at
    // click 0 instead of 1.
    const int countInClicks =
        juce::jlimit(1, 16, metronome.getBeatsPerBar());

    countInRemaining.store(countInClicks, std::memory_order_relaxed);
    countInTotal.store(countInClicks, std::memory_order_relaxed);
    countInActive.store(true, std::memory_order_release);
    tablatureComponent.setPlayheadRunning(false);

    const auto countInSamples =
        static_cast<uint64_t>(
            std::llround(clickIntervalSamples
                         * static_cast<double>(countInClicks)));

    gameplayStartSample.store(
        currentSample + countInSamples,
        std::memory_order_relaxed);

    // Beat 0 of the song timeline is the first sample after the count-in.
    tempoAnchorBeat.store(0.0, std::memory_order_relaxed);
    tempoAnchorSample.store(currentSample + countInSamples,
                            std::memory_order_relaxed);

    currentPlayheadBeat.store(0.0, std::memory_order_relaxed);
    tablatureComponent.setPlayheadBeat(0.0);

    metronome.resetScheduler(currentSample, 0);

    metronomeSamplesRemaining = 0;
    metronomePlaybackPosition = 0;

    // A new run must never inherit a gate or a hold scheduled by the previous
    // one. The hold is normally cleared by returnToSongSelect(), but the audio
    // thread can commit a gate concurrently with leaving the screen, so reset
    // it here unconditionally.
    practiceGateArmed.store(false, std::memory_order_relaxed);
    practiceGateSample.store(0, std::memory_order_relaxed);
    practiceHoldActive.store(false, std::memory_order_relaxed);
    practiceHoldStartSample.store(0, std::memory_order_relaxed);

    gameSession.setTargetNoteIndex(0);
    setTargetFromMidiIndex(0);

    if (practiceMode)
    {
        gameplayStats.reset();
        beginPracticeRun();
    }

    gameState = GameState::Playing;
    gameStartButton.setButtonText("START");
    menuButton.setButtonText("MAIN MENU");
    tunerStartButton.setEnabled(false);
    pauseButton.setButtonText("PAUSE");
    hitLabel.setText(practiceMode ? "PRACTICE - PLAY THE HIGHLIGHTED NOTE"
                                  : "COUNT-IN 3",
                     juce::dontSendNotification);

    resized();
    repaint();
}

void MainComponent::pauseGame()
{
    if (gameState != GameState::Playing)
        return;

    stateBeforePause =
        gameState;

    const auto currentSample =
        audioSampleCounter.load(
            std::memory_order_relaxed);

    pauseStartSample.store(
        currentSample,
        std::memory_order_relaxed);

    metronome.pauseScheduler();

    metronomeSamplesRemaining = 0;
    metronomePlaybackPosition = 0;

    // A scheduled practice gate belongs to the old timeline: the pause shift
    // moves the anchors, so the gate must be recomputed after the resume.
    practiceGateArmed.store(false, std::memory_order_relaxed);
    practiceGateSample.store(0, std::memory_order_relaxed);

    // Stop the tablature scroll: its own 60 Hz clock must not advance while
    // the game is paused.
    tablatureComponent.setPlayheadRunning(false);

    gameState =
        GameState::Paused;

    pauseButton.setButtonText(
        "RESUME");

    hitLabel.setText(
        "PAUSED",
        juce::dontSendNotification);

    resized();
    repaint();
}

void MainComponent::resumeGame()
{
    if (gameState != GameState::Paused)
        return;

    const auto currentSample =
        audioSampleCounter.load(
            std::memory_order_relaxed);

    const auto pauseStart =
        pauseStartSample.load(
            std::memory_order_relaxed);

    //==========================================================================

    uint64_t pauseDuration = 0;

    if (currentSample > pauseStart)
    {
        pauseDuration =
            currentSample - pauseStart;

        pausedSamples.fetch_add(
            pauseDuration,
            std::memory_order_relaxed);
    }

    //==========================================================================
    // IMPORTANT:
    //
    // Move gameplayStartSample forward by exactly the amount of time
    // spent paused.
    //
    // This means:
    //
    // MIDI beat 4 remains beat 4.
    // Target timing does NOT continue running during pause.
    //
    // Example:
    //
    // START        = sample 1000
    // target beat  = +50000 samples
    // pause        = 20000 samples
    //
    // after resume:
    //
    // gameplayStartSample = 21000
    // target sample       = 21000 + 50000
    //
    // So the target is delayed by the pause duration.

    if (pauseDuration > 0)
    {
        gameplayStartSample.fetch_add(
            pauseDuration,
            std::memory_order_relaxed);

        // The beat-mapped timeline is anchored to the audio sample counter,
        // so it has to move forward by the pause duration as well.
        tempoAnchorSample.fetch_add(
            pauseDuration,
            std::memory_order_relaxed);
    }

    //==========================================================================

    pauseStartSample.store(
        0,
        std::memory_order_relaxed);

    // A practice hold survives the pause. The anchors were just shifted by the
    // pause duration, so the hold must restart from the resume sample instead
    // of counting the paused time twice when it is released.
    if (practiceHoldActive.load(std::memory_order_acquire))
        practiceHoldStartSample.store(currentSample, std::memory_order_relaxed);

    // Resume on the shifted song grid so the metronome stays aligned with
    // the MIDI backing and the already-running external audio timeline.
    const auto resumeSongStart =
        gameplayStartSample.load(
            std::memory_order_relaxed);

    const double resumeBeat =
        getSongBeatForSample(currentSample);

    if (resumeBeat >= 0.0)
    {
        // Exact beat position keeps the bar accent intact across pauses.
        metronome.resumeAtBeat(currentSample, resumeBeat);
    }
    else
    {
        // Count-in / pre-song: fall back to the grid-based resume.
        metronome.resumeScheduler(
            currentSample,
            resumeSongStart);
    }

    metronomeSamplesRemaining = 0;

    // Resume the tablature from the same beat. During the count-in the visual
    // timeline stays frozen until the count-in ends; a practice hold keeps it
    // frozen too, because the transport is still waiting for the target.
    const bool practiceHeld =
        practiceHoldActive.load(std::memory_order_acquire);

    tablatureComponent.setPlayheadBeat(
        currentPlayheadBeat.load(std::memory_order_relaxed));
    tablatureComponent.setPlayheadRunning(
        ! countInActive.load(std::memory_order_acquire)
        && ! practiceHeld);

    gameState =
        stateBeforePause;

    pauseButton.setButtonText(
        "PAUSE");

    hitLabel.setText(
        practiceHeld ? "PLAY THE HIGHLIGHTED NOTE" : "READY",
        juce::dontSendNotification);

    resized();
    repaint();
}

void MainComponent::returnToSongSelect()
{
    // A training run ends here too: no state may leak into the next game.
    practiceActive.store(false, std::memory_order_relaxed);
    practiceHoldActive.store(false, std::memory_order_relaxed);
    practiceHoldStartSample.store(0, std::memory_order_relaxed);
    practiceGateArmed.store(false, std::memory_order_relaxed);
    practiceGateSample.store(0, std::memory_order_relaxed);
    practicePitchSample.store(0, std::memory_order_relaxed);
    practiceResumeSample = 0;
    practiceChordHold = false;
    practiceHoldUiApplied = false;
    practiceCompleted = false;

    countInActive.store(false, std::memory_order_release);
    countInRemaining.store(0, std::memory_order_relaxed);
    tablatureComponent.setPlayheadRunning(false);

    tunerActive.store(
        false,
        std::memory_order_relaxed);

    metronome.pauseScheduler();

    metronomeSamplesRemaining = 0;
    metronomePlaybackPosition = 0;

    gameState =
        GameState::SongSelect;

    gameStartButton.setButtonText("START");
    menuButton.setButtonText("MAIN MENU");

    gameSession.setTargetHit(false);
    gameSession.resetResolvedTarget();
    gameSession.setTargetNoteIndex(0);
    gameSession.setTargetNote("---");

    tuner.resetTunedStrings();

    pausedSamples.store(
        0,
        std::memory_order_relaxed);

    pauseStartSample.store(
        0,
        std::memory_order_relaxed);

    tempoAnchorBeat.store(
        0.0,
        std::memory_order_relaxed);

    tempoAnchorSample.store(
        0,
        std::memory_order_relaxed);

    currentPlayheadBeat.store(
        0.0,
        std::memory_order_relaxed);

    tunerStartButton.setEnabled(
        false);

    pauseButton.setButtonText(
        "PAUSE");

    resized();
    repaint();
}

MainComponent::SongEntry* MainComponent::findOnlineLibraryEntry(const juce::String& slug)
{
    if (slug.isEmpty())
        return nullptr;

    const auto found = std::find_if(songLibrary.begin(), songLibrary.end(),
        [&slug](const SongEntry& entry) { return entry.slug == slug; });

    return found == songLibrary.end() ? nullptr : &(*found);
}

int MainComponent::getBestScoreForSong(const juce::String& slug, const juce::String& title)
{
    // Server songs use the slug; the title key keeps scores saved by older
    // client versions.
    int best = getBestScore(title);

    if (slug.isNotEmpty())
    {
        best = juce::jmax(best, getBestScore(slug));

        const auto remote = remoteBestScores.find(slug);
        if (remote != remoteBestScores.end())
            best = juce::jmax(best, remote->second);
    }

    return best;
}

void MainComponent::startSelectedSong()
{
    // A new round starts from silence: the preview and the background track are
    // stopped before any game audio is scheduled.
    stopPreviewPlayback();
    backgroundPlayer.stop();

    // A catalog pick is always a normal session, never an official daily one.
    currentSessionIsDaily = false;

    if (catalogSongs.empty())
    {
        onlineCatalogStatusLabel.setText(
            "No songs loaded. Press REFRESH CATALOG to fetch the server catalogue.",
            juce::dontSendNotification);
        return;
    }

    selectedCatalogIndex = juce::jlimit(0, static_cast<int>(catalogSongs.size()) - 1,
                                        selectedCatalogIndex);
    const auto& song = catalogSongs[static_cast<size_t>(selectedCatalogIndex)];

    // Locked tiers cannot be started: the row already says which player level
    // is required, this is the second guard for the START button and the
    // Enter key.
    if (! isSongUnlocked(song))
    {
        onlineCatalogStatusLabel.setText(
            song.title + " unlocks at level "
                + juce::String(requiredPlayerLevelForSong(song.difficulty))
                + " (you are level " + juce::String(progressionLevel) + ").",
            juce::dontSendNotification);
        return;
    }

    // The published difficulty drives the per-note points multiplier.
    songLevel.store(juce::jlimit(1, 10, song.difficulty), std::memory_order_relaxed);

    // An unpublished draft is only playable in admin preview mode; the server
    // records that attempt as ``preview`` and ranks nothing. The label is
    // confirmed by the server answer in startOnlineGameSession().
    currentSessionIsPreview = ! song.published && adminPreviewMode && onlineIsAdmin;

    // Cached online songs are playable without any network traffic.
    if (auto* entry = findOnlineLibraryEntry(song.slug))
    {
        onlineSongUuid = song.serverId;
        if (loadSongMidi(entry->title))
        {
            beginTuningForSelectedSong();
            return;
        }
    }

    // Assets are downloaded on demand, never for the whole catalogue at once.
    // Say so before the request starts: otherwise the disabled START button
    // looks like a hang while the first stems arrive.
    onlineCatalogStatusLabel.setText(
        "Downloading " + song.title + "...", juce::dontSendNotification);
    songPridumayButton.setEnabled(false);
    downloadOnlineSong(song.slug, song.serverId, true);
}

bool MainComponent::loadSongAudio(const SongEntry& song)
{
    for (auto& track : externalAudioTracks)
    {
        track.audio.setSize(0, 0);
        track.loaded.store(false, std::memory_order_release);
        track.sourceSampleRate = 44100.0;
    }
    // Mixer audio index order: 0 drums, 1 bass, 2 rhythm guitar, 3 others.
    appendAudioDebugLog("loadSongAudio " + song.title
                        + " drums=" + song.drums.getFullPathName()
                        + " bass=" + song.bass.getFullPathName()
                        + " guitar=" + song.rhythmGuitar.getFullPathName()
                        + " others=" + song.others.getFullPathName());

    if (song.drums.existsAsFile()) loadExternalAudioTrack(0, song.drums);
    if (song.bass.existsAsFile()) loadExternalAudioTrack(1, song.bass);
    if (song.rhythmGuitar.existsAsFile()) loadExternalAudioTrack(2, song.rhythmGuitar);
    if (song.others.existsAsFile()) loadExternalAudioTrack(3, song.others);
    prepareExternalAudioForSpeed(speedPercent);

    int loadedTracks = 0;
    for (const auto& track : externalAudioTracks)
        if (track.loaded.load(std::memory_order_acquire))
            ++loadedTracks;

    appendAudioDebugLog("loadSongAudio done: loaded tracks=" + juce::String(loadedTracks));
    return true;
}

bool MainComponent::loadSongMidi(const juce::String& songId)
{
    auto found = std::find_if(songLibrary.begin(), songLibrary.end(),
        [&songId](const SongEntry& entry) { return entry.title == songId; });
    if (found == songLibrary.end())
    {
        DBG("Song not found in catalogue: " + songId);
        return false;
    }

    std::vector<TabNote> parsedNotes;
    appendAudioDebugLog("loadSongMidi " + songId
                        + " midi=" + found->soloMidi.getFullPathName()
                        + " size=" + juce::String(found->soloMidi.getSize())
                        + " slug=" + found->slug);
    currentMidiFile = found->soloMidi;

    if (!MidiParser::loadMidi(found->soloMidi, parsedNotes, currentTuning)
        || parsedNotes.empty())
    {
        appendAudioDebugLog("MIDI LOAD FAILED " + found->soloMidi.getFullPathName());
        DBG("MIDI load failed: " + found->soloMidi.getFullPathName());
        return false;
    }
    appendAudioDebugLog("MIDI OK notes=" + juce::String(static_cast<int>(parsedNotes.size())));

    selectedSong = *found;
    if (found->slug.isEmpty())
    {
        // No server identity: an online game session cannot be opened.
        onlineSongUuid.clear();
        onlineGameSessionUuid.clear();
    }
    selectedSongId = found->title;
    selectedSongSlug = found->slug;
    dailyChallengeLoaded.store(false, std::memory_order_relaxed);
    gameSongTitleLabel.setText(found->title, juce::dontSendNotification);

    // Read the MIDI tempo map and the time-signature meta event. The tempo is
    // always taken from the file (the stems are rendered at that tempo); the
    // meter prefers the admin value published by the server and falls back to
    // the file, then to 4/4.
    juce::MidiFile tempoMidi;
    int midiSignatureNumerator = 0;
    int midiSignatureDenominator = 0;
    bool tempoFound = false;
    bool signatureFound = false;
    juce::FileInputStream tempoInput(found->soloMidi);
    if (tempoInput.openedOk() && tempoMidi.readFrom(tempoInput))
    {
        for (int t = 0;
             t < tempoMidi.getNumTracks() && ! (tempoFound && signatureFound);
             ++t)
            if (const auto* track = tempoMidi.getTrack(t))
                for (int e = 0; e < track->getNumEvents(); ++e)
                {
                    const auto& message = track->getEventPointer(e)->message;

                    if (! tempoFound && message.isTempoMetaEvent())
                    {
                        const double secondsPerBeat = message.getTempoSecondsPerQuarterNote();
                        if (secondsPerBeat > 0.0)
                        {
                            found->bpm = 60.0 / secondsPerBeat;
                            tempoFound = true;
                        }
                    }

                    if (! signatureFound && message.isTimeSignatureMetaEvent())
                    {
                        int numerator = 0;
                        int denominator = 0;
                        message.getTimeSignatureInfo(numerator, denominator);

                        if (numerator > 0 && denominator > 0)
                        {
                            midiSignatureNumerator = numerator;
                            midiSignatureDenominator = denominator;
                            signatureFound = true;
                        }
                    }

                    if (tempoFound && signatureFound)
                        break;
                }
    }
    const double midiBpm = juce::jlimit(30.0, 300.0, found->bpm);

    // The admin value wins: a chart may be written in 3/4 or 7/8 and the
    // server stores it explicitly. The MIDI meta event only fills the gap for
    // local files and for servers that predate the field.
    applySongTimeSignature(found->timeSignatureNumerator > 0
                               ? found->timeSignatureNumerator
                               : midiSignatureNumerator,
                           found->timeSignatureDenominator > 0
                               ? found->timeSignatureDenominator
                               : midiSignatureDenominator);

    // The chart runs to the end of the MIDI file: no fixed duration cap.
    // Instrumental stems are loaded to their full length as well.
    loadSongAudio(*found);
    baseBpm = midiBpm;

    const double actualBpm = baseBpm * speedPercent / 100.0;
    bpm.store(actualBpm, std::memory_order_relaxed);
    audioPlaybackRate.store(speedPercent / 100.0, std::memory_order_relaxed);
    metronome.setBpm(actualBpm);
    gameSession.setBaseBpm(baseBpm);
    gameSession.setSpeedPercent(speedPercent);
    gameSession.setBpm(actualBpm);
    bpmLabel.setText(juce::String(static_cast<int>(std::round(actualBpm))), juce::dontSendNotification);
    gameSession.getMidiNotes() = std::move(parsedNotes);
    tablatureComponent.setNotes(gameSession.getMidiNotes());
    tablatureComponent.setBpm(actualBpm);
    gameSession.setTargetNoteIndex(0);
    gameSession.resetResolvedTarget();
    setTargetFromMidiIndex(0);
    updateSongBestScoreLabels();
    DBG("Selected song: " + found->title + ", BPM=" + juce::String(baseBpm)
        + ", notes=" + juce::String((int)gameSession.getMidiNotes().size()));
    return true;
}

bool MainComponent::generateDailyChallengeMidi(bool forceRandomSeed)
{
    MidiMelodyGenerator::Settings settings;
    settings.difficulty = 1;
    settings.durationSeconds = 20;
    settings.bpm = 110.0;
    const int selectedStyleId = melodyStyleCombo.getSelectedId();
    // Genre-specific tempo ranges are intentionally distinct. The same selected BPM
    // is passed to the lead and backing generators so their MIDI remains synchronized.
    const int tempoSeed = forceRandomSeed ? juce::Random::getSystemRandom().nextInt()
        : juce::Time::getCurrentTime().getYear() * 10000
          + static_cast<int>(juce::Time::getCurrentTime().getMonth()) * 100
          + static_cast<int>(juce::Time::getCurrentTime().getDayOfMonth());
    const int tempoRoll = static_cast<int>(static_cast<unsigned int>(tempoSeed) & 0x7fffffffU);
    auto chooseTempo = [tempoRoll](int low, int high)
    {
        return static_cast<double>(low + (tempoRoll % (high - low + 1)));
    };
    switch (selectedStyleId)
    {
        case 1:  settings.bpm = chooseTempo(95, 155); break;  // Steve Vai / expressive rock
        case 2:  settings.bpm = chooseTempo(170, 200); break; // Power Rock reference range
        case 3:  settings.bpm = chooseTempo(95, 150); break;  // Progressive Metal
        case 4:  settings.bpm = chooseTempo(80, 130); break;  // Blues Rock
        case 5:  settings.bpm = chooseTempo(95, 145); break;  // Hard Rock
        case 6:  settings.bpm = chooseTempo(110, 155); break; // Polyphia-inspired
        case 7:  settings.bpm = chooseTempo(145, 180); break; // Trivium-inspired
        case 8:  settings.bpm = chooseTempo(90, 145); break;  // Djent
        case 9:  settings.bpm = chooseTempo(145, 180); break; // Neoclassical Metal
        case 10: settings.bpm = chooseTempo(90, 140); break;  // Classic Rock
        case 11: settings.bpm = chooseTempo(115, 165); break; // Heavy Metal
        case 12: settings.bpm = chooseTempo(140, 180); break; // Symphonic Metal
        case 13: settings.bpm = chooseTempo(130, 180); break; // Technical Metal
        case 14: settings.bpm = chooseTempo(125, 175); break; // Modern Metalcore
        case 15: settings.bpm = chooseTempo(170, 200); break; // Guitar Fusion
        case 16: settings.bpm = chooseTempo(70, 115); break;  // Blues
        case 17: settings.bpm = chooseTempo(90, 125); break;  // Funk / Soul
        case 18: settings.bpm = chooseTempo(100, 155); break; // Jazz Fusion
        case 19: settings.bpm = chooseTempo(80, 130); break;  // Alternative / Indie
        case 20: settings.bpm = chooseTempo(65, 110); break;  // Cinematic / Experimental
        case 21: settings.bpm = chooseTempo(80, 125); break;  // Country / Americana
        default: settings.bpm = chooseTempo(170, 200); break;
    }
    settings.bpm = juce::jlimit(170.0, 200.0, settings.bpm);
    settings.style = MidiMelodyGenerator::Style::PowerMetal; // Single available style: Power Rock.

    // Normal startup keeps the daily challenge stable for the local calendar day.
    // The NEW GENERATION test button deliberately bypasses that seed so each click
    // can explore another melody and a matching backing arrangement.
    const auto today = juce::Time::getCurrentTime();
    if (forceRandomSeed)
    {
        settings.seed = juce::Random::getSystemRandom().nextInt();
        if (settings.seed == 0)
            settings.seed = 1;
    }
    else
    {
        settings.seed = today.getYear() * 10000 + static_cast<int>(today.getMonth()) * 100
                        + static_cast<int>(today.getDayOfMonth());
    }

    const auto outputRoot = juce::File::getSpecialLocation(
        juce::File::userDocumentsDirectory)
        .getChildFile("MusicMastersArena")
        .getChildFile("GeneratedChallenges");

    const auto dateTag = today.formatted("%Y%m%d");
    const auto outputDirectory = outputRoot.getChildFile(dateTag);
    dailyChallengeMidiFile = outputDirectory.getChildFile("DailyChallenge_" + dateTag + "_Guitar.mid");

    MidiMelodyGenerator::Result generated;
    juce::String error;
    if (!MidiMelodyGenerator::generate(settings, dailyChallengeMidiFile, generated, &error))
    {
        DBG("DAILY CHALLENGE GENERATION FAILED: " + error);
        generateMelodyButton.setEnabled(false);
        generateMelodyButton.setButtonText("Daily Challenge Monday (unavailable)");
        return false;
    }

    // Generate the backing parts from the same deterministic daily seed.
    // The guitar-only MIDI above remains the sole source for tablature/scoring.
    SongArrangement::Settings arrangementSettings;
    arrangementSettings.seed = settings.seed;
    arrangementSettings.bpm = settings.bpm;
    arrangementSettings.style = static_cast<int>(settings.style);
    // Match the backing length to the generated guitar challenge duration.
    arrangementSettings.bars = juce::jlimit(4, 32,
        static_cast<int>(std::ceil((settings.durationSeconds * settings.bpm / 60.0) / 4.0)));
    SongArrangement::Result arrangement;
    if (!SongArrangement::generateBacking(arrangementSettings, outputDirectory, arrangement, &error))
    {
        DBG("DAILY CHALLENGE BACKING GENERATION FAILED: " + error);
        generateMelodyButton.setEnabled(false);
        generateMelodyButton.setButtonText("Daily Challenge Monday (unavailable)");
        return false;
    }

    if (!loadBackingMidiEvents(outputDirectory))
        DBG("DAILY CHALLENGE: backing MIDI files were generated but could not be loaded for playback.");

    generateMelodyButton.setEnabled(true);
    generateMelodyButton.setButtonText("Daily Challenge Monday");
    DBG("DAILY CHALLENGE MIDI CREATED: " + dailyChallengeMidiFile.getFullPathName()
        + ", seed=" + juce::String(settings.seed)
        + ", notes=" + juce::String(static_cast<int>(generated.notes.size()))
        + ", backing=" + arrangement.backingMidi.getFullPathName()
        + ", chords=" + arrangement.progression);
    return true;
}

bool MainComponent::loadBackingMidiEvents(const juce::File& directory)
{
    backingNotes.clear();
    struct Source { const char* fileName; int instrument; };
    const Source sources[] = {
        { "DailyChallenge_Bass.mid", 1 },
        { "DailyChallenge_Drums.mid", 2 },
        { "DailyChallenge_Piano.mid", 3 },
        { "DailyChallenge_RhythmGuitar.mid", 4 }
    };
    int loadedFiles = 0;
    for (const auto& source : sources)
    {
        juce::FileInputStream stream(directory.getChildFile(source.fileName));
        if (!stream.openedOk())
            continue;
        juce::MidiFile midi;
        if (!midi.readFrom(stream))
            continue;
        const int tpq = midi.getTimeFormat();
        if (tpq <= 0)
            continue;
        for (int trackIndex = 0; trackIndex < midi.getNumTracks(); ++trackIndex)
        {
            auto* track = midi.getTrack(trackIndex);
            if (track == nullptr)
                continue;
            double startTicks[128]{};
            int velocities[128]{};
            bool active[128]{};
            for (int eventIndex = 0; eventIndex < track->getNumEvents(); ++eventIndex)
            {
                const auto& message = track->getEventPointer(eventIndex)->message;
                if (message.isNoteOn())
                {
                    const int note = message.getNoteNumber();
                    if (note >= 0 && note < 128)
                    {
                        startTicks[note] = message.getTimeStamp();
                        velocities[note] = message.getVelocity();
                        active[note] = true;
                    }
                }
                else if (message.isNoteOff())
                {
                    const int note = message.getNoteNumber();
                    if (note >= 0 && note < 128 && active[note])
                    {
                        const double startBeat = startTicks[note] / static_cast<double>(tpq);
                        const double endBeat = message.getTimeStamp() / static_cast<double>(tpq);
                        BackingNote item;
                        item.startBeat = juce::jmax(0.0, startBeat);
                        item.durationBeats = juce::jmax(0.04, endBeat - startBeat);
                        item.midiNote = note;
                        item.velocity = velocities[note];
                        item.instrument = source.instrument;
                        backingNotes.push_back(item);
                        active[note] = false;
                    }
                }
            }
        }
        ++loadedFiles;
    }
    std::sort(backingNotes.begin(), backingNotes.end(),
        [](const BackingNote& a, const BackingNote& b) { return a.startBeat < b.startBeat; });
    DBG("DAILY CHALLENGE BACKING EVENTS: files=" + juce::String(loadedFiles)
        + ", notes=" + juce::String(static_cast<int>(backingNotes.size())));
    return loadedFiles == 4 && !backingNotes.empty();
}

bool MainComponent::loadGeneratedMelody()
{
    // Do not use generator-returned notes here: read the generated .mid file
    // with the exact same parser used for the three built-in songs.
    if (!dailyChallengeMidiFile.existsAsFile() && !generateDailyChallengeMidi())
        return false;

    std::vector<TabNote> parsedNotes;
    currentMidiFile = dailyChallengeMidiFile;

    if (!MidiParser::loadMidi(dailyChallengeMidiFile, parsedNotes, currentTuning)
        || parsedNotes.empty())
    {
        DBG("DAILY CHALLENGE MIDI PARSE FAILED: " + dailyChallengeMidiFile.getFullPathName());
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Daily Challenge unavailable",
            "The generated MIDI could not be read by the MIDI parser.");
        return false;
    }

    selectedSongId = "daily_challenge_monday";
    selectedSongSlug.clear();
    dailyChallengeLoaded.store(true, std::memory_order_relaxed);
    gameSongTitleLabel.setText("Daily Challenge Monday", juce::dontSendNotification);

    // The MIDI parser reads the tempo from the saved file; this MVP uses 110 BPM.
    baseBpm = 110.0;
    const double actualBpm = baseBpm * speedPercent / 100.0;
    bpm.store(actualBpm, std::memory_order_relaxed);
    audioPlaybackRate.store(speedPercent / 100.0, std::memory_order_relaxed);
    metronome.setBpm(actualBpm);

    // The generator writes a 4/4 time-signature meta event; applying it here
    // keeps the metronome in step after a 7/8 server song was played.
    applySongTimeSignature(4, 4);
    gameSession.setBaseBpm(baseBpm);
    gameSession.setSpeedPercent(speedPercent);
    gameSession.setBpm(actualBpm);
    bpmLabel.setText(juce::String(static_cast<int>(std::round(actualBpm))),
                     juce::dontSendNotification);

    gameSession.getMidiNotes() = std::move(parsedNotes);
    tablatureComponent.setNotes(gameSession.getMidiNotes());
    tablatureComponent.setBpm(actualBpm);
    gameSession.setTargetNoteIndex(0);
    gameSession.resetResolvedTarget();
    setTargetFromMidiIndex(0);
    updateSongBestScoreLabels();

    DBG("DAILY CHALLENGE LOADED THROUGH MidiParser: notes="
        + juce::String(static_cast<int>(gameSession.getMidiNotes().size()))
        + ", MIDI=" + dailyChallengeMidiFile.getFullPathName());
    return true;
}

void MainComponent::beginTuningForSelectedSong()
{
    tuner.resetTunedStrings();
    syncTuningUi();
    tunerActive.store(true, std::memory_order_relaxed);
    previousRmsValue = 0.0;
    previousRmsValue2 = 0.0;
    lastOnsetSample.store(0, std::memory_order_relaxed);
    lastNoteOnSample.store(0, std::memory_order_relaxed);
    tunerStartButton.setEnabled(false);
    tunerDetectedString = -1;
    tunerFrequency = 0.0;
    tunerCents = 0.0;
    tunerNoteLabel.setText("---", juce::dontSendNotification);
    tunerFrequencyLabel.setText("--- Hz", juce::dontSendNotification);
    tunerStringLabel.setText("---", juce::dontSendNotification);
    tunerCentsLabel.setText("--- cents", juce::dontSendNotification);
    tunerStatusLabel.setText("PLAY A STRING", juce::dontSendNotification);
    tunerStatusLabel.setColour(juce::Label::textColourId, UiColors::blue);
    tunerNoteLabel.setColour(juce::Label::textColourId, UiColors::blue);
    tunerCentsLabel.setColour(juce::Label::textColourId, UiColors::text);
    gameState = GameState::Tuner;
    resized();
    repaint();
}

void MainComponent::setCurrentTuning(const GuitarTuning::Tuning& tuning,
                                     const juce::String& name,
                                     bool persist)
{
    // Normalise the value once: the slot count stays in range, unused slots
    // are marked empty and every active note is a valid MIDI number.
    GuitarTuning::Tuning nextTuning = tuning;
    nextTuning.count = juce::jlimit(1, GuitarTuning::maxStrings, nextTuning.count);

    for (int slot = 0; slot < GuitarTuning::maxStrings; ++slot)
    {
        if (slot >= nextTuning.count)
        {
            nextTuning.notes[static_cast<size_t>(slot)] = GuitarTuning::emptyNote;
            continue;
        }

        const int note = nextTuning.notes[static_cast<size_t>(slot)];

        if (note != GuitarTuning::emptyNote)
            nextTuning.notes[static_cast<size_t>(slot)] = juce::jlimit(24, 96, note);
    }

    // A tuning with no playable string would silently produce an empty chart.
    if (GuitarTuning::activeCount(nextTuning) == 0)
        return;

    const bool changed = ! GuitarTuning::sameTuning(nextTuning, currentTuning);

    currentTuning = nextTuning;
    currentTuningName = name.isNotEmpty() ? name : juce::String("CUSTOM");

    // Tuner targets and tab positions share one source of truth.
    tuner.setTuning(currentTuning);
    tablatureComponent.setTuning(currentTuning, currentTuningName);

    if (persist)
    {
        if (auto* settings = appProperties.getUserSettings())
        {
            // The list length is the slot count and -1 marks an empty string,
            // so the selector restores exactly what the player configured.
            juce::StringArray values;
            for (int slot = 0; slot < currentTuning.count; ++slot)
                values.add(juce::String(currentTuning.notes[static_cast<size_t>(slot)]));

            settings->setValue("TuningNotes", values.joinIntoString(","));
            settings->setValue("TuningName", currentTuningName);
            settings->saveIfNeeded();
        }
    }

    if (changed)
    {
        rebuildTablatureForTuning();
        repaint();
    }
}

void MainComponent::rebuildTablatureForTuning()
{
    // The selector is only shown before the song starts. Refuse a rebuild
    // while the transport is running in case a future UI allows it.
    if (gameState == GameState::Playing)
        return;

    if (!currentMidiFile.existsAsFile())
        return;

    std::vector<TabNote> parsedNotes;

    if (!MidiParser::loadMidi(currentMidiFile, parsedNotes, currentTuning)
        || parsedNotes.empty())
        return;

    gameSession.getMidiNotes() = std::move(parsedNotes);
    tablatureComponent.setNotes(gameSession.getMidiNotes());
    tablatureComponent.setTuning(currentTuning, currentTuningName);

    // The first target points at the first note of the rebuilt chart.
    gameSession.setTargetNoteIndex(0);
    gameSession.resetResolvedTarget();
    setTargetFromMidiIndex(0);
}

int MainComponent::getBestScore(const juce::String& songId)
{
    if (auto* settings = appProperties.getUserSettings())
        return settings->getIntValue(juce::String("BestScore_") + songId, 0);
    return 0;
}

void MainComponent::saveBestScoreForSelectedSong(int score)
{
    if (auto* settings = appProperties.getUserSettings())
    {
        // Server songs are keyed by slug. The title key written by older client
        // versions is still read as a fallback in getBestScoreForSong().
        const auto key = juce::String("BestScore_")
            + (selectedSongSlug.isNotEmpty() ? selectedSongSlug : selectedSongId);
        const int previous = settings->getIntValue(key, 0);
        if (score > previous)
        {
            settings->setValue(key, score);
            settings->saveIfNeeded();
        }
    }
    updateSongBestScoreLabels();
}

void MainComponent::setTargetFromMidiIndex(
    int index)
{
    if (gameSession.getMidiNotes().empty())
    {
        gameSession.setTargetNote("---");

        targetNoteLabel.setText(
            "---",
            juce::dontSendNotification);

        return;
    }

    index =
        juce::jlimit(
            0,
            static_cast<int>(
                gameSession.getMidiNotes().size()) - 1,
            index);

    gameSession.setTargetNoteIndex(index);

    const int midi =
        gameSession.getMidiNotes()[index].midiNote;

    static const char* names[] =
    {
        "C",
        "C#",
        "D",
        "D#",
        "E",
        "F",
        "F#",
        "G",
        "G#",
        "A",
        "A#",
        "B"
    };

    const int noteIndex =
        (midi % 12 + 12) % 12;

    const int octave =
        (midi / 12) - 1;

    const auto noteName =
        juce::String(
            names[noteIndex])
        + juce::String(
            octave);

    gameSession.setTargetNote(
        noteName);

    targetNoteLabel.setText(
        noteName,
        juce::dontSendNotification);
}

juce::Range<double> MainComponent::getPitchAnalysisRange() const
{
    double minFrequency = 60.0;
    double maxFrequency = 700.0;

    if (gameState.load(std::memory_order_relaxed) == GameState::Playing)
    {
        const int targetIndex = gameSession.getTargetNoteIndex();
        const auto& midiNotes = gameSession.getMidiNotes();

        if (targetIndex >= 0
            && targetIndex < static_cast<int>(midiNotes.size()))
        {
            const double targetFrequency =
                juce::MidiMessage::getMidiNoteInHertz(
                    midiNotes[static_cast<size_t>(targetIndex)].midiNote);

            // High frets on the first two strings go above the default 700 Hz
            // ceiling. A window around the target keeps them detectable while
            // avoiding octave errors on the low strings.
            if (targetFrequency > 600.0)
            {
                minFrequency = juce::jmax(60.0, targetFrequency * 0.75);
                maxFrequency = juce::jmin(1500.0, targetFrequency * 1.35);
            }
        }
    }

    return { minFrequency, maxFrequency };
}

uint64_t MainComponent::getTargetSample(
    int index,
    double sampleRate) const
{
    if (index < 0
        || index >= static_cast<int>(
            gameSession.getMidiNotes().size())
        || sampleRate <= 0.0)
    {
        return 0;
    }

    const double beat =
        gameSession.getMidiNotes()[index].startBeat;

    if (beat < 0.0)
        return 0;

    juce::ignoreUnused(sampleRate);

    // Beat-to-sample mapping honours tempo changes: the current tempo applies
    // from the anchor point, not retroactively from the start of the song.
    return getSongSampleForBeat(beat);
}

void MainComponent::advanceTarget()
{
    const int currentIndex =
        gameSession.getTargetNoteIndex();

    // GameSession owns target-index transitions and per-target resets.
    const int nextIndex =
        gameSession.advanceTargetIndex();

    //==========================================================================
    // Finished

    if (nextIndex >=
        static_cast<int>(
            gameSession.getMidiNotes().size()))
    {
        DBG("ADVANCE: "
            + juce::String(currentIndex)
            + " -> "
            + juce::String(nextIndex));

        gameState =
            GameState::Finished;

        juce::MessageManager::callAsync(
            [this]
            {
                saveBestScoreForSelectedSong(gameplayStats.getScore());
                submitOnlineGameResult();
                gameStartButton.setButtonText("PLAY AGAIN");
                menuButton.setButtonText("MAIN MENU");

                hitLabel.setText(
                    "FINISHED",
                    juce::dontSendNotification);

                targetNoteLabel.setText(
                    "---",
                    juce::dontSendNotification);

                resized();
                repaint();
            });

        return;
    }

    //==========================================================================
    // Next target

    juce::MessageManager::callAsync(
        [this, nextIndex]
        {
            const int midi =
                gameSession.getMidiNotes()[nextIndex].midiNote;

            static const char* names[] =
            {
                "C",
                "C#",
                "D",
                "D#",
                "E",
                "F",
                "F#",
                "G",
                "G#",
                "A",
                "A#",
                "B"
            };

            const int noteIndex =
                (midi % 12 + 12) % 12;

            const int octave =
                (midi / 12) - 1;

            const auto noteName =
                juce::String(
                    names[noteIndex])
                + juce::String(
                    octave);

            gameSession.setTargetNote(
                noteName);

            targetNoteLabel.setText(
                noteName,
                juce::dontSendNotification);

            // Only the target label changed. Repainting the whole window here
            // forced the tablature to repaint once per note on top of its own
            // 60 Hz clock.
            targetNoteLabel.repaint();
        });
}

