#include "DuelClient.h"

#include "ApiClient.h"

#include <memory>

namespace
{
    juce::String propString (const juce::var& object, const char* key)
    {
        return object.getProperty (key, juce::var()).toString();
    }

    int propInt (const juce::var& object, const char* key, int fallback = 0)
    {
        const auto value = object.getProperty (key, juce::var());
        return value.isVoid() ? fallback : static_cast<int> (value);
    }

    juce::int64 propInt64 (const juce::var& object, const char* key, juce::int64 fallback = 0)
    {
        const auto value = object.getProperty (key, juce::var());
        return value.isVoid() ? fallback : static_cast<juce::int64> (static_cast<double> (value));
    }

    double propDouble (const juce::var& object, const char* key, double fallback = 0.0)
    {
        const auto value = object.getProperty (key, juce::var());
        return value.isVoid() ? fallback : static_cast<double> (value);
    }

    bool propBool (const juce::var& object, const char* key, bool fallback = false)
    {
        const auto value = object.getProperty (key, juce::var());
        return value.isVoid() ? fallback : static_cast<bool> (value);
    }

    /** detail is either {"code": "...", "message": "..."} or a plain string. */
    void readError (const juce::var& json, DuelReply& reply)
    {
        if (! json.isObject())
            return;

        const auto detail = json.getProperty ("detail", juce::var());

        if (detail.isObject())
        {
            reply.errorCode = propString (detail, "code");
            reply.errorMessage = propString (detail, "message");

            if (reply.errorMessage.isEmpty() && reply.errorCode.isEmpty())
                reply.errorMessage = detail.toString();
        }
        else if (! detail.isVoid())
        {
            reply.errorMessage = detail.toString();
        }
    }

    DuelReply makeReply (const ApiClient::Response& response)
    {
        DuelReply reply;
        reply.transportOk = response.transportOk;
        reply.statusCode = response.statusCode;
        reply.ok = response.transportOk
                   && response.statusCode >= 200
                   && response.statusCode < 300;
        reply.json = response.json;

        if (! reply.ok)
            readError (response.json, reply);

        return reply;
    }

    bool isRetryableStatus (int statusCode)
    {
        return statusCode == 0
               || statusCode == 408
               || statusCode == 425
               || statusCode == 429
               || statusCode >= 500;
    }

    void parseParticipant (const juce::var& object, DuelParticipantState& participant)
    {
        participant.side = propString (object, "side");
        participant.userId = propString (object, "user_id");
        participant.username = propString (object, "username");
        participant.displayName = propString (object, "display_name");
        participant.isBot = propBool (object, "is_bot");
        participant.botName = propString (object, "bot_name");
        participant.botAvatar = propString (object, "bot_avatar");
        participant.ready = propBool (object, "ready");
        participant.connected = propBool (object, "connected");
        participant.left = propBool (object, "left");
        participant.hp = propDouble (object, "hp", 100.0);
        participant.perfect = propInt (object, "perfect");
        participant.great = propInt (object, "great");
        participant.good = propInt (object, "good");
        participant.miss = propInt (object, "miss");
        participant.notes = propInt (object, "notes");
        participant.accuracy = propDouble (object, "accuracy");
        participant.damageDealt = propDouble (object, "damage_dealt");
        participant.damageTaken = propDouble (object, "damage_taken");
        participant.xpAwarded = propInt (object, "xp_awarded");
        participant.ratingBefore = propInt (object, "rating_before", -1);
        participant.ratingAfter = propInt (object, "rating_after", -1);
        participant.ratingDelta = propInt (object, "rating_delta");
        participant.result = propString (object, "result");
    }

    void parseExchange (const juce::var& object, DuelExchangeOut& exchange)
    {
        exchange.noteIndex = propInt (object, "note_index");
        exchange.exchange = propString (object, "exchange");

        const auto quality = object.getProperty ("quality", juce::var());
        exchange.playerQuality = propString (quality, "player");
        exchange.opponentQuality = propString (quality, "opponent");

        exchange.damageToPlayer = propDouble (object, "damage_to_player");
        exchange.damageToOpponent = propDouble (object, "damage_to_opponent");
        exchange.playerHp = propDouble (object, "player_hp", 100.0);
        exchange.opponentHp = propDouble (object, "opponent_hp", 100.0);
    }

    void parseHistoryItem (const juce::var& object, DuelHistoryEntry& item)
    {
        item.matchId = propString (object, "match_id");
        item.mode = propString (object, "mode");
        item.status = propString (object, "status");

        const auto song = object.getProperty ("song", juce::var());
        item.songSlug = propString (song, "slug");
        item.songTitle = propString (song, "title");

        const auto opponent = object.getProperty ("opponent", juce::var());
        if (opponent.isObject())
        {
            DuelParticipantState participant;
            parseParticipant (opponent, participant);
            item.opponentName = participant.displayNameOrUsername();
        }

        item.yourSide = propString (object, "your_side");
        item.result = propString (object, "result");
        item.hp = propDouble (object, "hp");
        item.opponentHp = propDouble (object, "opponent_hp");
        item.xpAwarded = propInt (object, "xp_awarded");
        item.ratingDelta = propInt (object, "rating_delta");
        item.createdAt = propString (object, "created_at");
        item.finishedAt = propString (object, "finished_at");
    }

    void parseTotals (const juce::var& object, DuelProfileTotals& totals)
    {
        totals.duels = propInt (object, "duels");
        totals.wins = propInt (object, "wins");
        totals.losses = propInt (object, "losses");
        totals.draws = propInt (object, "draws");
        totals.winRate = propDouble (object, "win_rate");
        totals.xpEarned = propInt (object, "xp_earned");
        totals.accuracy = propDouble (object, "accuracy");
        totals.damageDealt = propDouble (object, "damage_dealt");
        totals.bestWinStreak = propInt (object, "best_win_streak");
    }
}

//==============================================================================

juce::var DuelClient::emptyObject()
{
    juce::DynamicObject::Ptr object = new juce::DynamicObject();
    return juce::var (object.get());
}

//==============================================================================
// Parsers

DuelMatchSnapshot DuelClient::parseMatch (const juce::var& json)
{
    DuelMatchSnapshot match;

    if (! json.isObject())
        return match;

    match.matchId = propString (json, "match_id");
    match.mode = propString (json, "mode");
    match.status = propString (json, "status");
    match.rulesVersion = propInt (json, "rules_version");
    match.seed = propInt64 (json, "seed");
    match.botLevel = propString (json, "bot_level");

    const auto song = json.getProperty ("song", juce::var());
    match.songId = propString (song, "id");
    match.songSlug = propString (song, "slug");
    match.songTitle = propString (song, "title");
    match.songArtist = propString (song, "artist");
    match.songBpm = propInt (song, "bpm");
    match.songDifficulty = propInt (song, "difficulty");
    match.songDurationSeconds = propInt (song, "duration_seconds");

    match.noteCount = propInt (json, "note_count");
    match.nextNoteIndex = propInt (json, "next_note_index");
    match.yourSide = propString (json, "your_side");
    if (match.yourSide == "null")
        match.yourSide.clear();
    match.startsInMs = propInt (json, "starts_in_ms");
    match.serverTimeMs = propInt (json, "server_time_ms");
    match.waitingSeconds = propInt (json, "waiting_seconds");
    match.waitingExpired = propBool (json, "waiting_expired");
    match.winnerUserId = propString (json, "winner_user_id");
    match.winnerSide = propString (json, "winner_side");
    match.createdAt = propString (json, "created_at");
    match.startedAt = propString (json, "started_at");
    match.finishedAt = propString (json, "finished_at");

    if (const auto* participants = json.getProperty ("participants", juce::var()).getArray())
    {
        match.participants.reserve (static_cast<size_t> (participants->size()));

        for (const auto& item : *participants)
        {
            DuelParticipantState participant;
            parseParticipant (item, participant);
            match.participants.push_back (participant);
        }
    }

    match.valid = match.matchId.isNotEmpty();
    return match;
}

DuelEventsReply DuelClient::parseEvents (const juce::var& json)
{
    DuelEventsReply reply;

    if (! json.isObject())
        return reply;

    reply.match = parseMatch (json.getProperty ("match", juce::var()));

    auto readIndices = [] (const juce::var& arrayValue, std::vector<int>& output)
    {
        if (const auto* array = arrayValue.getArray())
            for (const auto& item : *array)
                output.push_back (static_cast<int> (item));
    };

    readIndices (json.getProperty ("accepted", juce::var()), reply.accepted);
    readIndices (json.getProperty ("duplicates", juce::var()), reply.duplicates);

    if (const auto* rejected = json.getProperty ("rejected", juce::var()).getArray())
    {
        for (const auto& item : *rejected)
        {
            DuelRejectedEvent entry;
            entry.index = propInt (item, "index");
            entry.noteIndex = propInt (item, "note_index");
            entry.code = propString (item, "code");
            entry.message = propString (item, "message");
            reply.rejected.push_back (entry);
        }
    }

    if (const auto* exchanges = json.getProperty ("exchanges", juce::var()).getArray())
    {
        for (const auto& item : *exchanges)
        {
            DuelExchangeOut exchange;
            parseExchange (item, exchange);
            reply.exchanges.push_back (exchange);
        }
    }

    reply.finished = propBool (json, "finished");
    reply.valid = reply.match.valid;
    return reply;
}

DuelFinishReply DuelClient::parseFinish (const juce::var& json)
{
    DuelFinishReply reply;

    if (! json.isObject())
        return reply;

    reply.match = parseMatch (json.getProperty ("match", juce::var()));
    reply.alreadyFinished = propBool (json, "already_finished");
    reply.waitingForOpponent = propBool (json, "waiting_for_opponent");
    reply.valid = reply.match.valid;
    return reply;
}

DuelHistoryReply DuelClient::parseHistory (const juce::var& json)
{
    DuelHistoryReply reply;

    if (! json.isObject())
        return reply;

    if (const auto* items = json.getProperty ("items", juce::var()).getArray())
        for (const auto& item : *items)
        {
            DuelHistoryEntry entry;
            parseHistoryItem (item, entry);
            reply.items.push_back (entry);
        }

    reply.total = propInt (json, "total");
    reply.limit = propInt (json, "limit");
    reply.offset = propInt (json, "offset");
    reply.valid = true;
    return reply;
}

DuelLeaderboardReply DuelClient::parseLeaderboard (const juce::var& json)
{
    DuelLeaderboardReply reply;

    if (! json.isObject())
        return reply;

    reply.period = propString (json, "period");

    if (const auto* items = json.getProperty ("items", juce::var()).getArray())
    {
        for (const auto& item : *items)
        {
            DuelLeaderboardEntry entry;
            entry.rank = propInt (item, "rank");
            entry.userId = propString (item, "user_id");
            entry.username = propString (item, "username");
            entry.displayName = propString (item, "display_name");
            entry.level = propInt (item, "level");
            entry.rating = propInt (item, "rating", 1000);
            entry.duels = propInt (item, "duels");
            entry.wins = propInt (item, "wins");
            entry.losses = propInt (item, "losses");
            entry.draws = propInt (item, "draws");
            entry.winRate = propDouble (item, "win_rate");
            entry.xpEarned = propInt (item, "xp_earned");
            entry.accuracy = propDouble (item, "accuracy");
            entry.damageDealt = propDouble (item, "damage_dealt");
            reply.items.push_back (entry);
        }
    }

    reply.total = propInt (json, "total");
    reply.limit = propInt (json, "limit");
    reply.offset = propInt (json, "offset");
    reply.valid = true;
    return reply;
}

DuelLobbyReply DuelClient::parseLobby (const juce::var& json)
{
    DuelLobbyReply reply;

    if (! json.isObject())
        return reply;

    if (const auto* items = json.getProperty ("items", juce::var()).getArray())
    {
        for (const auto& item : *items)
        {
            DuelLobbyRoom room;
            room.matchId = propString (item, "match_id");
            room.songId = propString (item, "song_id");
            room.songSlug = propString (item, "song_slug");
            room.songTitle = propString (item, "song_title");
            room.hostName = propString (item, "host_name");
            room.hostRating = propInt (item, "host_rating", 1000);
            room.waitingSeconds = propInt (item, "waiting_seconds");
            room.noteCount = propInt (item, "note_count");
            reply.rooms.push_back (room);
        }
    }

    reply.total = propInt (json, "total");
    reply.valid = true;
    return reply;
}

DuelProfileReply DuelClient::parseProfile (const juce::var& json){
    DuelProfileReply reply;

    if (! json.isObject())
        return reply;

    reply.userId = propString (json, "user_id");
    reply.username = propString (json, "username");
    reply.displayName = propString (json, "display_name");
    reply.level = propInt (json, "level");
    reply.rating = propInt (json, "rating", 1000);
    reply.peakRating = propInt (json, "peak_rating", 1000);

    parseTotals (json.getProperty ("online", juce::var()), reply.online);
    parseTotals (json.getProperty ("bot", juce::var()), reply.bot);

    if (const auto* items = json.getProperty ("recent_matches", juce::var()).getArray())
        for (const auto& item : *items)
        {
            DuelHistoryEntry entry;
            parseHistoryItem (item, entry);
            reply.recentMatches.push_back (entry);
        }

    const auto active = json.getProperty ("active_match", juce::var());
    reply.hasActiveMatch = active.isObject();
    if (reply.hasActiveMatch)
        reply.activeMatch = parseMatch (active);

    reply.valid = reply.userId.isNotEmpty();
    return reply;
}

//==============================================================================
// Idempotent GET helper

void DuelClient::getWithRetry (const juce::String& path,
                               const juce::String& bearerToken,
                               SimpleCallback onResult)
{
    auto raw = std::make_shared<ApiClient::Response>();

    api.runAsync (
        [raw, path, bearerToken]
        {
            constexpr int maxAttempts = 3;

            for (int attempt = 0; attempt < maxAttempts; ++attempt)
            {
                *raw = ApiClient::get (path, bearerToken, 4000);

                if (raw->transportOk && ! isRetryableStatus (raw->statusCode))
                    return;

                if (attempt + 1 < maxAttempts)
                    juce::Thread::sleep (200 + attempt * 250);
            }
        },
        [raw, onResult]
        {
            if (onResult != nullptr)
                onResult (makeReply (*raw));
        });
}

//==============================================================================
// Match lifecycle

void DuelClient::createMatch (const juce::String& songId,
                              const juce::String& mode,
                              const juce::String& botLevel,
                              const juce::String& bearerToken,
                              MatchCallback onResult)
{
    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty ("song_id", songId);
    body->setProperty ("mode", mode);

    // The server requires bot_level for mode=bot and rejects it for online.
    if (mode == "bot")
        body->setProperty ("bot_level", botLevel);

    api.postJson ("/duels", juce::var (body.get()), bearerToken, 6000,
                  [onResult] (ApiClient::Response response)
                  {
                      const auto reply = makeReply (response);

                      if (onResult != nullptr)
                          onResult (reply, parseMatch (response.json));
                  });
}

void DuelClient::join (const juce::String& matchId,
                       const juce::String& bearerToken,
                       MatchCallback onResult)
{
    api.postJson ("/duels/" + matchId + "/join", emptyObject(), bearerToken, 6000,
                  [onResult] (ApiClient::Response response)
                  {
                      const auto reply = makeReply (response);

                      if (onResult != nullptr)
                          onResult (reply, parseMatch (response.json));
                  });
}

void DuelClient::ready (const juce::String& matchId,
                        const juce::String& bearerToken,
                        MatchCallback onResult)
{
    api.postJson ("/duels/" + matchId + "/ready", emptyObject(), bearerToken, 6000,
                  [onResult] (ApiClient::Response response)
                  {
                      const auto reply = makeReply (response);

                      if (onResult != nullptr)
                          onResult (reply, parseMatch (response.json));
                  });
}

void DuelClient::getState (const juce::String& matchId,
                           const juce::String& bearerToken,
                           MatchCallback onResult)
{
    getWithRetry ("/duels/" + matchId, bearerToken,
                  [onResult] (const DuelReply& reply)
                  {
                      if (onResult != nullptr)
                          onResult (reply, parseMatch (reply.json));
                  });
}

void DuelClient::submitEvents (const juce::String& matchId,
                               const std::vector<DuelEventOut>& events,
                               const juce::String& bearerToken,
                               EventsCallback onResult)
{
    juce::Array<juce::var> array;
    array.ensureStorageAllocated (static_cast<int> (events.size()));

    for (const auto& event : events)
    {
        juce::DynamicObject::Ptr item = new juce::DynamicObject();
        item->setProperty ("note_index", event.noteIndex);
        item->setProperty ("quality", event.quality);
        array.add (juce::var (item.get()));
    }

    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty ("events", juce::var (array));

    api.postJson ("/duels/" + matchId + "/events", juce::var (body.get()),
                  bearerToken, 6000,
                  [onResult] (ApiClient::Response response)
                  {
                      const auto reply = makeReply (response);

                      if (onResult != nullptr)
                          onResult (reply, parseEvents (response.json));
                  });
}

void DuelClient::finish (const juce::String& matchId,
                         const juce::String& bearerToken,
                         FinishCallback onResult)
{
    api.postJson ("/duels/" + matchId + "/finish", emptyObject(), bearerToken, 6000,
                  [onResult] (ApiClient::Response response)
                  {
                      const auto reply = makeReply (response);

                      if (onResult != nullptr)
                          onResult (reply, parseFinish (response.json));
                  });
}

void DuelClient::leave (const juce::String& matchId,
                        const juce::String& bearerToken,
                        SimpleCallback onResult)
{
    api.postJson ("/duels/" + matchId + "/leave", emptyObject(), bearerToken, 4000,
                  [onResult] (ApiClient::Response response)
                  {
                      if (onResult != nullptr)
                          onResult (makeReply (response));
                  });
}

void DuelClient::result (const juce::String& matchId,
                         const juce::String& bearerToken,
                         MatchCallback onResult)
{
    getWithRetry ("/duels/" + matchId + "/result", bearerToken,
                  [onResult] (const DuelReply& reply)
                  {
                      if (onResult != nullptr)
                          onResult (reply, parseMatch (reply.json));
                  });
}

//==============================================================================
// Read-only endpoints

void DuelClient::history (const juce::String& bearerToken,
                          int limit,
                          int offset,
                          const juce::String& mode,
                          const juce::String& result,
                          HistoryCallback onResult)
{
    juce::String path = "/duels/history?limit=" + juce::String (juce::jlimit (1, 100, limit))
                        + "&offset=" + juce::String (juce::jmax (0, offset));

    if (mode.isNotEmpty())
        path += "&mode=" + mode;

    if (result.isNotEmpty())
        path += "&result=" + result;

    getWithRetry (path, bearerToken,
                  [onResult] (const DuelReply& reply)
                  {
                      if (onResult != nullptr)
                          onResult (reply, parseHistory (reply.json));
                  });
}

void DuelClient::leaderboard (const juce::String& period,
                              int limit,
                              int offset,
                              const juce::String& bearerToken,
                              LeaderboardCallback onResult)
{
    const auto safePeriod = period == "week" || period == "month" || period == "all"
                                ? period : juce::String ("all");

    const auto path = "/duels/leaderboard?period=" + safePeriod
                      + "&limit=" + juce::String (juce::jlimit (1, 100, limit))
                      + "&offset=" + juce::String (juce::jmax (0, offset));

    getWithRetry (path, bearerToken,
                  [onResult] (const DuelReply& reply)
                  {
                      if (onResult != nullptr)
                          onResult (reply, parseLeaderboard (reply.json));
                  });
}

void DuelClient::me (const juce::String& bearerToken, ProfileCallback onResult)
{
    getWithRetry ("/duels/me", bearerToken,
                  [onResult] (const DuelReply& reply)
                  {
                      if (onResult != nullptr)
                          onResult (reply, parseProfile (reply.json));
                  });
}

void DuelClient::lobby (const juce::String& bearerToken, LobbyCallback onResult)
{
    getWithRetry ("/duels/lobby", bearerToken,
                  [onResult] (const DuelReply& reply)
                  {
                      if (onResult != nullptr)
                          onResult (reply, parseLobby (reply.json));
                  });
}

void DuelClient::playerProfile (const juce::String& userId,
                                const juce::String& bearerToken,
                                ProfileCallback onResult)
{
    getWithRetry ("/duels/players/" + userId, bearerToken,
                  [onResult] (const DuelReply& reply)
                  {
                      if (onResult != nullptr)
                          onResult (reply, parseProfile (reply.json));
                  });
}
