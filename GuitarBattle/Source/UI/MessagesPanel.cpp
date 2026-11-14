#include "MessagesPanel.h"

#include "GameStyle.h"
#include "../Network/ApiClient.h"

#include <cmath>

namespace
{
    juce::String presenceText (const juce::String& presence)
    {
        if (presence == "online")   return "ONLINE";
        if (presence == "in_lobby") return "IN LOBBY";
        if (presence == "in_match") return "IN MATCH";

        return "OFFLINE";
    }

    juce::Colour presenceTint (const juce::String& presence)
    {
        if (presence == "online")   return UiColors::green;
        if (presence == "in_lobby") return UiColors::accent;
        if (presence == "in_match") return UiColors::orange;

        return UiColors::muted;
    }

    juce::String peerName (const juce::String& displayName, const juce::String& username)
    {
        if (displayName.isNotEmpty())
            return displayName;

        return username.isNotEmpty() ? username : juce::String ("PLAYER");
    }
}

MessagesPanel::MessagesPanel (ApiClient& apiToUse, juce::String bearerToken)
    : api (apiToUse), token (std::move (bearerToken))
{
    composer.setMultiLine (false);
    composer.setReturnKeyStartsNewLine (false);
    composer.setTextToShowWhenEmpty ("Write a message...", UiColors::muted);
    composer.setColour (juce::TextEditor::backgroundColourId, UiColors::surface2);
    composer.setColour (juce::TextEditor::outlineColourId, UiColors::border);
    composer.setColour (juce::TextEditor::focusedOutlineColourId, UiColors::accent);
    composer.setColour (juce::TextEditor::textColourId, UiColors::text);
    composer.setColour (juce::TextEditor::highlightColourId,
                        UiColors::accent.withAlpha (0.30f));
    composer.onReturnKey = [this] { sendMessage(); };
    addAndMakeVisible (composer);

    sendButton.setColour (juce::TextButton::buttonColourId, UiColors::accent);
    sendButton.setColour (juce::TextButton::buttonOnColourId, UiColors::accentStrong);
    sendButton.setColour (juce::TextButton::textColourOffId, UiColors::onAccent);
    sendButton.setColour (juce::TextButton::textColourOnId, UiColors::onAccent);
    sendButton.onClick = [this] { sendMessage(); };
    addAndMakeVisible (sendButton);

    refreshButton.setColour (juce::TextButton::buttonColourId,
                             juce::Colours::transparentBlack);
    refreshButton.setColour (juce::TextButton::buttonOnColourId, UiColors::surface2);
    refreshButton.setColour (juce::TextButton::textColourOffId, UiColors::text);
    refreshButton.setColour (juce::TextButton::textColourOnId, UiColors::text);
    refreshButton.onClick = [this] { refreshAll(); };
    addAndMakeVisible (refreshButton);

    startTimerHz (2);
}

MessagesPanel::~MessagesPanel() = default;

void MessagesPanel::refreshAll()
{
    loadConversations();

    if (activeUserId.isNotEmpty())
        loadThread (false);
}

void MessagesPanel::openConversation (const juce::String& userId,
                                      const juce::String& displayName)
{
    activeUserId = userId;
    activeName = displayName;
    activePresence.clear();
    thread.clear();
    threadScroll = 0;
    statusLine.clear();

    loadThread (true);
    repaint();
}

void MessagesPanel::timerCallback()
{
    if (++pollTicks < 10) // 2 Hz timer, poll every 5 s
        return;

    pollTicks = 0;

    if (! isVisible())
        return;

    loadConversations();

    if (activeUserId.isNotEmpty())
        loadThread (false);
}

//==============================================================================
// Network

void MessagesPanel::loadConversations()
{
    if (token.isEmpty())
        return;

    const int requestId = ++conversationsRequestId;
    juce::Component::SafePointer<MessagesPanel> safeThis (this);

    api.getJson ("/messages/conversations?limit=50", token, 6000,
        [safeThis, requestId] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr || requestId != self->conversationsRequestId)
                return;

            if (! reply.transportOk || ! reply.json.isObject())
                return;

            self->conversations.clear();

            if (const auto* items = reply.json.getProperty ("items", juce::var()).getArray())
            {
                for (const auto& item : *items)
                {
                    const auto user = item.getProperty ("user", juce::var());
                    const auto last = item.getProperty ("last_message", juce::var());

                    Conversation conversation;
                    conversation.userId =
                        user.getProperty ("user_id", juce::var()).toString();
                    conversation.username =
                        user.getProperty ("username", juce::var()).toString();
                    conversation.displayName =
                        user.getProperty ("display_name", juce::var()).toString();
                    conversation.presence =
                        user.getProperty ("presence", "offline").toString();
                    conversation.lastBody =
                        last.getProperty ("body", juce::var()).toString();
                    conversation.lastTime =
                        shortTime (last.getProperty ("created_at", juce::var()).toString());
                    conversation.unread = static_cast<int> (item.getProperty ("unread", 0));

                    self->conversations.push_back (std::move (conversation));
                }
            }

            self->unreadTotal =
                static_cast<int> (reply.json.getProperty ("unread_total", 0));

            if (self->onUnreadChanged != nullptr)
                self->onUnreadChanged (self->unreadTotal);

            self->repaint();
        });
}

void MessagesPanel::loadThread (bool scrollToBottom)
{
    if (token.isEmpty() || activeUserId.isEmpty())
        return;

    const int requestId = ++threadRequestId;
    const auto userId = activeUserId;
    juce::Component::SafePointer<MessagesPanel> safeThis (this);

    api.getJson ("/messages/" + userId + "?limit=50", token, 6000,
        [safeThis, requestId, userId, scrollToBottom] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr || requestId != self->threadRequestId
                || self->activeUserId != userId)
                return;

            if (! reply.transportOk || ! reply.json.isObject())
            {
                if (reply.statusCode == 404)
                {
                    // The peer was removed or blocked: drop the open thread.
                    self->activeUserId.clear();
                    self->activeName.clear();
                    self->activePresence.clear();
                    self->thread.clear();
                    self->statusLine = "This player is no longer available.";
                    self->loadConversations();
                }
                else
                {
                    self->statusLine = "Could not load the conversation.";
                }

                self->repaint();
                return;
            }

            const auto peer = reply.json.getProperty ("peer", juce::var());

            if (peer.isObject())
            {
                const auto displayName =
                    peer.getProperty ("display_name", juce::var()).toString();
                const auto username =
                    peer.getProperty ("username", juce::var()).toString();

                self->activeName = peerName (displayName, username);
                self->activePresence =
                    peer.getProperty ("presence", "offline").toString();
            }

            self->thread.clear();

            if (const auto* items = reply.json.getProperty ("items", juce::var()).getArray())
            {
                for (const auto& item : *items)
                {
                    ChatMessage message;
                    message.id = item.getProperty ("id", juce::var()).toString();
                    message.body = item.getProperty ("body", juce::var()).toString();
                    message.time = shortTime (
                        item.getProperty ("created_at", juce::var()).toString());
                    message.mine = static_cast<bool> (item.getProperty ("mine", false));
                    message.read = static_cast<bool> (item.getProperty ("read", false));
                    self->thread.push_back (std::move (message));
                }
            }

            if (scrollToBottom)
                self->threadScroll = 0;

            self->statusLine.clear();
            self->repaint();
        });
}

void MessagesPanel::markThreadRead()
{
    if (token.isEmpty() || activeUserId.isEmpty())
        return;

    juce::Component::SafePointer<MessagesPanel> safeThis (this);

    api.postJson ("/messages/" + activeUserId + "/read",
                  juce::var (new juce::DynamicObject()), token, 5000,
        [safeThis] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr || ! reply.transportOk)
                return;

            self->unreadTotal =
                static_cast<int> (reply.json.getProperty ("unread_total", 0));

            if (self->onUnreadChanged != nullptr)
                self->onUnreadChanged (self->unreadTotal);
        });
}

void MessagesPanel::sendMessage()
{
    if (sending || activeUserId.isEmpty() || token.isEmpty())
        return;

    const auto body = composer.getText().trim();

    if (body.isEmpty())
        return;

    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty ("body", body);

    sending = true;
    sendButton.setEnabled (false);

    juce::Component::SafePointer<MessagesPanel> safeThis (this);
    const auto userId = activeUserId;

    api.postJson ("/messages/" + userId, juce::var (payload.get()), token, 6000,
        [safeThis, body, userId] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->sending = false;
            self->sendButton.setEnabled (true);

            if (! reply.transportOk || reply.statusCode >= 400)
            {
                self->statusLine = reply.statusCode == 403
                                       ? "You cannot message this player."
                                       : "Could not send the message.";
                self->repaint();
                return;
            }

            // Only clear the editor when the message actually left the client.
            if (self->composer.getText().trim() == body)
                self->composer.clear();

            self->statusLine.clear();
            self->threadScroll = 0;
            self->loadThread (true);
            self->loadConversations();
        });
}

//==============================================================================
// Helpers

juce::String MessagesPanel::shortTime (const juce::String& iso)
{
    if (iso.length() < 16)
        return iso;

    const auto date = iso.substring (0, 10);
    const auto time = iso.substring (11, 16);
    const auto today = juce::Time::getCurrentTime().formatted ("%Y-%m-%d");

    if (date == today)
        return time;

    return iso.substring (8, 10) + "." + iso.substring (5, 7);
}

int MessagesPanel::threadContentHeight (int width) const
{
    if (width <= 0)
        return 0;

    const int bubbleMax = juce::jmax (140, juce::roundToInt (width * 0.62f));
    const int textWidth = juce::jmax (80, bubbleMax - 26);
    const auto font = GameUi::font (12.5f);

    int height = 8;

    for (const auto& message : thread)
    {
        const auto textW = juce::GlyphArrangement::getStringWidthInt (font, message.body);
        const int lines = juce::jmax (1, (textW + textWidth - 1) / textWidth);
        height += 14 + lines * 17 + 14 + 8;
    }

    return height;
}

int MessagesPanel::rowAt (juce::Point<int> position) const
{
    constexpr int rowH = 62;
    const int listTop = listArea.getY() + 40;

    if (! listArea.contains (position) || position.y < listTop)
        return -1;

    const int index = (position.y - listTop) / rowH;

    return juce::isPositiveAndBelow (index, static_cast<int> (conversations.size()))
               ? index
               : -1;
}

//==============================================================================
// Layout & painting

void MessagesPanel::resized()
{
    const int listW = juce::jlimit (240, 360, getWidth() / 3);

    listArea = { 0, 0, listW, getHeight() };

    const int rightX = listW + 14;
    const int rightW = juce::jmax (200, getWidth() - rightX);

    headerArea = { rightX, 0, rightW, 44 };
    composerArea = { rightX, getHeight() - 46, juce::jmax (120, rightW - 104), 38 };
    threadArea = { rightX, headerArea.getBottom() + 8, rightW,
                   juce::jmax (80, composerArea.getY() - 8 - headerArea.getBottom() - 8) };

    composer.setBounds (composerArea);
    sendButton.setBounds (composerArea.getRight() + 8, composerArea.getY(), 88, 38);
    refreshButton.setBounds (listArea.getWidth() - 96, 8, 88, 26);
}

void MessagesPanel::paint (juce::Graphics& g)
{
    g.fillAll (UiColors::background);

    // --- conversation list ---
    GameUi::drawCard (g, listArea.toFloat(), 16.0f);

    g.setColour (UiColors::accent);
    g.setFont (GameUi::eyebrow (10.0f));
    g.drawText ("CONVERSATIONS", listArea.getX() + 16, listArea.getY() + 12,
                listArea.getWidth() - 130, 14,
                juce::Justification::centredLeft, false);

    if (conversations.empty())
    {
        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (11.5f));
        g.drawFittedText ("No messages yet.\nOpen a player profile and press MESSAGE.",
                          listArea.reduced (16).withTrimmedTop (40),
                          juce::Justification::centredTop, 3);
    }

    constexpr int rowH = 62;
    const int listTop = listArea.getY() + 40;

    for (int i = 0; i < static_cast<int> (conversations.size()); ++i)
    {
        const auto& conversation = conversations[static_cast<size_t> (i)];
        const juce::Rectangle<int> row (listArea.getX() + 8, listTop + i * rowH,
                                        listArea.getWidth() - 16, rowH - 4);

        if (row.getBottom() > listArea.getBottom() - 8)
            break;

        if (conversation.userId == activeUserId)
        {
            g.setColour (UiColors::accent.withAlpha (0.13f));
            g.fillRoundedRectangle (row.toFloat(), 10.0f);
        }
        else if (i == hoveredRow)
        {
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.fillRoundedRectangle (row.toFloat(), 10.0f);
        }

        const auto avatar = juce::Rectangle<int> (row.getX() + 8, row.getCentreY() - 18,
                                                  36, 36);
        g.setColour (UiColors::surface2);
        g.fillEllipse (avatar.toFloat());
        g.setColour (UiColors::accent);
        g.setFont (GameUi::heading (14.0f));
        g.drawText (peerName (conversation.displayName, conversation.username)
                        .substring (0, 1).toUpperCase (),
                    avatar, juce::Justification::centred, false);

        const int textX = avatar.getRight() + 10;
        const int textW = juce::jmax (60, row.getRight() - textX - (conversation.unread > 0 ? 40 : 10));

        g.setColour (UiColors::text);
        g.setFont (GameUi::font (12.5f, true));
        g.drawFittedText (peerName (conversation.displayName, conversation.username),
                          textX, row.getY() + 8, textW, 16,
                          juce::Justification::centredLeft, 1);

        g.setColour (conversation.unread > 0 ? UiColors::text : UiColors::muted);
        g.setFont (GameUi::font (11.0f));
        g.drawFittedText (conversation.lastBody, textX, row.getY() + 26,
                          juce::jmax (60, row.getRight() - textX - 10), 16,
                          juce::Justification::centredLeft, 1);

        if (conversation.unread > 0)
        {
            const auto badge = juce::Rectangle<float> (
                static_cast<float> (row.getRight()) - 30.0f,
                static_cast<float> (row.getCentreY()) - 9.0f, 22.0f, 18.0f);
            g.setColour (UiColors::accent);
            g.fillRoundedRectangle (badge, 9.0f);
            g.setColour (UiColors::onAccent);
            g.setFont (GameUi::font (10.0f, true));
            g.drawText (juce::String (juce::jmin (conversation.unread, 99)), badge,
                        juce::Justification::centred, false);
        }
    }

    // --- thread ---
    if (activeUserId.isEmpty())
    {
        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (13.0f));
        g.drawText ("Select a conversation to read and write messages.",
                    threadArea.withTrimmedTop (40), juce::Justification::centred, false);
        return;
    }

    g.setColour (UiColors::text);
    g.setFont (GameUi::heading (18.0f));
    g.drawFittedText (activeName, headerArea.getX(), headerArea.getY() + 4,
                      juce::jmax (120, headerArea.getWidth() - 140), 24,
                      juce::Justification::centredLeft, 1);

    const auto presenceArea = juce::Rectangle<float> (
        static_cast<float> (headerArea.getX()),
        static_cast<float> (headerArea.getY() + 26), 96.0f, 18.0f);
    GameUi::drawPill (g, presenceArea, presenceText (activePresence),
                      presenceTint (activePresence), true);

    GameUi::drawDivider (g, (float) threadArea.getX(),
                         (float) threadArea.getRight(),
                         (float) threadArea.getY() - 4.0f);

    threadContentH = threadContentHeight (threadArea.getWidth());

    {
        const juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (threadArea);

        const int bubbleMax = juce::jmax (140, juce::roundToInt (threadArea.getWidth() * 0.62f));
        const int textWidth = juce::jmax (80, bubbleMax - 26);
        const auto font = GameUi::font (12.5f);
        const auto timeFont = GameUi::font (9.5f);

        int y = threadArea.getBottom() - 6 + threadScroll;

        for (int i = static_cast<int> (thread.size()); --i >= 0;)
        {
            const auto& message = thread[static_cast<size_t> (i)];
            const auto textW = juce::GlyphArrangement::getStringWidthInt (font, message.body);
            const int lines = juce::jmax (1, (textW + textWidth - 1) / textWidth);
            const int bubbleH = 14 + lines * 17 + 14;

            y -= bubbleH;

            const juce::Rectangle<int> bubble (
                message.mine ? threadArea.getRight() - bubbleMax - 8
                             : threadArea.getX(),
                y, bubbleMax, bubbleH);

            if (bubble.getBottom() > threadArea.getY() - 8
                && bubble.getY() < threadArea.getBottom())
            {
                g.setColour (message.mine
                                 ? UiColors::accent.withAlpha (0.20f)
                                 : UiColors::surface2);
                g.fillRoundedRectangle (bubble.toFloat(), 12.0f);

                g.setColour (UiColors::text);
                g.setFont (font);
                g.drawFittedText (message.body,
                                  bubble.reduced (13, 10).withTrimmedBottom (12),
                                  juce::Justification::topLeft,
                                  juce::jmax (1, lines));

                g.setColour (UiColors::muted);
                g.setFont (timeFont);
                g.drawText (message.time, bubble.reduced (13, 6),
                            juce::Justification::bottomRight, false);
            }

            y -= 8;
        }
    }

    if (statusLine.isNotEmpty())
    {
        g.setColour (UiColors::accent);
        g.setFont (GameUi::font (11.0f));
        g.drawText (statusLine, composerArea.getX(), composerArea.getY() - 18,
                    composerArea.getWidth(), 16,
                    juce::Justification::centredLeft, true);
    }
}

//==============================================================================
// Mouse

void MessagesPanel::mouseMove (const juce::MouseEvent& event)
{
    const int row = rowAt (event.getPosition());

    if (row != hoveredRow)
    {
        hoveredRow = row;
        repaint();
    }
}

void MessagesPanel::mouseExit (const juce::MouseEvent&)
{
    if (hoveredRow != -1)
    {
        hoveredRow = -1;
        repaint();
    }
}

void MessagesPanel::mouseUp (const juce::MouseEvent& event)
{
    const auto position = event.getPosition();

    // Header name opens the peer profile.
    if (activeUserId.isNotEmpty()
        && headerArea.withTrimmedRight (140).contains (position))
    {
        if (onOpenProfile != nullptr)
            onOpenProfile (activeUserId);

        return;
    }

    const int row = rowAt (position);

    if (row >= 0)
    {
        const auto& conversation = conversations[static_cast<size_t> (row)];

        openConversation (conversation.userId,
                          peerName (conversation.displayName, conversation.username));
        markThreadRead();
        loadConversations();
    }
}

void MessagesPanel::mouseWheelMove (const juce::MouseEvent& event,
                                    const juce::MouseWheelDetails& wheel)
{
    if (! threadArea.contains (event.getPosition()))
        return;

    const int viewport = threadArea.getHeight();
    const int maxScroll = juce::jmax (0, threadContentH - viewport);

    const int step = juce::roundToInt (wheel.deltaY * 240.0f);
    threadScroll = juce::jlimit (0, maxScroll, threadScroll - step);

    repaint();
}
