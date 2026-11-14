#include "../MainComponent.h"
#include "../UI/OnlineDuelRoom.h"

namespace
{
    // Temporary diagnostics for the duel flow: notes whether the match was
    // created and what every exchange applied. Written next to the settings,
    // only from the message/analysis threads that already log elsewhere.
    void duelDebugLog (const juce::String& line)
    {
        auto directory = juce::File::getSpecialLocation (
                             juce::File::userApplicationDataDirectory)
                             .getChildFile ("GuitarBattle");

        directory.createDirectory();
        directory.getChildFile ("duel-debug.log")
                 .appendText (juce::Time::getCurrentTime().toString (true, true)
                              + "  " + line + "\n");
    }

    DuelQuality toDuelQuality (int hitQuality)
    {
        switch (static_cast<HitQuality> (hitQuality))
        {
            case HitQuality::Perfect: return DuelQuality::Perfect;
            case HitQuality::Great:   return DuelQuality::Great;
            case HitQuality::Good:    return DuelQuality::Good;
            case HitQuality::Miss:    break;
        }

        return DuelQuality::Miss;
    }

    // Wire token used by POST /duels/{id}/events and the WS `note` message.
    juce::String toDuelQualityToken (int hitQuality)
    {
        switch (static_cast<HitQuality> (hitQuality))
        {
            case HitQuality::Perfect: return "perfect";
            case HitQuality::Great:   return "great";
            case HitQuality::Good:    return "good";
            case HitQuality::Miss:    break;
        }

        return "miss";
    }

    void publishHealth (const DuelMatch& match,
                        std::atomic<int>& playerMilli,
                        std::atomic<int>& opponentMilli)
    {
        playerMilli.store (static_cast<int> (
            std::lround (match.getPlayerHealth() * 1000.0)),
            std::memory_order_relaxed);
        opponentMilli.store (static_cast<int> (
            std::lround (match.getOpponentHealth() * 1000.0)),
            std::memory_order_relaxed);
    }
}

//==============================================================================
// Guitar Duels (client side vs bots)

void MainComponent::resetDuelState()
{
    // Leaving the game while an online match is waiting or playing resigns it
    // on the server (POST /leave). The request runs on the worker pool, so it
    // never blocks the message thread.
    if (duelOnlinePhase != DuelOnlinePhase::None
        && duelOnlinePhase != DuelOnlinePhase::Finished)
        cancelOnlineDuel (true);
    else
        clearOnlineDuelState();

    {
        const juce::SpinLock::ScopedLockType lock (duelLock);
        duelMatch.reset();
    }

    duelActive.store (false, std::memory_order_relaxed);
    duelPlayerHealthMilli.store (100000, std::memory_order_relaxed);
    duelOpponentHealthMilli.store (100000, std::memory_order_relaxed);
    duelLastExchange.store (0, std::memory_order_relaxed);
    duelLastDamageMilli.store (0, std::memory_order_relaxed);
    duelFlashUntilMs.store (0.0, std::memory_order_relaxed);
    duelStageResultActive = false;
    duelResultCardInSession = false;
}

void MainComponent::clearOnlineDuelState()
{
    // The ready prompt belongs to the match being cleared: close it without
    // letting its callback cancel the next flow (the generation is bumped by
    // the caller before this runs).
    if (auto* prompt = duelOnlineReadyDialog.getComponent())
    {
        duelOnlineReadyPromptClosing = true;
        prompt->exitModalState (1);
        duelOnlineReadyPromptClosing = false;
    }

    duelOnlineReadyDialog = nullptr;
    duelOnlineReadyConfirmed = false;
    duelOnlineReadyPromptShown = false;

    {
        const juce::SpinLock::ScopedLockType lock (duelLock);
        duelOnlinePendingEvents.clear();
        duelOnlineInFlightEvents.clear();
        duelOnlineLastQueuedIndex = -1;
    }

    duelOnlineMatch = {};
    duelOnlineMatchId.clear();
    duelOnlineLobbyId.clear();
    duelOnlineConfirmedInRoom = false;
    duelOnlinePreloadedSlug.clear();
    duelOnlineSongId.clear();
    duelOnlineSongSlug.clear();
    duelOnlineOpponentName.clear();
    onlineCharactersAppliedKey.clear();
    duelOnlineExpectedNotes = 0;
    duelOnlinePollFailures = 0;
    duelOnlineFinishAttempts = 0;
    duelOnlinePlaybackPending = false;
    duelOnlineChartReady = false;
    duelOnlineChartReloadAttempted = false;
    duelOnlineStartAborted = false;
    duelOnlinePollInFlight = false;
    duelOnlineReadyInFlight = false;
    duelOnlineEventInFlight = false;
    duelOnlineFinishRequested = false;
    duelOnlineFinishSent = false;
    duelOnlineFinishInFlight = false;
    duelOnlineResultInFlight = false;
    duelOnlineEventSendingStopped = false;
    duelOnlineDownloadInProgress = false;
    duelOnlineDownloadJoinAfter = false;
    duelOnlinePreloadInProgress = false;
    duelOnlineDownloadMatchId.clear();
    duelOnlineDownloadSlug.clear();
    duelOnlineDownloadPercentShown = -1;
    duelOnlineNextPollMs = 0.0;
    duelOnlineNextEventMs = 0.0;
    duelOnlineNextReadyMs = 0.0;
    duelOnlineSnapshotAtMs = 0.0;
    duelOnlineOneWayMs = 0.0;

    duelOnlineActive.store (false, std::memory_order_relaxed);
    duelOnlineNoteCount.store (0, std::memory_order_relaxed);
    duelOnlinePhase = DuelOnlinePhase::None;
}

void MainComponent::requestDuelWithBot (int botLevel)
{
    matchOriginSection = currentSection;
    // Every duel runs at the analysed 100% tempo, exactly like the server's
    // frozen chart and the bot's note timeline. Refuse to start while a tempo
    // change is still rendering.
    if (! prepareDuelTempo())
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::InfoIcon,
            "Tempo change in progress",
            "A tempo change is still being prepared. Try the duel again in a moment.",
            "OK", this);
        return;
    }

    // A chart download is already running: its completion starts the pending
    // duel with the fresh chart, so a second request must not queue another
    // download or clear the state underneath it.
    if (songDownloadProgressState != nullptr)
    {
        duelDebugLog ("duel request ignored: song download in progress");
        return;
    }

    duelPendingLevel = static_cast<DuelBotLevel> (
        juce::jlimit (0, 2, botLevel));
    duelPending = true;

    // Practice mode never scores: a duel must run the competitive path.
    if (practiceMode)
        setPracticeMode (false);

    // Keep the finished match (and its victory/defeat card) on screen until the
    // next chart is actually ready. beginPendingDuelIfNeeded() replaces it once
    // the song is loaded; clearing it here dropped the player onto the generic
    // SONG COMPLETE card whenever the next stage still had to be downloaded.
    if (duelOnlinePhase != DuelOnlinePhase::None
        && duelOnlinePhase != DuelOnlinePhase::Finished)
        cancelOnlineDuel (true);
    else
        clearOnlineDuelState();

    duelActive.store (false, std::memory_order_relaxed);

    duelDebugLog ("duel requested level=" + juce::String (botLevel));

    // The duel uses the normal song flow: the chart is loaded exactly as for
    // a regular game, then beginPendingDuelIfNeeded() attaches the duel state
    // once the note count is known.
    startSelectedSong();
}

void MainComponent::abortPendingDuelStart (const juce::String& reason)
{
    if (! duelPending && ! duelOnlinePlaybackPending)
        return;

    // The chart never arrived (locked song, missing download, parse failure).
    // Drop the pending request but keep the finished match and its result card
    // so the player can press NEXT OPPONENT/RETRY again instead of landing on
    // the generic SONG COMPLETE screen.
    duelPending = false;
    duelOnlinePlaybackPending = false;
    skipTunerForNextStage = false;

    duelDebugLog ("duel start aborted: " + reason);

    onlineCatalogStatusLabel.setText (reason, juce::dontSendNotification);
    resized();
    repaint();
    overlayComponent.repaint();
}

void MainComponent::beginPendingDuelIfNeeded()
{
    // Online match: the chart was loaded by startSelectedSong(). The server
    // froze its note count when the match was created, so the local chart must
    // have exactly the same length; otherwise note_index would drift and every
    // exchange would be scored against the wrong note.
    if (duelOnlinePlaybackPending)
    {
        duelOnlinePlaybackPending = false;

        const int noteCount =
            static_cast<int> (gameSession.getMidiNotes().size());
        const int expected = duelOnlineExpectedNotes;

        if (noteCount <= 0 || noteCount != expected)
        {
            abortOnlineDuelForChartMismatch (noteCount);
            return;
        }

        duelOnlineNoteCount.store (noteCount, std::memory_order_relaxed);
        duelOnlineChartReady = true;
        return;
    }

    if (! duelPending)
        return;

    duelPending = false;

    const int noteCount =
        static_cast<int> (gameSession.getMidiNotes().size());

    if (noteCount <= 0)
    {
        duelDebugLog ("bot match skipped: no notes");
        return;
    }

    const auto seed = static_cast<std::uint32_t> (
        juce::Random::getSystemRandom().nextInt());

    {
        const juce::SpinLock::ScopedLockType lock (duelLock);
        duelMatch = std::make_unique<DuelMatch> (
            DuelConfig{}, duelPendingLevel,
            songLevel.load (std::memory_order_relaxed), seed, noteCount);
    }

    duelBotLevelValue.store (static_cast<int> (duelPendingLevel),
                             std::memory_order_relaxed);
    duelPlayerHealthMilli.store (100000, std::memory_order_relaxed);
    duelOpponentHealthMilli.store (100000, std::memory_order_relaxed);
    duelLastExchange.store (0, std::memory_order_relaxed);
    duelLastDamageMilli.store (0, std::memory_order_relaxed);
    duelFlashUntilMs.store (0.0, std::memory_order_relaxed);
    duelActive.store (true, std::memory_order_relaxed);
    duelStageResultActive = false;
    duelResultCardInSession = false;

    // A new run has no decided result yet. The previous victory card stays
    // visible until this point, so the campaign buttons keep offering NEXT
    // OPPONENT while the next chart is still being prepared.
    campaignStageWon = false;

    // Stage backdrop and opponent sprite come from the server for this song
    // (admin-assigned stage/character), never from bundled art.
    applySongStageVisuals (selectedSongSlug);

    duelDebugLog ("bot match created notes=" + juce::String (noteCount)
                  + " level=" + juce::String (static_cast<int> (duelPendingLevel))
                  + " seed=" + juce::String (static_cast<int> (seed))
                  + " damageScale=" + juce::String (
                        duelDamageScaleForNoteCount (noteCount), 3));

    repaint();
    overlayComponent.repaint();
}

// Called on the pitch-analysis thread for every resolved chart note and on the
// message thread for the wall-clock auto miss.
void MainComponent::onDuelNoteResolved (int noteIndex, int hitQuality)
{
    // Online: HP, damage and the winner are server-side. The client only
    // queues the resolved note; the message-thread timer batches and posts it.
    if (duelOnlineActive.load (std::memory_order_relaxed))
    {
        // Diagnostic: the per-note quality is what the server stores, so a
        // "played correctly but counted as a miss" report can be checked
        // against the duel log.
        duelDebugLog ("online note idx=" + juce::String (noteIndex)
                      + " q=" + toDuelQualityToken (hitQuality)
                      + " phase=" + juce::String (static_cast<int> (duelOnlinePhase)));
        queueOnlineDuelNote (noteIndex, hitQuality);
        return;
    }

    if (! duelActive.load (std::memory_order_relaxed))
        return;

    const auto quality = toDuelQuality (hitQuality);

    DuelExchange exchange = DuelExchange::BothMiss;
    int damageMilli = 0;
    bool finished = false;

    {
        const juce::SpinLock::ScopedLockType lock (duelLock);

        if (duelMatch == nullptr)
            return;

        const auto result = duelMatch->submit (noteIndex, quality);

        exchange = result.exchange;
        damageMilli = static_cast<int> (
            std::lround (result.damageToOpponent * 1000.0));

        publishHealth (*duelMatch, duelPlayerHealthMilli,
                       duelOpponentHealthMilli);

        finished = duelMatch->isFinished();
    }

    duelLastExchange.store (static_cast<int> (exchange),
                            std::memory_order_relaxed);
    duelLastDamageMilli.store (damageMilli, std::memory_order_relaxed);
    duelFlashUntilMs.store (
        juce::Time::getMillisecondCounterHiRes() + 900.0,
        std::memory_order_relaxed);

    // Pixel-art reaction: the left guitarist plays the full 8-frame strum on
    // the player's hit, the right one on the opponent's. The trigger methods
    // only touch atomics, so this is safe from the pitch-analysis thread.
    if (exchange == DuelExchange::PlayerHit || exchange == DuelExchange::Block)
        duelStage.triggerPlayerHit();

    if (exchange == DuelExchange::OpponentHit || exchange == DuelExchange::Block)
        duelStage.triggerOpponentHit();

    duelDebugLog ("note idx=" + juce::String (noteIndex)
                  + " q=" + juce::String (static_cast<int> (quality))
                  + " exc=" + juce::String (static_cast<int> (exchange))
                  + " dmgO=" + juce::String (damageMilli / 1000.0, 1)
                  + " hpP=" + juce::String (duelPlayerHealthMilli.load() / 1000.0, 1)
                  + " hpO=" + juce::String (duelOpponentHealthMilli.load() / 1000.0, 1));

    if (finished)
        juce::MessageManager::callAsync ([this] { finishDuel(); });
}

void MainComponent::finishDuel()
{
    duelDebugLog ("finishDuel entry phase="
                  + juce::String (static_cast<int> (duelOnlinePhase))
                  + " active=" + juce::String (duelActive.load() ? 1 : 0)
                  + " campaign=" + juce::String (campaignFightActive ? 1 : 0)
                  + " stage=" + juce::String (campaignStage)
                  + " state=" + juce::String (static_cast<int> (
                        gameState.load (std::memory_order_relaxed))));

    // Online: the chart ended (or the server stopped it). Nothing is decided
    // locally: drain the queued notes, POST /finish and read /result.
    if (duelOnlinePhase == DuelOnlinePhase::Playing
        || duelOnlinePhase == DuelOnlinePhase::Finishing)
    {
        if (duelOnlinePhase != DuelOnlinePhase::Playing)
        {
            duelDebugLog ("finishDuel skipped: online finishing");
            return;
        }

        duelOnlinePhase = DuelOnlinePhase::Finishing;
        duelOnlineFinishRequested = true;
        duelOnlineActive.store (false, std::memory_order_relaxed);
        duelActive.store (false, std::memory_order_relaxed);

        practiceHoldActive.store (false, std::memory_order_release);

        if (gameState.load (std::memory_order_relaxed) == GameState::Playing)
            gameState.store (GameState::Finished, std::memory_order_relaxed);

        juce::MessageManager::callAsync ([this]
        {
            repaint();
            overlayComponent.repaint();
        });
        return;
    }

    if (! duelActive.exchange (false))
    {
        duelDebugLog ("finishDuel skipped: duelActive already false");
        return;
    }

    // Keep the pixel-art stage behind the bot result card until a new match
    // or MAIN MENU resets the duel state.
    duelStageResultActive = true;

    {
        const juce::SpinLock::ScopedLockType lock (duelLock);

        if (duelMatch != nullptr)
        {
            // A knockout already finished the match; a chart that ended
            // without one is decided by the remaining health.
            duelMatch->finishByHealth();

            publishHealth (*duelMatch, duelPlayerHealthMilli,
                           duelOpponentHealthMilli);
        }
    }

    if (campaignFightActive)
    {
        bool won = false;
        {
            const juce::SpinLock::ScopedLockType lock (duelLock);

            if (duelMatch != nullptr)
                won = duelMatch->playerWon();
        }

        const int playedStage = campaignStage;

        campaignStageWon = won;
        duelDebugLog ("campaign result won=" + juce::String (won ? 1 : 0)
                      + " stage=" + juce::String (campaignStage));

        // The server owns Battle XP: report the actual score and the attempt
        // facts; the result card shows only what the server answers.
        if (selectedCampaignId.isNotEmpty())
        {
            if (campaignStageRunId.isEmpty())
                campaignStageRunId = juce::Uuid().toString();

            campaignStageReportFailed = onlineAccessToken.isEmpty();
            campaignStageReportPending = ! campaignStageReportFailed;
            reportCampaignStageResult (selectedCampaignId, playedStage,
                                       campaignStageRunId, won);
        }
        else
        {
            campaignStageReportFailed = true;
            campaignStageReportPending = false;
        }

        if (won)
        {
            const int next = juce::jmin (campaignStageCount(), campaignStage + 1);

            if (next > campaignStage)
            {
                campaignStage = next;
                campaignStageWriteMs = juce::Time::getMillisecondCounterHiRes();

                // Local mirror for the panel only. The stage-result response
                // above replaces it with the server's value, which is the sole
                // authority for the cleared stage count (a defeat resets it).
                if (auto* settings = appProperties.getUserSettings())
                {
                    settings->setValue (campaignBestKey(), campaignStage);
                    settings->saveIfNeeded();
                }
            }
        }
        else
        {
            // A loss sends the whole run back to stage one.
            campaignStage = 0;
            campaignStageWriteMs = juce::Time::getMillisecondCounterHiRes();

            if (auto* settings = appProperties.getUserSettings())
            {
                settings->setValue (campaignBestKey(), 0);
                settings->saveIfNeeded();
            }
        }

        // The result screen stays until the player acts. A victory offers
        // NEXT OPPONENT, a loss offers RETRY; the CAMPAIGN button on that card
        // is the only way back to the campaign hub.
    }

    // Knockout or the end of the chart: the transport stops and the results
    // panel (painted from the same DuelMatch state) takes over.
    practiceHoldActive.store (false, std::memory_order_release);
    tablatureComponent.setPlayheadRunning (false);

    if (gameState.load (std::memory_order_relaxed) == GameState::Playing)
        gameState.store (GameState::Finished, std::memory_order_relaxed);

    juce::MessageManager::callAsync ([this]
    {
        // The finished layout hides the gameplay controls and shows PLAY
        // AGAIN / MAIN MENU; without resized() the session-complete screen
        // keeps the previous layout until the next window resize.
        resized();
        repaint();
        overlayComponent.repaint();
    });
}

//==============================================================================
// Battle (tower) stage result: the server is the only source of XP
//==============================================================================

void MainComponent::reportCampaignStageResult (const juce::String& campaignId,
                                               int position,
                                               const juce::String& runId,
                                               bool won)
{
    if (campaignId.isEmpty() || runId.isEmpty())
        return;

    if (onlineAccessToken.isEmpty())
    {
        // Not signed in: the run is still playable, but the reward cannot be
        // saved. The card shows local stats and no invented XP.
        campaignStageReportPending = false;
        campaignStageReportFailed = true;
        campaignStageServerXp = -2;
        campaignStageNormalizedScore = -1;
        return;
    }

    int perfect = 0, great = 0, good = 0, miss = 0, notesPlayed = 0;
    double accuracy = 0.0;

    {
        const juce::SpinLock::ScopedLockType lock (duelLock);

        if (duelMatch != nullptr)
        {
            const auto& stats = duelMatch->getPlayerStats();
            perfect = stats.perfect;
            great = stats.great;
            good = stats.good;
            miss = stats.miss;
            notesPlayed = stats.notes;
            accuracy = stats.accuracy() * 100.0;
        }
    }

    // Completion is measured against the actual chart, the same way the solo
    // finish endpoint measures it. A knockout before the end is a small value
    // and the server extrapolates it (or stores NULL when it is too early).
    const int chartNotes = static_cast<int> (gameSession.getMidiNotes().size());
    const int notesTotal = juce::jmax (0, chartNotes);
    const int played = juce::jlimit (0, notesTotal, juce::jmax (0, notesPlayed));
    const double completion =
        notesTotal > 0
            ? juce::jlimit (0.0, 100.0,
                            100.0 * static_cast<double> (played)
                                / static_cast<double> (notesTotal))
            : 0.0;

    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty ("run_id", runId);
    payload->setProperty ("won", won);
    payload->setProperty ("score_points", juce::jmax (0, gameplayStats.getScore()));
    payload->setProperty ("accuracy_percent", juce::jlimit (0.0, 100.0, accuracy));
    payload->setProperty ("perfect_count", juce::jmax (0, perfect));
    payload->setProperty ("great_count", juce::jmax (0, great));
    payload->setProperty ("good_count", juce::jmax (0, good));
    payload->setProperty ("miss_count", juce::jmax (0, miss));
    payload->setProperty ("completion_percent", completion);
    payload->setProperty ("notes_total", notesTotal);
    payload->setProperty ("notes_played", played);

    const auto body = juce::JSON::toString (juce::var (payload.get()));
    const auto token = onlineAccessToken;
    const auto url = "/campaigns/" + campaignId + "/stages/"
                     + juce::String (position) + "/result";

    auto result = std::make_shared<ApiClient::Response>();
    juce::Component::SafePointer<MainComponent> safeThis (this);

    // Three attempts with the SAME run id: a transport failure or a 5xx is
    // retried, a 4xx is final. Runs on the ApiClient pool, never on the message
    // or audio thread; the result is applied on the message thread.
    apiClient.runAsync (
        [url, token, body, result]()
        {
            const auto parsed = juce::JSON::parse (body);

            for (int attempt = 0; attempt < 3; ++attempt)
            {
                *result = ApiClient::post (url, parsed, token, 8000);

                if (result->transportOk && result->statusCode < 500)
                    break;
            }
        },
        [safeThis, result, campaignId, runId]()
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            // A late answer must not overwrite the card of the next attempt.
            if (self->campaignStageRunId != runId)
                return;

            self->campaignStageReportPending = false;

            if (! result->transportOk || result->statusCode != 200
                || ! result->json.isObject())
            {
                // No invented XP: the card keeps the local stats and explains
                // that the reward was not saved.
                self->campaignStageReportFailed = true;
                self->campaignStageServerXp = -2;
                self->campaignStageNormalizedScore = -1;
                self->repaint();
                self->overlayComponent.repaint();
                return;
            }

            const auto& json = result->json;
            self->campaignStageReportFailed = false;
            self->campaignStageServerXp =
                static_cast<int> (json.getProperty ("xp_earned", 0));

            const auto normalized = json.getProperty ("normalized_score", juce::var());
            self->campaignStageNormalizedScore =
                (normalized.isInt() || normalized.isInt64() || normalized.isDouble())
                    ? static_cast<int> (normalized)
                    : -1;

            // Keep the local mirror in sync with the server's value. A defeat
            // resets the run to the first opponent (the server stores
            // best_stage=0 for it), so the local stage key must be cleared as
            // well; a win mirrors the server's advanced stage.
            const bool won = static_cast<bool> (json.getProperty ("won", false));
            const auto bestVar = json.getProperty ("best_stage", juce::var());

            if (won
                && (bestVar.isInt() || bestVar.isInt64() || bestVar.isDouble()))
            {
                int best = static_cast<int> (bestVar);

                if (const auto* campaign = self->findServerCampaign (campaignId))
                    best = juce::jlimit (0, static_cast<int> (campaign->stages.size()),
                                         best);
                else
                    best = juce::jmax (0, best);

                if (auto* settings = self->appProperties.getUserSettings())
                {
                    settings->setValue ("CampaignBest:" + campaignId, best);
                    settings->saveIfNeeded();
                }

                // This answer is the authoritative cleared-stage count. Apply
                // it to the open panel too: a progress GET that was answered
                // before this POST committed may have left the previous stage
                // on screen, and leaving the card to Battle showed the stage
                // that was just played as still current.
                if (campaignId == self->selectedCampaignId)
                {
                    self->campaignStage = best;
                    self->campaignStageWriteMs =
                        juce::Time::getMillisecondCounterHiRes();
                    self->applyServerCampaigns();
                }
            }
            else if (! won)
            {
                if (auto* settings = self->appProperties.getUserSettings())
                {
                    settings->setValue ("CampaignBest:" + campaignId, 0);
                    settings->saveIfNeeded();
                }

                if (campaignId == self->selectedCampaignId)
                {
                    self->campaignStage = 0;
                    self->campaignStageWriteMs =
                        juce::Time::getMillisecondCounterHiRes();
                    self->applyServerCampaigns();
                }
            }

            if (const auto* unlocked = json
                    .getProperty ("unlocked_character", juce::var())
                    .getDynamicObject())
            {
                const auto name = unlocked->getProperty ("name").toString();

                if (name.isNotEmpty())
                    self->notifyCharacterUnlocked (name);
            }

            self->repaint();
            self->overlayComponent.repaint();
        });
}

//==============================================================================
// Duel UI
//==============================================================================

void MainComponent::showDuelModeMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("GUITAR DUELS");
    menu.addItem (1, "VS BOT - EASY (RUSTY)");
    menu.addItem (2, "VS BOT - NORMAL (MIDNIGHT)");
    menu.addItem (3, "VS BOT - HARD (VOLT)");
    menu.addSeparator();
    menu.addItem (4, "ONLINE MATCH");

    juce::Component::SafePointer<MainComponent> safeThis (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetComponent (&duelModeButton)
                            .withMinimumWidth (250),
                        [safeThis] (int result)
                        {
                            if (safeThis == nullptr)
                                return;

                            if (result >= 1 && result <= 3)
                                safeThis->requestDuelWithBot (result - 1);
                            else if (result == 4)
                                safeThis->startOnlineDuel();
                        });
}

void MainComponent::repaintDuelHud()
{
    if (getWidth() <= 0 || overlayComponent.getWidth() <= 0)
        return;

    const juce::Rectangle<int> strip = getDuelHealthStripBounds();

    // The strip carries the HP bars, the judgement text and the red damage
    // amount, so it needs a repaint while a duel is running.
    overlayComponent.repaint (strip.expanded (4));

    // The exchange text (HIT / BLOCK / DAMAGE) is painted at the guitarists'
    // level, outside the strip: invalidate that band too so the 30 Hz duel
    // timer animates it instead of leaving a stale label on screen.
    const float tabTop = static_cast<float> (tablatureComponent.getY());
    const float stageCentre = (static_cast<float> (strip.getBottom()) + tabTop) * 0.5f;
    overlayComponent.repaint (juce::Rectangle<int> (
        80, juce::roundToInt (stageCentre - 34.0f),
        juce::jmax (240, getWidth() - 160), 68));
}

void MainComponent::paintDuelOverlay (juce::Graphics& g)
{    const auto state = gameState.load (std::memory_order_relaxed);

    if (state != GameState::Playing && state != GameState::Finished)
        return;

    // Online matches draw the authoritative server snapshot (HP, counters,
    // result, XP and rating) instead of the local bot simulation.
    if (duelOnlinePhase == DuelOnlinePhase::Playing
        || duelOnlinePhase == DuelOnlinePhase::Finishing
        || duelOnlinePhase == DuelOnlinePhase::Finished)
    {
        paintOnlineDuelOverlay (g);
        return;
    }

    // Snapshot the match under the same lock the analysis thread uses.
    bool haveMatch = false;
    bool finished = false;
    bool won = false;
    bool draw = false;
    int perfect = 0;
    int great = 0;
    int good = 0;
    int miss = 0;
    int notes = 0;
    double accuracy = 0.0;
    double damageDealt = 0.0;
    int xp = 0;
    int opPerfect = 0;
    int opGreat = 0;
    int opGood = 0;
    int opMiss = 0;
    double opAccuracy = 0.0;
    double opDamage = 0.0;
    const auto level = static_cast<DuelBotLevel> (
        duelBotLevelValue.load (std::memory_order_relaxed));

    {
        const juce::SpinLock::ScopedLockType lock (duelLock);

        if (duelMatch != nullptr)
        {
            haveMatch = true;
            finished = duelMatch->isFinished();
            won = duelMatch->playerWon();
            draw = duelMatch->isDraw();

            const auto& stats = duelMatch->getPlayerStats();
            perfect = stats.perfect;
            great = stats.great;
            good = stats.good;
            miss = stats.miss;
            notes = stats.notes;
            accuracy = stats.accuracy();
            damageDealt = stats.damageDealt;
            xp = duelMatch->getRewards().xp;

            const auto& opponent = duelMatch->getOpponentStats();
            opPerfect = opponent.perfect;
            opGreat = opponent.great;
            opGood = opponent.good;
            opMiss = opponent.miss;
            opAccuracy = opponent.accuracy();
            opDamage = opponent.damageDealt;
        }
    }

    if (! haveMatch)
        return;

    const float width = static_cast<float> (getWidth());
    const auto& profile = duelBotProfile (level);
    // Campaigns are fought against the character assigned to the stage's song,
    // exactly the sprite on the right of the stage; other bot duels keep the
    // difficulty profile name.
    const auto campaignOpponent = campaignOpponentCharacterName();
    const auto opponentLabel = campaignOpponent.isNotEmpty()
                                   ? campaignOpponent
                                   : juce::String (profile.name);

    //--------------------------------------------------------------------------
    // Results card
    if (finished)
    {
        // When the session card already renders the duel result (Victory /
        // Defeat) there is no second card to draw.
        if (duelResultCardInSession)
        {
            duelResultCardInSession = false;
            return;
        }

        const float cardW = juce::jmin (640.0f, width - 48.0f);
        const juce::Rectangle<float> card ((width - cardW) * 0.5f, 128.0f,
                                           cardW, 326.0f);

        g.setColour (UiColors::background.withAlpha (0.82f));
        g.fillRoundedRectangle (card.expanded (6.0f), 24.0f);
        GameUi::drawCard (g, card, 20.0f, true);

        const auto title = draw ? "DRAW" : (won ? "VICTORY" : "DEFEAT");
        const auto titleColour = draw ? UiColors::muted
                                      : (won ? UiColors::green : UiColors::red);

        g.setColour (UiColors::accent);
        g.setFont (GameUi::eyebrow (10.0f));
        g.drawText ("GUITAR DUEL", card.withHeight (44.0f).translated (0.0f, 14.0f),
                    juce::Justification::centred, false);

        g.setColour (titleColour);
        g.setFont (GameUi::heading (30.0f));
        g.drawText (title, card.withHeight (54.0f).translated (0.0f, 30.0f),
                    juce::Justification::centred, false);

        const float leftX = card.getX() + 40.0f;
        const float rightX = card.getX() + card.getWidth() * 0.56f;
        const float top = card.getY() + 108.0f;

        auto drawColumn = [&g] (float x, float y, const juce::String& name,
                                const juce::Colour& colour,
                                int hp, int p, int gr, int gd, int ms,
                                double acc, double dealt)
        {
            g.setColour (colour);
            g.setFont (GameUi::font (12.0f, true));
            g.drawText (name, juce::Rectangle<float> (x, y, 220.0f, 18.0f),
                        juce::Justification::centredLeft, false);

            g.setColour (UiColors::text);
            g.setFont (GameUi::heading (22.0f));
            g.drawText (juce::String (hp) + " HP",
                        juce::Rectangle<float> (x, y + 20.0f, 220.0f, 28.0f),
                        juce::Justification::centredLeft, false);

            g.setColour (UiColors::muted);
            g.setFont (GameUi::font (11.0f));
            g.drawText ("PERFECT " + juce::String (p)
                            + "    GREAT " + juce::String (gr),
                        juce::Rectangle<float> (x, y + 54.0f, 260.0f, 18.0f),
                        juce::Justification::centredLeft, false);
            g.drawText ("GOOD " + juce::String (gd)
                            + "    MISS " + juce::String (ms),
                        juce::Rectangle<float> (x, y + 74.0f, 260.0f, 18.0f),
                        juce::Justification::centredLeft, false);
            g.drawText ("ACCURACY " + juce::String (juce::roundToInt (acc * 100.0)) + "%",
                        juce::Rectangle<float> (x, y + 94.0f, 260.0f, 18.0f),
                        juce::Justification::centredLeft, false);
            g.drawText ("DAMAGE " + juce::String (dealt, 1),
                        juce::Rectangle<float> (x, y + 114.0f, 260.0f, 18.0f),
                        juce::Justification::centredLeft, false);
        };

        drawColumn (leftX, top, "YOU", UiColors::green,
                    juce::roundToInt (duelPlayerHealthMilli.load() / 1000.0),
                    perfect, great, good, miss, accuracy, damageDealt);

        drawColumn (rightX, top, opponentLabel, UiColors::orange,
                    juce::roundToInt (duelOpponentHealthMilli.load() / 1000.0),
                    opPerfect, opGreat, opGood, opMiss, opAccuracy, opDamage);

        // Note count and rewards footer.
        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (10.0f));
        g.drawText (juce::String (notes) + " NOTES",
                    juce::Rectangle<float> (card.getX() + 40.0f,
                                            card.getBottom() - 62.0f, 200.0f, 16.0f),
                    juce::Justification::centredLeft, false);

        g.setColour (UiColors::accent);
        g.setFont (GameUi::font (13.0f, true));
        g.drawText ("XP +" + juce::String (xp),
                    juce::Rectangle<float> (card.getX() + card.getWidth() - 240.0f,
                                            card.getBottom() - 66.0f, 200.0f, 20.0f),
                    juce::Justification::centredRight, false);

        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (9.0f));
        g.drawText ("BOT MATCH - RATING UNCHANGED",
                    juce::Rectangle<float> (card.getX() + 40.0f,
                                            card.getBottom() - 40.0f,
                                            card.getWidth() - 80.0f, 14.0f),
                    juce::Justification::centred, false);

        return;
    }

    //--------------------------------------------------------------------------
    // Live health strip
    if (! duelActive.load (std::memory_order_relaxed))
        return;

    const juce::Rectangle<float> strip = getDuelHealthStripBounds().toFloat();

    // Faint backing only; the strip has no outline.
    g.setColour (UiColors::background.withAlpha (0.42f));
    g.fillRoundedRectangle (strip, 14.0f);

    const float playerHp = duelPlayerHealthMilli.load() / 1000.0f;
    const float opponentHp = duelOpponentHealthMilli.load() / 1000.0f;

    auto healthColour = [] (float hp)
    {
        if (hp > 50.0f) return UiColors::green;
        if (hp > 25.0f) return UiColors::orange;
        return UiColors::red;
    };

    constexpr float barH = 16.0f;
    const float barW = strip.getWidth() * 0.30f;
    const float barY = strip.getY() + 22.0f;
    const juce::Rectangle<float> leftBar (strip.getX() + 16.0f, barY, barW, barH);
    const juce::Rectangle<float> rightBar (strip.getRight() - 16.0f - barW,
                                           barY, barW, barH);

    auto drawBar = [&g, &healthColour] (juce::Rectangle<float> bar, float hp)
    {
        g.setColour (juce::Colours::black.withAlpha (0.62f));
        g.fillRoundedRectangle (bar.expanded (2.0f, 1.0f), bar.getHeight() * 0.5f);

        const float ratio = juce::jlimit (0.0f, 1.0f, hp / 100.0f);

        if (ratio > 0.0f)
        {
            g.setColour (healthColour (hp).brighter (0.15f));
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * ratio),
                                    bar.getHeight() * 0.5f);
        }
    };

    drawBar (leftBar, playerHp);
    drawBar (rightBar, opponentHp);

    g.setColour (UiColors::text);
    g.setFont (GameUi::font (13.0f, true));
    g.drawText ("YOU", juce::Rectangle<float> (strip.getX() + 16.0f,
                                               strip.getY() + 2.0f, 220.0f, 18.0f),
                juce::Justification::centredLeft, false);
    g.drawText (opponentLabel,
                juce::Rectangle<float> (strip.getRight() - 236.0f,
                                        strip.getY() + 2.0f, 220.0f, 18.0f),
                juce::Justification::centredRight, false);

    g.setColour (healthColour (playerHp).brighter (0.2f));
    g.setFont (GameUi::font (13.0f, true));
    g.drawText (juce::String (juce::roundToInt (playerHp)),
                juce::Rectangle<float> (leftBar.getRight() + 10.0f,
                                        barY - 2.0f, 56.0f, 20.0f),
                juce::Justification::centredLeft, false);
    g.setColour (healthColour (opponentHp).brighter (0.2f));
    g.drawText (juce::String (juce::roundToInt (opponentHp)),
                juce::Rectangle<float> (rightBar.getX() - 66.0f,
                                        barY - 2.0f, 56.0f, 20.0f),
                juce::Justification::centredRight, false);

    // Duels do not show the per-hit judgement text: only the exchange
    // (HIT / BLOCK / MISS / DAMAGE) is painted, at the guitarists' level.

    // Last exchange: HIT / BLOCK / DAMAGE, large, centred at the guitarists.
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto flashUntil = duelFlashUntilMs.load (std::memory_order_relaxed);
    const auto exchange = static_cast<DuelExchange> (
        duelLastExchange.load (std::memory_order_relaxed));

    if (now < flashUntil)
    {
        juce::String text;
        juce::Colour colour = UiColors::text;

        switch (exchange)
        {
            case DuelExchange::Block:
                text = "BLOCK";
                colour = UiColors::blue;
                break;

            case DuelExchange::PlayerHit:
                text = "YOU HIT +" + juce::String (duelLastDamageMilli.load() / 1000.0, 1)
                       + " HP";
                colour = UiColors::green;
                break;

            case DuelExchange::OpponentHit:
                text = juce::String (profile.name) + " HITS YOU -"
                       + juce::String (duelLastDamageMilli.load() / 1000.0, 1)
                       + " HP";
                colour = UiColors::red;
                break;

            case DuelExchange::BothMiss:
                text = "MISS";
                colour = UiColors::text;
                break;
        }

        const float tabTop = static_cast<float> (tablatureComponent.getY());
        const float stageCentre = (strip.getBottom() + tabTop) * 0.5f;

        // Solid colour for the whole flash: the exchange label (HIT / BLOCK /
        // MISS / DAMAGE) must never look semi-transparent.
        g.setColour (colour.brighter (0.2f));
        g.setFont (GameUi::heading (38.0f));
        g.drawFittedText (text,
                          juce::Rectangle<int> (
                              80, juce::roundToInt (stageCentre - 34.0f),
                              juce::jmax (240, getWidth() - 160), 68),
                          juce::Justification::centred, 1, 0.70f);
    }

    // The damage amount flashes in red inside the bar that lost HP for 0.5 s.
    const double damageEnd = flashUntil - 400.0;

    if (now < damageEnd
        && (exchange == DuelExchange::PlayerHit
            || exchange == DuelExchange::OpponentHit))
    {
        const auto bar = exchange == DuelExchange::OpponentHit ? leftBar : rightBar;
        const float alpha = static_cast<float> (
            juce::jlimit (0.0, 1.0, (damageEnd - now) / 500.0));
        const juce::Rectangle<float> chip (bar.getCentreX() - 30.0f,
                                           bar.getCentreY() - 7.0f, 60.0f, 14.0f);

        g.setColour (juce::Colours::black.withAlpha (0.55f * alpha));
        g.fillRoundedRectangle (chip, 5.0f);
        g.setColour (UiColors::red.withAlpha (alpha));
        g.setFont (GameUi::font (11.0f, true));
        g.drawText ("-" + juce::String (duelLastDamageMilli.load() / 1000.0, 1),
                    chip, juce::Justification::centred, false);
    }
}

//==============================================================================
// Online duel flow (server-authoritative)
//==============================================================================

namespace
{
    double duelNowMs()
    {
        return juce::Time::getMillisecondCounterHiRes();
    }

    juce::String duelAccuracyText (double percent)
    {
        return juce::String (juce::roundToInt (juce::jlimit (0.0, 100.0, percent))) + "%";
    }

    juce::String duelRatingText (const DuelParticipantState* participant)
    {
        if (participant == nullptr || participant->ratingAfter < 0)
            return "RATING UNCHANGED";

        const auto delta = participant->ratingDelta;
        const auto sign = delta > 0 ? juce::String ("+") : juce::String();

        return "RATING " + juce::String (participant->ratingAfter)
               + "  (" + sign + juce::String (delta) + ")";
    }
}

void MainComponent::refreshDuelLobby()
{
    if (duelLobby == nullptr || duelLobbyInFlight)
        return;

    if (onlineAccessToken.isEmpty())
    {
        duelLobby->setSignedIn (false);
        return;
    }

    duelLobby->setSignedIn (true);
    duelLobbyInFlight = true;
    juce::Component::SafePointer<MainComponent> safeThis (this);

    duelClient.rooms (false, onlineAccessToken,
        [safeThis] (const DuelReply& reply, const DuelRoomListReply& list)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->duelLobbyInFlight = false;

            if (self->duelLobby == nullptr)
                return;

            if (! reply.ok)
            {
                self->duelLobby->setStatus (
                    reply.errorMessage.isNotEmpty()
                        ? "Lobby unavailable: " + reply.errorMessage
                        : juce::String ("Lobby unavailable."));
                return;
            }

            std::vector<DuelLobbyRoom> mapped;

            for (const auto& room : list.rooms)
            {
                if (! room.isWaiting())
                    continue;

                DuelLobbyRoom row;
                row.matchId = room.lobbyId;
                row.songId = room.songId;
                row.songSlug = room.songSlug;
                row.songTitle = room.songTitle.isNotEmpty()
                    ? room.songTitle : juce::String ("NO SONG YET");
                row.hostName = room.name;
                row.hasPassword = room.hasPassword;
                row.playerCount = static_cast<int> (room.players.size());
                mapped.push_back (row);
            }

            self->duelLobby->setRooms (mapped);
        });
}

void MainComponent::openCreateDuelRoomDialog()
{
    if (onlineAccessToken.isEmpty())
        return;

    auto* content = new CreateDuelRoomWindow (
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        (juce::String name, juce::String password)
        {
            if (auto* self = safeThis.getComponent())
            {
                if (self->onlineAccessToken.isEmpty())
                    return;

                self->duelClient.createRoom (
                    name, password, self->onlineAccessToken,
                    [safeThis] (const DuelReply& reply, const DuelRoomSnapshot& room)
                    {
                        if (auto* s = safeThis.getComponent())
                        {
                            if (! reply.ok || ! room.valid)
                            {
                                juce::AlertWindow::showMessageBoxAsync (
                                    juce::MessageBoxIconType::WarningIcon,
                                    "Could not create room",
                                    reply.errorMessage.isNotEmpty()
                                        ? reply.errorMessage
                                        : juce::String ("Please try again."),
                                    "OK");
                                return;
                            }

                            s->openOnlineDuelRoom (room.lobbyId);
                        }
                    });
            }
        });

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (content);
    options.dialogTitle = "Create online room";
    options.componentToCentreAround = this;
    options.dialogBackgroundColour = UiColors::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void MainComponent::openOnlineDuelRoom (const juce::String& lobbyId)
{
    if (lobbyId.isEmpty() || onlineAccessToken.isEmpty())
        return;

    // An abort of a room match reopens the same room; never stack two windows.
    if (onlineDuelRoomDialog != nullptr)
    {
        onlineDuelRoomDialog->toFront (true);
        return;
    }

    std::vector<OnlineDuelRoomWindow::SongChoice> songs;

    for (const auto& song : catalogSongs)
        if (song.serverId.isNotEmpty() && song.published)
            songs.push_back ({ song.serverId, song.title });

    auto* content = new OnlineDuelRoomWindow (
        duelClient, onlineAccessToken, currentUsername, lobbyId, std::move (songs),
        [safeThis = juce::Component::SafePointer<MainComponent> (this), lobbyId]
        (juce::String matchId)
        {
            if (auto* self = safeThis.getComponent())
                self->joinOnlineDuelRoom (matchId, lobbyId);
        },
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* self = safeThis.getComponent())
            {
                // The room window also closes when the match starts. Voice
                // must survive into the duel; only stop it when no online
                // match is waiting or running.
                const bool matchActive =
                    self->duelOnlinePhase == DuelOnlinePhase::Waiting
                    || self->duelOnlinePhase == DuelOnlinePhase::Playing;

                if (! matchActive || self->duelOnlineMatchId.isEmpty())
                    self->stopLobbyVoiceChat();
            }
        });

    activeVoiceLobbyId = lobbyId;

    // The call is connected as soon as the room opens: polling, receiving and
    // direct-UDP signaling run immediately, so the opponent is heard without
    // pressing anything and the peers are already connected when the mic is
    // unmuted. The VOICE button only toggles the microphone. A room reopened
    // after a duel keeps its mic state.
    if (voiceChat == nullptr)
        voiceChat = std::make_unique<VoiceChat>();

    if (! voiceChat->isStarted() || voiceChat->getLobbyId() != lobbyId)
    {
        voiceChat->stop();
        voiceChat->start (lobbyId, onlineAccessToken);
    }

    content->onVoiceChatToggled =
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        (bool enabled)
        {
            if (auto* self = safeThis.getComponent())
                self->setLobbyMicrophoneEnabled (enabled);
        };

    // A room reopened after a duel (or after the opponent left) must show the
    // microphone state that the workers actually have: they survive the match.
    if (voiceChat != nullptr)
        content->setVoiceChatEnabled (voiceChat->isEnabled());

    content->onOpenTuner =
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* self = safeThis.getComponent())
                self->openLobbyTuner();
        };

    content->onClosed =
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* self = safeThis.getComponent())
                self->stopLobbyVoiceChat();
        };

    content->voiceStatusProvider =
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]() -> juce::String
        {
            if (auto* self = safeThis.getComponent())
                return self->getVoiceStatusText();

            return {};
        };

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (content);
    options.dialogTitle = "Online room";
    options.componentToCentreAround = this;
    options.dialogBackgroundColour = UiColors::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    onlineDuelRoomDialog = options.launchAsync();
}

void MainComponent::joinDuelLobbyRoom (const juce::String& lobbyId,
                                       const juce::String& password)
{
    if (lobbyId.isEmpty() || onlineAccessToken.isEmpty())
        return;

    juce::Component::SafePointer<MainComponent> safeThis (this);

    duelClient.joinRoom (lobbyId, password, onlineAccessToken,
        [safeThis] (const DuelReply& reply, const DuelRoomSnapshot& room)
        {
            if (auto* self = safeThis.getComponent())
            {
                if (! reply.ok || ! room.valid)
                {
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "Could not join room",
                        reply.errorMessage.isNotEmpty()
                            ? reply.errorMessage
                            : juce::String ("Please try again."),
                        "OK");
                    return;
                }

                self->openOnlineDuelRoom (room.lobbyId);
            }
        });
}

void MainComponent::joinOnlineDuelRoom (const juce::String& matchId,
                                        const juce::String& lobbyId)
{
    if (matchId.isEmpty() || onlineAccessToken.isEmpty())
        return;

    if (duelOnlinePhase != DuelOnlinePhase::None
        && duelOnlinePhase != DuelOnlinePhase::Finished)
        return;

    // A finished match can never be joined again. A room snapshot that still
    // points at the previous round must not restart that match: the host picks
    // a new song and the next start creates a new match, so the room is asked
    // for its current state instead.
    if (matchId == duelOnlineFinishedMatchId)
    {
        duelDebugLog ("join refused: match " + matchId + " already finished");
        returnOnlineDuelToRoom (lobbyId, matchId);
        return;
    }

    // The server froze the chart at the song tempo; joining must run at 100%.
    if (! prepareDuelTempo())
    {
        // This side cannot join: cancel the fresh room match so the opponent is
        // not left waiting on a partner that will never report ready.
        if (lobbyId.isNotEmpty())
        {
            juce::Component::SafePointer<MainComponent> safeThis (this);

            duelClient.leave (matchId, onlineAccessToken,
                [safeThis, lobbyId, matchId] (const DuelReply&)
                {
                    if (auto* self = safeThis.getComponent())
                        self->returnOnlineDuelToRoom (lobbyId, matchId);
                });
        }

        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::InfoIcon,
            "Tempo change in progress",
            "A tempo change is still being prepared. Try joining the room again in a moment.",
            "OK", this);
        return;
    }

    resetDuelState();

    ++duelOnlineGeneration;
    const int generation = duelOnlineGeneration;

    duelOnlineMatchId = matchId;
    duelOnlinePhase = DuelOnlinePhase::Waiting;
    duelOnlinePollFailures = 0;
    // The room is the only caller that knows its lobby id: remember the room
    // context before the join reply arrives, so the handoff can never look like
    // a quick-queue match (and later show the second confirmation dialog).
    duelOnlineLobbyId = lobbyId;
    duelOnlineConfirmedInRoom = lobbyId.isNotEmpty();
    showOnlineDuelDialog ("JOINING DUEL...");

    juce::Component::SafePointer<MainComponent> safeThis (this);

    duelClient.join (matchId, onlineAccessToken,
        [safeThis, generation] (const DuelReply& reply, const DuelMatchSnapshot& match)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr || generation != self->duelOnlineGeneration)
                return;

            if (! reply.ok || ! match.valid)
            {
                self->duelOnlinePhase = DuelOnlinePhase::None;
                self->closeOnlineDuelDialog();
                self->handleOnlineDuelReplyError (reply, "join");
                return;
            }

            if (match.status == "cancelled" || match.status == "finished")
            {
                const auto lobbyId = match.lobbyId;
                const auto deadMatchId = match.matchId;

                self->duelOnlinePhase = DuelOnlinePhase::None;
                self->closeOnlineDuelDialog();
                self->clearOnlineDuelState();

                // A room match was aborted on the other side: the server has
                // already rolled the room back to `waiting`, so both players
                // return to the same room window.
                if (lobbyId.isNotEmpty())
                {
                    self->returnOnlineDuelToRoom (lobbyId, deadMatchId);
                    return;
                }

                juce::AlertWindow::showMessageBoxAsync (
                    juce::MessageBoxIconType::InfoIcon,
                    "Room closed",
                    "This duel room is no longer available.",
                    "OK", self);
                return;
            }

            int index = -1;

            for (int i = 0; i < static_cast<int> (self->catalogSongs.size()); ++i)
            {
                if (self->catalogSongs[static_cast<size_t> (i)].slug
                    == match.songSlug)
                {
                    index = i;
                    break;
                }
            }

            self->duelOnlineMatchId = match.matchId;
            self->duelOnlineMatch = match;
            // The lobby id from the join snapshot is the authoritative one; the
            // id the room passed in is kept when the snapshot omits it, so the
            // room handoff always stays a room match.
            self->duelOnlineLobbyId = match.lobbyId.isNotEmpty() ? match.lobbyId
                                                                 : self->duelOnlineLobbyId;
            self->duelOnlineConfirmedInRoom =
                self->duelOnlineConfirmedInRoom || match.lobbyId.isNotEmpty();
            self->duelOnlineSongId = match.songId;
            self->duelOnlineSongSlug = match.songSlug;
            self->duelOnlineSnapshotAtMs = duelNowMs();
            self->duelOnlineExpectedNotes = match.noteCount;
            self->duelOnlinePhase = DuelOnlinePhase::Waiting;
            self->duelOnlinePollFailures = 0;
            self->duelOnlineNextPollMs = duelNowMs() + 800.0;
            self->updateOnlineDuelHud();

            if (index < 0)
            {
                self->cancelOnlineDuel (true);

                juce::AlertWindow::showMessageBoxAsync (
                    juce::MessageBoxIconType::InfoIcon,
                    "Online lobby",
                    "This room's song is not in the current catalogue. "
                        "Refresh the catalogue and try again.",
                    "OK", self);
                return;
            }

            self->selectedCatalogIndex = index;

            // The judging window (timing and pitch) belongs to the song being
            // played. The room never calls selectCatalogSong(), so without this
            // an online chart was judged with the level of whatever song was
            // selected last - a hard chart's narrow window on an easy song.
            self->songLevel.store (
                juce::jlimit (1, 10,
                              self->catalogSongs[static_cast<size_t> (index)]
                                  .difficulty),
                std::memory_order_relaxed);

            // The room's song decides the stage; the sprites are each player's
            // own character from the match snapshot.
            self->applyOnlineDuelCharacters();

            const bool downloaded =
                self->catalogSongs[static_cast<size_t> (index)].downloaded
                && self->findOnlineLibraryEntry (match.songSlug) != nullptr;

            // The server froze the room's chart; the client must play exactly
            // that chart. Download the missing stems and preload before `ready`.
            if (! downloaded)
            {
                self->beginOnlineDuelSongPreparation (
                    match.songSlug, match.songId, true, match.matchId);
                return;
            }

            self->beginOnlineDuelPreload (match.songSlug, true);
        });
}

void MainComponent::startOnlineDuel()
{
    matchOriginSection = currentSection;

    if (duelOnlinePhase != DuelOnlinePhase::None
        && duelOnlinePhase != DuelOnlinePhase::Finished)
        return;

    auto showInfo = [this] (const juce::String& title, const juce::String& message)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                                title, message, "OK", this);
    };

    if (onlineAccessToken.isEmpty())
    {
        showInfo ("Sign in required",
                  "Sign in to your FretFight account to play online.");
        return;
    }

    if (catalogSongs.empty()
        || ! juce::isPositiveAndBelow (selectedCatalogIndex,
                                       static_cast<int> (catalogSongs.size())))
    {
        showInfo ("Choose a song", "Select a server song in the catalogue first.");
        return;
    }

    const auto& song = catalogSongs[static_cast<size_t> (selectedCatalogIndex)];

    if (song.serverId.isEmpty())
    {
        showInfo ("Online unavailable",
                  "This song has no server id. Pick a published catalogue song.");
        return;
    }

    // Every catalogue song is playable regardless of the player level: an
    // online duel is never refused because of the song's difficulty.

    // The server freezes the analysed chart at the song tempo. A 50-120%
    // client tempo would move every note away from its frozen timestamp, so an
    // online duel always runs at 100%, exactly like the analysed MIDI.
    if (! prepareDuelTempo())
    {
        showInfo ("Tempo change in progress",
                  "A tempo change is still being prepared. Try ONLINE MATCH again in a moment.");
        return;
    }

    // A duel starts a fresh match; clear any finished result card first.
    resetDuelState();

    // The server freezes note_count and note times when the match is created.
    // Download the stems automatically and preload the chart before the
    // 3-second server countdown can start.
    if (! song.downloaded || findOnlineLibraryEntry (song.slug) == nullptr)
    {
        beginOnlineDuelSongPreparation (song.slug, song.serverId, false, {});
        return;
    }

    beginOnlineDuelPreload (song.slug, false);
}

void MainComponent::startOnlineDuelCreate()
{
    if (catalogSongs.empty()
        || ! juce::isPositiveAndBelow (selectedCatalogIndex,
                                       static_cast<int> (catalogSongs.size())))
    {
        closeOnlineDuelDialog();
        return;
    }

    const auto& song = catalogSongs[static_cast<size_t> (selectedCatalogIndex)];

    if (! song.downloaded || findOnlineLibraryEntry (song.slug) == nullptr
        || ! duelOnlineChartReady)
    {
        closeOnlineDuelDialog();

        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Online unavailable",
            "The song chart is not ready yet. Try ONLINE MATCH again.",
            "OK", this);
        return;
    }

    ++duelOnlineGeneration;
    const int generation = duelOnlineGeneration;

    duelOnlineSongId = song.serverId;
    duelOnlineSongSlug = song.slug;
    duelOnlinePhase = DuelOnlinePhase::Searching;
    duelOnlinePollFailures = 0;
    showOnlineDuelDialog ("SEARCHING FOR OPPONENT...\n\n" + song.title);

    juce::Component::SafePointer<MainComponent> safeThis (this);

    duelClient.createMatch (song.serverId, "online", {}, onlineAccessToken,
        [safeThis, generation] (const DuelReply& reply, const DuelMatchSnapshot& match)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || generation != self->duelOnlineGeneration)
                return;

            if (! reply.ok || ! match.valid)
            {
                self->duelOnlinePhase = DuelOnlinePhase::None;
                self->closeOnlineDuelDialog();
                self->handleOnlineDuelReplyError (reply, "create");
                return;
            }

            self->duelOnlineMatchId = match.matchId;
            self->duelOnlineMatch = match;
            self->duelOnlineSnapshotAtMs = duelNowMs();
            self->duelOnlineExpectedNotes = match.noteCount;
            self->applyOnlineDuelCharacters();

            // The match is frozen by the server; the preloaded local chart must
            // have exactly the same note count or note_index would drift.
            const int localNotes =
                self->duelOnlineNoteCount.load (std::memory_order_relaxed);

            if (localNotes <= 0 || localNotes != match.noteCount)
            {
                self->abortOnlineDuelForChartMismatch (localNotes);
                return;
            }

            self->duelOnlinePhase = DuelOnlinePhase::Waiting;
            self->duelOnlinePollFailures = 0;
            self->updateOnlineDuelHud();

            self->showOnlineDuelDialog (
                "WAITING FOR OPPONENT...\n\n"
                + (match.songTitle.isNotEmpty() ? match.songTitle : self->duelOnlineSongSlug)
                + (match.waitingSeconds > 0
                       ? "\nWaiting " + juce::String (match.waitingSeconds) + " s"
                       : juce::String()));

            self->duelOnlineNextPollMs = duelNowMs() + 800.0;
            self->promptOnlineDuelReady();
        });
}

bool MainComponent::prepareDuelTempo()
{
    if (std::abs (speedPercent.load (std::memory_order_relaxed) - 100.0) <= 0.01)
        return true;

    // Duel charts are frozen at the analysed tempo. Restore 100% before the
    // chart is loaded; the caller refuses to start if the change is still
    // being rendered.
    applyTempoPercent (100.0);

    // applyTempoPercent() queues the change while another render is running.
    return std::abs (speedPercent.load (std::memory_order_relaxed) - 100.0) <= 0.01;
}

bool MainComponent::preloadOnlineDuelSong (const juce::String& slug)
{
    if (slug.isEmpty())
        return false;

    auto* entry = findOnlineLibraryEntry (slug);

    if (entry == nullptr)
        return false;

    // The preload replaces the loaded song, so the preview/background track
    // must stop exactly as in the normal start path.
    stopPreviewPlayback();
    backgroundPlayer.stop();

    // loadSongMidi() loads the chart and decodes/time-stretches every stem on
    // the message thread. Doing it before `ready` keeps the 3-second server
    // countdown free of I/O and audio preparation.
    if (! loadSongMidi (entry->title))
        return false;

    const int noteCount =
        static_cast<int> (gameSession.getMidiNotes().size());

    if (noteCount <= 0)
        return false;

    if (duelOnlineExpectedNotes > 0 && noteCount != duelOnlineExpectedNotes)
    {
        duelDebugLog ("online chart mismatch local=" + juce::String (noteCount)
                      + " server=" + juce::String (duelOnlineExpectedNotes));
        return false;
    }

    duelOnlineNoteCount.store (noteCount, std::memory_order_relaxed);
    duelOnlineChartReady = true;
    // The readiness flag belongs to this slug: a chart left over from the
    // previous match can never satisfy the new one.
    duelOnlinePreloadedSlug = slug;

    const auto& notes = gameSession.getMidiNotes();
    const double bpmNow = baseBpm;
    const double firstSeconds = bpmNow > 0.0
        ? notes.front().startBeat * 60.0 / bpmNow : 0.0;
    const double lastSeconds = bpmNow > 0.0
        ? notes.back().startBeat * 60.0 / bpmNow : 0.0;

    duelDebugLog ("online preload slug=" + slug
                  + " notes=" + juce::String (noteCount)
                  + " bpm=" + juce::String (bpmNow, 2)
                  + " songBpm=" + juce::String (duelOnlineMatch.songBpm)
                  + " first=" + juce::String (firstSeconds, 3)
                  + " last=" + juce::String (lastSeconds, 3)
                  + " speed=" + juce::String (
                        speedPercent.load (std::memory_order_relaxed), 1));

    return true;
}

void MainComponent::beginOnlineDuelSongPreparation (const juce::String& slug,
                                                    const juce::String& serverId,
                                                    bool joinAfterDownload,
                                                    const juce::String& joinMatchId)
{
    if (slug.isEmpty() || duelOnlineDownloadInProgress)
        return;

    duelOnlineDownloadInProgress = true;
    duelOnlineDownloadJoinAfter = joinAfterDownload;
    duelOnlineDownloadMatchId = joinMatchId;
    duelOnlineDownloadSlug = slug;
    duelOnlineDownloadPercentShown = -1;
    duelOnlinePhase = DuelOnlinePhase::Preparing;

    juce::String title = slug;

    for (const auto& row : catalogSongs)
        if (row.slug == slug && row.title.isNotEmpty())
        {
            title = row.title;
            break;
        }

    showOnlineDuelDialog ("DOWNLOADING SONG...\n\n" + title);
    duelDebugLog ("online download start slug=" + slug
                  + " joinAfter=" + juce::String (joinAfterDownload ? 1 : 0));
    downloadOnlineSong (slug, serverId, false, true);
}

void MainComponent::completeOnlineDuelSongPreparation (bool downloadOk,
                                                       const juce::String& downloadedSlug)
{
    if (! duelOnlineDownloadInProgress)
        return;

    duelOnlineDownloadInProgress = false;

    const auto slug = duelOnlineDownloadSlug.isNotEmpty()
                          ? duelOnlineDownloadSlug
                          : downloadedSlug;
    const bool joinAfter = duelOnlineDownloadJoinAfter;
    const auto joinMatchId = duelOnlineDownloadMatchId;

    duelOnlineDownloadJoinAfter = false;
    duelOnlineDownloadMatchId.clear();

    duelDebugLog ("online download done ok=" + juce::String (downloadOk ? 1 : 0)
                  + " slug=" + slug
                  + " joinAfter=" + juce::String (joinAfter ? 1 : 0));

    if (! downloadOk)
    {
        if (joinAfter && joinMatchId.isNotEmpty())
            cancelOnlineDuel (true);
        else
        {
            duelOnlinePhase = DuelOnlinePhase::None;
            closeOnlineDuelDialog();
            clearOnlineDuelState();
        }

        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Download failed",
            "The song and its stems could not be downloaded. "
                "Check the connection and try again.",
            "OK", this);
        return;
    }

    beginOnlineDuelPreload (slug, joinAfter);
}

void MainComponent::beginOnlineDuelPreload (const juce::String& slug, bool joinAfter)
{
    if (slug.isEmpty() || duelOnlinePreloadInProgress)
        return;

    // The downloaded row may have been added or refreshed by the completion;
    // keep the selection on the song that is actually about to be played.
    int index = -1;

    for (int i = 0; i < static_cast<int> (catalogSongs.size()); ++i)
        if (catalogSongs[static_cast<size_t> (i)].slug == slug)
        {
            index = i;
            break;
        }

    if (index < 0)
    {
        if (joinAfter)
            cancelOnlineDuel (true);
        else
        {
            duelOnlinePhase = DuelOnlinePhase::None;
            closeOnlineDuelDialog();
            clearOnlineDuelState();
        }

        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Online unavailable",
            "The song is not in the current catalogue. "
                "Refresh the catalogue and try again.",
            "OK", this);
        return;
    }

    selectedCatalogIndex = index;
    // Online: both guitarists are the characters the players picked. The
    // song's assigned character is only the fallback.
    if (duelOnlineMatch.valid && duelOnlineMatch.songSlug.isNotEmpty())
        applyOnlineDuelCharacters();
    else
        applySongStageVisuals (slug);

    juce::String title = slug;

    for (const auto& row : catalogSongs)
        if (row.slug == slug && row.title.isNotEmpty())
        {
            title = row.title;
            break;
        }

    // Let the modal repaint once before the synchronous chart/stem preload.
    showOnlineDuelDialog ("PREPARING SONG...\n\n" + title);
    duelOnlinePreloadInProgress = true;
    const int generation = duelOnlineGeneration;
    juce::Component::SafePointer<MainComponent> safeThis (this);

    juce::MessageManager::callAsync ([safeThis, slug, joinAfter, generation]()
    {
        auto* self = safeThis.getComponent();

        if (self == nullptr || generation != self->duelOnlineGeneration)
            return;

        self->duelOnlinePreloadInProgress = false;

        // A finished/cancelled match invalidates the preparation even if the
        // generation did not change (the server can close the room mid-download).
        if (self->duelOnlinePhase != DuelOnlinePhase::Preparing
            && self->duelOnlinePhase != DuelOnlinePhase::Waiting
            && self->duelOnlinePhase != DuelOnlinePhase::Searching
            && self->duelOnlinePhase != DuelOnlinePhase::None)
            return;

        if (! self->preloadOnlineDuelSong (slug))
        {
            // The local chart is missing or does not match the frozen server
            // chart. Re-download the song once with the normal progress
            // dialog, then retry the preload; only cancel if it still differs.
            if (! self->duelOnlineChartReloadAttempted
                && self->duelOnlineMatchId.isNotEmpty())
            {
                self->duelOnlineChartReloadAttempted = true;
                self->beginOnlineDuelSongPreparation (
                    slug, self->duelOnlineMatch.songId, joinAfter,
                    self->duelOnlineMatch.matchId);
                return;
            }

            const int localNotes =
                static_cast<int> (self->gameSession.getMidiNotes().size());

            if (joinAfter)
            {
                if (localNotes > 0)
                {
                    self->abortOnlineDuelForChartMismatch (localNotes);
                }
                else
                {
                    self->cancelOnlineDuel (true);

                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "Chart unavailable",
                        "The room's chart could not be loaded. "
                            "Try downloading the song again.",
                        "OK", self);
                }
            }
            else
            {
                self->duelOnlinePhase = DuelOnlinePhase::None;
                self->closeOnlineDuelDialog();
                self->clearOnlineDuelState();

                juce::AlertWindow::showMessageBoxAsync (
                    juce::MessageBoxIconType::WarningIcon,
                    "Chart unavailable",
                    "The downloaded chart could not be loaded. "
                        "Try downloading the song again.",
                    "OK", self);
            }
            return;
        }

        if (joinAfter)
        {
            self->duelOnlineChartReloadAttempted = false;

            // The join already succeeded; only the readiness confirmation was
            // waiting for the local chart. The server countdown starts now.
            self->duelOnlinePhase = DuelOnlinePhase::Waiting;
            self->updateOnlineDuelHud();
            self->showOnlineDuelDialog ("WAITING FOR OPPONENT...\n\n"
                                        + (self->duelOnlineMatch.songTitle.isNotEmpty()
                                               ? self->duelOnlineMatch.songTitle
                                               : self->duelOnlineSongSlug));
            self->duelOnlineNextPollMs = duelNowMs() + 800.0;

            // A room match was already confirmed by both players in the room
            // window: the download is the only thing left, so report `ready`
            // without a second "Start the duel?" dialog. The quick queue keeps
            // its own confirmation.
            if (self->isOnlineRoomMatch())
            {
                self->duelOnlineReadyConfirmed = true;
                self->sendOnlineDuelReady();
            }
            else
            {
                self->promptOnlineDuelReady();
            }
        }
        else
        {
            self->startOnlineDuelCreate();
        }
    });
}

void MainComponent::updateOnlineDuelPreparationDialog()
{
    if (! duelOnlineDownloadInProgress || songDownloadProgressState == nullptr)
        return;

    const int percent = juce::jlimit (
        0, 100, static_cast<int> (std::lround (songDownloadProgressValue * 100.0)));

    if (percent == duelOnlineDownloadPercentShown)
        return;

    duelOnlineDownloadPercentShown = percent;

    juce::String title = duelOnlineDownloadSlug;

    for (const auto& row : catalogSongs)
        if (row.slug == duelOnlineDownloadSlug && row.title.isNotEmpty())
        {
            title = row.title;
            break;
        }

    showOnlineDuelDialog ("DOWNLOADING SONG... " + juce::String (percent) + "%\n\n" + title);
}


void MainComponent::cancelOnlineDuel (bool sendLeave)
{
    const auto matchId = duelOnlineMatchId;
    const auto lobbyId = duelOnlineLobbyId;
    const auto token = onlineAccessToken;
    const bool hadMatch = matchId.isNotEmpty()
                          && duelOnlinePhase != DuelOnlinePhase::None
                          && duelOnlinePhase != DuelOnlinePhase::Finished;

    // Any reply from the cancelled flow is ignored from now on.
    ++duelOnlineGeneration;
    duelOnlinePhase = DuelOnlinePhase::None;
    duelOnlineActive.store (false, std::memory_order_relaxed);
    closeOnlineDuelDialog();
    clearOnlineDuelState();

    if (sendLeave && hadMatch && token.isNotEmpty())
    {
        if (lobbyId.isNotEmpty())
        {
            // A room match: after leaving, put both players back into the room
            // (the server cancels the fresh match and rolls the room back).
            juce::Component::SafePointer<MainComponent> safeThis (this);

            duelClient.leave (matchId, token,
                [safeThis, lobbyId, matchId] (const DuelReply&)
                {
                    if (auto* self = safeThis.getComponent())
                        self->returnOnlineDuelToRoom (lobbyId, matchId);
                });
            return;
        }

        duelClient.leave (matchId, token, nullptr);
        return;
    }

    // No leave request was sent (the server already cancelled the match), but
    // the room still has to be restored on this side.
    if (lobbyId.isNotEmpty() && hadMatch)
        returnOnlineDuelToRoom (lobbyId, matchId);
}

void MainComponent::returnOnlineDuelToRoom (const juce::String& lobbyId,
                                           const juce::String& matchId)
{
    if (lobbyId.isEmpty() || onlineAccessToken.isEmpty())
        return;

    const auto token = onlineAccessToken;
    juce::Component::SafePointer<MainComponent> safeThis (this);

    auto openRoom = [safeThis, lobbyId] ()
    {
        if (auto* self = safeThis.getComponent())
            self->openOnlineDuelRoom (lobbyId);
    };

    if (matchId.isEmpty())
    {
        openRoom();
        return;
    }

    // `return-to-lobby` is idempotent for a cancelled/finished match; a room
    // that the host closed answers 409 and must not be reopened.
    duelClient.returnToLobby (matchId, token,
        [openRoom] (const DuelReply& reply, const DuelRoomSnapshot& returnedRoom)
        {
            if (reply.ok && returnedRoom.valid)
                openRoom();
        });
}

void MainComponent::goToOnlineDuelLobby()
{
    // GO TO LOBBY on a finished online match: the server puts the room back to
    // `waiting` keeping the chat and the current song, then the room window is
    // reopened with its fresh state.
    const auto matchId = duelOnlineMatchId;
    const auto token = onlineAccessToken;

    // A quick-queue match has no room to return to: the Online section (the
    // lobby list) is the only sensible destination.
    if (! isOnlineRoomMatch() || matchId.isEmpty() || token.isEmpty())
    {
        leaveOnlineDuelLobby();
        return;
    }

    juce::Component::SafePointer<MainComponent> safeThis (this);

    duelClient.returnToLobby (matchId, token,
        [safeThis] (const DuelReply& reply, const DuelRoomSnapshot& room)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            if (! reply.ok || ! room.valid)
            {
                // A closed room must not dead-end the player: fall back to the
                // Online section, where the lobby list is already refreshed.
                self->leaveOnlineDuelLobby();

                juce::AlertWindow::showMessageBoxAsync (
                    juce::MessageBoxIconType::InfoIcon,
                    "Online room",
                    reply.errorMessage.isNotEmpty()
                        ? reply.errorMessage
                        : juce::String ("This room is no longer available."),
                    "OK", self);
                return;
            }

            self->clearOnlineDuelState();
            self->returnToSongSelect();
            self->openOnlineDuelRoom (room.lobbyId);
        });
}

void MainComponent::leaveOnlineDuelLobby()
{
    // LEAVE LOBBY on a finished online match: drop the room and land on the
    // Online section (the lobby list). The player is never left on the result
    // screen and never ends up inside the room.
    const auto lobbyId = duelOnlineLobbyId;
    const auto token = onlineAccessToken;

    // Ignore every reply of the finished match before the state goes away.
    ++duelOnlineGeneration;
    duelOnlineResultInFlight = false;
    duelOnlinePollInFlight = false;
    duelOnlineReadyInFlight = false;

    clearOnlineDuelState();
    returnToSongSelect();

    // The Online section is the lobby list; setSection() starts a fresh
    // refresh of it. It is a no-op when the section is already active, hence
    // the explicit refresh in that case.
    if (currentSection != AppSection::OnlineDuel)
        setSection (AppSection::OnlineDuel);
    else
    {
        duelLobbyRefreshTicks = 0;
        refreshDuelLobby();
    }

    if (lobbyId.isEmpty() || token.isEmpty())
        return;

    // The room may already be gone (the host left, the server closed it): 404
    // and 409 are expected answers and must not stop the navigation above.
    duelClient.leaveRoom (lobbyId, token, nullptr);
}

void MainComponent::showOnlineDuelDialog (const juce::String& message)
{
    if (auto* existing = duelOnlineDialog.getComponent())
    {
        existing->setMessage (message);
        return;
    }

    auto* dialog = new juce::AlertWindow ("Guitar Duel", message,
                                          juce::AlertWindow::NoIcon);
    dialog->addButton ("CANCEL", 0);
    duelOnlineDialog = dialog;

    juce::Component::SafePointer<MainComponent> safeThis (this);
    const int generation = duelOnlineGeneration;

    dialog->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [safeThis, generation] (int result)
            {
                auto* self = safeThis.getComponent();
                if (self == nullptr || generation != self->duelOnlineGeneration)
                    return;

                // Any dismissal that is not the internal "match started"
                // result (1) is a cancellation.
                if (result == 0)
                    self->cancelOnlineDuel (true);
            }),
        true);
}

void MainComponent::closeOnlineDuelDialog()
{
    if (auto* dialog = duelOnlineDialog.getComponent())
        dialog->exitModalState (1);

    duelOnlineDialog = nullptr;
}

void MainComponent::promptOnlineDuelReady()
{
    if (duelOnlineReadyConfirmed
        || duelOnlineReadyPromptShown
        || duelOnlineMatchId.isEmpty())
        return;

    // A match that was confirmed in the room window is never confirmed again.
    // Some handoff paths (a state reset during the download, a join snapshot
    // without lobby_id) lose the lobby id, which used to turn the room match
    // into a "queue" match and showed this dialog a second time. Reporting
    // readiness directly keeps the single-confirmation promise.
    if (isOnlineRoomMatch())
    {
        duelDebugLog ("ready prompt suppressed: match confirmed in the room");
        duelOnlineReadyConfirmed = true;
        duelOnlineReadyPromptShown = true;
        sendOnlineDuelReady();
        return;
    }

    duelOnlineReadyPromptShown = true;

    const auto songTitle = duelOnlineMatch.songTitle.isNotEmpty()
        ? duelOnlineMatch.songTitle
        : (duelOnlineSongSlug.isNotEmpty() ? duelOnlineSongSlug
                                           : juce::String ("the selected song"));

    // The preload has finished, so this side's stems are on disk. Both players
    // press READY; the server starts the shared countdown when the second
    // confirmation arrives.
    closeOnlineDuelDialog();

    auto* dialog = new juce::AlertWindow (
        "Start the duel?",
        "Both players must confirm before the song starts.\n\n"
        + songTitle
        + "\n\nThe downloads are finished on this side. "
          "The match begins when your opponent is ready too.",
        juce::AlertWindow::QuestionIcon);

    dialog->addButton ("CONFIRM START", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dialog->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    duelOnlineReadyDialog = dialog;

    juce::Component::SafePointer<MainComponent> safeThis (this);
    const int generation = duelOnlineGeneration;

    dialog->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [safeThis, generation, songTitle] (int result)
            {
                auto* self = safeThis.getComponent();

                if (self == nullptr || generation != self->duelOnlineGeneration
                    || self->duelOnlineReadyPromptClosing)
                    return;

                self->duelOnlineReadyDialog = nullptr;

                if (result != 1)
                {
                    self->cancelOnlineDuel (true);
                    return;
                }

                self->duelOnlineReadyConfirmed = true;
                self->showOnlineDuelDialog ("WAITING FOR OPPONENT...\n\n"
                                            + songTitle);
                self->sendOnlineDuelReady();
            }),
        true);
}

bool MainComponent::onlineDuelContentComplete (const juce::String& slug)
{
    if (slug.isEmpty())
        return false;

    auto* entry = findOnlineLibraryEntry (slug);

    if (entry == nullptr
        || ! entry->soloMidi.existsAsFile()
        || entry->soloMidi.getSize() <= 0)
        return false;

    // The catalogue row carries the content version the server published. The
    // downloader rewrites version.txt only after the whole asset set of that
    // version finished, so a mismatch (or a missing file while a re-download of
    // a bumped version is running) means "not ready yet".
    int catalogVersion = 0;

    for (const auto& row : catalogSongs)
        if (row.slug == slug)
        {
            catalogVersion = row.version;
            break;
        }

    if (catalogVersion > 0)
    {
        const auto versionFile = entry->folder.getChildFile ("version.txt");

        if (! versionFile.existsAsFile()
            || versionFile.loadFileAsString().trim().getIntValue() != catalogVersion)
            return false;
    }

    // Every stem the downloader recorded must still be a file on disk. While a
    // stem is being written the version gate above already blocks the report;
    // this catches a deleted or evicted file.
    for (const auto* stem : { &entry->drums, &entry->bass,
                              &entry->rhythmGuitar, &entry->others })
        if (stem->getFullPathName().isNotEmpty() && ! stem->existsAsFile())
            return false;

    return true;
}

void MainComponent::sendOnlineDuelReady()
{
    if (! duelOnlineReadyConfirmed
        || duelOnlineReadyInFlight
        || duelOnlineMatchId.isEmpty()
        || onlineAccessToken.isEmpty())
        return;

    const auto now = duelNowMs();

    if (now < duelOnlineNextReadyMs)
        return;

    // Readiness is a promise that the chart AND the audio stems are local: the
    // server starts the shared countdown for both players the moment the second
    // confirmation arrives. Reporting ready while anything is still downloading
    // started a match whose song one side could not play, so the check is the
    // last line of defence for every caller.
    const auto contentSlug = duelOnlineMatch.songSlug.isNotEmpty()
                                 ? duelOnlineMatch.songSlug
                                 : duelOnlineSongSlug;

    // A snapshot without song info cannot be verified against the cache. Do not
    // dead-lock an otherwise valid match (older servers may omit it): only a
    // known slug is gated.
    const bool contentVerified =
        contentSlug.isEmpty()
        || (duelOnlineChartReady && duelOnlinePreloadedSlug == contentSlug
            && onlineDuelContentComplete (contentSlug));

    if (! contentVerified)
    {
        duelDebugLog ("ready deferred: content incomplete slug=" + contentSlug
                      + " chart=" + juce::String (duelOnlineChartReady ? 1 : 0)
                      + " preloaded=" + duelOnlinePreloadedSlug);

        // Repair instead of confirming: fetch the missing (or version-mismatched)
        // assets, and the normal preparation path reports readiness once the
        // preload for this slug has succeeded.
        if (! duelOnlineDownloadInProgress && ! duelOnlinePreloadInProgress
            && contentSlug.isNotEmpty())
            beginOnlineDuelSongPreparation (
                contentSlug,
                duelOnlineMatch.songId.isNotEmpty() ? duelOnlineMatch.songId
                                                    : duelOnlineSongId,
                true, duelOnlineMatchId);

        duelOnlineNextReadyMs = now + 1000.0;
        return;
    }

    duelOnlineReadyInFlight = true;
    duelOnlineNextReadyMs = now + 2000.0;

    const int generation = duelOnlineGeneration;
    const auto matchId = duelOnlineMatchId;
    const auto sentMs = duelNowMs();
    juce::Component::SafePointer<MainComponent> safeThis (this);

    duelClient.ready (matchId, onlineAccessToken,
        [safeThis, generation, sentMs] (const DuelReply& reply, const DuelMatchSnapshot& match)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || generation != self->duelOnlineGeneration)
                return;

            self->duelOnlineReadyInFlight = false;

            if (! reply.ok)
            {
                if (reply.statusCode == 409 && reply.isErrorCode ("no_opponent"))
                {
                    self->offerBotAfterNoOpponent();
                    return;
                }

                if (reply.statusCode == 401
                    || reply.statusCode == 403
                    || reply.statusCode == 404
                    || (reply.statusCode == 409 && reply.errorCode.isNotEmpty()))
                {
                    self->handleOnlineDuelReplyError (reply, "ready");
                    return;
                }

                // Transient failure: the polling loop retries `ready` while the
                // match is still waiting.
                return;
            }

            self->noteOnlineDuelRoundTrip (sentMs);
            self->applyOnlineDuelState (match);
        });
}

void MainComponent::pollOnlineDuel()
{
    if (duelOnlinePollInFlight
        || duelOnlineMatchId.isEmpty()
        || onlineAccessToken.isEmpty())
        return;

    const auto now = duelNowMs();

    if (now < duelOnlineNextPollMs)
        return;

    duelOnlinePollInFlight = true;
    duelOnlineNextPollMs = now + 900.0;

    const int generation = duelOnlineGeneration;
    const auto matchId = duelOnlineMatchId;
    const auto token = onlineAccessToken;
    const auto sentMs = duelNowMs();
    juce::Component::SafePointer<MainComponent> safeThis (this);

    duelClient.getState (matchId, token,
        [safeThis, generation, sentMs] (const DuelReply& reply, const DuelMatchSnapshot& match)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || generation != self->duelOnlineGeneration)
                return;

            self->duelOnlinePollInFlight = false;
            self->noteOnlineDuelRoundTrip (sentMs);

            if (! reply.transportOk)
            {
                ++self->duelOnlinePollFailures;

                duelDebugLog ("poll transport error failures="
                              + juce::String (self->duelOnlinePollFailures));

                // The match itself survives a network hiccup: keep polling and
                // the next successful GET restores HP and next_note_index.
                if (self->duelOnlinePollFailures >= 6
                    && self->duelOnlinePhase == DuelOnlinePhase::Waiting)
                {
                    self->cancelOnlineDuel (false);
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "Server unavailable",
                        "Could not reach the duel server. Try again later.",
                        "OK", self);
                }
                return;
            }

            if (reply.statusCode == 401
                || reply.statusCode == 403
                || reply.statusCode == 404)
            {
                duelDebugLog ("poll error status=" + juce::String (reply.statusCode)
                              + " code=" + reply.errorCode);
                self->handleOnlineDuelReplyError (reply, "state");
                return;
            }

            if (! reply.ok || ! match.valid)
            {
                duelDebugLog ("poll rejected status=" + juce::String (reply.statusCode)
                              + " code=" + reply.errorCode
                              + " msg=" + reply.errorMessage);
                return;
            }

            self->duelOnlinePollFailures = 0;
            self->applyOnlineDuelState (match);
        });
}

void MainComponent::updateOnlineDuel()
{
    if (duelOnlinePhase == DuelOnlinePhase::None
        || duelOnlinePhase == DuelOnlinePhase::Finished)
        return;

    if (duelOnlineDownloadInProgress)
        updateOnlineDuelPreparationDialog();

    pollOnlineDuel();

    if (duelOnlinePhase == DuelOnlinePhase::Playing
        || duelOnlinePhase == DuelOnlinePhase::Finishing)
        flushOnlineDuelEvents();
}

void MainComponent::applyOnlineDuelState (const DuelMatchSnapshot& incoming)
{
    if (! incoming.valid)
        return;

    // Once the result card is shown, an in-flight poll started before the
    // finish can still deliver an older `playing` snapshot. Applying it would
    // overwrite duelOnlineMatch and hide the victory/defeat card.
    if (duelOnlinePhase == DuelOnlinePhase::Finished
        && incoming.status != "finished")
    {
        duelDebugLog ("online stale snapshot ignored status=" + incoming.status
                      + " next=" + juce::String (incoming.nextNoteIndex));
        return;
    }

    auto snapshot = incoming;

    // Broadcast snapshots carry your_side = null; a REST snapshot always has
    // it. Keep the last known side rather than losing the mapping.
    if (snapshot.yourSide.isEmpty())
        snapshot.yourSide = duelOnlineMatch.yourSide;

    duelOnlineMatch = snapshot;
    duelOnlineMatchId = snapshot.matchId.isNotEmpty() ? snapshot.matchId
                                                      : duelOnlineMatchId;
    duelOnlineSnapshotAtMs = duelNowMs();
    duelOnlineExpectedNotes = snapshot.noteCount;
    updateOnlineDuelHud();
    // Each poll refreshes the characters too: the opponent can change theirs in
    // the lobby before the start, and the snapshot carries the final choice.
    applyOnlineDuelCharacters();

    duelDebugLog ("online state status=" + snapshot.status
                  + " rules=" + juce::String (snapshot.rulesVersion)
                  + " phase=" + juce::String (static_cast<int> (duelOnlinePhase))
                  + " hp=" + juce::String (snapshot.findYou() != nullptr
                                               ? snapshot.findYou()->hp : -1.0, 1)
                  + " next=" + juce::String (snapshot.nextNoteIndex)
                  + " notes=" + juce::String (snapshot.noteCount));

    // While the song is downloading or preloading, the match must stay in the
    // preparation phase: `ready` may only be sent once the chart and stems are
    // cached, otherwise the server countdown starts on an unprepared client.
    if ((duelOnlineDownloadInProgress || duelOnlinePreloadInProgress)
        && snapshot.status != "cancelled"
        && snapshot.status != "finished")
    {
        duelDebugLog ("online preparation keeps phase="
                      + juce::String (static_cast<int> (duelOnlinePhase))
                      + " status=" + snapshot.status);
        return;
    }

    if (snapshot.status == "waiting")
    {
        if (duelOnlinePhase == DuelOnlinePhase::Playing
            || duelOnlinePhase == DuelOnlinePhase::Finishing
            || duelOnlinePhase == DuelOnlinePhase::Finished)
            return;

        duelOnlinePhase = DuelOnlinePhase::Waiting;

        if (snapshot.waitingExpired && ! isOnlineRoomMatch())
        {
            offerBotAfterNoOpponent();
            return;
        }

        showOnlineDuelDialog (
            "WAITING FOR OPPONENT...\n\n"
            + (snapshot.songTitle.isNotEmpty() ? snapshot.songTitle
                                               : duelOnlineSongSlug)
            + (snapshot.waitingSeconds > 0
                   ? "\nWaiting " + juce::String (snapshot.waitingSeconds) + " s"
                   : juce::String()));

        // Once the opponent has joined, both sides must confirm readiness
        // after their own stems have been prepared. A confirmed side retries
        // `ready` on the poll cadence until the server records it.
        if (snapshot.participants.size() >= 2)
        {
            const auto* you = snapshot.findYou();

            if (you == nullptr || ! you->ready)
            {
                // A room match was confirmed in the room window before the
                // download: no second prompt is shown, the client just reports
                // that its content is ready.
                if (isOnlineRoomMatch())
                {
                    duelOnlineReadyConfirmed = true;
                    duelOnlineReadyPromptShown = true;
                    sendOnlineDuelReady();
                }
                else if (duelOnlineReadyConfirmed)
                    sendOnlineDuelReady();
                else
                    promptOnlineDuelReady();
            }
        }
        return;
    }

    if (snapshot.status == "playing")
    {
        closeOnlineDuelDialog();

        if (duelOnlinePhase != DuelOnlinePhase::Playing
            && duelOnlinePhase != DuelOnlinePhase::Finishing
            && duelOnlinePhase != DuelOnlinePhase::Finished)
        {
            beginOnlineDuelPlayback();
        }
        return;
    }

    if (snapshot.status == "finished")
    {
        finishOnlineDuelFromServer();
        return;
    }

    if (snapshot.status == "cancelled")
    {
        if (isOnlineRoomMatch())
        {
            // A room match was aborted (the opponent left, or the server
            // cancelled it). The room was rolled back to `waiting`, so both
            // players simply return to the same room window.
            cancelOnlineDuel (false);
            return;
        }

        const bool wasWaiting = duelOnlinePhase == DuelOnlinePhase::Waiting
                                || duelOnlinePhase == DuelOnlinePhase::Searching;

        if (wasWaiting)
        {
            offerBotAfterNoOpponent();
        }
        else if (duelOnlinePhase != DuelOnlinePhase::Finished)
        {
            cancelOnlineDuel (false);
            juce::AlertWindow::showMessageBoxAsync (
                juce::MessageBoxIconType::WarningIcon,
                "Duel cancelled",
                "The duel match was cancelled.",
                "OK", this);
        }
    }
}

void MainComponent::applyOnlineDuelExchangeFeedback (const DuelEventsReply& reply)
{
    if (reply.exchanges.empty())
        return;

    const auto& last = reply.exchanges.back();
    const bool youArePlayer = duelOnlineMatch.yourSide != "opponent";

    // Trigger the pixel-art reaction for every exchange in the batch, not only
    // the last one: the server may resolve several due notes at once.
    for (const auto& item : reply.exchanges)
    {
        auto token = item.exchange;

        if (! youArePlayer)
        {
            if (token == "player_hit")
                token = "opponent_hit";
            else if (token == "opponent_hit")
                token = "player_hit";
        }

        if (token == "player_hit" || token == "block")
            duelStage.triggerPlayerHit();

        if (token == "opponent_hit" || token == "block")
            duelStage.triggerOpponentHit();
    }

    juce::String yourExchange = last.exchange;

    if (! youArePlayer)
    {
        if (yourExchange == "player_hit")
            yourExchange = "opponent_hit";
        else if (yourExchange == "opponent_hit")
            yourExchange = "player_hit";
    }

    DuelExchange exchange = DuelExchange::BothMiss;
    int damageMilli = 0;

    if (yourExchange == "block")
    {
        exchange = DuelExchange::Block;
    }
    else if (yourExchange == "player_hit")
    {
        exchange = DuelExchange::PlayerHit;
        damageMilli = static_cast<int> (std::lround (
            (youArePlayer ? last.damageToOpponent : last.damageToPlayer) * 1000.0));
    }
    else if (yourExchange == "opponent_hit")
    {
        exchange = DuelExchange::OpponentHit;
        damageMilli = static_cast<int> (std::lround (
            (youArePlayer ? last.damageToPlayer : last.damageToOpponent) * 1000.0));
    }

    duelLastExchange.store (static_cast<int> (exchange), std::memory_order_relaxed);
    duelLastDamageMilli.store (damageMilli, std::memory_order_relaxed);
    duelFlashUntilMs.store (duelNowMs() + 900.0, std::memory_order_relaxed);
}

void MainComponent::handleOnlineDuelReplyError (const DuelReply& reply,
                                                const juce::String& stage)
{
    if (reply.statusCode == 409 && reply.isErrorCode ("no_opponent"))
    {
        offerBotAfterNoOpponent();
        return;
    }

    // Invalidate every callback of the failed flow before showing the dialog.
    ++duelOnlineGeneration;
    duelOnlinePhase = DuelOnlinePhase::None;
    duelOnlineActive.store (false, std::memory_order_relaxed);
    closeOnlineDuelDialog();
    clearOnlineDuelState();

    juce::String title = "Online unavailable";
    juce::String message;

    if (reply.statusCode == 401)
    {
        // Access tokens live in memory only; the refresh flow is the way back.
        title = "Sign in again";
        message = "Your session expired. Sign in again to play online.";
    }
    else if (reply.statusCode == 403 || reply.statusCode == 404)
    {
        title = "Match closed";
        message = "The duel match is no longer available.";
    }
    else if (reply.statusCode == 429)
    {
        title = "Slow down";
        message = "Too many duel requests. Wait a moment and try again.";
    }
    else if (! reply.transportOk)
    {
        title = "Server unavailable";
        message = "Could not reach the duel server. Check the connection and try again.";
    }
    else if (reply.errorMessage.isNotEmpty())
    {
        message = reply.errorMessage;
    }
    else
    {
        message = "The server rejected the request (" + stage + ").";
    }

    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                            title, message, "OK", this);
}

void MainComponent::offerBotAfterNoOpponent()
{
    const auto matchId = duelOnlineMatchId;
    const auto token = onlineAccessToken;
    const auto songTitle = duelOnlineMatch.songTitle.isNotEmpty()
                               ? duelOnlineMatch.songTitle
                               : duelOnlineSongSlug;

    ++duelOnlineGeneration;
    duelOnlinePhase = DuelOnlinePhase::None;
    duelOnlineActive.store (false, std::memory_order_relaxed);
    closeOnlineDuelDialog();
    clearOnlineDuelState();

    // The server already cancelled a timed-out queue; the call is idempotent.
    if (matchId.isNotEmpty() && token.isNotEmpty())
        duelClient.leave (matchId, token, nullptr);

    juce::Component::SafePointer<MainComponent> safeThis (this);

    juce::AlertWindow::showOkCancelBox (
        juce::MessageBoxIconType::QuestionIcon,
        "No opponent found",
        "No opponent joined in time"
            + (songTitle.isNotEmpty() ? " for " + songTitle : juce::String())
            + ". Play against a bot on the same song?",
        "PLAY BOT", "CANCEL", this,
        juce::ModalCallbackFunction::create (
            [safeThis] (int result)
            {
                if (result == 0)
                    return;

                if (auto* self = safeThis.getComponent())
                    self->requestDuelWithBot (1);
            }));
}

void MainComponent::abortOnlineDuelForChartMismatch (int localNotes)
{
    const int expectedNotes = duelOnlineExpectedNotes;
    const auto songTitle = duelOnlineMatch.songTitle.isNotEmpty()
                               ? duelOnlineMatch.songTitle
                               : duelOnlineSongSlug;
    const bool roomMatch = isOnlineRoomMatch();

    cancelOnlineDuel (true);

    // Keep the start path in beginTuningForSelectedSong() from opening the
    // tuner for a match that no longer exists.
    duelOnlineStartAborted = true;

    if (roomMatch)
    {
        // Both players are back in the room. A bot duel on a different chart
        // makes no sense there, so only explain what happened.
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Online match cancelled",
            "The server chart" + (songTitle.isNotEmpty() ? " of " + songTitle : juce::String())
                + " has " + juce::String (expectedNotes)
                + " notes, but your local chart has " + juce::String (localNotes)
                + ". The duel was cancelled and you are back in the room.",
            "OK", this);
        return;
    }

    juce::Component::SafePointer<MainComponent> safeThis (this);

    juce::AlertWindow::showOkCancelBox (
        juce::MessageBoxIconType::WarningIcon,
        "Online match cancelled",
        "The server chart" + (songTitle.isNotEmpty() ? " of " + songTitle : juce::String())
            + " has " + juce::String (expectedNotes)
            + " notes, but your local chart has " + juce::String (localNotes)
            + ". Note indices would not line up, so the online match was cancelled. "
              "Play against a bot instead?",
        "PLAY BOT", "CANCEL", this,
        juce::ModalCallbackFunction::create (
            [safeThis] (int result)
            {
                if (result == 0)
                    return;

                if (auto* self = safeThis.getComponent())
                    self->requestDuelWithBot (1);
            }));
}

void MainComponent::queueOnlineDuelNote (int noteIndex, int hitQuality)
{
    // The server already decided the match: trailing chart notes are not part
    // of the duel and must not be sent as extra misses.
    if (duelOnlinePhase == DuelOnlinePhase::Finishing
        || duelOnlinePhase == DuelOnlinePhase::Finished)
        return;

    const int noteCount = duelOnlineNoteCount.load (std::memory_order_relaxed);

    if (noteIndex < 0 || (noteCount > 0 && noteIndex >= noteCount))
        return;

    const auto quality = toDuelQualityToken (hitQuality);

    const juce::SpinLock::ScopedLockType lock (duelLock);

    // Notes resolve strictly in chart order. Ignoring a lower index makes a
    // duplicate resolution (a retry or a chord edge case) harmless even before
    // the server sees it; the server also has UNIQUE(match, user, note_index).
    if (noteIndex <= duelOnlineLastQueuedIndex)
        return;

    duelOnlineLastQueuedIndex = noteIndex;
    duelOnlinePendingEvents.push_back ({ noteIndex, quality });
}

void MainComponent::flushOnlineDuelEvents()
{
    if (duelOnlineEventSendingStopped
        || duelOnlineEventInFlight
        || duelOnlineMatchId.isEmpty()
        || onlineAccessToken.isEmpty())
        return;

    const auto now = duelNowMs();

    if (now < duelOnlineNextEventMs)
        return;

    duelOnlineInFlightEvents.clear();

    {
        const juce::SpinLock::ScopedLockType lock (duelLock);

        // One request at a time keeps note_index strictly ordered; a batch of
        // 32 is far below the server limit of 100.
        while (! duelOnlinePendingEvents.empty()
               && duelOnlineInFlightEvents.size() < 32)
        {
            duelOnlineInFlightEvents.push_back (duelOnlinePendingEvents.front());
            duelOnlinePendingEvents.pop_front();
        }
    }

    if (duelOnlineInFlightEvents.empty())
    {
        if (duelOnlineFinishRequested
            && ! duelOnlineFinishSent
            && ! duelOnlineFinishInFlight)
            sendOnlineDuelFinish();

        return;
    }

    duelOnlineEventInFlight = true;

    const int generation = duelOnlineGeneration;
    const auto matchId = duelOnlineMatchId;
    const auto token = onlineAccessToken;
    const auto batch = duelOnlineInFlightEvents;
    juce::Component::SafePointer<MainComponent> safeThis (this);

    duelClient.submitEvents (matchId, batch, token,
        [safeThis, generation, batch] (const DuelReply& reply,
                                       const DuelEventsReply& events)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || generation != self->duelOnlineGeneration)
                return;

            self->duelOnlineEventInFlight = false;

            if (! reply.ok)
            {
                duelDebugLog ("events error status=" + juce::String (reply.statusCode)
                              + " code=" + reply.errorCode
                              + " msg=" + reply.errorMessage);

                if (reply.statusCode == 401)
                {
                    self->handleOnlineDuelReplyError (reply, "events");
                    return;
                }

                if (reply.statusCode == 403 || reply.statusCode == 404)
                {
                    self->handleOnlineDuelReplyError (reply, "events");
                    return;
                }

                if (reply.statusCode == 409)
                {
                    // match_finished / match_cancelled / participant_left:
                    // stop submitting and read the final result instead.
                    self->duelOnlineFinishRequested = false;
                    self->duelOnlineNextEventMs = duelNowMs() + 1500.0;
                    self->requestOnlineDuelResult();
                    return;
                }

                // Transport error or rate limit: keep the batch and retry.
                // The server deduplicates (match, user, note_index), so a
                // response lost in flight cannot apply a note twice.
                {
                    const juce::SpinLock::ScopedLockType lock (self->duelLock);

                    for (auto it = batch.rbegin(); it != batch.rend(); ++it)
                        self->duelOnlinePendingEvents.push_front (*it);
                }

                self->duelOnlineNextEventMs = duelNowMs() + 750.0;
                return;
            }

            self->applyOnlineDuelExchangeFeedback (events);
            self->applyOnlineDuelState (events.match);

            duelDebugLog ("events ok accepted=" + juce::String (static_cast<int> (events.accepted.size()))
                          + " dup=" + juce::String (static_cast<int> (events.duplicates.size()))
                          + " rejected=" + juce::String (static_cast<int> (events.rejected.size()))
                          + (events.rejected.empty() ? juce::String()
                                                     : " first=" + juce::String (events.rejected.front().noteIndex)
                                                           + ":" + events.rejected.front().code));

            if (events.finished)
                self->finishOnlineDuelFromServer();
        });
}

void MainComponent::sendOnlineDuelFinish()
{
    if (duelOnlineFinishInFlight
        || duelOnlineFinishSent
        || duelOnlineMatchId.isEmpty()
        || onlineAccessToken.isEmpty())
        return;

    duelOnlineFinishInFlight = true;
    const int generation = duelOnlineGeneration;
    const auto matchId = duelOnlineMatchId;
    const auto token = onlineAccessToken;
    juce::Component::SafePointer<MainComponent> safeThis (this);

    duelClient.finish (matchId, token,
        [safeThis, generation] (const DuelReply& reply, const DuelFinishReply& finish)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || generation != self->duelOnlineGeneration)
                return;

            self->duelOnlineFinishInFlight = false;

            if (! reply.ok)
            {
                if (reply.statusCode == 401
                    || reply.statusCode == 403
                    || reply.statusCode == 404)
                {
                    self->handleOnlineDuelReplyError (reply, "finish");
                    return;
                }

                if (reply.statusCode == 409)
                {
                    self->duelOnlineFinishSent = true;
                    self->requestOnlineDuelResult();
                    return;
                }

                // Retry the idempotent finish; the server awards XP once.
                self->duelOnlineNextEventMs = duelNowMs() + 1500.0;
                ++self->duelOnlineFinishAttempts;
                return;
            }

            self->duelOnlineFinishSent = true;

            if (finish.match.valid)
                self->applyOnlineDuelState (finish.match);

            if (finish.alreadyFinished || finish.match.status == "finished")
                self->requestOnlineDuelResult();
            // waiting_for_opponent: the polling loop keeps reading the state
            // until the second finish (or the resolved chart) closes it.
        });
}

void MainComponent::requestOnlineDuelResult()
{
    if (duelOnlineResultInFlight
        || duelOnlineMatchId.isEmpty()
        || onlineAccessToken.isEmpty())
        return;

    duelOnlineResultInFlight = true;
    const int generation = duelOnlineGeneration;
    const auto matchId = duelOnlineMatchId;
    const auto token = onlineAccessToken;
    juce::Component::SafePointer<MainComponent> safeThis (this);

    duelClient.result (matchId, token,
        [safeThis, generation] (const DuelReply& reply, const DuelMatchSnapshot& match)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || generation != self->duelOnlineGeneration)
                return;

            self->duelOnlineResultInFlight = false;

            duelDebugLog ("result ok=" + juce::String (reply.ok ? 1 : 0)
                          + " status=" + juce::String (reply.statusCode)
                          + " code=" + reply.errorCode);

            if (! reply.ok)
            {
                if (reply.statusCode == 409)
                {
                    // match_not_finished: the opponent has not finished yet.
                    // Do not downgrade a card that was already shown from the
                    // authoritative `finished` snapshot.
                    if (self->duelOnlinePhase != DuelOnlinePhase::Finished)
                        self->duelOnlinePhase = DuelOnlinePhase::Finishing;
                    return;
                }

                if (reply.statusCode == 401
                    || reply.statusCode == 403
                    || reply.statusCode == 404)
                    self->handleOnlineDuelReplyError (reply, "result");

                return;
            }

            if (! match.valid)
                return;

            self->duelOnlineMatch = match;
            self->duelOnlinePhase = DuelOnlinePhase::Finished;
            if (match.matchId.isNotEmpty())
                self->duelOnlineFinishedMatchId = match.matchId;
            self->duelOnlineActive.store (false, std::memory_order_relaxed);
            self->duelActive.store (false, std::memory_order_relaxed);
            self->duelOnlineSnapshotAtMs = duelNowMs();
            self->enterOnlineDuelResultView();
            self->updateOnlineDuelHud();
            self->stopOnlineDuelTransport();

            juce::MessageManager::callAsync ([safeThis]
            {
                if (auto* self = safeThis.getComponent())
                {
                    self->repaint();
                    self->overlayComponent.repaint();
                }
            });
        });
}

void MainComponent::enterOnlineDuelResultView()
{
    // The result card is painted by paintDuelOverlay(), which only runs on the
    // Playing/Finished screens. A match can finish while the joining/search
    // modal is still open (the opponent left before the first note), so remove
    // the modal and switch to the result layout as well.
    closeOnlineDuelDialog();

    const auto state = gameState.load (std::memory_order_relaxed);
    const auto* you = duelOnlineMatch.findYou();

    duelDebugLog ("online result view state=" + juce::String (static_cast<int> (state))
                  + " status=" + duelOnlineMatch.status
                  + " next=" + juce::String (duelOnlineMatch.nextNoteIndex)
                  + " result=" + (you != nullptr ? you->result : juce::String ("?"))
                  + " winnerSide=" + duelOnlineMatch.winnerSide);

    // Every non-result screen must leave the gameplay layout. The match can be
    // decided before the local chart ends (the opponent knocks the player out,
    // the opponent leaves, or the server closes it): while the state stayed
    // Playing, the tablature kept painting above the overlay and covered the
    // result card with a frozen, stopped chart.
    if (state != GameState::Finished)
    {
        countInActive.store (false, std::memory_order_release);
        countInRemaining.store (0, std::memory_order_relaxed);
        practiceHoldActive.store (false, std::memory_order_release);
        tunerActive.store (false, std::memory_order_relaxed);
        gameState.store (GameState::Finished, std::memory_order_relaxed);

        juce::Component::SafePointer<MainComponent> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* self = safeThis.getComponent())
            {
                self->resized();
                self->repaint();
                self->overlayComponent.repaint();
            }
        });
    }
}

void MainComponent::finishOnlineDuelFromServer()
{
    if (duelOnlinePhase == DuelOnlinePhase::Finished
        || duelOnlinePhase == DuelOnlinePhase::None)
        return;

    // Remember that this match is over: a stale room snapshot must never make
    // the client join it again instead of the fresh match of the next round.
    if (duelOnlineMatchId.isNotEmpty())
        duelOnlineFinishedMatchId = duelOnlineMatchId;

    // A download/preload may still be running when the host leaves or the
    // server closes the room. Invalidate its completion before showing the card.
    duelOnlineDownloadInProgress = false;
    duelOnlineDownloadJoinAfter = false;
    duelOnlinePreloadInProgress = false;
    duelOnlineDownloadMatchId.clear();

    // The authoritative `finished` snapshot already carries the winner, HP,
    // XP and rating. Show the result card immediately instead of leaving the
    // player on "waiting for result"; GET /result below only refreshes it.
    duelOnlinePhase = DuelOnlinePhase::Finished;
    duelOnlineActive.store (false, std::memory_order_relaxed);
    duelActive.store (false, std::memory_order_relaxed);
    duelOnlineFinishRequested = false;
    duelOnlineFinishSent = true;
    duelOnlineEventSendingStopped = true;

    enterOnlineDuelResultView();

    // No note may be submitted after the authoritative finish: drop whatever
    // is still queued.
    {
        const juce::SpinLock::ScopedLockType lock (duelLock);
        duelOnlinePendingEvents.clear();
        duelOnlineInFlightEvents.clear();
    }

    updateOnlineDuelHud();
    stopOnlineDuelTransport();
    requestOnlineDuelResult();

    juce::Component::SafePointer<MainComponent> safeThis (this);

    juce::MessageManager::callAsync ([safeThis]
    {
        if (auto* self = safeThis.getComponent())
        {
            self->repaint();
            self->overlayComponent.repaint();
        }
    });
}

void MainComponent::noteOnlineDuelRoundTrip (double sentMs)
{
    const double rtt = duelNowMs() - sentMs;

    if (rtt <= 0.0 || rtt > 5000.0)
        return;

    const double oneWay = rtt * 0.5;

    // The smallest sample is the one least distorted by queueing, so it is the
    // closest estimate of the real one-way latency.
    duelOnlineOneWayMs = duelOnlineOneWayMs > 0.0
                             ? juce::jmin (duelOnlineOneWayMs, oneWay)
                             : oneWay;
}

void MainComponent::beginOnlineDuelPlayback()
{
    // The server has already put the match on its timeline. The chart and the
    // stems were preloaded before `ready`, so this path normally starts the
    // transport immediately; the legacy reload below is only a safety net.
    duelOnlineStartAborted = false;
    duelOnlinePhase = DuelOnlinePhase::Playing;

    // Only a chart preloaded for THIS match may start the transport: after a
    // finished match the previous song is still loaded in the session, and the
    // next round may have selected a different one.
    const bool chartForThisMatch =
        duelOnlineChartReady
        && (duelOnlineMatch.songSlug.isEmpty()
            || duelOnlinePreloadedSlug == duelOnlineMatch.songSlug);

    if (chartForThisMatch)
    {
        duelOnlinePlaybackPending = false;
        startOnlineDuelTimeline();
        return;
    }

    if (duelOnlineChartReady)
        duelDebugLog ("online chart belongs to '" + duelOnlinePreloadedSlug
                      + "' but the match is '" + duelOnlineMatch.songSlug
                      + "': reloading");

    duelOnlineChartReady = false;
    duelOnlinePlaybackPending = true;

    // Resolve the match's song by slug right before the load: an index cached
    // earlier can point at another row once the catalogue was refreshed.
    if (duelOnlineMatch.songSlug.isNotEmpty())
        for (int i = 0; i < static_cast<int> (catalogSongs.size()); ++i)
            if (catalogSongs[static_cast<size_t> (i)].slug == duelOnlineMatch.songSlug)
            {
                selectedCatalogIndex = i;
                break;
            }

    startSelectedSong();

    // A cached chart loads synchronously; if it did not, the online match must
    // not run with an unknown chart length.
    if (! duelOnlineActive.load (std::memory_order_relaxed)
        && duelOnlinePhase == DuelOnlinePhase::Playing)
    {
        cancelOnlineDuel (true);
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Online match cancelled",
            "The song could not be loaded, so the online match was cancelled.",
            "OK", this);
    }
}

void MainComponent::startOnlineDuelTimeline()
{
    duelOnlineChartReady = false;
    duelOnlinePreloadedSlug.clear();

    if (gameSession.getMidiNotes().empty())
    {
        cancelOnlineDuel (true);
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Online match cancelled",
            "The song chart could not be loaded.",
            "OK", this);
        return;
    }

    duelOnlinePhase = DuelOnlinePhase::Playing;

    // Reuse the normal Ready reset (session reset, stats reset, tempo restore)
    // without showing the tuner: the server countdown is already running.
    tuner.markAllStringsTuned();
    tunerActive.store (false, std::memory_order_relaxed);
    tunerStartButton.onClick();

    if (gameState.load (std::memory_order_relaxed) != GameState::Ready)
    {
        cancelOnlineDuel (true);
        return;
    }

    duelOnlineActive.store (true, std::memory_order_relaxed);
    duelActive.store (true, std::memory_order_relaxed);

    // Prepares the transport, stats and metronome. It does not open a regular
    // /games/sessions attempt for an online duel (see beginGameplayCountIn).
    beginGameplayCountIn();

    // Replace the local count-in with the server timeline: beat 0 is the
    // server's started_at, so note timing windows line up with the frozen
    // chart. `starts_in_ms` is measured at the snapshot time, hence the
    // elapsed correction.
    auto* device = deviceManager.getCurrentAudioDevice();
    const double sampleRate = device != nullptr ? device->getCurrentSampleRate() : 44100.0;
    const double now = duelNowMs();
    // The snapshot describes the server state from one one-way latency before
    // it arrived; both parts are time already spent since `starts_in_ms` was
    // measured.
    const auto elapsedMs = juce::jmax (0.0, now - duelOnlineSnapshotAtMs)
                           + juce::jmax (0.0, duelOnlineOneWayMs);
    const int remainingMs = juce::jmax (0, duelOnlineMatch.startsInMs
                                             - static_cast<int> (std::lround (elapsedMs)));

    // Diagnostic: the chart's first/last seconds at the tempo the client will
    // use. When this differs from the server's frozen timeline, every event is
    // rejected as event_too_late; keeping it in the log makes that visible.
    {
        const auto& chartNotes = gameSession.getMidiNotes();
        const double chartBpm = baseBpm > 0.0 ? baseBpm : 120.0;
        const double firstSeconds = chartNotes.empty()
            ? 0.0 : chartNotes.front().startBeat * 60.0 / chartBpm;
        const double lastSeconds = chartNotes.empty()
            ? 0.0 : chartNotes.back().startBeat * 60.0 / chartBpm;

        duelDebugLog ("online timeline bpm=" + juce::String (baseBpm, 2)
                      + " speed=" + juce::String (
                            speedPercent.load (std::memory_order_relaxed), 1)
                      + " songBpm=" + juce::String (duelOnlineMatch.songBpm)
                      + " notes=" + juce::String (
                            static_cast<int> (chartNotes.size()))
                      + " first=" + juce::String (firstSeconds, 3)
                      + " last=" + juce::String (lastSeconds, 3)
                      + " startsIn=" + juce::String (duelOnlineMatch.startsInMs)
                      + " elapsed=" + juce::String (elapsedMs, 0)
                      + " remaining=" + juce::String (remainingMs));
    }

    const auto nowSamples = audioSampleCounter.load (std::memory_order_relaxed);
    const auto beatZeroSample = nowSamples
        + static_cast<uint64_t> (std::llround (static_cast<double> (remainingMs)
                                               * sampleRate / 1000.0));

    gameplayStartSample.store (beatZeroSample, std::memory_order_relaxed);
    tempoAnchorBeat.store (0.0, std::memory_order_relaxed);
    tempoAnchorSample.store (beatZeroSample, std::memory_order_relaxed);
    currentPlayheadBeat.store (0.0, std::memory_order_relaxed);
    audiblePlayheadBeat.store (0.0, std::memory_order_relaxed);
    countInActive.store (false, std::memory_order_release);
    countInRemaining.store (0, std::memory_order_relaxed);
    tablatureComponent.setPlayheadBeat (0.0);
    metronome.resetScheduler (beatZeroSample, 0);

    hitLabel.setText ("ONLINE", juce::dontSendNotification);

    updateOnlineDuelHud();
    resized();
    repaint();
    overlayComponent.repaint();
}

void MainComponent::stopOnlineDuelTransport()
{
    practiceHoldActive.store (false, std::memory_order_release);
    countInActive.store (false, std::memory_order_release);
    countInRemaining.store (0, std::memory_order_relaxed);

    // The chart is not played any more: freeze the tablature playhead so a
    // stopped song never leaves a moving timeline behind the result card.
    tablatureComponent.setPlayheadRunning (false);

    const auto state = gameState.load (std::memory_order_relaxed);

    if (state == GameState::Playing || state == GameState::Paused)
        gameState.store (GameState::Finished, std::memory_order_relaxed);

    juce::Component::SafePointer<MainComponent> safeThis (this);

    juce::MessageManager::callAsync ([safeThis]
    {
        if (auto* self = safeThis.getComponent())
        {
            self->repaint();
            self->overlayComponent.repaint();
        }
    });
}

void MainComponent::updateOnlineDuelHud()
{
    if (! duelOnlineMatch.valid)
        return;

    const auto* you = duelOnlineMatch.findYou();
    const auto* opponent = duelOnlineMatch.findOpponent();

    if (you != nullptr)
        duelPlayerHealthMilli.store (
            static_cast<int> (std::lround (you->hp * 1000.0)),
            std::memory_order_relaxed);

    if (opponent != nullptr)
    {
        duelOpponentHealthMilli.store (
            static_cast<int> (std::lround (opponent->hp * 1000.0)),
            std::memory_order_relaxed);
        duelOnlineOpponentName = opponent->displayNameOrUsername();
    }
}

juce::Rectangle<int> MainComponent::getDuelHealthStripBounds() const
{
    // Compact gameplay header: top bar 12..80, HUD 88..152, then this strip.
    constexpr int top = 88 + 64 + 6;
    return { 24, top, getWidth() - 48, 46 };
}

bool MainComponent::isDuelStageActive() const
{    const auto state = gameState.load (std::memory_order_relaxed);

    const bool duel = duelActive.load (std::memory_order_relaxed)
                      || duelOnlineActive.load (std::memory_order_relaxed)
                      || duelStageResultActive
                      || duelOnlinePhase == DuelOnlinePhase::Playing
                      || duelOnlinePhase == DuelOnlinePhase::Finishing
                      || duelOnlinePhase == DuelOnlinePhase::Finished;

    return duel
           && (state == GameState::Playing
               || state == GameState::Paused
               || state == GameState::Finished
               || state == GameState::Ready);
}

juce::String MainComponent::campaignOpponentCharacterName() const
{
    if (! campaignFightActive)
        return {};

    if (! juce::isPositiveAndBelow (selectedCatalogIndex,
                                    static_cast<int> (catalogSongs.size())))
        return {};

    return catalogSongs[static_cast<size_t> (selectedCatalogIndex)].characterName;
}

void MainComponent::applySongStageVisuals (const juce::String& songSlug,
                                           const juce::String& opponentSheetUrl)
{
    const CatalogSong* song = nullptr;

    if (songSlug.isNotEmpty())
        for (const auto& row : catalogSongs)
            if (row.slug == songSlug)
            {
                song = &row;
                break;
            }

    if (song == nullptr
        && juce::isPositiveAndBelow (selectedCatalogIndex,
                                     static_cast<int> (catalogSongs.size())))
        song = &catalogSongs[static_cast<size_t> (selectedCatalogIndex)];

    // A later request (another song) invalidates the images still in flight.
    const int generation = ++duelVisualGeneration;

    if (song == nullptr && opponentSheetUrl.isEmpty())
    {
        duelStage.clearStageBackground();
        duelStage.clearOpponentSheet();
        return;
    }

    if (song != nullptr)
        requestDuelImage (song->stageBackgroundUrl, true, generation);

    // Online: the opponent is the character THAT PLAYER picked. The character
    // assigned to the song in the catalogue stays as the fallback (bot duels,
    // campaign, or an opponent without a profile character).
    requestDuelImage (opponentSheetUrl.isNotEmpty()
                          ? opponentSheetUrl
                          : (song != nullptr ? song->characterSheetUrl
                                             : juce::String()),
                      false, generation);
}

void MainComponent::applyOnlineDuelCharacters()
{
    if (! duelOnlineMatch.valid)
        return;

    // Restarting the downloads on every poll would cancel them while in
    // flight; only a real change of either character re-applies the sprites.
    const auto key = duelOnlineMatch.youCharacterSheetUrl + "|"
                     + duelOnlineMatch.opponentCharacterSheetUrl;

    if (key == onlineCharactersAppliedKey)
        return;

    onlineCharactersAppliedKey = key;

    applySongStageVisuals (duelOnlineMatch.songSlug,
                           duelOnlineMatch.opponentCharacterSheetUrl);

    // The server's copy of this player's selection wins; an empty value keeps
    // the locally remembered sheet (profile not loaded, older server).
    if (duelOnlineMatch.youCharacterSheetUrl.isNotEmpty())
        applyPlayerCharacterSheetFromUrl (duelOnlineMatch.youCharacterSheetUrl);
}

void MainComponent::requestDuelImage (const juce::String& relativeUrl,
                                      bool background,
                                      int generation)
{
    if (relativeUrl.isEmpty())
    {
        if (background)
            duelStage.clearStageBackground();
        else
            duelStage.clearOpponentSheet();

        return;
    }

    const auto cached = duelImageCache.find (relativeUrl);

    if (cached != duelImageCache.end())
    {
        if (background)
            duelStage.setStageBackground (cached->second);
        else
            duelStage.setOpponentSheet (cached->second);

        return;
    }

    const auto absolute = relativeUrl.startsWith ("http")
        ? relativeUrl
        : juce::String (ApiClient::serverOrigin) + relativeUrl;

    juce::Component::SafePointer<MainComponent> safeThis (this);

    // Network + decode on the worker pool; the image is applied on the message
    // thread and only when this is still the song the stage should show.
    apiClient.runAsync (
        [safeThis, relativeUrl, absolute, background, generation]
        {
            const auto data = ApiClient::download (absolute, 15000);

            juce::MessageManager::callAsync (
                [safeThis, relativeUrl, data, background, generation]
                {
                    if (auto* self = safeThis.getComponent())
                    {
                        if (data.getSize() == 0
                            || generation != self->duelVisualGeneration)
                            return;

                        // One stage + one character per song; cap the cache so
                        // a large catalogue cannot grow it without bound.
                        if (self->duelImageCache.size() >= 24)
                            self->duelImageCache.clear();

                        self->duelImageCache[relativeUrl] = data;

                        if (background)
                            self->duelStage.setStageBackground (data);
                        else
                            self->duelStage.setOpponentSheet (data);
                    }
                });
        },
        nullptr);
}

//==============================================================================
// Duel result inside the big session card

void MainComponent::paintDuelResultInSessionCard (juce::Graphics& g,
                                                  juce::Rectangle<float> card)
{
    int perfect = 0, great = 0, good = 0, miss = 0, notes = 0;
    double accuracy = 0.0, damageDealt = 0.0;
    int opPerfect = 0, opGreat = 0, opGood = 0, opMiss = 0;
    double opAccuracy = 0.0, opDamage = 0.0;
    bool won = false, draw = false;
    int xp = 0;

    {
        const juce::SpinLock::ScopedLockType lock (duelLock);

        if (duelMatch != nullptr)
        {
            won = duelMatch->playerWon();
            draw = duelMatch->isDraw();

            const auto& stats = duelMatch->getPlayerStats();
            perfect = stats.perfect;
            great = stats.great;
            good = stats.good;
            miss = stats.miss;
            notes = stats.notes;
            accuracy = stats.accuracy();
            damageDealt = stats.damageDealt;
            xp = duelMatch->getRewards().xp;

            const auto& opponent = duelMatch->getOpponentStats();
            opPerfect = opponent.perfect;
            opGreat = opponent.great;
            opGood = opponent.good;
            opMiss = opponent.miss;
            opAccuracy = opponent.accuracy();
            opDamage = opponent.damageDealt;
        }
    }

    const auto level = static_cast<DuelBotLevel> (
        duelBotLevelValue.load (std::memory_order_relaxed));
    const auto& profile = duelBotProfile (level);
    const auto campaignOpponent = campaignOpponentCharacterName();
    const auto opponentLabel = campaignOpponent.isNotEmpty()
                                   ? campaignOpponent
                                   : juce::String (profile.name);

    const auto title = draw ? "DRAW" : (won ? "VICTORY" : "DEFEAT");
    const auto titleColour = draw ? UiColors::muted
                                  : (won ? UiColors::green : UiColors::red);

    g.setColour (UiColors::accent);
    g.setFont (GameUi::eyebrow (10.0f));
    g.drawText ("GUITAR DUEL",
                juce::Rectangle<float> (card.getX(), card.getY() + 26.0f,
                                        card.getWidth(), 16.0f),
                juce::Justification::centred, false);

    g.setColour (titleColour);
    g.setFont (GameUi::heading (40.0f));
    g.drawText (title,
                juce::Rectangle<float> (card.getX(), card.getY() + 44.0f,
                                        card.getWidth(), 50.0f),
                juce::Justification::centred, false);

    g.setColour (UiColors::muted);
    g.setFont (GameUi::font (13.0f));
    g.drawText ("Duel results - you and " + opponentLabel,
                juce::Rectangle<float> (card.getX() + 32.0f, card.getY() + 98.0f,
                                        card.getWidth() - 64.0f, 20.0f),
                juce::Justification::centred, false);

    GameUi::drawDivider (g, card.getX() + 40.0f, card.getRight() - 40.0f,
                         card.getY() + 128.0f);

    const float leftX = card.getX() + 56.0f;
    const float rightX = card.getX() + card.getWidth() * 0.54f;
    const float top = card.getY() + 152.0f;

    // Small stat tile (accuracy/damage) sharing the judgement-tile look.
    auto drawStatTile = [&g] (float x, float y, float width, float height,
                              const juce::String& caption,
                              const juce::String& value,
                              juce::Colour valueColour)
    {
        const juce::Rectangle<float> tile (x, y, width, height);
        g.setColour (UiColors::surface2.withAlpha (0.72f));
        g.fillRoundedRectangle (tile, 12.0f);
        g.setColour (UiColors::border);
        g.drawRoundedRectangle (tile, 12.0f, 1.0f);

        g.setColour (valueColour);
        g.setFont (GameUi::heading (21.0f));
        g.drawText (value,
                    juce::Rectangle<float> (tile.getX(), tile.getY() + 3.0f,
                                            tile.getWidth(), 24.0f),
                    juce::Justification::centred, false);

        g.setColour (UiColors::muted);
        g.setFont (GameUi::eyebrow (8.0f));
        g.drawText (caption,
                    juce::Rectangle<float> (tile.getX(), tile.getY() + 27.0f,
                                            tile.getWidth(), 12.0f),
                    juce::Justification::centred, false);
    };

    // Judgement tiles reuse the SONG COMPLETE styling: coloured value, small
    // uppercase caption on a rounded surface tile. Four tiles fit one duel
    // column, so both the player and the bot get the same readable breakdown.
    auto drawJudgementTiles = [&g] (float x, float y, float width,
                                    int p, int gr, int gd, int ms)
    {
        const float gap = 8.0f;
        const float tileW = (width - gap * 3.0f) / 4.0f;
        const float tileH = 54.0f;

        const std::array<int, 4> counts { p, gr, gd, ms };
        const std::array<const char*, 4> captions
        {
            "PERFECT", "GREAT", "GOOD", "MISS"
        };
        const std::array<juce::Colour, 4> colours
        {
            UiColors::accent, UiColors::green, UiColors::info, UiColors::red
        };

        for (int i = 0; i < 4; ++i)
        {
            const juce::Rectangle<float> tile (
                x + static_cast<float> (i) * (tileW + gap), y, tileW, tileH);

            g.setColour (UiColors::surface2.withAlpha (0.72f));
            g.fillRoundedRectangle (tile, 12.0f);
            g.setColour (UiColors::border);
            g.drawRoundedRectangle (tile, 12.0f, 1.0f);

            g.setColour (colours[static_cast<size_t> (i)]);
            g.setFont (GameUi::heading (24.0f));
            g.drawText (juce::String (counts[static_cast<size_t> (i)]),
                        juce::Rectangle<float> (tile.getX(), tile.getY() + 5.0f,
                                                tile.getWidth(), 30.0f),
                        juce::Justification::centred, false);

            g.setColour (UiColors::muted);
            g.setFont (GameUi::eyebrow (8.0f));
            g.drawText (captions[static_cast<size_t> (i)],
                        juce::Rectangle<float> (tile.getX(), tile.getY() + 36.0f,
                                                tile.getWidth(), 12.0f),
                        juce::Justification::centred, false);
        }
    };

    auto drawColumn = [&g, &drawJudgementTiles, &drawStatTile] (
                          float x, float y, const juce::String& name,
                          juce::Colour colour, int hp, int p, int gr, int gd,
                          int ms, double acc, double dealt)
    {
        g.setColour (colour);
        g.setFont (GameUi::font (13.0f, true));
        g.drawText (name, juce::Rectangle<float> (x, y, 300.0f, 18.0f),
                    juce::Justification::centredLeft, false);

        g.setColour (UiColors::text);
        g.setFont (GameUi::heading (26.0f));
        g.drawText (juce::String (hp) + " HP",
                    juce::Rectangle<float> (x, y + 22.0f, 300.0f, 32.0f),
                    juce::Justification::centredLeft, false);

        drawJudgementTiles (x, y + 62.0f, 300.0f, p, gr, gd, ms);

        const float statW = (300.0f - 8.0f) * 0.5f;

        drawStatTile (x, y + 122.0f, statW, 44.0f, "ACCURACY",
                      juce::String (juce::roundToInt (acc * 100.0)) + "%",
                      UiColors::text);
        drawStatTile (x + statW + 8.0f, y + 122.0f, statW, 44.0f, "DAMAGE",
                      juce::String (dealt, 1),
                      UiColors::orange);
    };

    drawColumn (leftX, top, "YOU", UiColors::green,
                juce::roundToInt (duelPlayerHealthMilli.load() / 1000.0),
                perfect, great, good, miss, accuracy, damageDealt);

    drawColumn (rightX, top, opponentLabel, UiColors::orange,
                juce::roundToInt (duelOpponentHealthMilli.load() / 1000.0),
                opPerfect, opGreat, opGood, opMiss, opAccuracy, opDamage);

    // Score strip below both result columns.
    const juce::Rectangle<float> scorePanel (card.getX() + 48.0f,
                                             card.getY() + 330.0f,
                                             card.getWidth() - 96.0f, 116.0f);
    juce::ColourGradient scoreGradient (
        UiColors::accentStrong, scorePanel.getX(), scorePanel.getY(),
        UiColors::accent.darker (0.46f), scorePanel.getRight(), scorePanel.getBottom(),
        false);
    g.setGradientFill (scoreGradient);
    g.fillRoundedRectangle (scorePanel, 20.0f);

    auto drawScoreCell = [&g, &scorePanel] (int index, int count,
                                            const juce::String& caption,
                                            const juce::String& value)
    {
        const float width = scorePanel.getWidth() / static_cast<float> (count);
        const juce::Rectangle<float> cell (scorePanel.getX()
                                               + width * static_cast<float> (index),
                                           scorePanel.getY(), width,
                                           scorePanel.getHeight());
        g.setColour (UiColors::onAccent.withAlpha (0.72f));
        g.setFont (GameUi::eyebrow (9.0f));
        g.drawText (caption,
                    cell.withY (cell.getY() + 16.0f).withHeight (14.0f),
                    juce::Justification::centred, false);

        g.setColour (UiColors::onAccent);
        g.setFont (GameUi::heading (32.0f));
        g.drawText (value,
                    cell.withY (cell.getY() + 34.0f).withHeight (44.0f),
                    juce::Justification::centred, false);
    };

    if (campaignFightActive)
    {
        // Battle: actual score, the server's normalized score and the XP the
        // SERVER returned. The local duel reward is never shown here.
        const juce::String normalizedText =
            campaignStageNormalizedScore >= 0
                ? juce::String (campaignStageNormalizedScore)
                : juce::String ("-");
        const juce::String xpText =
            campaignStageReportPending
                ? juce::String ("...")
                : (campaignStageServerXp >= 0
                       ? "+" + juce::String (campaignStageServerXp)
                       : juce::String ("-"));

        drawScoreCell (0, 4, "TOTAL SCORE",
                       juce::String (gameplayStats.getScore()));
        drawScoreCell (1, 4, "NORMALIZED", normalizedText);
        drawScoreCell (2, 4, "NOTES", juce::String (notes));
        drawScoreCell (3, 4, "XP", xpText);

        if (campaignStageReportPending)
        {
            g.setColour (UiColors::muted);
            g.setFont (GameUi::font (10.5f));
            g.drawText ("Saving reward...",
                        juce::Rectangle<float> (scorePanel.getX(),
                                                scorePanel.getBottom() + 4.0f,
                                                scorePanel.getWidth(), 14.0f),
                        juce::Justification::centred, false);
        }
        else if (campaignStageReportFailed)
        {
            g.setColour (UiColors::red);
            g.setFont (GameUi::font (10.5f));
            g.drawText ("Reward not saved - check connection",
                        juce::Rectangle<float> (scorePanel.getX(),
                                                scorePanel.getBottom() + 4.0f,
                                                scorePanel.getWidth(), 14.0f),
                        juce::Justification::centred, false);
        }
    }
    else
    {
        drawScoreCell (0, 3, "TOTAL SCORE", juce::String (gameplayStats.getScore()));
        drawScoreCell (1, 3, "NOTES", juce::String (notes));
        drawScoreCell (2, 3, "XP", "+" + juce::String (xp));
    }

    // Same judgement breakdown as SONG COMPLETE, for the player's run.
    if (card.getHeight() > 600.0f)
    {
        const int judged = juce::jmax (1, perfect + great + good + miss);

        GameUi::drawEyebrow (g, "JUDGEMENT BREAKDOWN",
                             juce::Rectangle<float> (card.getX() + 48.0f,
                                                     card.getY() + 470.0f,
                                                     420.0f, 14.0f),
                             UiColors::muted);

        const juce::Rectangle<float> bar (card.getX() + 48.0f,
                                          card.getY() + 492.0f,
                                          card.getWidth() - 96.0f,
                                          12.0f);
        g.setColour (UiColors::surface2);
        g.fillRoundedRectangle (bar, 6.0f);

        {
            juce::Graphics::ScopedSaveState saved (g);
            juce::Path clip;
            clip.addRoundedRectangle (bar, 6.0f);
            g.reduceClipRegion (clip);

            const std::array<int, 4> counts { perfect, great, good, miss };
            const std::array<juce::Colour, 4> colours
            {
                UiColors::accent, UiColors::green, UiColors::info, UiColors::red
            };
            float segmentX = bar.getX();

            for (size_t i = 0; i < counts.size(); ++i)
            {
                const float segmentW = bar.getWidth()
                    * static_cast<float> (counts[i])
                    / static_cast<float> (judged);

                if (segmentW > 1.5f)
                {
                    g.setColour (colours[i]);
                    g.fillRect (segmentX, bar.getY(),
                                juce::jmax (0.0f, segmentW - 2.0f),
                                bar.getHeight());
                }

                segmentX += segmentW;
            }
        }

        const std::array<juce::String, 4> legend
        {
            "PERFECT  " + juce::String (perfect),
            "GREAT  " + juce::String (great),
            "GOOD  " + juce::String (good),
            "MISS  " + juce::String (miss)
        };
        const std::array<juce::Colour, 4> legendColours
        {
            UiColors::accent, UiColors::green, UiColors::info, UiColors::red
        };
        const float legendWidth = (card.getWidth() - 96.0f) / 4.0f;

        for (int i = 0; i < 4; ++i)
        {
            const float x = card.getX() + 48.0f
                + static_cast<float> (i) * legendWidth;
            const float y = card.getY() + 514.0f;

            g.setColour (legendColours[static_cast<size_t> (i)]);
            g.fillEllipse (x, y + 4.0f, 7.0f, 7.0f);
            g.setColour (UiColors::muted);
            g.setFont (GameUi::font (10.5f));
            g.drawText (legend[static_cast<size_t> (i)],
                        juce::Rectangle<float> (x + 13.0f, y,
                                                legendWidth - 20.0f, 16.0f),
                        juce::Justification::centredLeft, false);
        }
    }

    g.setColour (UiColors::muted);
    g.setFont (GameUi::font (9.5f));
    g.drawText ("BOT MATCH - RATING UNCHANGED",
                juce::Rectangle<float> (card.getX() + 40.0f,
                                        card.getBottom() - 104.0f,
                                        card.getWidth() - 80.0f, 16.0f),
                juce::Justification::centred, false);
}

//==============================================================================
// Online HUD and result card

void MainComponent::paintOnlineDuelResultInSessionCard (juce::Graphics& g,
                                                        juce::Rectangle<float> card)
{
    const auto* you = duelOnlineMatch.findYou();
    const auto* opponent = duelOnlineMatch.findOpponent();

    const juce::String yourName = you != nullptr
        ? you->displayNameOrUsername()
        : (currentUsername.isNotEmpty() ? currentUsername
                                        : juce::String ("YOU"));
    const juce::String opponentName = opponent != nullptr
        ? opponent->displayNameOrUsername()
        : (duelOnlineOpponentName.isNotEmpty() ? duelOnlineOpponentName
                                               : juce::String ("OPPONENT"));

    juce::String title = "MATCH OVER";
    juce::Colour titleColour = UiColors::muted;

    if (you != nullptr)
    {
        if (you->result == "win")
        {
            title = "VICTORY";
            titleColour = UiColors::green;
        }
        else if (you->result == "loss")
        {
            title = "DEFEAT";
            titleColour = UiColors::red;
        }
        else if (you->result == "draw")
        {
            title = "DRAW";
            titleColour = UiColors::muted;
        }
    }
    else if (duelOnlineMatch.winnerSide.isNotEmpty()
             && duelOnlineMatch.yourSide.isNotEmpty())
    {
        if (duelOnlineMatch.winnerSide == duelOnlineMatch.yourSide)
        {
            title = "VICTORY";
            titleColour = UiColors::green;
        }
        else
        {
            title = "DEFEAT";
            titleColour = UiColors::red;
        }
    }

    const int hpYou = you != nullptr ? juce::roundToInt (you->hp) : 0;
    const int hpOpponent = opponent != nullptr ? juce::roundToInt (opponent->hp) : 0;

    const int perfect = you != nullptr ? you->perfect : 0;
    const int great = you != nullptr ? you->great : 0;
    const int good = you != nullptr ? you->good : 0;
    const int miss = you != nullptr ? you->miss : 0;
    const double accuracy = you != nullptr ? you->accuracy : 0.0;
    const double damageDealt = you != nullptr ? you->damageDealt : 0.0;

    const int opPerfect = opponent != nullptr ? opponent->perfect : 0;
    const int opGreat = opponent != nullptr ? opponent->great : 0;
    const int opGood = opponent != nullptr ? opponent->good : 0;
    const int opMiss = opponent != nullptr ? opponent->miss : 0;
    const double opAccuracy = opponent != nullptr ? opponent->accuracy : 0.0;
    const double opDamage = opponent != nullptr ? opponent->damageDealt : 0.0;

    const int notes = you != nullptr ? you->notes : duelOnlineMatch.noteCount;
    const int xp = you != nullptr ? you->xpAwarded : 0;
    const int score = gameplayStats.getScore();

    g.setColour (UiColors::accent);
    g.setFont (GameUi::eyebrow (10.0f));
    g.drawText ("GUITAR DUEL - ONLINE",
                juce::Rectangle<float> (card.getX(), card.getY() + 26.0f,
                                        card.getWidth(), 16.0f),
                juce::Justification::centred, false);

    g.setColour (titleColour);
    g.setFont (GameUi::heading (40.0f));
    g.drawText (title,
                juce::Rectangle<float> (card.getX(), card.getY() + 44.0f,
                                        card.getWidth(), 50.0f),
                juce::Justification::centred, false);

    g.setColour (UiColors::muted);
    g.setFont (GameUi::font (13.0f));
    g.drawText ("Duel results - you and " + opponentName,
                juce::Rectangle<float> (card.getX() + 32.0f, card.getY() + 98.0f,
                                        card.getWidth() - 64.0f, 20.0f),
                juce::Justification::centred, false);

    GameUi::drawDivider (g, card.getX() + 40.0f, card.getRight() - 40.0f,
                         card.getY() + 128.0f);

    const float leftX = card.getX() + 56.0f;
    const float rightX = card.getX() + card.getWidth() * 0.54f;
    const float top = card.getY() + 152.0f;

    // Same judgement/stat tiles as the campaign result card.
    auto drawStatTile = [&g] (float x, float y, float width, float height,
                              const juce::String& caption,
                              const juce::String& value,
                              juce::Colour valueColour)
    {
        const juce::Rectangle<float> tile (x, y, width, height);
        g.setColour (UiColors::surface2.withAlpha (0.72f));
        g.fillRoundedRectangle (tile, 12.0f);
        g.setColour (UiColors::border);
        g.drawRoundedRectangle (tile, 12.0f, 1.0f);

        g.setColour (valueColour);
        g.setFont (GameUi::heading (21.0f));
        g.drawText (value,
                    juce::Rectangle<float> (tile.getX(), tile.getY() + 3.0f,
                                            tile.getWidth(), 24.0f),
                    juce::Justification::centred, false);

        g.setColour (UiColors::muted);
        g.setFont (GameUi::eyebrow (8.0f));
        g.drawText (caption,
                    juce::Rectangle<float> (tile.getX(), tile.getY() + 27.0f,
                                            tile.getWidth(), 12.0f),
                    juce::Justification::centred, false);
    };

    auto drawJudgementTiles = [&g] (float x, float y, float width,
                                    int p, int gr, int gd, int ms)
    {
        const float gap = 8.0f;
        const float tileW = (width - gap * 3.0f) / 4.0f;
        const float tileH = 54.0f;

        const std::array<int, 4> counts { p, gr, gd, ms };
        const std::array<const char*, 4> captions
        {
            "PERFECT", "GREAT", "GOOD", "MISS"
        };
        const std::array<juce::Colour, 4> colours
        {
            UiColors::accent, UiColors::green, UiColors::info, UiColors::red
        };

        for (int i = 0; i < 4; ++i)
        {
            const juce::Rectangle<float> tile (
                x + static_cast<float> (i) * (tileW + gap), y, tileW, tileH);

            g.setColour (UiColors::surface2.withAlpha (0.72f));
            g.fillRoundedRectangle (tile, 12.0f);
            g.setColour (UiColors::border);
            g.drawRoundedRectangle (tile, 12.0f, 1.0f);

            g.setColour (colours[static_cast<size_t> (i)]);
            g.setFont (GameUi::heading (24.0f));
            g.drawText (juce::String (counts[static_cast<size_t> (i)]),
                        juce::Rectangle<float> (tile.getX(), tile.getY() + 5.0f,
                                                tile.getWidth(), 30.0f),
                        juce::Justification::centred, false);

            g.setColour (UiColors::muted);
            g.setFont (GameUi::eyebrow (8.0f));
            g.drawText (captions[static_cast<size_t> (i)],
                        juce::Rectangle<float> (tile.getX(), tile.getY() + 36.0f,
                                                tile.getWidth(), 12.0f),
                        juce::Justification::centred, false);
        }
    };

    auto drawColumn = [&g, &drawJudgementTiles, &drawStatTile] (
                          float x, float y, const juce::String& name,
                          juce::Colour colour, int hp, int p, int gr, int gd,
                          int ms, double acc, double dealt)
    {
        g.setColour (colour);
        g.setFont (GameUi::font (13.0f, true));
        g.drawText (name, juce::Rectangle<float> (x, y, 300.0f, 18.0f),
                    juce::Justification::centredLeft, false);

        g.setColour (UiColors::text);
        g.setFont (GameUi::heading (26.0f));
        g.drawText (juce::String (hp) + " HP",
                    juce::Rectangle<float> (x, y + 22.0f, 300.0f, 32.0f),
                    juce::Justification::centredLeft, false);

        drawJudgementTiles (x, y + 62.0f, 300.0f, p, gr, gd, ms);

        const float statW = (300.0f - 8.0f) * 0.5f;

        drawStatTile (x, y + 122.0f, statW, 44.0f, "ACCURACY",
                      duelAccuracyText (acc), UiColors::text);
        drawStatTile (x + statW + 8.0f, y + 122.0f, statW, 44.0f, "DAMAGE",
                      juce::String (dealt, 1), UiColors::orange);
    };

    drawColumn (leftX, top, yourName, UiColors::green, hpYou,
                perfect, great, good, miss, accuracy, damageDealt);
    drawColumn (rightX, top, opponentName, UiColors::orange, hpOpponent,
                opPerfect, opGreat, opGood, opMiss, opAccuracy, opDamage);

    // Score strip below both result columns.
    const juce::Rectangle<float> scorePanel (card.getX() + 48.0f,
                                             card.getY() + 330.0f,
                                             card.getWidth() - 96.0f, 116.0f);
    juce::ColourGradient scoreGradient (
        UiColors::accentStrong, scorePanel.getX(), scorePanel.getY(),
        UiColors::accent.darker (0.46f), scorePanel.getRight(), scorePanel.getBottom(),
        false);
    g.setGradientFill (scoreGradient);
    g.fillRoundedRectangle (scorePanel, 20.0f);

    const float third = scorePanel.getWidth() / 3.0f;

    auto drawScoreCell = [&g, &scorePanel, third] (int index,
                                                   const juce::String& caption,
                                                   const juce::String& value)
    {
        const juce::Rectangle<float> cell (scorePanel.getX()
                                               + third * static_cast<float> (index),
                                           scorePanel.getY(), third,
                                           scorePanel.getHeight());
        g.setColour (UiColors::onAccent.withAlpha (0.72f));
        g.setFont (GameUi::eyebrow (9.0f));
        g.drawText (caption,
                    cell.withY (cell.getY() + 16.0f).withHeight (14.0f),
                    juce::Justification::centred, false);

        g.setColour (UiColors::onAccent);
        g.setFont (GameUi::heading (32.0f));
        g.drawText (value,
                    cell.withY (cell.getY() + 34.0f).withHeight (44.0f),
                    juce::Justification::centred, false);
    };

    drawScoreCell (0, "TOTAL SCORE", juce::String (score));
    drawScoreCell (1, "NOTES", juce::String (notes));
    drawScoreCell (2, "XP", "+" + juce::String (xp));

    // Judgement breakdown for the player's run.
    if (card.getHeight() > 600.0f)
    {
        const int judged = juce::jmax (1, perfect + great + good + miss);

        GameUi::drawEyebrow (g, "JUDGEMENT BREAKDOWN",
                             juce::Rectangle<float> (card.getX() + 48.0f,
                                                     card.getY() + 470.0f,
                                                     420.0f, 14.0f),
                             UiColors::muted);

        const juce::Rectangle<float> bar (card.getX() + 48.0f,
                                          card.getY() + 492.0f,
                                          card.getWidth() - 96.0f,
                                          12.0f);
        g.setColour (UiColors::surface2);
        g.fillRoundedRectangle (bar, 6.0f);

        {
            juce::Graphics::ScopedSaveState saved (g);
            juce::Path clip;
            clip.addRoundedRectangle (bar, 6.0f);
            g.reduceClipRegion (clip);

            const std::array<int, 4> counts { perfect, great, good, miss };
            const std::array<juce::Colour, 4> colours
            {
                UiColors::accent, UiColors::green, UiColors::info, UiColors::red
            };
            float segmentX = bar.getX();

            for (size_t i = 0; i < counts.size(); ++i)
            {
                const float segmentW = bar.getWidth()
                    * static_cast<float> (counts[i])
                    / static_cast<float> (judged);

                if (segmentW > 1.5f)
                {
                    g.setColour (colours[i]);
                    g.fillRect (segmentX, bar.getY(),
                                juce::jmax (0.0f, segmentW - 2.0f),
                                bar.getHeight());
                }

                segmentX += segmentW;
            }
        }

        const std::array<juce::String, 4> legend
        {
            "PERFECT  " + juce::String (perfect),
            "GREAT  " + juce::String (great),
            "GOOD  " + juce::String (good),
            "MISS  " + juce::String (miss)
        };
        const std::array<juce::Colour, 4> legendColours
        {
            UiColors::accent, UiColors::green, UiColors::info, UiColors::red
        };
        const float legendWidth = (card.getWidth() - 96.0f) / 4.0f;

        for (int i = 0; i < 4; ++i)
        {
            const float x = card.getX() + 48.0f
                + static_cast<float> (i) * legendWidth;
            const float y = card.getY() + 514.0f;

            g.setColour (legendColours[static_cast<size_t> (i)]);
            g.fillEllipse (x, y + 4.0f, 7.0f, 7.0f);
            g.setColour (UiColors::muted);
            g.setFont (GameUi::font (10.5f));
            g.drawText (legend[static_cast<size_t> (i)],
                        juce::Rectangle<float> (x + 13.0f, y,
                                                legendWidth - 20.0f, 16.0f),
                        juce::Justification::centredLeft, false);
        }
    }

    g.setColour (UiColors::muted);
    g.setFont (GameUi::font (9.5f));
    g.drawText (duelRatingText (you),
                juce::Rectangle<float> (card.getX() + 40.0f,
                                        card.getBottom() - 104.0f,
                                        card.getWidth() - 80.0f, 16.0f),
                juce::Justification::centred, false);
}

void MainComponent::paintOnlineDuelOverlay (juce::Graphics& g)
{
    if (! duelOnlineMatch.valid)
        return;

    const auto* you = duelOnlineMatch.findYou();
    const auto* opponent = duelOnlineMatch.findOpponent();

    const float width = static_cast<float> (getWidth());
    const bool finished = duelOnlineMatch.status == "finished";
    const bool waitingForResult = ! finished
                                  && (duelOnlinePhase == DuelOnlinePhase::Finishing);
    const bool endedEarly = finished
                            && duelOnlineMatch.nextNoteIndex <= 0;

    const juce::String yourName = you != nullptr
                                      ? you->displayNameOrUsername()
                                      : (currentUsername.isNotEmpty() ? currentUsername
                                                                      : juce::String ("YOU"));
    const juce::String opponentName = opponent != nullptr
                                          ? opponent->displayNameOrUsername()
                                          : (duelOnlineOpponentName.isNotEmpty()
                                                 ? duelOnlineOpponentName
                                                 : juce::String ("OPPONENT"));

    //--------------------------------------------------------------------------
    // Results card
    if (finished || waitingForResult)
    {
        // The finished match is rendered by the campaign-style session card;
        // do not draw the old compact card on top of it.
        if (finished && duelResultCardInSession)
        {
            duelResultCardInSession = false;
            return;
        }
        const float cardW = juce::jmin (640.0f, width - 48.0f);
        const juce::Rectangle<float> card ((width - cardW) * 0.5f, 96.0f,
                                           cardW, 350.0f);

        g.setColour (UiColors::background.withAlpha (0.84f));
        g.fillRoundedRectangle (card.expanded (6.0f), 24.0f);
        GameUi::drawCard (g, card, 20.0f, true);

        juce::String title = finished ? juce::String ("MATCH OVER")
                                      : juce::String ("WAITING FOR RESULT");
        juce::Colour titleColour = UiColors::muted;

        if (finished && you != nullptr)
        {
            if (you->result == "win")
            {
                title = "VICTORY";
                titleColour = UiColors::green;
            }
            else if (you->result == "loss")
            {
                title = "DEFEAT";
                titleColour = UiColors::red;
            }
            else if (you->result == "draw")
            {
                title = "DRAW";
                titleColour = UiColors::muted;
            }
        }
        else if (finished && duelOnlineMatch.winnerSide.isNotEmpty())
        {
            if (duelOnlineMatch.winnerSide == duelOnlineMatch.yourSide)
            {
                title = "VICTORY";
                titleColour = UiColors::green;
            }
            else if (duelOnlineMatch.yourSide.isNotEmpty())
            {
                title = "DEFEAT";
                titleColour = UiColors::red;
            }
        }

        g.setColour (UiColors::accent);
        g.setFont (GameUi::eyebrow (10.0f));
        g.drawText ("GUITAR DUEL - ONLINE",
                    card.withHeight (44.0f).translated (0.0f, 14.0f),
                    juce::Justification::centred, false);

        g.setColour (titleColour);
        g.setFont (GameUi::heading (30.0f));
        g.drawText (title, card.withHeight (54.0f).translated (0.0f, 30.0f),
                    juce::Justification::centred, false);

        const float leftX = card.getX() + 40.0f;
        const float rightX = card.getX() + card.getWidth() * 0.56f;
        const float top = card.getY() + 108.0f;

        auto drawColumn = [&g] (float x, float y, const juce::String& name,
                                const juce::Colour& colour,
                                double hp, int p, int gr, int gd, int ms,
                                double accuracy, double dealt)
        {
            g.setColour (colour);
            g.setFont (GameUi::font (12.0f, true));
            g.drawText (name, juce::Rectangle<float> (x, y, 240.0f, 18.0f),
                        juce::Justification::centredLeft, false);

            g.setColour (UiColors::text);
            g.setFont (GameUi::heading (22.0f));
            g.drawText (juce::String (juce::roundToInt (hp)) + " HP",
                        juce::Rectangle<float> (x, y + 20.0f, 240.0f, 28.0f),
                        juce::Justification::centredLeft, false);

            g.setColour (UiColors::muted);
            g.setFont (GameUi::font (11.0f));
            g.drawText ("PERFECT " + juce::String (p)
                            + "    GREAT " + juce::String (gr),
                        juce::Rectangle<float> (x, y + 54.0f, 280.0f, 18.0f),
                        juce::Justification::centredLeft, false);
            g.drawText ("GOOD " + juce::String (gd)
                            + "    MISS " + juce::String (ms),
                        juce::Rectangle<float> (x, y + 74.0f, 280.0f, 18.0f),
                        juce::Justification::centredLeft, false);
            g.drawText ("ACCURACY " + duelAccuracyText (accuracy),
                        juce::Rectangle<float> (x, y + 94.0f, 280.0f, 18.0f),
                        juce::Justification::centredLeft, false);
            g.drawText ("DAMAGE " + juce::String (dealt, 1),
                        juce::Rectangle<float> (x, y + 114.0f, 280.0f, 18.0f),
                        juce::Justification::centredLeft, false);
        };

        drawColumn (leftX, top, yourName, UiColors::green,
                    you != nullptr ? you->hp : 100.0,
                    you != nullptr ? you->perfect : 0,
                    you != nullptr ? you->great : 0,
                    you != nullptr ? you->good : 0,
                    you != nullptr ? you->miss : 0,
                    you != nullptr ? you->accuracy : 0.0,
                    you != nullptr ? you->damageDealt : 0.0);

        drawColumn (rightX, top, opponentName, UiColors::orange,
                    opponent != nullptr ? opponent->hp : 100.0,
                    opponent != nullptr ? opponent->perfect : 0,
                    opponent != nullptr ? opponent->great : 0,
                    opponent != nullptr ? opponent->good : 0,
                    opponent != nullptr ? opponent->miss : 0,
                    opponent != nullptr ? opponent->accuracy : 0.0,
                    opponent != nullptr ? opponent->damageDealt : 0.0);

        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (10.0f));
        g.drawText (juce::String (you != nullptr ? you->notes : duelOnlineMatch.noteCount)
                        + " NOTES",
                    juce::Rectangle<float> (card.getX() + 40.0f,
                                            card.getBottom() - 74.0f, 200.0f, 16.0f),
                    juce::Justification::centredLeft, false);

        g.setColour (UiColors::accent);
        g.setFont (GameUi::font (14.0f, true));
        g.drawText ("XP +" + juce::String (you != nullptr ? you->xpAwarded : 0),
                    juce::Rectangle<float> (card.getX() + card.getWidth() - 260.0f,
                                            card.getBottom() - 78.0f, 220.0f, 20.0f),
                    juce::Justification::centredRight, false);

        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (10.0f, true));
        g.drawText (waitingForResult
                        ? juce::String ("WAITING FOR OPPONENT / SERVER RESULT...")
                        : (endedEarly
                               ? juce::String ("MATCH ENDED EARLY - ")
                                     + duelRatingText (you)
                               : duelRatingText (you)),
                    juce::Rectangle<float> (card.getX() + 40.0f,
                                            card.getBottom() - 48.0f,
                                            card.getWidth() - 80.0f, 16.0f),
                    juce::Justification::centred, false);

        return;
    }

    //--------------------------------------------------------------------------
    // Live health strip
    if (! duelOnlineActive.load (std::memory_order_relaxed))
        return;

    const juce::Rectangle<float> strip = getDuelHealthStripBounds().toFloat();

    // Faint backing only; no outline, larger bars, names above them.
    g.setColour (UiColors::background.withAlpha (0.42f));
    g.fillRoundedRectangle (strip, 14.0f);

    const float playerHp = duelPlayerHealthMilli.load() / 1000.0f;
    const float opponentHp = duelOpponentHealthMilli.load() / 1000.0f;

    auto healthColour = [] (float hp)
    {
        if (hp > 50.0f) return UiColors::green;
        if (hp > 25.0f) return UiColors::orange;
        return UiColors::red;
    };

    constexpr float barH = 16.0f;
    const float barW = strip.getWidth() * 0.30f;
    const float barY = strip.getY() + 22.0f;
    const juce::Rectangle<float> leftBar (strip.getX() + 16.0f, barY, barW, barH);
    const juce::Rectangle<float> rightBar (strip.getRight() - 16.0f - barW,
                                           barY, barW, barH);

    auto drawBar = [&g, &healthColour] (juce::Rectangle<float> bar, float hp)
    {
        g.setColour (juce::Colours::black.withAlpha (0.62f));
        g.fillRoundedRectangle (bar.expanded (2.0f, 1.0f), bar.getHeight() * 0.5f);

        const float ratio = juce::jlimit (0.0f, 1.0f, hp / 100.0f);

        if (ratio > 0.0f)
        {
            g.setColour (healthColour (hp).brighter (0.15f));
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * ratio),
                                    bar.getHeight() * 0.5f);
        }
    };

    drawBar (leftBar, playerHp);
    drawBar (rightBar, opponentHp);

    g.setColour (UiColors::text);
    g.setFont (GameUi::font (13.0f, true));
    g.drawText (yourName, juce::Rectangle<float> (strip.getX() + 16.0f,
                                                  strip.getY() + 2.0f, 220.0f, 18.0f),
                juce::Justification::centredLeft, false);
    g.drawText (opponentName,
                juce::Rectangle<float> (strip.getRight() - 236.0f,
                                        strip.getY() + 2.0f, 220.0f, 18.0f),
                juce::Justification::centredRight, false);

    g.setColour (healthColour (playerHp).brighter (0.2f));
    g.setFont (GameUi::font (13.0f, true));
    g.drawText (juce::String (juce::roundToInt (playerHp)),
                juce::Rectangle<float> (leftBar.getRight() + 10.0f,
                                        barY - 2.0f, 56.0f, 20.0f),
                juce::Justification::centredLeft, false);
    g.setColour (healthColour (opponentHp).brighter (0.2f));
    g.drawText (juce::String (juce::roundToInt (opponentHp)),
                juce::Rectangle<float> (rightBar.getX() - 66.0f,
                                        barY - 2.0f, 56.0f, 20.0f),
                juce::Justification::centredRight, false);

    // Duels do not show the per-hit judgement text: only the exchange
    // (HIT / BLOCK / MISS / DAMAGE) is painted, at the guitarists' level.

    // Last exchange reported by the server: large, at the guitarists' level.
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto flashUntil = duelFlashUntilMs.load (std::memory_order_relaxed);
    const auto exchange = static_cast<DuelExchange> (
        duelLastExchange.load (std::memory_order_relaxed));

    if (now < flashUntil)
    {
        juce::String text;
        juce::Colour colour = UiColors::text;

        switch (exchange)
        {
            case DuelExchange::Block:
                text = "BLOCK";
                colour = UiColors::blue;
                break;

            case DuelExchange::PlayerHit:
                text = "DAMAGE " + juce::String (duelLastDamageMilli.load() / 1000.0, 1);
                colour = UiColors::green;
                break;

            case DuelExchange::OpponentHit:
                text = "HIT -" + juce::String (duelLastDamageMilli.load() / 1000.0, 1);
                colour = UiColors::red;
                break;

            case DuelExchange::BothMiss:
                text = "MISS";
                colour = UiColors::text;
                break;
        }

        const float tabTop = static_cast<float> (tablatureComponent.getY());
        const float stageCentre = (strip.getBottom() + tabTop) * 0.5f;

        // Solid colour for the whole flash: the exchange label (HIT / BLOCK /
        // MISS / DAMAGE) must never look semi-transparent.
        g.setColour (colour.brighter (0.2f));
        g.setFont (GameUi::heading (38.0f));
        g.drawFittedText (text,
                          juce::Rectangle<int> (
                              80, juce::roundToInt (stageCentre - 34.0f),
                              juce::jmax (240, getWidth() - 160), 68),
                          juce::Justification::centred, 1, 0.70f);
    }

    // Red damage amount inside the bar that lost HP for 0.5 s.
    const double damageEnd = flashUntil - 400.0;

    if (now < damageEnd
        && (exchange == DuelExchange::PlayerHit
            || exchange == DuelExchange::OpponentHit))
    {
        const auto bar = exchange == DuelExchange::OpponentHit ? leftBar : rightBar;
        const float alpha = static_cast<float> (
            juce::jlimit (0.0, 1.0, (damageEnd - now) / 500.0));
        const juce::Rectangle<float> chip (bar.getCentreX() - 30.0f,
                                           bar.getCentreY() - 7.0f, 60.0f, 14.0f);

        g.setColour (juce::Colours::black.withAlpha (0.55f * alpha));
        g.fillRoundedRectangle (chip, 5.0f);
        g.setColour (UiColors::red.withAlpha (alpha));
        g.setFont (GameUi::font (11.0f, true));
        g.drawText ("-" + juce::String (duelLastDamageMilli.load() / 1000.0, 1),
                    chip, juce::Justification::centred, false);
    }
}
