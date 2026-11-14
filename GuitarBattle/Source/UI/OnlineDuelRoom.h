#pragma once

#include <JuceHeader.h>

#include <functional>
#include <vector>

#include "../Network/DuelClient.h"

// Create-room form: a room name and an optional password.
class CreateDuelRoomWindow final : public juce::Component
{
public:
    using SendCallback = std::function<void (juce::String name, juce::String password)>;

    explicit CreateDuelRoomWindow (SendCallback onSendToUse);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void submit();

    SendCallback onSend;
    juce::Label heading, nameLabel, passwordLabel, status;
    juce::TextEditor nameEditor, passwordEditor;
    juce::ToggleButton passwordToggle { "PASSWORD" };
    juce::TextButton createButton { "CREATE" }, cancelButton { "CANCEL" };
};

// Shared waiting room: players, chat and song selection. Polls the server.
class OnlineDuelRoomWindow final : public juce::Component,
                                   private juce::Timer
{
public:
    struct SongChoice
    {
        juce::String id;
        juce::String title;
    };

    OnlineDuelRoomWindow (DuelClient& clientToUse,
                          juce::String bearerToken,
                          juce::String currentUsername,
                          juce::String lobbyId,
                          std::vector<SongChoice> songs,
                          std::function<void (juce::String matchId)> onStartMatch,
                          std::function<void()> onLeaveRoom);
    ~OnlineDuelRoomWindow() override;

    /** Toggle the lobby voice relay. The owner starts/stops the engine. */
    std::function<void (bool)> onVoiceChatToggled;

    /** Open the standalone tuner without closing the room. */
    std::function<void()> onOpenTuner;

    /** The room window is being destroyed (match start, leave or window X). */
    std::function<void()> onClosed;

    /** Live "MIC: Input 2 IN -34 dB OUT -12 dB" status for the voice row. */
    std::function<juce::String()> voiceStatusProvider;

    void setVoiceChatEnabled (bool enabled);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshRoom();
    void refreshChat();
    void sendChat();
    void selectSong();
    // Sends one song pick. Only one request runs at a time: a newer pick made
    // while it is in flight is remembered in ``pendingSongId`` and sent when
    // the reply arrives.
    void submitSongPick (const juce::String& songId);
    // Points the picker at the host's newest pick, or at the room's song when
    // there is none. A room poll must never undo a pick that is being saved.
    void restoreSongPicker();
    void startMatch();
    void confirmStart();
    void declineStart();
    void leaveRoom();
    void applyRoom (const DuelRoomSnapshot& room);
    // Applies a response of this window's own request (confirm, decline, song
    // pick, start request) and makes every older poll reply stale.
    void applyAuthoritative (const DuelRoomSnapshot& snapshot);
    void appendChat (const DuelChatMessage& message);
    void maybeShowStartDialog();
    void closeStartDialog (bool programmatic);
    void setTransientStatus (const juce::String& text);
    bool hasIConfirmedStart() const;
    bool isHost() const;
    juce::String startRequestKey() const;
    bool isConfirmedStartKey (const juce::String& key) const;

    DuelClient& client;
    juce::String token;
    juce::String myUsername;
    juce::String lobbyId;
    std::vector<SongChoice> songChoices;

    std::function<void (juce::String)> onStart;
    std::function<void()> onLeave;

    DuelRoomSnapshot room;
    juce::int64 lastChatId = 0;
    bool started = false;
    bool pollInFlight = false;
    bool chatPollInFlight = false;

    // Poll replies may arrive out of order (a snapshot requested before the
    // confirmation must not overwrite the confirmed one and reopen the dialog).
    // Only a reply newer than the last applied one is used.
    int roomRequestSeq = 0;
    int appliedRoomSeq = 0;
    // Identity of the start request this side confirmed, so the dialog stays
    // closed for the rest of that round even if the flags lag behind.
    juce::String confirmedStartKey;

    // The host's newest song pick that the server has not confirmed yet. The
    // room polls every 1.2 s; without this the poll put the previous song back
    // into the picker before the host could save the choice.
    juce::String pendingSongId;
    bool songSelectInFlight = false;

    // "Start the duel?" confirmation, shown to BOTH players before the song is
    // downloaded. The match is created by the server only when both confirm.
    bool startDialogOpen = false;
    bool suppressStartDialogResult = false;
    bool startActionInFlight = false;
    juce::Component::SafePointer<juce::AlertWindow> startDialog;

    // A short message that must survive the next room poll (the poll rewrites
    // the status line on every successful refresh).
    juce::String transientStatus;
    double transientStatusUntilMs = 0.0;

    juce::Label titleLabel, playersLabel, statusLabel, songLabel;
    juce::ComboBox songBox;
    juce::TextButton selectSongButton { "SELECT SONG" };
    juce::TextButton startButton { "START GAME" };
    juce::TextEditor chatLog;
    juce::TextEditor chatInput;
    juce::TextButton sendButton { "SEND" };
    juce::TextButton leaveButton { "LEAVE" };
    juce::TextButton voiceButton { "VOICE: OFF" };
    juce::TextButton tunerButton { "TUNER" };
    juce::Label voiceLabel;
    bool voiceChatOn = false;
    bool roomHasTwoPlayers = false;
    int pollTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OnlineDuelRoomWindow)
};
