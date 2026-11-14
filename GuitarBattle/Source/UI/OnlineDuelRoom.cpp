#include "OnlineDuelRoom.h"

#include "GameStyle.h"

namespace
{
    void styleEditor (juce::TextEditor& editor, const juce::String& placeholder,
                      bool multiline = false)
    {
        editor.setMultiLine (multiline, multiline);
        editor.setReturnKeyStartsNewLine (multiline);
        editor.setTextToShowWhenEmpty (placeholder, UiColors::muted);
        editor.setColour (juce::TextEditor::backgroundColourId, UiColors::surface2);
        editor.setColour (juce::TextEditor::outlineColourId, UiColors::border);
        editor.setColour (juce::TextEditor::textColourId, UiColors::text);
    }

    void styleButton (juce::TextButton& button, bool primary)
    {
        button.setColour (juce::TextButton::buttonColourId,
                          primary ? UiColors::accent : UiColors::surface2);
        button.setColour (juce::TextButton::buttonOnColourId,
                          primary ? UiColors::accentStrong : UiColors::surface2);
        button.setColour (juce::TextButton::textColourOffId,
                          primary ? UiColors::onAccent : UiColors::text);
        button.setColour (juce::TextButton::textColourOnId,
                          primary ? UiColors::onAccent : UiColors::text);
        button.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void closeDialog (juce::Component& component)
    {
        if (auto* dialog = component.findParentComponentOfClass<juce::DialogWindow>())
            dialog->exitModalState (0);
    }
}

//==============================================================================
CreateDuelRoomWindow::CreateDuelRoomWindow (SendCallback onSendToUse)
    : onSend (std::move (onSendToUse))
{
    setSize (480, 300);

    heading.setText ("CREATE ONLINE ROOM", juce::dontSendNotification);
    heading.setFont (GameUi::heading (22.0f));
    heading.setColour (juce::Label::textColourId, UiColors::text);
    addAndMakeVisible (heading);

    nameLabel.setText ("ROOM NAME", juce::dontSendNotification);
    nameLabel.setFont (GameUi::eyebrow (9.0f));
    nameLabel.setColour (juce::Label::textColourId, UiColors::accent);
    addAndMakeVisible (nameLabel);

    styleEditor (nameEditor, "Friday night duel");
    nameEditor.setMultiLine (false);
    addAndMakeVisible (nameEditor);

    passwordToggle.setColour (juce::ToggleButton::textColourId, UiColors::text);
    passwordToggle.setColour (juce::ToggleButton::tickColourId, UiColors::accent);
    passwordToggle.onClick = [this]
    {
        passwordEditor.setVisible (passwordToggle.getToggleState());
        passwordLabel.setVisible (passwordToggle.getToggleState());
        resized();
    };
    addAndMakeVisible (passwordToggle);

    passwordLabel.setText ("PASSWORD", juce::dontSendNotification);
    passwordLabel.setFont (GameUi::eyebrow (9.0f));
    passwordLabel.setColour (juce::Label::textColourId, UiColors::muted);
    passwordLabel.setVisible (false);
    addAndMakeVisible (passwordLabel);

    styleEditor (passwordEditor, "password");
    passwordEditor.setMultiLine (false);
    passwordEditor.setPasswordCharacter (0x2022);
    passwordEditor.setVisible (false);
    addAndMakeVisible (passwordEditor);

    status.setText ({}, juce::dontSendNotification);
    status.setFont (GameUi::font (11.0f, true));
    status.setColour (juce::Label::textColourId, UiColors::red);
    addAndMakeVisible (status);

    styleButton (createButton, true);
    styleButton (cancelButton, false);
    createButton.onClick = [this] { submit(); };
    cancelButton.onClick = [this] { closeDialog (*this); };
    addAndMakeVisible (createButton);
    addAndMakeVisible (cancelButton);
}

void CreateDuelRoomWindow::paint (juce::Graphics& g)
{
    g.fillAll (UiColors::background);
    GameUi::drawCard (g, getLocalBounds().toFloat().reduced (10.0f), 18.0f);
    g.setColour (UiColors::accent);
    g.fillRoundedRectangle (20.0f, 12.0f, 76.0f, 3.0f, 1.5f);
}

void CreateDuelRoomWindow::resized()
{
    auto area = getLocalBounds().reduced (30, 22);

    heading.setBounds (area.removeFromTop (28));
    area.removeFromTop (8);

    nameLabel.setBounds (area.removeFromTop (14));
    nameEditor.setBounds (area.removeFromTop (30));
    area.removeFromTop (10);

    passwordToggle.setBounds (area.removeFromTop (24).removeFromLeft (140));
    area.removeFromTop (4);

    if (passwordToggle.getToggleState())
    {
        passwordLabel.setBounds (area.removeFromTop (14));
        passwordEditor.setBounds (area.removeFromTop (28));
    }

    auto buttons = area.removeFromBottom (34);
    cancelButton.setBounds (buttons.removeFromRight (110));
    buttons.removeFromRight (10);
    createButton.setBounds (buttons.removeFromRight (110));

    status.setBounds (area.removeFromBottom (18));
}

void CreateDuelRoomWindow::submit()
{
    const auto name = nameEditor.getText().trim();

    if (name.isEmpty())
    {
        status.setText ("Enter a room name.", juce::dontSendNotification);
        return;
    }

    auto password = passwordToggle.getToggleState()
        ? passwordEditor.getText().trim()
        : juce::String();

    if (passwordToggle.getToggleState() && password.isEmpty())
    {
        status.setText ("Enter a password or turn the lock off.",
                        juce::dontSendNotification);
        return;
    }

    if (onSend != nullptr)
        onSend (name, password);

    closeDialog (*this);
}

//==============================================================================
OnlineDuelRoomWindow::OnlineDuelRoomWindow (
    DuelClient& clientToUse,
    juce::String bearerToken,
    juce::String currentUsername,
    juce::String lobbyIdToUse,
    std::vector<SongChoice> songs,
    std::function<void (juce::String)> onStartMatch,
    std::function<void()> onLeaveRoom)
    : client (clientToUse),
      token (std::move (bearerToken)),
      myUsername (std::move (currentUsername)),
      lobbyId (std::move (lobbyIdToUse)),
      songChoices (std::move (songs)),
      onStart (std::move (onStartMatch)),
      onLeave (std::move (onLeaveRoom))
{
    setSize (760, 600);

    titleLabel.setFont (GameUi::heading (22.0f));
    titleLabel.setColour (juce::Label::textColourId, UiColors::text);
    addAndMakeVisible (titleLabel);

    playersLabel.setFont (GameUi::font (12.0f, true));
    playersLabel.setColour (juce::Label::textColourId, UiColors::text);
    addAndMakeVisible (playersLabel);

    statusLabel.setFont (GameUi::font (11.0f));
    statusLabel.setColour (juce::Label::textColourId, UiColors::muted);
    addAndMakeVisible (statusLabel);

    songLabel.setFont (GameUi::font (13.0f, true));
    songLabel.setColour (juce::Label::textColourId, UiColors::text);
    songLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (songLabel);

    for (int i = 0; i < static_cast<int> (songChoices.size()); ++i)
        songBox.addItem (songChoices[static_cast<size_t> (i)].title, i + 1);

    songBox.setTextWhenNothingSelected ("No song selected");
    songBox.setColour (juce::ComboBox::backgroundColourId, UiColors::surface2);
    songBox.setColour (juce::ComboBox::outlineColourId, UiColors::border);
    songBox.setColour (juce::ComboBox::textColourId, UiColors::text);
    songBox.setColour (juce::ComboBox::arrowColourId, UiColors::accent);
    addAndMakeVisible (songBox);

    styleButton (selectSongButton, false);
    styleButton (startButton, true);
    styleButton (sendButton, false);
    styleButton (leaveButton, false);
    selectSongButton.onClick = [this] { selectSong(); };
    // Picking a song saves it immediately. The room polls every 1.2 s and used
    // to put the previous song back unless SELECT SONG was pressed in time.
    songBox.onChange = [this] { selectSong(); };
    startButton.onClick = [this] { startMatch(); };
    sendButton.onClick = [this] { sendChat(); };
    leaveButton.onClick = [this] { leaveRoom(); };
    addAndMakeVisible (selectSongButton);
    addAndMakeVisible (startButton);
    addAndMakeVisible (sendButton);
    addAndMakeVisible (leaveButton);

    styleButton (voiceButton, false);
    styleButton (tunerButton, false);
    voiceButton.setTooltip ("Talk to the other player. Uses the microphone input "
                            "selected in Settings.");
    voiceButton.onClick = [this]
    {
        voiceChatOn = ! voiceChatOn;
        setVoiceChatEnabled (voiceChatOn);

        if (onVoiceChatToggled != nullptr)
            onVoiceChatToggled (voiceChatOn);
    };
    tunerButton.setTooltip ("Tune your guitar without leaving the room.");
    tunerButton.onClick = [this]
    {
        if (onOpenTuner != nullptr)
            onOpenTuner();
    };
    voiceLabel.setText ("WAITING FOR THE SECOND PLAYER...",
                        juce::dontSendNotification);
    voiceButton.setEnabled (false);
    voiceLabel.setFont (GameUi::font (10.0f));
    voiceLabel.setColour (juce::Label::textColourId, UiColors::muted);
    voiceLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (voiceButton);
    addAndMakeVisible (tunerButton);
    addAndMakeVisible (voiceLabel);

    styleEditor (chatLog, "Chat");
    chatLog.setMultiLine (true, true);
    chatLog.setReadOnly (true);
    chatLog.setScrollbarsShown (true);
    addAndMakeVisible (chatLog);

    styleEditor (chatInput, "Type a message...");
    chatInput.setMultiLine (false);
    chatInput.onReturnKey = [this] { sendChat(); };
    addAndMakeVisible (chatInput);

    startTimer (200);
    refreshRoom();
    refreshChat();
}

OnlineDuelRoomWindow::~OnlineDuelRoomWindow()
{
    // The confirmation is a separate top-level modal window: it must never
    // outlive the room it belongs to.
    if (auto* dialog = startDialog.getComponent())
    {
        suppressStartDialogResult = true;
        dialog->exitModalState (0);
    }

    startDialog = nullptr;
    startDialogOpen = false;

    // The dialog can also be closed with its X button, which never goes through
    // leaveRoom(); the owner uses this to stop the voice relay.
    if (onClosed != nullptr)
        onClosed();
}

void OnlineDuelRoomWindow::setVoiceChatEnabled (bool enabled)
{
    voiceChatOn = enabled;
    voiceButton.setButtonText (enabled ? "VOICE: ON" : "VOICE: OFF");
    voiceButton.setColour (juce::TextButton::buttonColourId,
                           enabled ? UiColors::green.withAlpha (0.35f)
                                   : UiColors::surface2);
    voiceButton.setColour (juce::TextButton::textColourOffId,
                           enabled ? UiColors::text : UiColors::muted);
}

void OnlineDuelRoomWindow::paint (juce::Graphics& g)
{
    g.fillAll (UiColors::background);
    GameUi::drawCard (g, getLocalBounds().toFloat().reduced (10.0f), 18.0f);

    g.setColour (UiColors::accent);
    g.fillRoundedRectangle (22.0f, 12.0f, 76.0f, 3.0f, 1.5f);

    GameUi::drawCard (g, chatLog.getBounds().toFloat().expanded (4.0f), 12.0f);
}

void OnlineDuelRoomWindow::resized()
{
    auto area = getLocalBounds().reduced (28, 20);

    titleLabel.setBounds (area.removeFromTop (28));
    playersLabel.setBounds (area.removeFromTop (20));
    statusLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (4);

    // The selected song is visible to both players, not only to the host who
    // picked it.
    songLabel.setBounds (area.removeFromTop (44));
    area.removeFromTop (6);

    auto songRow = area.removeFromTop (30);
    songBox.setBounds (songRow.removeFromLeft (320));
    songRow.removeFromLeft (8);
    selectSongButton.setBounds (songRow.removeFromLeft (130));
    songRow.removeFromLeft (8);
    startButton.setBounds (songRow.removeFromLeft (150));

    area.removeFromTop (8);

    auto voiceRow = area.removeFromTop (30);
    voiceButton.setBounds (voiceRow.removeFromLeft (116));
    voiceRow.removeFromLeft (8);
    tunerButton.setBounds (voiceRow.removeFromLeft (92));
    voiceRow.removeFromLeft (12);
    voiceLabel.setBounds (voiceRow);

    area.removeFromTop (10);

    auto buttons = area.removeFromBottom (34);
    leaveButton.setBounds (buttons.removeFromRight (100));
    buttons.removeFromRight (8);
    sendButton.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    chatInput.setBounds (buttons);
    area.removeFromBottom (8);

    chatLog.setBounds (area);
}

bool OnlineDuelRoomWindow::isHost() const
{
    for (const auto& player : room.players)
        if (player.username == myUsername)
            return player.isHost;

    return false;
}

bool OnlineDuelRoomWindow::hasIConfirmedStart() const
{
    if (! room.start.active)
        return false;

    return isHost() ? room.start.hostConfirmed : room.start.guestConfirmed;
}

juce::String OnlineDuelRoomWindow::startRequestKey() const
{
    // A confirmation belongs to one round: who requested the start and which
    // song both players saw. Picking another song or a new request after a
    // decline produces a different (or empty) key, so the dialog shows again.
    if (! room.hasPendingStart())
        return {};

    return room.start.requestedBy + "|" + room.songId;
}

bool OnlineDuelRoomWindow::isConfirmedStartKey (const juce::String& key) const
{
    return key.isNotEmpty() && key == confirmedStartKey;
}

void OnlineDuelRoomWindow::applyRoom (const DuelRoomSnapshot& newRoom)
{
    if (! newRoom.valid)
        return;

    room = newRoom;

    titleLabel.setText (room.name.isNotEmpty() ? room.name : "ONLINE ROOM",
                        juce::dontSendNotification);

    juce::StringArray names;

    for (const auto& player : room.players)
        names.add ((player.isHost ? "HOST  " : "GUEST  ")
                   + player.displayNameOrUsername());

    playersLabel.setText (names.isEmpty() ? "Waiting for players..."
                                          : names.joinIntoString ("   |   "),
                          juce::dontSendNotification);

    roomHasTwoPlayers = room.players.size() >= 2;
    voiceButton.setEnabled (roomHasTwoPlayers);

    if (! roomHasTwoPlayers && voiceChatOn)
    {
        voiceChatOn = false;
        setVoiceChatEnabled (false);

        if (onVoiceChatToggled != nullptr)
            onVoiceChatToggled (false);
    }

    const auto waiting = room.status == "waiting";
    const bool host = isHost();
    const bool pendingStart = room.hasPendingStart() && waiting;
    const auto pendingKey = pendingStart ? startRequestKey() : juce::String();

    // This side already confirmed the round: remember it by request identity and
    // never show the dialog again for it. A round that ended (decline, match
    // created, room reset) clears the memory so the next request asks again.
    if (pendingKey.isEmpty())
        confirmedStartKey.clear();
    else if (hasIConfirmedStart())
        confirmedStartKey = pendingKey;
    else if (pendingKey != confirmedStartKey)
        confirmedStartKey.clear();

    // Both players see the host's selection; the guest cannot change it.
    if (room.songTitle.isNotEmpty())
    {
        songLabel.setText ("SELECTED SONG\n" + room.songTitle
                               + (room.songArtist.isNotEmpty()
                                      ? "  -  " + room.songArtist
                                      : juce::String()),
                           juce::dontSendNotification);
        statusLabel.setText (host
                                 ? "Press START GAME when the guest is in. "
                                   "Both players confirm before the song is downloaded."
                                 : "The host will start the match. "
                                   "You confirm before the song is downloaded.",
                             juce::dontSendNotification);
    }
    else
    {
        songLabel.setText ("SELECTED SONG\n- not chosen yet -",
                           juce::dontSendNotification);
        statusLabel.setText (host ? "Pick a song, then press START GAME."
                                  : "Waiting for the host to pick a song.",
                             juce::dontSendNotification);
    }

    // Keep the picker on the host's newest pick (or on the song the room
    // actually has). A poll that answers before the selection request was
    // committed must not put the previous song back.
    restoreSongPicker();

    if (pendingStart)
        statusLabel.setText ("Waiting for both players to confirm...",
                             juce::dontSendNotification);

    // A short local message (declined / waiting for the opponent) must not be
    // wiped by the next poll one second later.
    if (transientStatusUntilMs > juce::Time::getMillisecondCounterHiRes())
        statusLabel.setText (transientStatus, juce::dontSendNotification);

    selectSongButton.setEnabled (host && waiting && ! songChoices.empty());
    songBox.setEnabled (host && waiting);
    startButton.setEnabled (host && waiting && ! pendingStart
                            && room.players.size() >= 2
                            && room.songId.isNotEmpty());

    if (room.status == "playing" && room.matchId.isNotEmpty() && ! started)
    {
        started = true;
        stopTimer();
        closeStartDialog (true);

        if (onStart != nullptr)
            onStart (room.matchId);

        closeDialog (*this);
        return;
    }

    if (room.status == "closed")
    {
        stopTimer();
        closeStartDialog (true);
        closeDialog (*this);

        if (onLeave != nullptr)
            onLeave();

        return;
    }

    // The confirmation is shown to both players as soon as the host requests a
    // start. Nothing is downloaded before both press CONFIRM START.
    if (pendingStart)
    {
        maybeShowStartDialog();
    }
    else if (startDialogOpen && ! startActionInFlight)
    {
        // The request disappeared while our dialog was open: the other player
        // declined (or the server cleared the round). Stay in the room.
        closeStartDialog (true);
        setTransientStatus ("The other player declined the start.");

        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::InfoIcon,
            "Start cancelled",
            "The other player declined the start. "
                "Press START GAME to try again.",
            "OK");
    }
}

void OnlineDuelRoomWindow::maybeShowStartDialog()
{
    if (! room.hasPendingStart() || started || startDialogOpen || startActionInFlight)
        return;

    // This side already confirmed: the room now waits for the other player.
    if (hasIConfirmedStart())
        return;

    // Same confirmation round as the one already answered here (a snapshot
    // without the flags, e.g. one requested before the confirmation).
    if (isConfirmedStartKey (startRequestKey()))
        return;

    startDialogOpen = true;

    const auto songTitle = room.songTitle.isNotEmpty()
                               ? room.songTitle
                               : juce::String ("the selected song");

    auto* dialog = new juce::AlertWindow (
        "Start the duel?",
        songTitle + "\n\n"
            "The song is downloaded after you confirm. "
            "Both players must confirm before the match begins.",
        juce::AlertWindow::QuestionIcon);

    dialog->addButton ("CONFIRM START", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dialog->addButton ("DECLINE", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    startDialog = dialog;

    juce::Component::SafePointer<OnlineDuelRoomWindow> safeThis (this);

    dialog->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [safeThis] (int result)
            {
                if (auto* self = safeThis.getComponent())
                {
                    self->startDialog = nullptr;
                    self->startDialogOpen = false;

                    if (self->suppressStartDialogResult)
                    {
                        self->suppressStartDialogResult = false;
                        return;
                    }

                    if (result == 1)
                        self->confirmStart();
                    else
                        self->declineStart();
                }
            }),
        true);
}

void OnlineDuelRoomWindow::applyAuthoritative (const DuelRoomSnapshot& snapshot)
{
    if (! snapshot.valid)
        return;

    // Responses carry newer data than any poll that is still in flight.
    appliedRoomSeq = roomRequestSeq;
    applyRoom (snapshot);
}

void OnlineDuelRoomWindow::closeStartDialog (bool programmatic)
{
    if (! startDialogOpen)
    {
        startDialog = nullptr;
        return;
    }

    suppressStartDialogResult = programmatic;

    if (auto* dialog = startDialog.getComponent())
        dialog->exitModalState (0);

    startDialog = nullptr;
    startDialogOpen = false;
}

void OnlineDuelRoomWindow::setTransientStatus (const juce::String& text)
{
    transientStatus = text;
    transientStatusUntilMs = juce::Time::getMillisecondCounterHiRes() + 6000.0;
    statusLabel.setText (text, juce::dontSendNotification);
}


void OnlineDuelRoomWindow::refreshRoom()
{
    if (pollInFlight || lobbyId.isEmpty())
        return;

    pollInFlight = true;

    // Every request gets a number; a reply that was sent before a newer one (or
    // before an out-of-band confirmation) is stale and must be dropped.
    const int seq = ++roomRequestSeq;
    juce::Component::SafePointer<OnlineDuelRoomWindow> safeThis (this);

    client.room (lobbyId, token,
                 [safeThis, seq] (const DuelReply& reply, const DuelRoomSnapshot& snapshot)
                 {
                     if (auto* self = safeThis.getComponent())
                     {
                         self->pollInFlight = false;

                         if (! reply.ok || seq <= self->appliedRoomSeq)
                             return;

                         self->appliedRoomSeq = seq;
                         self->applyRoom (snapshot);
                     }
                 });
}

void OnlineDuelRoomWindow::refreshChat()
{
    if (lobbyId.isEmpty() || chatPollInFlight)
        return;

    chatPollInFlight = true;
    juce::Component::SafePointer<OnlineDuelRoomWindow> safeThis (this);

    client.roomChat (lobbyId, lastChatId, 100, token,
                     [safeThis] (const DuelReply& reply,
                                 const DuelChatHistoryReply& history)
                     {
                         if (auto* self = safeThis.getComponent())
                         {
                             self->chatPollInFlight = false;

                             if (! reply.ok || ! history.valid)
                                 return;

                             for (const auto& message : history.messages)
                             {
                                 self->appendChat (message);
                                 self->lastChatId = juce::jmax (self->lastChatId,
                                                                message.id);
                             }
                         }
                     });
}

void OnlineDuelRoomWindow::appendChat (const DuelChatMessage& message)
{
    chatLog.moveCaretToEnd();
    chatLog.insertTextAtCaret (message.displayNameOrUsername() + ": "
                               + message.text + "\n");
}

void OnlineDuelRoomWindow::sendChat()
{
    const auto text = chatInput.getText().trim();

    if (text.isEmpty() || lobbyId.isEmpty())
        return;

    juce::Component::SafePointer<OnlineDuelRoomWindow> safeThis (this);

    client.sendRoomChat (lobbyId, text, token,
                         [safeThis] (const DuelReply& reply,
                                     const DuelChatMessage& message)
                         {
                             if (auto* self = safeThis.getComponent())
                             {
                                 if (reply.ok)
                                 {
                                     self->chatInput.clear();
                                     self->appendChat (message);
                                     self->lastChatId = juce::jmax (self->lastChatId,
                                                                    message.id);
                                 }
                             }
                         });
}

void OnlineDuelRoomWindow::selectSong()
{
    const int index = songBox.getSelectedId() - 1;

    if (index < 0 || index >= static_cast<int> (songChoices.size()))
        return;

    const auto songId = songChoices[static_cast<size_t> (index)].id;

    // The picker is only interactive for the host in a waiting room; a program
    // driven change (or the guest's disabled box) never reaches the server.
    if (! isHost() || room.status != "waiting")
    {
        restoreSongPicker();
        return;
    }

    if (songId == room.songId)
    {
        pendingSongId.clear();

        // A pick that is still being saved would otherwise win over this "back
        // to the room's song" choice: re-assert it when that reply arrives.
        if (songSelectInFlight)
            pendingSongId = songId;

        return;
    }

    pendingSongId = songId;
    submitSongPick (songId);
}

void OnlineDuelRoomWindow::submitSongPick (const juce::String& songId)
{
    if (songId.isEmpty() || songSelectInFlight)
        return;

    songSelectInFlight = true;
    juce::Component::SafePointer<OnlineDuelRoomWindow> safeThis (this);

    client.selectRoomSong (lobbyId, songId, token,
        [safeThis, songId] (const DuelReply& reply,
                            const DuelRoomSnapshot& snapshot)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->songSelectInFlight = false;

            if (reply.ok)
            {
                if (songId == self->pendingSongId)
                    self->pendingSongId.clear();

                self->applyAuthoritative (snapshot);

                // The host picked another song while this one was being saved.
                if (self->pendingSongId.isNotEmpty()
                    && self->pendingSongId != self->room.songId)
                {
                    self->submitSongPick (self->pendingSongId);
                }
                else
                {
                    self->pendingSongId.clear();
                    self->restoreSongPicker();
                }

                return;
            }

            // The server kept its song: show it again instead of leaving a
            // selection that will never be played.
            self->pendingSongId.clear();
            self->restoreSongPicker();
            self->setTransientStatus (reply.errorMessage.isNotEmpty()
                                          ? reply.errorMessage
                                          : juce::String ("Could not select the song."));
        });
}

void OnlineDuelRoomWindow::restoreSongPicker()
{
    const auto wanted = pendingSongId.isNotEmpty() ? pendingSongId : room.songId;

    if (wanted.isEmpty())
    {
        songBox.setSelectedId (0, juce::dontSendNotification);
        return;
    }

    for (int i = 0; i < static_cast<int> (songChoices.size()); ++i)
        if (songChoices[static_cast<size_t> (i)].id == wanted)
        {
            if (songBox.getSelectedId() != i + 1)
                songBox.setSelectedId (i + 1, juce::dontSendNotification);

            return;
        }

    // The room's song is not in this catalogue (removed or unpublished): leave
    // the picker empty so the host notices instead of seeing a wrong title.
    songBox.setSelectedId (0, juce::dontSendNotification);
}

void OnlineDuelRoomWindow::startMatch()
{
    // The host asks both players to confirm. The match (and the download) is
    // only created when both confirmed — nothing is downloaded here.
    if (lobbyId.isEmpty() || ! isHost() || startActionInFlight)
        return;

    startActionInFlight = true;

    juce::Component::SafePointer<OnlineDuelRoomWindow> safeThis (this);

    client.requestLobbyStart (lobbyId, token,
        [safeThis] (const DuelReply& reply, const DuelRoomSnapshot& snapshot)
        {
            if (auto* self = safeThis.getComponent())
            {
                self->startActionInFlight = false;

                if (! reply.ok || ! snapshot.valid)
                {
                    self->setTransientStatus (
                        reply.errorMessage.isNotEmpty()
                            ? reply.errorMessage
                            : juce::String ("Could not start the duel."));
                    return;
                }

                self->applyAuthoritative (snapshot);
                self->setTransientStatus ("Waiting for both players to confirm...");
            }
        });
}

void OnlineDuelRoomWindow::confirmStart()
{
    if (lobbyId.isEmpty() || startActionInFlight)
        return;

    startActionInFlight = true;

    juce::Component::SafePointer<OnlineDuelRoomWindow> safeThis (this);

    client.confirmLobbyStart (lobbyId, token,
        [safeThis] (const DuelReply& reply, const DuelRoomStartReply& start)
        {
            if (auto* self = safeThis.getComponent())
            {
                if (! reply.ok || ! start.valid)
                {
                    self->startActionInFlight = false;
                    self->setTransientStatus (
                        reply.errorMessage.isNotEmpty()
                            ? reply.errorMessage
                            : juce::String ("Could not confirm the start."));
                    return;
                }

                // applyAuthoritative() starts the match (closes the room, joins
                // the match) when the second confirmation arrived and the server
                // created the match. The confirmation is remembered by request
                // identity, so a lagging snapshot cannot reopen this dialog for
                // the same round.
                self->confirmedStartKey = start.lobby.hasPendingStart()
                    ? start.lobby.start.requestedBy + "|" + start.lobby.songId
                    : juce::String();

                self->applyAuthoritative (start.lobby);
                self->startActionInFlight = false;

                if (! start.hasMatch())
                    self->setTransientStatus ("Waiting for the other player...");
            }
        });
}

void OnlineDuelRoomWindow::declineStart()
{
    if (lobbyId.isEmpty())
        return;

    startActionInFlight = true;

    juce::Component::SafePointer<OnlineDuelRoomWindow> safeThis (this);

    client.declineLobbyStart (lobbyId, token,
        [safeThis] (const DuelReply& reply, const DuelRoomSnapshot& snapshot)
        {
            if (auto* self = safeThis.getComponent())
            {
                if (reply.ok)
                    self->applyAuthoritative (snapshot);

                self->startActionInFlight = false;

                self->setTransientStatus (
                    "Start cancelled. Press START GAME to try again.");
            }
        });
}

void OnlineDuelRoomWindow::leaveRoom()
{
    closeStartDialog (true);

    juce::Component::SafePointer<OnlineDuelRoomWindow> safeThis (this);

    client.leaveRoom (lobbyId, token,
                      [safeThis] (const DuelReply&, const DuelRoomSnapshot&)
                      {
                          if (auto* self = safeThis.getComponent())
                          {
                              self->stopTimer();
                              closeDialog (*self);

                              if (self->onLeave != nullptr)
                                  self->onLeave();
                          }
                      });
}

void OnlineDuelRoomWindow::timerCallback()
{
    if (! roomHasTwoPlayers)
    {
        voiceLabel.setText ("WAITING FOR THE SECOND PLAYER...",
                            juce::dontSendNotification);
    }
    else if (voiceStatusProvider != nullptr)
    {
        voiceLabel.setText (voiceStatusProvider(), juce::dontSendNotification);
    }

    // The voice meter refreshes at 5 Hz; the room/chat polls stay at 1.2 s.
    if (++pollTicks % 6 != 0)
        return;

    refreshRoom();
    refreshChat();
}
