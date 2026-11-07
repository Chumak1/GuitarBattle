/*
    GuitarBattle - Guitar Duels REST client (client side of /api/v1/duels).

    The server contract lives in GuitarBattleServer/docs/DUELS_API.md. The
    server is authoritative: the client sends only (note_index, quality) pairs
    and reads HP, counters, XP and the rating change back from the server.

    Rules that this file follows:

      * no blocking request ever runs on the message thread: every call is
        queued on ApiClient's worker pool and the callback is delivered with
        juce::MessageManager::callAsync() (ApiClient::deliver),
      * idempotent GETs (state / result / history / leaderboard / me) are
        retried on transport errors and 5xx with a short backoff inside the
        worker job,
      * WebSocket is not available in JUCE 9.0.2 (juce_core/network has no
        juce::WebSocket), so the match is polled through GET /duels/{id};
        the polling loop itself lives in MainComponentDuel.cpp.

    This file is only included by MainComponent.h.
*/

#pragma once

#include <JuceHeader.h>

#include <functional>
#include <vector>

class ApiClient;

//==============================================================================
// Plain copies of the server schemas (app/schemas/duels.py).

struct DuelParticipantState
{
    juce::String side;          // "player" / "opponent"
    juce::String userId;
    juce::String username;
    juce::String displayName;
    bool isBot = false;
    juce::String botName;
    juce::String botAvatar;
    bool ready = false;
    bool connected = false;
    bool left = false;
    double hp = 100.0;
    int perfect = 0;
    int great = 0;
    int good = 0;
    int miss = 0;
    int notes = 0;
    double accuracy = 0.0;      // 0..100, exactly as the server reports it
    double damageDealt = 0.0;
    double damageTaken = 0.0;
    int xpAwarded = 0;
    int ratingBefore = -1;
    int ratingAfter = -1;
    int ratingDelta = 0;
    juce::String result;        // "win" / "loss" / "draw" / empty

    juce::String displayNameOrUsername() const
    {
        if (displayName.isNotEmpty())
            return displayName;
        if (username.isNotEmpty())
            return username;
        if (botName.isNotEmpty())
            return botName;
        return isBot ? juce::String ("BOT") : juce::String ("PLAYER");
    }
};

struct DuelMatchSnapshot
{
    bool valid = false;
    juce::String matchId;
    juce::String mode;                  // "bot" / "online"
    juce::String status;                // waiting / playing / finished / cancelled
    int rulesVersion = 0;
    juce::int64 seed = 0;               // uint32 on the server, can exceed int32
    juce::String botLevel;

    juce::String songId;
    juce::String songSlug;
    juce::String songTitle;
    juce::String songArtist;
    int songBpm = 0;
    int songDifficulty = 0;
    int songDurationSeconds = 0;

    int noteCount = 0;
    int nextNoteIndex = 0;
    juce::String yourSide;              // empty in broadcast snapshots
    int startsInMs = 0;
    int serverTimeMs = 0;
    int waitingSeconds = 0;
    bool waitingExpired = false;
    juce::String winnerUserId;
    juce::String winnerSide;
    juce::String createdAt;
    juce::String startedAt;
    juce::String finishedAt;

    std::vector<DuelParticipantState> participants;

    const DuelParticipantState* findParticipant (const juce::String& side) const
    {
        if (side.isEmpty())
            return nullptr;

        for (const auto& participant : participants)
            if (participant.side == side)
                return &participant;

        return nullptr;
    }

    const DuelParticipantState* findYou() const { return findParticipant (yourSide); }

    const DuelParticipantState* findOpponent() const
    {
        const auto opposing = yourSide == "player" ? juce::String ("opponent")
                                                   : juce::String ("player");
        return findParticipant (opposing);
    }
};

// Request payload for POST /duels/{id}/events.
struct DuelEventOut
{
    int noteIndex = 0;
    juce::String quality;   // perfect / great / good / miss
};

struct DuelRejectedEvent
{
    int index = 0;
    int noteIndex = 0;
    juce::String code;
    juce::String message;
};

struct DuelExchangeOut
{
    int noteIndex = 0;
    juce::String exchange;          // player_hit / opponent_hit / block / both_miss
    juce::String playerQuality;
    juce::String opponentQuality;
    double damageToPlayer = 0.0;
    double damageToOpponent = 0.0;
    double playerHp = 100.0;
    double opponentHp = 100.0;
};

//==============================================================================
// Parsed reply envelopes.

struct DuelReply
{
    bool transportOk = false;   // an HTTP response arrived (any status)
    int statusCode = 0;
    bool ok = false;            // 2xx
    juce::String errorCode;     // detail.code, e.g. "no_opponent"
    juce::String errorMessage;  // detail.message or detail string
    juce::var json;

    bool isErrorCode (const char* code) const { return errorCode == code; }
};

struct DuelEventsReply
{
    bool valid = false;
    DuelMatchSnapshot match;
    std::vector<int> accepted;
    std::vector<int> duplicates;
    std::vector<DuelRejectedEvent> rejected;
    std::vector<DuelExchangeOut> exchanges;
    bool finished = false;
};

struct DuelFinishReply
{
    bool valid = false;
    DuelMatchSnapshot match;
    bool alreadyFinished = false;
    bool waitingForOpponent = false;
};

struct DuelHistoryEntry
{
    juce::String matchId;
    juce::String mode;
    juce::String status;
    juce::String songSlug;
    juce::String songTitle;
    juce::String opponentName;
    juce::String yourSide;
    juce::String result;
    double hp = 0.0;
    double opponentHp = 0.0;
    int xpAwarded = 0;
    int ratingDelta = 0;
    juce::String createdAt;
    juce::String finishedAt;
};

struct DuelHistoryReply
{
    bool valid = false;
    std::vector<DuelHistoryEntry> items;
    int total = 0;
    int limit = 0;
    int offset = 0;
};

struct DuelLeaderboardEntry
{
    int rank = 0;
    juce::String userId;
    juce::String username;
    juce::String displayName;
    int level = 0;
    int rating = 0;
    int duels = 0;
    int wins = 0;
    int losses = 0;
    int draws = 0;
    double winRate = 0.0;
    int xpEarned = 0;
    double accuracy = 0.0;
    double damageDealt = 0.0;
};

struct DuelLeaderboardReply
{
    bool valid = false;
    juce::String period;
    std::vector<DuelLeaderboardEntry> items;
    int total = 0;
    int limit = 0;
    int offset = 0;
};

// One open room in the duel lobby. The host created an online match and is
// waiting for a second player.
struct DuelLobbyRoom
{
    juce::String matchId;
    juce::String songId;
    juce::String songSlug;
    juce::String songTitle;
    juce::String hostName;
    int hostRating = 1000;
    int waitingSeconds = 0;
    int noteCount = 0;
};

struct DuelLobbyReply
{
    bool valid = false;
    std::vector<DuelLobbyRoom> rooms;
    int total = 0;
};

struct DuelProfileTotals
{
    int duels = 0;
    int wins = 0;
    int losses = 0;
    int draws = 0;
    double winRate = 0.0;
    int xpEarned = 0;
    double accuracy = 0.0;
    double damageDealt = 0.0;
    int bestWinStreak = 0;
};

struct DuelProfileReply
{
    bool valid = false;
    juce::String userId;
    juce::String username;
    juce::String displayName;
    int level = 0;
    int rating = 1000;
    int peakRating = 1000;
    DuelProfileTotals online;
    DuelProfileTotals bot;
    std::vector<DuelHistoryEntry> recentMatches;
    bool hasActiveMatch = false;
    DuelMatchSnapshot activeMatch;
};

//==============================================================================

class DuelClient
{
public:
    using MatchCallback       = std::function<void (const DuelReply&, const DuelMatchSnapshot&)>;
    using EventsCallback      = std::function<void (const DuelReply&, const DuelEventsReply&)>;
    using FinishCallback      = std::function<void (const DuelReply&, const DuelFinishReply&)>;
    using SimpleCallback      = std::function<void (const DuelReply&)>;
    using HistoryCallback     = std::function<void (const DuelReply&, const DuelHistoryReply&)>;
    using LeaderboardCallback = std::function<void (const DuelReply&, const DuelLeaderboardReply&)>;
    using ProfileCallback     = std::function<void (const DuelReply&, const DuelProfileReply&)>;
using LobbyCallback       = std::function<void (const DuelReply&, const DuelLobbyReply&)>;

    explicit DuelClient (ApiClient& client) : api (client) {}

    JUCE_DECLARE_NON_COPYABLE (DuelClient)

    //--------------------------------------------------------------------------
    // Match lifecycle

    void createMatch (const juce::String& songId,
                      const juce::String& mode,
                      const juce::String& botLevel,
                      const juce::String& bearerToken,
                      MatchCallback onResult);

    void join (const juce::String& matchId,
               const juce::String& bearerToken,
               MatchCallback onResult);

    void ready (const juce::String& matchId,
                const juce::String& bearerToken,
                MatchCallback onResult);

    /** GET /duels/{id}. Retried on transport errors and 5xx. */
    void getState (const juce::String& matchId,
                   const juce::String& bearerToken,
                   MatchCallback onResult);

    void submitEvents (const juce::String& matchId,
                       const std::vector<DuelEventOut>& events,
                       const juce::String& bearerToken,
                       EventsCallback onResult);

    void finish (const juce::String& matchId,
                 const juce::String& bearerToken,
                 FinishCallback onResult);

    void leave (const juce::String& matchId,
                const juce::String& bearerToken,
                SimpleCallback onResult = {});

    /** GET /duels/{id}/result (409 match_not_finished until both sides end). */
    void result (const juce::String& matchId,
                 const juce::String& bearerToken,
                 MatchCallback onResult);

    //--------------------------------------------------------------------------
    // Read-only endpoints (history / leaderboard / profile)

    void history (const juce::String& bearerToken,
                  int limit,
                  int offset,
                  const juce::String& mode,       // "bot" / "online" / empty
                  const juce::String& result,     // "win" / "loss" / "draw" / empty
                  HistoryCallback onResult);

    void leaderboard (const juce::String& period,    // "week" / "month" / "all"
                      int limit,
                      int offset,
                      const juce::String& bearerToken,
                      LeaderboardCallback onResult);

    void me (const juce::String& bearerToken, ProfileCallback onResult);

    void playerProfile (const juce::String& userId,
                        const juce::String& bearerToken,
                        ProfileCallback onResult);

    /** GET /duels/lobby - open online rooms waiting for a second player. */
    void lobby (const juce::String& bearerToken, LobbyCallback onResult);

    //--------------------------------------------------------------------------
    // Parsers shared with MainComponentDuel.cpp

    static DuelMatchSnapshot parseMatch (const juce::var& json);
    static DuelEventsReply parseEvents (const juce::var& json);
    static DuelFinishReply parseFinish (const juce::var& json);
    static DuelHistoryReply parseHistory (const juce::var& json);
    static DuelLeaderboardReply parseLeaderboard (const juce::var& json);
    static DuelLobbyReply parseLobby (const juce::var& json);
    static DuelProfileReply parseProfile (const juce::var& json);

private:
    ApiClient& api;

    /** Runs an idempotent GET on the worker pool with a bounded retry loop. */
    void getWithRetry (const juce::String& path,
                       const juce::String& bearerToken,
                       SimpleCallback onResult);

    static juce::var emptyObject();
};
