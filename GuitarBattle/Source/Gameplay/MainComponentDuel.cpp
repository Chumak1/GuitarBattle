#include "../MainComponent.h"

namespace
{
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

void MainComponent::requestDuelWithBot (int botLevel)
{
    duelPendingLevel = static_cast<DuelBotLevel> (
        juce::jlimit (0, 2, botLevel));
    duelPending = true;

    resetDuelState();

    // The duel uses the normal song flow: the chart is loaded exactly as for
    // a regular game, then beginPendingDuelIfNeeded() attaches the duel state
    // once the note count is known.
    startSelectedSong();
}

void MainComponent::beginPendingDuelIfNeeded()
{
    if (! duelPending)
        return;

    duelPending = false;

    const int noteCount =
        static_cast<int> (gameSession.getMidiNotes().size());

    if (noteCount <= 0)
        return;

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

    repaint();
    overlayComponent.repaint();
}

// Called on the pitch-analysis thread for every resolved chart note and on the
// message thread for the wall-clock auto miss.
void MainComponent::onDuelNoteResolved (int noteIndex, int hitQuality)
{
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

    if (finished)
        juce::MessageManager::callAsync ([this] { finishDuel(); });
}

void MainComponent::finishDuel()
{
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
    practiceHoldActive.store (false, std::memory_order_acquire);

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
    menu.addItem (4, "ONLINE MATCH - SOON", false, false);

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
                        });
}

void MainComponent::paintDuelOverlay (juce::Graphics& g)
{
    const auto state = gameState.load (std::memory_order_relaxed);

    if (state != GameState::Playing && state != GameState::Finished)
        return;

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
    double damageTaken = 0.0;
    int xp = 0;
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
            damageTaken = stats.damageTaken;
            xp = duelMatch->getRewards().xp;
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
                    0, 0, 0, 0, 0.0, damageTaken);

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

    const float stripW = juce::jmin (720.0f, width - 48.0f);
    const juce::Rectangle<float> strip ((width - stripW) * 0.5f, 84.0f,
                                        stripW, 38.0f);

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

    const float barW = stripW * 0.30f;
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
                                            stripW - (barW + 90.0f) * 2.0f, 20.0f),
                    juce::Justification::centred, false);
    }
}
