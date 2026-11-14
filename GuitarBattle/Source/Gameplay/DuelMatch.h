#pragma once

#include "DuelEngine.h"

#include <vector>

// Per-side statistics for the results screen.
struct DuelStats
{
    int perfect = 0;
    int great = 0;
    int good = 0;
    int miss = 0;

    int notes = 0;

    double damageDealt = 0.0;
    double damageTaken = 0.0;

    double accuracy() const noexcept
    {
        if (notes <= 0)
            return 0.0;

        return (static_cast<double> (perfect)
                + static_cast<double> (great) * 0.85
                + static_cast<double> (good) * 0.70)
               / static_cast<double> (notes);
    }
};

struct DuelRewards
{
    int xp = 0;
    // Online matches only: the server decides the rating change. Bots never
    // touch the competitive rating.
    int ratingDelta = 0;
    bool ratingApplied = false;
};

class DuelMatch
{
public:
    DuelMatch (DuelConfig configuration,
               DuelBotLevel botLevel,
               int songDifficulty,
               std::uint32_t seed,
               int noteCount)
        : engine (duelConfigForNoteCount (configuration, noteCount)),
          config (duelConfigForNoteCount (configuration, noteCount)),
          level (botLevel),
          matchSeed (seed)
    {
        if (noteCount > 0)
            botQualities.resize (static_cast<size_t> (noteCount));

        for (int i = 0; i < noteCount; ++i)
            botQualities[static_cast<size_t> (i)] =
                static_cast<std::uint8_t> (
                    duelBotQuality (level, songDifficulty, matchSeed, i));

        playerHealth = config.maxHealth;
        opponentHealth = config.maxHealth;
    }

    const DuelConfig& getConfig() const noexcept { return config; }
    DuelEngine& getEngine() noexcept { return engine; }

    double getPlayerHealth() const noexcept { return playerHealth; }
    double getOpponentHealth() const noexcept { return opponentHealth; }

    bool isFinished() const noexcept { return finished; }

    bool playerWon() const noexcept
    {
        if (! finished || draw)
            return false;

        if (opponentHealth <= 0.0 && playerHealth > 0.0)
            return true;

        if (playerHealth <= 0.0 && opponentHealth > 0.0)
            return false;

        // The chart ended without a knockout: the remaining health decides.
        return playerHealth > opponentHealth;
    }

    bool isDraw() const noexcept { return draw; }

    int getLastNoteIndex() const noexcept { return lastNoteIndex; }
    const DuelNoteResult& getLastResult() const noexcept { return lastResult; }

    const DuelStats& getPlayerStats() const noexcept { return playerStats; }
    const DuelStats& getOpponentStats() const noexcept { return opponentStats; }

    int getNoteCount() const noexcept
    {
        return static_cast<int> (botQualities.size());
    }

    // Resolves one chart note. `noteIndex` is the chart's note index, so a
    // duplicated submit for the same note is ignored (protection against
    // double events and client retries). Returns the exchange that was
    // actually applied.
    DuelNoteResult submit (int noteIndex, DuelQuality playerQuality)
    {
        if (finished || noteIndex < 0
            || noteIndex >= static_cast<int> (botQualities.size()))
        {
            return {};
        }

        // Every note may be resolved once, in chart order. Out-of-order or
        // repeated events are dropped instead of being applied twice.
        if (noteIndex <= lastNoteIndex)
            return {};

        lastNoteIndex = noteIndex;

        const auto opponentQuality =
            static_cast<DuelQuality> (botQualities[static_cast<size_t> (noteIndex)]);

        const auto result = engine.resolve (playerQuality, opponentQuality);

        countQuality (playerStats, playerQuality);
        countQuality (opponentStats, opponentQuality);

        if (result.damageToOpponent > 0.0)
        {
            opponentHealth = std::max (0.0, opponentHealth - result.damageToOpponent);
            playerStats.damageDealt += result.damageToOpponent;
            opponentStats.damageTaken += result.damageToOpponent;
        }

        if (result.damageToPlayer > 0.0)
        {
            playerHealth = std::max (0.0, playerHealth - result.damageToPlayer);
            playerStats.damageTaken += result.damageToPlayer;
            opponentStats.damageDealt += result.damageToPlayer;
        }

        lastResult = result;

        if (opponentHealth <= 0.0 || playerHealth <= 0.0)
        {
            finished = true;
            draw = opponentHealth <= 0.0 && playerHealth <= 0.0;
        }

        return result;
    }

    // Called when the chart ends without a knockout: the remaining health
    // decides the winner.
    void finishByHealth() noexcept
    {
        if (finished)
            return;

        finished = true;

        if (std::abs (playerHealth - opponentHealth) < 0.001)
            draw = true;
    }

    // XP is awarded for participation, accuracy and the result. Bots never
    // change the competitive rating, so ratingDelta stays zero here.
    DuelRewards getRewards() const noexcept
    {
        DuelRewards rewards;

        if (! finished)
            return rewards;

        rewards.xp = 20
                     + playerStats.perfect * 5
                     + playerStats.great * 3
                     + playerStats.good * 2
                     + (playerWon() ? 40 : 0);

        return rewards;
    }

private:
    static void countQuality (DuelStats& stats, DuelQuality quality) noexcept
    {
        ++stats.notes;

        switch (quality)
        {
            case DuelQuality::Perfect: ++stats.perfect; break;
            case DuelQuality::Great:   ++stats.great;   break;
            case DuelQuality::Good:    ++stats.good;    break;
            case DuelQuality::Miss:    ++stats.miss;    break;
        }
    }

    DuelEngine engine;
    DuelConfig config;
    DuelBotLevel level;
    std::uint32_t matchSeed = 0;

    std::vector<std::uint8_t> botQualities;

    double playerHealth = 100.0;
    double opponentHealth = 100.0;

    bool finished = false;
    bool draw = false;

    int lastNoteIndex = -1;
    DuelNoteResult lastResult;

    DuelStats playerStats;
    DuelStats opponentStats;
};
