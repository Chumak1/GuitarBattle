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
    juce::String lobbyId;               // room this match was started from, or empty

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
    // Character each player picked in the profile, frozen into the public
    // snapshot: the online stage shows the opponent's own character, not the
    // one assigned to the song.
    juce::String youCharacterName;
    juce::String youCharacterSheetUrl;
    juce::String opponentCharacterName;
    juce::String opponentCharacterSheetUrl;
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
    // Room-based lobby (new flow): the id in matchId is the lobby id.
    bool hasPassword = false;
    int playerCount = 1;
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
// Shared duel lobby (server: app/schemas/duels.py, /api/v1/duels/lobbies).
//
// A lobby is a room, not a match: it is created with a name and an optional
// password, a second player joins, both chat, the host picks a published song
// and starts the match. After the match the same lobby is reused by
// POST /duels/{id}/rematch or POST /duels/{id}/return-to-lobby.

struct DuelRoomPlayer
{
    juce::String userId;
    juce::String username;
    juce::String displayName;
    bool isHost = false;

    juce::String displayNameOrUsername() const
    {
        return displayName.isNotEmpty() ? displayName
                                        : (username.isNotEmpty() ? username
                                                                 : juce::String ("PLAYER"));
    }
};

struct DuelRoomStartState
{
    // False when the server sent `start: null` (no pending request).
    bool active = false;
    juce::String requestedBy;
    bool hostConfirmed = false;
    bool guestConfirmed = false;
};

struct DuelRoomSnapshot
{
    bool valid = false;
    juce::String lobbyId;
    juce::String name;
    bool hasPassword = false;
    juce::String status;                // waiting / playing / closed
    juce::String hostUserId;
    juce::String guestUserId;
    juce::String matchId;               // newest match started from this room
    juce::String songId;                // empty until the host picks a song
    juce::String songSlug;
    juce::String songTitle;
    juce::String songArtist;
    int songBpm = 0;
    int songDifficulty = 0;
    int songDurationSeconds = 0;
    // Pending "start the duel?" handshake. Both players confirm BEFORE the song
    // is downloaded; the server only creates the match when both flags are set.
    DuelRoomStartState start;
    std::vector<DuelRoomPlayer> players;
    juce::String createdAt;
    juce::String updatedAt;

    bool hasSong() const            { return songId.isNotEmpty(); }
    bool hasTwoPlayers() const      { return players.size() >= 2; }
    bool isWaiting() const          { return status == "waiting"; }
    bool isPlaying() const          { return status == "playing"; }
    bool isClosed() const           { return status == "closed"; }

    bool hasPendingStart() const    { return start.active; }

    const DuelRoomPlayer* findPlayer (const juce::String& userId) const
    {
        for (const auto& player : players)
            if (player.userId == userId)
                return &player;

        return nullptr;
    }

    bool isHost (const juce::String& userId) const
    {
        return userId.isNotEmpty() && userId == hostUserId;
    }
};

struct DuelRoomListReply
{
    bool valid = false;
    std::vector<DuelRoomSnapshot> rooms;
    int total = 0;
};

struct DuelChatMessage
{
    juce::int64 id = 0;             // monotonic; pass as afterId on the next poll
    juce::String userId;
    juce::String username;
    juce::String displayName;
    juce::String text;
    juce::String createdAt;

    juce::String displayNameOrUsername() const
    {
        return displayName.isNotEmpty() ? displayName
                                        : (username.isNotEmpty() ? username
                                                                 : juce::String ("PLAYER"));
    }
};

struct DuelChatHistoryReply
{
    bool valid = false;
    std::vector<DuelChatMessage> messages;
    juce::int64 latestId = 0;       // last seen id; 0 when the chat is empty
};

struct DuelRoomStartReply
{
    bool valid = false;
    // Empty while the other player has not confirmed yet (the room keeps
    // polling); filled as soon as both confirmations created the match.
    juce::String matchId;
    DuelRoomSnapshot lobby;

    bool hasMatch() const { return matchId.isNotEmpty(); }
};

struct DuelRematchReply
{
    bool valid = false;
    juce::String matchId;
    juce::String previousMatchId;
    juce::String lobbyId;
    juce::String songId;
    juce::String songSlug;
    juce::String songTitle;
    // True when a fresh match already existed and was returned instead of
    // creating a duplicate (double click / retry).
    bool alreadyCreated = false;
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
    using RoomCallback        = std::function<void (const DuelReply&, const DuelRoomSnapshot&)>;
    using RoomListCallback    = std::function<void (const DuelReply&, const DuelRoomListReply&)>;
    using StartRoomCallback   = std::function<void (const DuelReply&, const DuelRoomStartReply&)>;
    using ChatHistoryCallback = std::function<void (const DuelReply&, const DuelChatHistoryReply&)>;
    using ChatMessageCallback = std::function<void (const DuelReply&, const DuelChatMessage&)>;
    using RematchCallback     = std::function<void (const DuelReply&, const DuelRematchReply&)>;

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
    // Shared duel lobby ("room"): create with name/password, join, chat, song
    // picker, start; then rematch or return to the same room after the match.
    //
    // Every call runs on ApiClient's worker pool and delivers on the message
    // thread, so the UI can call them directly from a button handler. For
    // polling use room() / roomChat() from a juce::Timer: they are idempotent
    // GETs with a bounded retry, exactly like getState().

    /** POST /duels/lobbies {name, password?} */
    void createRoom (const juce::String& name,
                     const juce::String& password,
                     const juce::String& bearerToken,
                     RoomCallback onResult);

    /** GET /duels/lobbies?include_full=... */
    void rooms (bool includeFull,
                const juce::String& bearerToken,
                RoomListCallback onResult);

    /** GET /duels/lobbies/{id} (members only). Retried on transport errors. */
    void room (const juce::String& lobbyId,
               const juce::String& bearerToken,
               RoomCallback onResult);

    /** POST /duels/lobbies/{id}/join {password?}; idempotent for members. */
    void joinRoom (const juce::String& lobbyId,
                   const juce::String& password,
                   const juce::String& bearerToken,
                   RoomCallback onResult);

    /** POST /duels/lobbies/{id}/leave (the host closes the room). */
    void leaveRoom (const juce::String& lobbyId,
                    const juce::String& bearerToken,
                    RoomCallback onResult);

    /** POST /duels/lobbies/{id}/song {song_id} - host only, waiting lobby. */
    void selectRoomSong (const juce::String& lobbyId,
                         const juce::String& songId,
                         const juce::String& bearerToken,
                         RoomCallback onResult);

    /** POST /duels/lobbies/{id}/start - host only, needs a guest and a song.
        Legacy one-shot start (immediate countdown); new clients use
        requestLobbyStart()/confirmLobbyStart() instead. */
    void startRoomMatch (const juce::String& lobbyId,
                         const juce::String& bearerToken,
                         StartRoomCallback onResult);

    /** POST /duels/lobbies/{id}/start/request - host only.
        Opens the "start the duel?" confirmation round; nothing is downloaded
        and the room stays `waiting`. */
    void requestLobbyStart (const juce::String& lobbyId,
                            const juce::String& bearerToken,
                            RoomCallback onResult);

    /** POST /duels/lobbies/{id}/start/confirm - either member.
        The reply has no matchId until both sides have confirmed. */
    void confirmLobbyStart (const juce::String& lobbyId,
                            const juce::String& bearerToken,
                            StartRoomCallback onResult);

    /** POST /duels/lobbies/{id}/start/decline - either member.
        Clears the pending request; the room keeps working. */
    void declineLobbyStart (const juce::String& lobbyId,
                            const juce::String& bearerToken,
                            RoomCallback onResult);

    /** GET /duels/lobbies/{id}/chat?after_id=... - incremental chat poll. */
    void roomChat (const juce::String& lobbyId,
                   juce::int64 afterId,
                   int limit,
                   const juce::String& bearerToken,
                   ChatHistoryCallback onResult);

    /** POST /duels/lobbies/{id}/chat {message}; rate limited by the server. */
    void sendRoomChat (const juce::String& lobbyId,
                       const juce::String& message,
                       const juce::String& bearerToken,
                       ChatMessageCallback onResult);

    /** POST /duels/{id}/rematch - same song, same two players, new match. */
    void rematch (const juce::String& matchId,
                  const juce::String& bearerToken,
                  RematchCallback onResult);

    /** POST /duels/{id}/return-to-lobby - back to the same chat/song screen. */
    void returnToLobby (const juce::String& matchId,
                        const juce::String& bearerToken,
                        RoomCallback onResult);

    //--------------------------------------------------------------------------
    // Parsers shared with MainComponentDuel.cpp

    static DuelMatchSnapshot parseMatch (const juce::var& json);
    static DuelEventsReply parseEvents (const juce::var& json);
    static DuelFinishReply parseFinish (const juce::var& json);
    static DuelHistoryReply parseHistory (const juce::var& json);
    static DuelLeaderboardReply parseLeaderboard (const juce::var& json);
    static DuelLobbyReply parseLobby (const juce::var& json);
    static DuelProfileReply parseProfile (const juce::var& json);
    static DuelRoomSnapshot parseRoom (const juce::var& json);
    static DuelRoomListReply parseRoomList (const juce::var& json);
    static DuelChatHistoryReply parseChatHistory (const juce::var& json);
    static DuelChatMessage parseChatMessage (const juce::var& json);
    static DuelRoomStartReply parseRoomStart (const juce::var& json);
    static DuelRematchReply parseRematch (const juce::var& json);

private:
    ApiClient& api;

    /** Runs an idempotent GET on the worker pool with a bounded retry loop. */
    void getWithRetry (const juce::String& path,
                       const juce::String& bearerToken,
                       SimpleCallback onResult);

    static juce::var emptyObject();
};
