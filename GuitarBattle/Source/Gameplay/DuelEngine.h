#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Guitar Duels: note-by-note comparison, damage and health.
//
// The engine is deliberately free of JUCE and of any game state: the client's
// existing scoring decides how well the player performed a chart note, the
// bot produces the opponent's quality for the same note, and this code only
// turns the pair into an exchange. That keeps the duel rules testable and
// identical for bots and for online matches (where the server runs the same
// rules).

// Hit quality as the duel sees it. The mapping from HitQuality is done by the
// caller; Great is the existing intermediate category of the game.
enum class DuelQuality
{
    Miss = 0,
    Good = 1,
    Great = 2,
    Perfect = 3
};

enum class DuelExchange
{
    BothMiss,     // nobody hit the note
    Block,        // both hit it with the same quality
    PlayerHit,    // the player was more accurate
    OpponentHit   // the opponent was more accurate
};

enum class DuelBotLevel
{
    Easy = 0,
    Normal = 1,
    Hard = 2
};

struct DuelConfig
{
    double maxHealth = 100.0;

    // Quality points used by the damage formula. Perfect and Good are the
    // values from the design document; Great sits between them.
    double pointsGood = 70.0;
    double pointsGreat = 85.0;
    double pointsPerfect = 100.0;

    // Damage per point of quality difference: Perfect vs Good (30 points) is
    // a small hit, Perfect vs Miss (100 points) is the maximum.
    double damagePerPoint = 0.100;

    // A clear advantage always deals at least this much, and one note can
    // never take more than this much health.
    double minDamage = 1.0;
    double maxDamage = 10.0;

    // Health gained back on a block, if the design wants chip healing later.
    double blockHeal = 0.0;
};

struct DuelNoteResult
{
    DuelExchange exchange = DuelExchange::BothMiss;
    double damageToOpponent = 0.0;
    double damageToPlayer = 0.0;

    bool blocked() const noexcept { return exchange == DuelExchange::Block; }
};

class DuelEngine
{
public:
    explicit DuelEngine (DuelConfig configuration = {})
        : config (configuration) {}

    const DuelConfig& getConfig() const noexcept { return config; }

    double qualityPoints (DuelQuality quality) const noexcept
    {
        switch (quality)
        {
            case DuelQuality::Perfect: return config.pointsPerfect;
            case DuelQuality::Great:   return config.pointsGreat;
            case DuelQuality::Good:    return config.pointsGood;
            case DuelQuality::Miss:    break;
        }

        return 0.0;
    }

    // Compares one chart note. The result is symmetric: swapping the inputs
    // swaps the damage and the exchange side.
    DuelNoteResult resolve (DuelQuality player,
                            DuelQuality opponent) const noexcept
    {
        DuelNoteResult result;

        const double playerPoints = qualityPoints (player);
        const double opponentPoints = qualityPoints (opponent);

        if (playerPoints <= 0.0 && opponentPoints <= 0.0)
        {
            result.exchange = DuelExchange::BothMiss;
            return result;
        }

        const double difference = playerPoints - opponentPoints;

        if (std::abs (difference) < 0.0001)
        {
            result.exchange = DuelExchange::Block;
            return result;
        }

        const double damage = std::clamp (std::abs (difference) * config.damagePerPoint,
                                          config.minDamage,
                                          config.maxDamage);

        if (difference > 0.0)
        {
            result.exchange = DuelExchange::PlayerHit;
            result.damageToOpponent = damage;
        }
        else
        {
            result.exchange = DuelExchange::OpponentHit;
            result.damageToPlayer = damage;
        }

        return result;
    }

private:
    DuelConfig config;
};

// One of the three bots. The profile drives both the visible identity and the
// note-by-note behaviour; the numbers follow the design document.
struct DuelBotProfile
{
    DuelBotLevel level = DuelBotLevel::Easy;
    const char* name = "BOT";
    const char* avatar = "bot";
    // Share of the chart notes the bot hits at all.
    double hitRate = 0.25;
    // Distribution of the quality among the hit notes.
    double perfectShare = 0.20;
    double greatShare = 0.30;
};

inline const DuelBotProfile& duelBotProfile (DuelBotLevel level) noexcept
{
    static const DuelBotProfile easy { DuelBotLevel::Easy, "RUSTY", "bot-easy",
                                       0.25, 0.20, 0.30 };
    static const DuelBotProfile normal { DuelBotLevel::Normal, "MIDNIGHT", "bot-normal",
                                         0.65, 0.35, 0.35 };
    static const DuelBotProfile hard { DuelBotLevel::Hard, "VOLT", "bot-hard",
                                       0.90, 0.55, 0.30 };

    switch (level)
    {
        case DuelBotLevel::Normal: return normal;
        case DuelBotLevel::Hard:   return hard;
        case DuelBotLevel::Easy:   break;
    }

    return easy;
}

// Deterministic per-note quality. The same seed and chart always produce the
// same bot performance, so a match can be replayed and tested.
inline std::uint32_t duelBotHash (std::uint32_t seed, int noteIndex) noexcept
{
    std::uint32_t value = seed * 0x9E3779B9u
                        + static_cast<std::uint32_t> (noteIndex) * 0x85EBCA6Bu;
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return value;
}

inline DuelQuality duelBotQuality (DuelBotLevel level,
                                   std::uint32_t seed,
                                   int noteIndex) noexcept
{
    const auto& profile = duelBotProfile (level);
    const std::uint32_t hash = duelBotHash (seed, noteIndex);

    // Use independent slices of the hash for the hit and the quality so the
    // two decisions do not correlate.
    const double hitRoll = static_cast<double> (hash & 0xFFFFu) / 65535.0;

    if (hitRoll >= profile.hitRate)
        return DuelQuality::Miss;

    const double qualityRoll =
        static_cast<double> ((hash >> 16) & 0xFFFFu) / 65535.0;

    if (qualityRoll < profile.perfectShare)
        return DuelQuality::Perfect;

    if (qualityRoll < profile.perfectShare + profile.greatShare)
        return DuelQuality::Great;

    return DuelQuality::Good;
}
