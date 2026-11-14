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
//
// Damage is also scaled by the chart length: charts longer than
// duelDamageReferenceNotes deal proportionally less damage per note, so a
// 3x longer song does not end the match 3x faster. Short/simple charts keep
// the full damage.

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

    // Charts longer than the reference deal proportionally less damage per
    // note, so a 2-3x longer song does not end the duel 2-3x faster. Set by
    // duelConfigForNoteCount() when a match is created.
    double damageScale = 1.0;

    // Health gained back on a block, if the design wants chip healing later.
    double blockHeal = 0.0;
};

//: Charts at or below this length keep the full damage. Longer charts scale
//: down so the total possible damage stays roughly comparable across songs.
inline constexpr int duelDamageReferenceNotes = 48;

//: Longest charts get at most this fraction of the base damage per note.
inline constexpr double duelDamageMinScale = 0.20;

inline double duelDamageScaleForNoteCount (int noteCount) noexcept
{
    if (noteCount <= duelDamageReferenceNotes)
        return 1.0;

    return std::clamp (static_cast<double> (duelDamageReferenceNotes)
                           / static_cast<double> (noteCount),
                       duelDamageMinScale, 1.0);
}

inline DuelConfig duelConfigForNoteCount (DuelConfig config, int noteCount) noexcept
{
    config.damageScale = duelDamageScaleForNoteCount (noteCount);
    return config;
}

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

        const double damage =
            std::clamp (std::abs (difference) * config.damagePerPoint,
                        config.minDamage,
                        config.maxDamage)
            * config.damageScale;

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
                                         0.56, 0.30, 0.36 };
    static const DuelBotProfile hard { DuelBotLevel::Hard, "VOLT", "bot-hard",
                                       0.76, 0.40, 0.36 };

    switch (level)
    {
        case DuelBotLevel::Normal: return normal;
        case DuelBotLevel::Hard:   return hard;
        case DuelBotLevel::Easy:   break;
    }

    return easy;
}

//: The bot's accuracy follows the published song difficulty: it hits more on
//: easy charts and less on hard ones. 1.0 at difficulty 1..3, dropping about
//: 3.5% per step down to ~0.69 at difficulty 10.
inline double duelBotDifficultyFactor (int songDifficulty) noexcept
{
    const int difficulty = std::clamp (songDifficulty, 1, 10);

    if (difficulty <= 3)
        return 1.0;

    return std::clamp (1.0 - 0.035 * static_cast<double> (difficulty - 3),
                       0.65, 1.0);
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
                                   int songDifficulty,
                                   std::uint32_t seed,
                                   int noteIndex) noexcept
{
    const auto& profile = duelBotProfile (level);
    const std::uint32_t hash = duelBotHash (seed, noteIndex);

    const double difficultyFactor = duelBotDifficultyFactor (songDifficulty);
    const double hitRate = std::clamp (profile.hitRate * difficultyFactor,
                                       0.05, 0.97);
    const double perfectShare = std::clamp (profile.perfectShare * difficultyFactor,
                                            0.05, 0.85);
    const double greatShare = std::clamp (profile.greatShare,
                                          0.0, 0.95 - perfectShare);

    // Use independent slices of the hash for the hit and the quality so the
    // two decisions do not correlate.
    const double hitRoll = static_cast<double> (hash & 0xFFFFu) / 65535.0;

    if (hitRoll >= hitRate)
        return DuelQuality::Miss;

    const double qualityRoll =
        static_cast<double> ((hash >> 16) & 0xFFFFu) / 65535.0;

    if (qualityRoll < perfectShare)
        return DuelQuality::Perfect;

    if (qualityRoll < perfectShare + greatShare)
        return DuelQuality::Great;

    return DuelQuality::Good;
}
