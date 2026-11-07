#include "../MainComponent.h"

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
}

void MainComponent::clearOnlineDuelState()
{
    {
        const juce::SpinLock::ScopedLockType lock (duelLock);
        duelOnlinePendingEvents.clear();
        duelOnlineInFlightEvents.clear();
        duelOnlineLastQueuedIndex = -1;
    }

    duelOnlineMatch = {};
    duelOnlineMatchId.clear();
    duelOnlineSongId.clear();
    duelOnlineSongSlug.clear();
    duelOnlineOpponentName.clear();
    duelOnlineExpectedNotes = 0;
    duelOnlinePollFailures = 0;
    duelOnlineFinishAttempts = 0;
    duelOnlinePlaybackPending = false;
    duelOnlineChartReady = false;
    duelOnlineStartAborted = false;
    duelOnlinePollInFlight = false;
    duelOnlineReadyInFlight = false;
    duelOnlineEventInFlight = false;
    duelOnlineFinishRequested = false;
    duelOnlineFinishSent = false;
    duelOnlineFinishInFlight = false;
    duelOnlineResultInFlight = false;
    duelOnlineEventSendingStopped = false;
    duelOnlineNextPollMs = 0.0;
    duelOnlineNextEventMs = 0.0;
    duelOnlineNextReadyMs = 0.0;
    duelOnlineSnapshotAtMs = 0.0;

    duelOnlineActive.store (false, std::memory_order_relaxed);
    duelOnlineNoteCount.store (0, std::memory_order_relaxed);
    duelOnlinePhase = DuelOnlinePhase::None;
}

void MainComponent::requestDuelWithBot (int botLevel)
{
    duelPendingLevel = static_cast<DuelBotLevel> (
        juce::jlimit (0, 2, botLevel));
    duelPending = true;

    // Practice mode never scores: a duel must run the competitive path.
    if (practiceMode)
        setPracticeMode (false);

    resetDuelState();

    duelDebugLog ("duel requested level=" + juce::String (botLevel));

    // The duel uses the normal song flow: the chart is loaded exactly as for
    // a regular game, then beginPendingDuelIfNeeded() attaches the duel state
    // once the note count is known.
    startSelectedSong();
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
            DuelConfig{}, duelPendingLevel, seed, noteCount);
    }

    duelBotLevelValue.store (static_cast<int> (duelPendingLevel),
                             std::memory_order_relaxed);
    duelPlayerHealthMilli.store (100000, std::memory_order_relaxed);
    duelOpponentHealthMilli.store (100000, std::memory_order_relaxed);
    duelLastExchange.store (0, std::memory_order_relaxed);
    duelLastDamageMilli.store (0, std::memory_order_relaxed);
    duelFlashUntilMs.store (0.0, std::memory_order_relaxed);
    duelActive.store (true, std::memory_order_relaxed);

    duelDebugLog ("bot match created notes=" + juce::String (noteCount)
                  + " level=" + juce::String (static_cast<int> (duelPendingLevel))
                  + " seed=" + juce::String (static_cast<int> (seed)));

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
    // Online: the chart ended (or the server stopped it). Nothing is decided
    // locally: drain the queued notes, POST /finish and read /result.
    if (duelOnlinePhase == DuelOnlinePhase::Playing
        || duelOnlinePhase == DuelOnlinePhase::Finishing)
    {
        if (duelOnlinePhase != DuelOnlinePhase::Playing)
            return;

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
        return;

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

    // Knockout or the end of the chart: the transport stops and the results
    // panel (painted from the same DuelMatch state) takes over.
    practiceHoldActive.store (false, std::memory_order_release);

    if (gameState.load (std::memory_order_relaxed) == GameState::Playing)
        gameState.store (GameState::Finished, std::memory_order_relaxed);

    juce::MessageManager::callAsync ([this]
    {
        repaint();
        overlayComponent.repaint();
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

    const juce::Rectangle<int> strip (24, 222, getWidth() - 48, 34);

    // Only the strip is invalidated: a full overlay repaint would also redraw
    // the tablature and the rest of the HUD on every 30 Hz tick.
    overlayComponent.repaint (strip.expanded (4));
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

    //--------------------------------------------------------------------------
    // Results card
    if (finished)
    {
        const float cardW = juce::jmin (640.0f, width - 48.0f);
        const juce::Rectangle<float> card ((width - cardW) * 0.5f, 96.0f,
                                           cardW, 330.0f);

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

        drawColumn (rightX, top, profile.name, UiColors::orange,
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

    const juce::Rectangle<float> strip (24.0f, 222.0f, width - 48.0f, 34.0f);

    g.setColour (UiColors::background.withAlpha (0.72f));
    g.fillRoundedRectangle (strip, 12.0f);
    g.setColour (UiColors::accent.withAlpha (0.35f));
    g.drawRoundedRectangle (strip.reduced (1.0f), 11.0f, 1.0f);

    const float playerHp = duelPlayerHealthMilli.load() / 1000.0f;
    const float opponentHp = duelOpponentHealthMilli.load() / 1000.0f;

    auto healthColour = [] (float hp)
    {
        if (hp > 50.0f) return UiColors::green;
        if (hp > 25.0f) return UiColors::orange;
        return UiColors::red;
    };

    const float barW = strip.getWidth() * 0.30f;
    const juce::Rectangle<float> leftBar (strip.getX() + 78.0f, strip.getY() + 14.0f,
                                          barW, 10.0f);
    const juce::Rectangle<float> rightBar (strip.getRight() - 78.0f - barW,
                                           strip.getY() + 14.0f, barW, 10.0f);

    auto drawBar = [&g, &healthColour] (juce::Rectangle<float> bar, float hp)
    {
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (bar, bar.getHeight() * 0.5f);

        const float ratio = juce::jlimit (0.0f, 1.0f, hp / 100.0f);

        if (ratio > 0.0f)
        {
            g.setColour (healthColour (hp));
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * ratio),
                                    bar.getHeight() * 0.5f);
        }

        g.setColour (UiColors::borderStrong);
        g.drawRoundedRectangle (bar.reduced (0.5f), bar.getHeight() * 0.5f, 1.0f);
    };

    drawBar (leftBar, playerHp);
    drawBar (rightBar, opponentHp);

    g.setColour (UiColors::text);
    g.setFont (GameUi::font (10.0f, true));
    g.drawText ("YOU", juce::Rectangle<float> (strip.getX() + 16.0f,
                                               strip.getY() + 11.0f, 60.0f, 16.0f),
                juce::Justification::centredLeft, false);
    g.drawText (profile.name,
                juce::Rectangle<float> (strip.getRight() - 140.0f,
                                        strip.getY() + 11.0f, 124.0f, 16.0f),
                juce::Justification::centredRight, false);

    g.setColour (healthColour (playerHp));
    g.setFont (GameUi::font (10.0f, true));
    g.drawText (juce::String (juce::roundToInt (playerHp)),
                leftBar.translated (barW + 8.0f, -2.0f).withHeight (14.0f),
                juce::Justification::centredLeft, false);
    g.setColour (healthColour (opponentHp));
    g.drawText (juce::String (juce::roundToInt (opponentHp)),
                rightBar.translated (-38.0f, -2.0f).withWidth (32.0f).withHeight (14.0f),
                juce::Justification::centredRight, false);

    // Last exchange: a short, readable flash between the two health bars.
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto flashUntil = duelFlashUntilMs.load (std::memory_order_relaxed);

    if (now < flashUntil)
    {
        const auto exchange = static_cast<DuelExchange> (
            duelLastExchange.load (std::memory_order_relaxed));
        const double remaining = (flashUntil - now) / 900.0;
        const float alpha = static_cast<float> (juce::jlimit (0.0, 1.0, remaining));

        juce::String text;
        juce::Colour colour = UiColors::muted;

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
                colour = UiColors::muted;
                break;
        }

        g.setColour (colour.withAlpha (alpha));
        g.setFont (GameUi::font (16.0f, true));
        g.drawText (text,
                    juce::Rectangle<float> (strip.getX() + barW + 90.0f,
                                            strip.getY() + 9.0f,
                                            strip.getWidth() - (barW + 90.0f) * 2.0f, 20.0f),
                    juce::Justification::centred, false);
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

void MainComponent::startOnlineDuel()
{
    if (duelOnlinePhase != DuelOnlinePhase::None)
        return;

    auto showInfo = [this] (const juce::String& title, const juce::String& message)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                                title, message, "OK", this);
    };

    if (onlineAccessToken.isEmpty())
    {
        showInfo ("Sign in required",
                  "Sign in to your FretFight account to play online duels.");
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
        showInfo ("Online duel unavailable",
                  "This song has no server id. Pick a published catalogue song.");
        return;
    }

    if (! isSongUnlocked (song))
    {
        showInfo ("Song locked",
                  song.title + " unlocks at level "
                      + juce::String (requiredPlayerLevelForSong (song.difficulty)) + ".");
        return;
    }

    // The server freezes note_count and note times when the match is created.
    // The local chart must be ready before that, otherwise the first exchange
    // could be rejected as event_too_early. A cached chart also keeps the
    // 3-second server countdown honest.
    if (! song.downloaded || findOnlineLibraryEntry (song.slug) == nullptr)
    {
        showInfo ("Download required",
                  "Press START SONG once to download " + song.title
                      + " and its stems, then try ONLINE MATCH.");
        return;
    }

    // The server freezes the analysed chart at the original tempo. A 50-120%
    // client tempo would move every note away from its frozen timestamp, so an
    // online duel always runs at 100%, exactly like the analysed MIDI.
    if (std::abs (speedPercent.load (std::memory_order_relaxed) - 100.0) > 0.01)
    {
        // Quietly restore the analysed tempo; the search dialog mentions it.
        applyTempoPercent (100.0);

        if (std::abs (speedPercent.load (std::memory_order_relaxed) - 100.0) > 0.01)
        {
            showInfo ("Tempo change in progress",
                      "A tempo change is still being prepared. Try ONLINE MATCH again in a moment.");
            return;
        }
    }

    resetDuelState();

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
            self->sendOnlineDuelReady();
        });
}

void MainComponent::cancelOnlineDuel (bool sendLeave)
{
    const auto matchId = duelOnlineMatchId;
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
        duelClient.leave (matchId, token, nullptr);
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

void MainComponent::sendOnlineDuelReady()
{
    if (duelOnlineReadyInFlight
        || duelOnlineMatchId.isEmpty()
        || onlineAccessToken.isEmpty())
        return;

    const auto now = duelNowMs();

    if (now < duelOnlineNextReadyMs)
        return;

    duelOnlineReadyInFlight = true;
    duelOnlineNextReadyMs = now + 2000.0;

    const int generation = duelOnlineGeneration;
    const auto matchId = duelOnlineMatchId;
    juce::Component::SafePointer<MainComponent> safeThis (this);

    duelClient.ready (matchId, onlineAccessToken,
        [safeThis, generation] (const DuelReply& reply, const DuelMatchSnapshot& match)
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
    juce::Component::SafePointer<MainComponent> safeThis (this);

    duelClient.getState (matchId, token,
        [safeThis, generation] (const DuelReply& reply, const DuelMatchSnapshot& match)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || generation != self->duelOnlineGeneration)
                return;

            self->duelOnlinePollInFlight = false;

            if (! reply.transportOk)
            {
                ++self->duelOnlinePollFailures;

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
                self->handleOnlineDuelReplyError (reply, "state");
                return;
            }

            if (! reply.ok || ! match.valid)
                return;

            self->duelOnlinePollFailures = 0;
            self->applyOnlineDuelState (match);
        });
}

void MainComponent::updateOnlineDuel()
{
    if (duelOnlinePhase == DuelOnlinePhase::None
        || duelOnlinePhase == DuelOnlinePhase::Finished)
        return;

    pollOnlineDuel();

    if (duelOnlinePhase == DuelOnlinePhase::Playing
        || duelOnlinePhase == DuelOnlinePhase::Finishing)
        flushOnlineDuelEvents();
}

void MainComponent::applyOnlineDuelState (const DuelMatchSnapshot& incoming)
{
    if (! incoming.valid)
        return;

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

    if (snapshot.status == "waiting")
    {
        if (duelOnlinePhase == DuelOnlinePhase::Playing
            || duelOnlinePhase == DuelOnlinePhase::Finishing
            || duelOnlinePhase == DuelOnlinePhase::Finished)
            return;

        duelOnlinePhase = DuelOnlinePhase::Waiting;

        if (snapshot.waitingExpired)
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

        // Once the opponent has joined, both sides must confirm readiness.
        if (snapshot.participants.size() >= 2)
        {
            const auto* you = snapshot.findYou();

            if (you == nullptr || ! you->ready)
                sendOnlineDuelReady();
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

    juce::String title = "Duel unavailable";
    juce::String message;

    if (reply.statusCode == 401)
    {
        // Access tokens live in memory only; the refresh flow is the way back.
        title = "Sign in again";
        message = "Your session expired. Sign in again to play online duels.";
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

    cancelOnlineDuel (true);

    // Keep the start path in beginTuningForSelectedSong() from opening the
    // tuner for a match that no longer exists.
    duelOnlineStartAborted = true;

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

            if (! reply.ok)
            {
                if (reply.statusCode == 409)
                {
                    // match_not_finished: the opponent has not finished yet.
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
            self->duelOnlineActive.store (false, std::memory_order_relaxed);
            self->duelActive.store (false, std::memory_order_relaxed);
            self->duelOnlineSnapshotAtMs = duelNowMs();
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

void MainComponent::finishOnlineDuelFromServer()
{
    if (duelOnlinePhase == DuelOnlinePhase::Finished
        || duelOnlinePhase == DuelOnlinePhase::None)
        return;

    if (duelOnlinePhase == DuelOnlinePhase::Finishing)
    {
        requestOnlineDuelResult();
        return;
    }

    duelOnlinePhase = DuelOnlinePhase::Finishing;
    duelOnlineActive.store (false, std::memory_order_relaxed);
    duelActive.store (false, std::memory_order_relaxed);
    duelOnlineFinishRequested = false;
    duelOnlineFinishSent = true;
    duelOnlineEventSendingStopped = true;

    // No note may be submitted after the authoritative finish: drop whatever
    // is still queued and read the result instead.
    {
        const juce::SpinLock::ScopedLockType lock (duelLock);
        duelOnlinePendingEvents.clear();
        duelOnlineInFlightEvents.clear();
    }

    stopOnlineDuelTransport();
    requestOnlineDuelResult();
}

void MainComponent::beginOnlineDuelPlayback()
{
    // The server has already put the match on its timeline. Load the same
    // chart the normal game uses, then align the transport with started_at.
    duelOnlinePlaybackPending = true;
    duelOnlineChartReady = false;
    duelOnlineStartAborted = false;
    duelOnlinePhase = DuelOnlinePhase::Playing;

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
    const auto elapsedMs = juce::jmax (0.0, now - duelOnlineSnapshotAtMs);
    const int remainingMs = juce::jmax (0, duelOnlineMatch.startsInMs
                                             - static_cast<int> (std::lround (elapsedMs)));

    const auto nowSamples = audioSampleCounter.load (std::memory_order_relaxed);
    const auto beatZeroSample = nowSamples
        + static_cast<uint64_t> (std::llround (static_cast<double> (remainingMs)
                                               * sampleRate / 1000.0));

    gameplayStartSample.store (beatZeroSample, std::memory_order_relaxed);
    tempoAnchorBeat.store (0.0, std::memory_order_relaxed);
    tempoAnchorSample.store (beatZeroSample, std::memory_order_relaxed);
    currentPlayheadBeat.store (0.0, std::memory_order_relaxed);
    countInActive.store (false, std::memory_order_release);
    countInRemaining.store (0, std::memory_order_relaxed);
    tablatureComponent.setPlayheadBeat (0.0);
    metronome.resetScheduler (beatZeroSample, 0);

    hitLabel.setText ("ONLINE DUEL", juce::dontSendNotification);

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

//==============================================================================
// Online HUD and result card

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

    const juce::String yourName = you != nullptr
                                      ? you->displayNameOrUsername()
                                      : (currentUsername.isNotEmpty() ? currentUsername
                                                                      : juce::String ("YOU"));
    const juce::String opponentName = duelOnlineOpponentName.isNotEmpty()
                                          ? duelOnlineOpponentName
                                          : (opponent != nullptr
                                                 ? opponent->displayNameOrUsername()
                                                 : juce::String ("OPPONENT"));

    //--------------------------------------------------------------------------
    // Results card
    if (finished || waitingForResult)
    {
        const float cardW = juce::jmin (640.0f, width - 48.0f);
        const juce::Rectangle<float> card ((width - cardW) * 0.5f, 96.0f,
                                           cardW, 350.0f);

        g.setColour (UiColors::background.withAlpha (0.84f));
        g.fillRoundedRectangle (card.expanded (6.0f), 24.0f);
        GameUi::drawCard (g, card, 20.0f, true);

        juce::String title = "WAITING FOR RESULT";
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
                        : duelRatingText (you),
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

    const juce::Rectangle<float> strip (24.0f, 222.0f, width - 48.0f, 34.0f);

    g.setColour (UiColors::background.withAlpha (0.72f));
    g.fillRoundedRectangle (strip, 12.0f);
    g.setColour (UiColors::accent.withAlpha (0.35f));
    g.drawRoundedRectangle (strip.reduced (1.0f), 11.0f, 1.0f);

    const float playerHp = duelPlayerHealthMilli.load() / 1000.0f;
    const float opponentHp = duelOpponentHealthMilli.load() / 1000.0f;

    auto healthColour = [] (float hp)
    {
        if (hp > 50.0f) return UiColors::green;
        if (hp > 25.0f) return UiColors::orange;
        return UiColors::red;
    };

    const float barW = strip.getWidth() * 0.30f;
    const juce::Rectangle<float> leftBar (strip.getX() + 78.0f, strip.getY() + 14.0f,
                                          barW, 10.0f);
    const juce::Rectangle<float> rightBar (strip.getRight() - 78.0f - barW,
                                           strip.getY() + 14.0f, barW, 10.0f);

    auto drawBar = [&g, &healthColour] (juce::Rectangle<float> bar, float hp)
    {
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (bar, bar.getHeight() * 0.5f);

        const float ratio = juce::jlimit (0.0f, 1.0f, hp / 100.0f);

        if (ratio > 0.0f)
        {
            g.setColour (healthColour (hp));
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * ratio),
                                    bar.getHeight() * 0.5f);
        }

        g.setColour (UiColors::borderStrong);
        g.drawRoundedRectangle (bar.reduced (0.5f), bar.getHeight() * 0.5f, 1.0f);
    };

    drawBar (leftBar, playerHp);
    drawBar (rightBar, opponentHp);

    g.setColour (UiColors::text);
    g.setFont (GameUi::font (10.0f, true));
    g.drawText (yourName, juce::Rectangle<float> (strip.getX() + 16.0f,
                                                  strip.getY() + 11.0f, 70.0f, 16.0f),
                juce::Justification::centredLeft, false);
    g.drawText (opponentName,
                juce::Rectangle<float> (strip.getRight() - 160.0f,
                                        strip.getY() + 11.0f, 144.0f, 16.0f),
                juce::Justification::centredRight, false);

    g.setColour (healthColour (playerHp));
    g.setFont (GameUi::font (10.0f, true));
    g.drawText (juce::String (juce::roundToInt (playerHp)),
                leftBar.translated (barW + 8.0f, -2.0f).withHeight (14.0f),
                juce::Justification::centredLeft, false);
    g.setColour (healthColour (opponentHp));
    g.drawText (juce::String (juce::roundToInt (opponentHp)),
                rightBar.translated (-38.0f, -2.0f).withWidth (32.0f).withHeight (14.0f),
                juce::Justification::centredRight, false);

    // Last exchange reported by the server (block / hit).
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto flashUntil = duelFlashUntilMs.load (std::memory_order_relaxed);

    if (now < flashUntil)
    {
        const auto exchange = static_cast<DuelExchange> (
            duelLastExchange.load (std::memory_order_relaxed));
        const double remaining = (flashUntil - now) / 900.0;
        const float alpha = static_cast<float> (juce::jlimit (0.0, 1.0, remaining));

        juce::String text;
        juce::Colour colour = UiColors::muted;

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
                colour = UiColors::muted;
                break;
        }

        g.setColour (colour.withAlpha (alpha));
        g.setFont (GameUi::font (16.0f, true));
        g.drawText (text,
                    juce::Rectangle<float> (strip.getX() + barW + 90.0f,
                                            strip.getY() + 9.0f,
                                            strip.getWidth() - (barW + 90.0f) * 2.0f, 20.0f),
                    juce::Justification::centred, false);
    }
}
