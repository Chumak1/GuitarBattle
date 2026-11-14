#include "FriendsPanel.h"

#include "PlayerCard.h"
#include "GameStyle.h"
#include "../Network/ApiClient.h"

#include <algorithm>

namespace
{
    juce::String humanKind (const juce::String& kind)
    {
        return kind.replaceCharacter ('_', ' ').toUpperCase();
    }

    juce::String shortDate (const juce::String& iso)
    {
        return iso.length() >= 16 ? iso.substring (0, 16).replaceCharacter ('T', ' ')
                                  : iso;
    }

    juce::String payloadName (const juce::var& payload)
    {
        auto name = payload.getProperty ("display_name", juce::var()).toString();

        if (name.isEmpty())
            name = payload.getProperty ("username", juce::var()).toString();

        if (name.isEmpty())
            name = payload.getProperty ("user_id", juce::var()).toString();

        return name.isNotEmpty() ? name : juce::String ("A player");
    }

    // Server kinds: friend_request, friend_accepted, activity_<kind>.
    juce::String describeNotification (const juce::String& kind, const juce::var& payload)
    {
        const auto who = payloadName (payload);

        if (kind == "friend_request")
            return who + " sent you a friend request.";

        if (kind == "friend_accepted")
            return who + " accepted your friend request.";

        if (kind.startsWith ("activity_"))
        {
            const auto inner = kind.substring (9);

            if (inner == "record")       return who + " set a new record.";
            if (inner == "level_up")     return who + " reached a new level.";
            if (inner == "achievement")  return who + " unlocked an achievement.";
            if (inner == "tower")        return who + " cleared a battle tower.";
            if (inner == "song")         return who + " mastered a new song.";

            return who + " - " + humanKind (inner);
        }

        return humanKind (kind);
    }
}

FriendsPanel::FriendsPanel (ApiClient& apiToUse, juce::String bearerToken)
    : api (apiToUse), token (std::move (bearerToken))
{
    viewport.setViewedComponent (&listContent, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    addAndMakeVisible (viewport);

    searchEditor.setMultiLine (false);
    searchEditor.setTextToShowWhenEmpty ("Search by nickname...", UiColors::muted);
    searchEditor.setColour (juce::TextEditor::backgroundColourId, UiColors::surface2);
    searchEditor.setColour (juce::TextEditor::outlineColourId, UiColors::border);
    searchEditor.setColour (juce::TextEditor::textColourId, UiColors::text);
    searchEditor.setVisible (false);
    searchEditor.onTextChange = [this]
    {
        searchQuery = searchEditor.getText();
        lastSearchMs = juce::Time::getMillisecondCounterHiRes();
        searchPending = true;
    };
    addAndMakeVisible (searchEditor);

    notificationsButton.setVisible (false);
    addAndMakeVisible (notificationsButton);

    startTimerHz (4);
}

FriendsPanel::~FriendsPanel() = default;

FriendsPanel::Player FriendsPanel::parsePlayer (const juce::var& object)
{
    Player player;
    player.id = object.getProperty ("user_id", juce::var()).toString();
    player.username = object.getProperty ("username", juce::var()).toString();
    player.displayName = object.getProperty ("display_name", juce::var()).toString();
    player.level = static_cast<int> (object.getProperty ("level", 1));
    player.rating = static_cast<int> (object.getProperty ("rating", 0));
    player.presence = object.getProperty ("presence", juce::var()).toString();
    player.relationship = object.getProperty ("relationship", juce::var()).toString();
    return player;
}

void FriendsPanel::timerCallback()
{
    if (! searchPending)
        return;

    if (juce::Time::getMillisecondCounterHiRes() - lastSearchMs < 350.0)
        return;

    searchPending = false;
    loadSearch();
}

void FriendsPanel::setTab (Tab newTab)
{
    if (tab == newTab && ! showNotifications)
        return;

    tab = newTab;
    showNotifications = false;
    rows.clear();
    rebuildCards();
    resized();
    repaint();

    if (tab == Tab::Friends)      loadFriends();
    else if (tab == Tab::Requests) loadRequests();
    else if (tab == Tab::Find)     loadSearch();
    else                           loadActivity();
}

void FriendsPanel::refreshAll()
{
    loadNotifications();

    if (tab == Tab::Friends)      loadFriends();
    else if (tab == Tab::Requests) loadRequests();
    else if (tab == Tab::Find)     loadSearch();
    else                           loadActivity();
}

void FriendsPanel::openNotificationsView()
{
    showNotifications = true;
    loadNotifications();
    // Opening the list is the moment the player sees the items: mark them.
    markAllRead();
    resized();
    repaint();
}

void FriendsPanel::loadFriends()
{
    juce::Component::SafePointer<FriendsPanel> safeThis (this);

    api.getJson ("/social/friends?limit=50", token, 6000,
        [safeThis] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->rows.clear();

            if (const auto* items = reply.json.getProperty ("items", juce::var()).getArray())
                for (const auto& item : *items)
                {
                    Row row;
                    row.player = parsePlayer (item);
                    row.action = RowAction::RemoveFriend;
                    self->rows.push_back (std::move (row));
                }

            self->statusLine = self->rows.empty()
                                   ? "No friends yet. Find players and add them."
                                   : juce::String (static_cast<int> (self->rows.size()))
                                         + " friends";
            self->rebuildCards();
        });
}

void FriendsPanel::loadRequests()
{
    juce::Component::SafePointer<FriendsPanel> safeThis (this);

    auto handle = [safeThis] (ApiClient::Response reply, bool incoming)
    {
        auto* self = safeThis.getComponent();

        if (self == nullptr)
            return;

        if (const auto* items = reply.json.getProperty ("items", juce::var()).getArray())
            for (const auto& item : *items)
            {
                Row row;
                row.player = parsePlayer (item.getProperty ("user", juce::var()));
                row.requestId = item.getProperty ("id", juce::var()).toString();
                row.action = incoming ? RowAction::AcceptRequest
                                      : RowAction::CancelRequest;
                self->rows.push_back (std::move (row));
            }

        self->statusLine = self->rows.empty() ? "No pending requests."
                                              : juce::String (static_cast<int> (self->rows.size()))
                                                    + " requests";
        self->rebuildCards();
    };

    api.getJson ("/social/requests?direction=incoming&limit=50", token, 6000,
        [handle] (ApiClient::Response reply) mutable { handle (reply, true); });

    api.getJson ("/social/requests?direction=outgoing&limit=50", token, 6000,
        [safeThis] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            if (const auto* items = reply.json.getProperty ("items", juce::var()).getArray())
                for (const auto& item : *items)
                {
                    Row row;
                    row.player = parsePlayer (item.getProperty ("user", juce::var()));
                    row.requestId = item.getProperty ("id", juce::var()).toString();
                    row.action = RowAction::CancelRequest;
                    self->rows.push_back (std::move (row));
                }

            self->statusLine = self->rows.empty() ? "No pending requests."
                                                  : juce::String (static_cast<int> (self->rows.size()))
                                                        + " requests";
            self->rebuildCards();
        });
}

void FriendsPanel::loadSearch()
{
    if (searchQuery.trim().isEmpty())
    {
        rows.clear();
        statusLine = "Type a nickname to find players.";
        rebuildCards();
        return;
    }

    juce::Component::SafePointer<FriendsPanel> safeThis (this);
    const auto query = searchQuery.trim().replaceCharacter (' ', '+');

    api.getJson ("/social/search?q=" + query + "&limit=30", token, 6000,
        [safeThis] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->rows.clear();

            if (const auto* items = reply.json.getProperty ("items", juce::var()).getArray())
                for (const auto& item : *items)
                {
                    Row row;
                    row.player = parsePlayer (item);
                    row.action = row.player.relationship == "outgoing"
                                     ? RowAction::CancelRequest
                                     : RowAction::AddFriend;
                    self->rows.push_back (std::move (row));
                }

            self->statusLine = self->rows.empty()
                                   ? "Nothing found for \"" + self->searchQuery + "\"."
                                   : juce::String (static_cast<int> (self->rows.size()))
                                         + " players";
            self->rebuildCards();
        });
}

void FriendsPanel::loadActivity()
{
    juce::Component::SafePointer<FriendsPanel> safeThis (this);

    api.getJson ("/social/activity?limit=40", token, 6000,
        [safeThis] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->notifications.clear();

            if (const auto* items = reply.json.getProperty ("items", juce::var()).getArray())
                for (const auto& item : *items)
                {
                    Notification entry;
                    const auto payload = item.getProperty ("payload", juce::var());
                    entry.kind = item.getProperty ("kind", juce::var()).toString();
                    entry.text = describeNotification (entry.kind, payload);
                    entry.time = shortDate (item.getProperty ("created_at", juce::var())
                                                .toString());
                    entry.userId = payload.getProperty ("user_id", juce::var()).toString();
                    entry.read = true;
                    self->notifications.push_back (std::move (entry));
                }

            self->statusLine = self->notifications.empty()
                                   ? "No recent activity from your friends."
                                   : juce::String (static_cast<int> (self->notifications.size()))
                                         + " events";
            self->rows.clear();
            self->rebuildCards();
        });
}

void FriendsPanel::loadNotifications()
{
    juce::Component::SafePointer<FriendsPanel> safeThis (this);

    api.getJson ("/social/notifications?limit=30", token, 6000,
        [safeThis] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->unreadCount = static_cast<int> (reply.json.getProperty ("unread", 0));
            self->notifications.clear();

            if (const auto* items = reply.json.getProperty ("items", juce::var()).getArray())
                for (const auto& item : *items)
                {
                    Notification entry;
                    const auto payload = item.getProperty ("payload", juce::var());
                    entry.kind = item.getProperty ("kind", juce::var()).toString();
                    entry.text = describeNotification (entry.kind, payload);
                    entry.time = shortDate (item.getProperty ("created_at", juce::var())
                                                .toString());
                    entry.userId = payload.getProperty ("user_id", juce::var()).toString();
                    entry.read = ! item.getProperty ("read_at", juce::var()).isVoid();
                    self->notifications.push_back (std::move (entry));
                }

            if (self->showNotifications)
                self->statusLine = self->notifications.empty()
                                       ? "No notifications."
                                       : juce::String (static_cast<int> (self->notifications.size()))
                                             + " notifications";

            self->notificationsButton.setButtonText (
                self->unreadCount > 0 ? "NOTIFICATIONS (" + juce::String (self->unreadCount) + ")"
                                      : "NOTIFICATIONS");

            if (self->onUnreadChanged != nullptr)
                self->onUnreadChanged (self->unreadCount);

            self->repaint();
        });
}

void FriendsPanel::rebuildCards()
{
    cards.clear();

    if (showNotifications || tab == Tab::Activity)
    {
        viewport.setVisible (false);
        resized();
        repaint();
        return;
    }

    viewport.setVisible (true);

    const int cardHeight = 152;
    const int gap = 12;
    const int width = juce::jmax (320, viewport.getMaximumVisibleWidth());

    for (int i = 0; i < static_cast<int> (rows.size()); ++i)
    {
        auto card = std::make_unique<PlayerCardComponent>();
        card->setMode (PlayerCardComponent::Mode::Standard);

        const auto& row = rows[static_cast<size_t> (i)];

        PlayerCardData data;
        data.userId = row.player.id;
        data.username = row.player.username;
        data.displayName = row.player.displayName;
        data.level = row.player.level;
        data.rating = row.player.rating;
        data.valid = true;

        if (row.player.presence == "online")
            data.presence = PlayerCardData::Presence::Online;
        else if (row.player.presence == "in_lobby")
            data.presence = PlayerCardData::Presence::InLobby;
        else if (row.player.presence == "in_match")
            data.presence = PlayerCardData::Presence::InMatch;
        else if (row.player.presence == "offline")
            data.presence = PlayerCardData::Presence::Offline;

        PlayerRelation relation = PlayerRelation::None;

        if (row.player.relationship == "friend" || row.action == RowAction::RemoveFriend)
            relation = PlayerRelation::Friend;
        else if (row.action == RowAction::AcceptRequest
                 || row.player.relationship == "incoming")
            relation = PlayerRelation::IncomingRequest;
        else if (row.action == RowAction::CancelRequest
                 || row.player.relationship == "outgoing")
            relation = PlayerRelation::OutgoingRequest;
        else if (row.player.relationship == "blocked")
            relation = PlayerRelation::Blocked;
        else if (row.player.relationship == "self")
            relation = PlayerRelation::Self;

        card->setData (data);
        card->setRelation (relation);

        const auto rowCopy = row;
        juce::Component::SafePointer<FriendsPanel> safeThis (this);

        card->setActionHandler ([safeThis, rowCopy] (PlayerCardComponent::Action picked)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            if (picked == PlayerCardComponent::Action::ViewProfile
                || picked == PlayerCardComponent::Action::Compare
                || picked == PlayerCardComponent::Action::Challenge)
            {
                if (self->onOpenProfile != nullptr)
                    self->onOpenProfile (rowCopy.player.id);

                return;
            }

            self->postAction (rowCopy, picked);
        });

        listContent.addAndMakeVisible (*card);
        card->setBounds (0, i * (cardHeight + gap), width, cardHeight);
        cards.push_back (std::move (card));
    }

    listContent.setSize (width,
                         juce::jmax (viewport.getHeight(),
                                     static_cast<int> (rows.size()) * (cardHeight + gap)));
    resized();
    repaint();
}

void FriendsPanel::postAction (const Row& row, PlayerCardComponent::Action picked)
{
    juce::Component::SafePointer<FriendsPanel> safeThis (this);

    auto done = [safeThis] (ApiClient::Response reply)
    {
        auto* self = safeThis.getComponent();

        if (self == nullptr)
            return;

        if (! reply.transportOk || (reply.statusCode >= 400))
            self->statusLine = reply.statusCode == 409
                                   ? "Already exists."
                                   : (reply.statusCode == 403 ? "Not allowed."
                                                              : "Request failed.");

        // Reload every tab so counts and relations stay in sync.
        self->refreshAll();
    };

    const auto emptyBody = juce::var (new juce::DynamicObject());

    switch (picked)
    {
        case PlayerCardComponent::Action::AddFriend:
            api.postJson ("/social/friends/" + row.player.id, emptyBody, token, 6000, done);
            break;

        case PlayerCardComponent::Action::CancelRequest:
            if (row.requestId.isNotEmpty())
                api.deleteJson ("/social/requests/" + row.requestId, token, 6000, done);
            break;

        case PlayerCardComponent::Action::AcceptRequest:
            if (row.requestId.isNotEmpty())
                api.postJson ("/social/requests/" + row.requestId + "/accept",
                              emptyBody, token, 6000, done);
            break;

        case PlayerCardComponent::Action::DeclineRequest:
            if (row.requestId.isNotEmpty())
                api.postJson ("/social/requests/" + row.requestId + "/decline",
                              emptyBody, token, 6000, done);
            break;

        case PlayerCardComponent::Action::RemoveFriend:
            api.deleteJson ("/social/friends/" + row.player.id, token, 6000, done);
            break;

        case PlayerCardComponent::Action::Block:
            api.postJson ("/social/blocks/" + row.player.id, emptyBody, token, 6000, done);
            break;

        default:
            break;
    }
}

void FriendsPanel::markAllRead()
{
    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty ("all", true);

    juce::Component::SafePointer<FriendsPanel> safeThis (this);
    api.postJson ("/social/notifications/read", juce::var (body.get()), token, 5000,
        [safeThis] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr || ! reply.transportOk)
                return;

            self->unreadCount = 0;

            for (auto& entry : self->notifications)
                entry.read = true;

            self->notificationsButton.setButtonText ("NOTIFICATIONS");

            if (self->onUnreadChanged != nullptr)
                self->onUnreadChanged (0);

            self->repaint();
        });
}

void FriendsPanel::layoutTabBar()
{
    const int width = getWidth();
    const int startY = 8;
    const int tabW = juce::jmax (110, (width - 200) / 4);

    for (int i = 0; i < 4; ++i)
        tabBounds[(size_t) i] = { 8 + i * (tabW + 8), startY, tabW, 30 };

    tabBounds[4] = { width - 8 - 200, startY, 200, 30 };
}

void FriendsPanel::paint (juce::Graphics& g)
{
    g.fillAll (UiColors::background);

    paintTabBar (g);

    if (showNotifications || tab == Tab::Activity)
    {
        paintNotifications (g);
        return;
    }

    if (rows.empty())
    {
        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (13.0f));
        g.drawText (statusLine.isNotEmpty() ? statusLine : "Loading...",
                    getLocalBounds().withTrimmedTop (120),
                    juce::Justification::centredTop, false);
    }
}

void FriendsPanel::paintTabBar (juce::Graphics& g)
{
    static const char* labels[] = { "FRIENDS", "REQUESTS", "FIND PLAYERS", "ACTIVITY" };

    for (int i = 0; i < 4; ++i)
    {
        const bool active = ! showNotifications
                            && static_cast<int> (tab) == i;
        const bool hovered = hoveredTab == i;

        if (active)
        {
            const juce::Colour top (0xFFF3C451);
            const juce::Colour bottom (0xFFE0A72C);
            juce::ColourGradient gradient (top, (float) tabBounds[(size_t) i].getX(),
                                           (float) tabBounds[(size_t) i].getY(),
                                           bottom, (float) tabBounds[(size_t) i].getX(),
                                           (float) tabBounds[(size_t) i].getBottom(), false);
            g.setGradientFill (gradient);
            g.fillRoundedRectangle (tabBounds[(size_t) i].toFloat(), 9.0f);
        }
        else if (hovered)
        {
            g.setColour (juce::Colours::white.withAlpha (0.05f));
            g.fillRoundedRectangle (tabBounds[(size_t) i].toFloat(), 9.0f);
        }

        g.setColour (active ? UiColors::onAccent : UiColors::muted);
        g.setFont (GameUi::font (11.0f, true));
        g.drawText (labels[i], tabBounds[(size_t) i], juce::Justification::centred, false);
    }

    const bool notificationsActive = showNotifications;
    auto area = tabBounds[4];

    g.setColour (notificationsActive ? UiColors::accent.withAlpha (0.20f)
                                     : UiColors::surface2);
    g.fillRoundedRectangle (area.toFloat(), 9.0f);

    if (unreadCount > 0)
    {
        g.setColour (UiColors::accent);
        g.fillEllipse ((float) area.getRight() - 18.0f, (float) area.getY() + 4.0f,
                       12.0f, 12.0f);
        g.setColour (UiColors::onAccent);
        g.setFont (GameUi::font (8.0f, true));
        g.drawText (juce::String (juce::jmin (unreadCount, 99)),
                    juce::Rectangle<float> ((float) area.getRight() - 18.0f,
                                            (float) area.getY() + 4.0f, 12.0f, 12.0f),
                    juce::Justification::centred, false);
    }

    g.setColour (unreadCount > 0 ? UiColors::text : UiColors::muted);
    g.setFont (GameUi::font (10.5f, true));
    g.drawText ("NOTIFICATIONS", area.withTrimmedRight (22),
                juce::Justification::centred, false);
}

void FriendsPanel::paintNotifications (juce::Graphics& g)
{
    auto area = getLocalBounds().reduced (8, 0).withTrimmedTop (52);
    const int rowH = 40;

    if (notifications.empty())
    {
        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (12.0f));
        g.drawText (tab == Tab::Activity ? "No recent activity."
                                         : "No notifications.",
                    area, juce::Justification::topLeft, false);
        return;
    }

    g.setFont (GameUi::font (11.5f));

    for (int i = 0; i < static_cast<int> (notifications.size()); ++i)
    {
        const auto& entry = notifications[static_cast<size_t> (i)];
        const juce::Rectangle<int> row (area.getX(), area.getY() + i * rowH,
                                        area.getWidth(), rowH - 4);

        if (row.getBottom() > getHeight())
            break;

        g.setColour (UiColors::surface2.withAlpha (0.55f));
        g.fillRoundedRectangle (row.toFloat(), 8.0f);

        if (! entry.read)
        {
            g.setColour (UiColors::accent);
            g.fillEllipse ((float) row.getX() + 12.0f, (float) row.getCentreY() - 3.0f,
                           6.0f, 6.0f);
        }

        g.setColour (entry.read ? UiColors::muted : UiColors::text);
        g.setFont (GameUi::font (11.5f));
        g.drawText (entry.text, row.withTrimmedLeft (28).withTrimmedRight (130),
                    juce::Justification::centredLeft, true);

        if (entry.time.isNotEmpty())
        {
            g.setColour (UiColors::muted);
            g.setFont (GameUi::font (10.0f));
            g.drawText (entry.time, row.withTrimmedRight (12),
                        juce::Justification::centredRight, false);
        }
    }
}

void FriendsPanel::resized()
{
    layoutTabBar();

    const int top = 52;
    searchEditor.setVisible (tab == Tab::Find && ! showNotifications);
    searchEditor.setBounds (8, top, juce::jmin (420, getWidth() - 16), 30);

    viewport.setBounds (8, top + (searchEditor.isVisible() ? 40 : 0),
                        getWidth() - 16,
                        juce::jmax (120, getHeight() - top - 16
                                             - (searchEditor.isVisible() ? 40 : 0)));
    listContent.setSize (juce::jmax (320, viewport.getMaximumVisibleWidth()),
                         juce::jmax (viewport.getHeight(),
                                     static_cast<int> (cards.size()) * 164));
}

void FriendsPanel::mouseMove (const juce::MouseEvent& event)
{
    int hovered = -1;

    for (int i = 0; i < 5; ++i)
        if (tabBounds[(size_t) i].contains (event.getPosition()))
            hovered = i;

    if (hovered != hoveredTab)
    {
        hoveredTab = hovered;
        repaint();
    }
}

void FriendsPanel::mouseExit (const juce::MouseEvent&)
{
    if (hoveredTab != -1)
    {
        hoveredTab = -1;
        repaint();
    }
}

void FriendsPanel::mouseUp (const juce::MouseEvent& event)
{
    const auto position = event.getPosition();

    for (int i = 0; i < 5; ++i)
    {
        if (! tabBounds[(size_t) i].contains (position))
            continue;

        if (i == 4)
        {
            showNotifications = ! showNotifications;

            if (showNotifications)
            {
                loadNotifications();
                // Everything visible in the panel counts as seen.
                markAllRead();
            }

            resized();
            repaint();
            return;
        }

        setTab (static_cast<Tab> (i));
        return;
    }

    // Notification rows open the player's profile.
    if (showNotifications || tab == Tab::Activity)
    {
        const auto area = getLocalBounds().reduced (8, 0).withTrimmedTop (52);
        const int rowH = 40;
        const int index = (position.y - area.getY()) / rowH;

        if (position.y >= area.getY()
            && juce::isPositiveAndBelow (index, static_cast<int> (notifications.size())))
        {
            const auto& entry = notifications[static_cast<size_t> (index)];

            if (entry.userId.isNotEmpty() && onOpenProfile != nullptr)
                onOpenProfile (entry.userId);
        }
    }
}
