#include "FriendsPanel.h"

#include "PlayerCard.h"
#include "GameStyle.h"
#include "Notifications.h"
#include "../Network/ApiClient.h"

#include <algorithm>

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
    if (tab == newTab)
        return;

    tab = newTab;
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
    if (tab == Tab::Friends)      loadFriends();
    else if (tab == Tab::Requests) loadRequests();
    else if (tab == Tab::Find)     loadSearch();
    else                           loadActivity();
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

            self->activityEntries.clear();

            if (const auto* items = reply.json.getProperty ("items", juce::var()).getArray())
                for (const auto& item : *items)
                {
                    ActivityEntry entry;
                    const auto payload = item.getProperty ("payload", juce::var());
                    entry.kind = item.getProperty ("kind", juce::var()).toString();
                    entry.text = NotificationText::describe (entry.kind, payload);
                    entry.time = NotificationText::shortDate (
                        item.getProperty ("created_at", juce::var()).toString());
                    entry.userId = payload.getProperty ("user_id", juce::var()).toString();
                    self->activityEntries.push_back (std::move (entry));
                }

            self->statusLine = self->activityEntries.empty()
                                   ? "No recent activity from your friends."
                                   : juce::String (static_cast<int> (self->activityEntries.size()))
                                         + " events";
            self->rows.clear();
            self->rebuildCards();
        });
}

void FriendsPanel::rebuildCards()
{
    cards.clear();

    if (tab == Tab::Activity)
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

void FriendsPanel::layoutTabBar()
{
    const int width = getWidth();
    const int startY = 8;
    const int tabW = juce::jmax (110, (width - 40) / 4);

    for (int i = 0; i < 4; ++i)
        tabBounds[(size_t) i] = { 8 + i * (tabW + 8), startY, tabW, 30 };
}

void FriendsPanel::paint (juce::Graphics& g)
{
    g.fillAll (UiColors::background);

    paintTabBar (g);

    if (tab == Tab::Activity)
    {
        paintActivity (g);
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
        const bool active = static_cast<int> (tab) == i;
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
}

void FriendsPanel::paintActivity (juce::Graphics& g)
{
    auto area = getLocalBounds().reduced (8, 0).withTrimmedTop (52);
    const int rowH = 40;

    if (activityEntries.empty())
    {
        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (12.0f));
        g.drawText ("No recent activity.",
                    area, juce::Justification::topLeft, false);
        return;
    }

    g.setFont (GameUi::font (11.5f));

    for (int i = 0; i < static_cast<int> (activityEntries.size()); ++i)
    {
        const auto& entry = activityEntries[static_cast<size_t> (i)];
        const juce::Rectangle<int> row (area.getX(), area.getY() + i * rowH,
                                        area.getWidth(), rowH - 4);

        if (row.getBottom() > getHeight())
            break;

        g.setColour (UiColors::surface2.withAlpha (0.55f));
        g.fillRoundedRectangle (row.toFloat(), 8.0f);

        g.setColour (NotificationText::kindColour (entry.kind));
        g.fillRoundedRectangle (row.toFloat().withWidth (3.0f).reduced (0.0f, 8.0f),
                                1.5f);

        g.setColour (UiColors::text);
        g.setFont (GameUi::font (11.5f));
        g.drawText (entry.text, row.withTrimmedLeft (20).withTrimmedRight (130),
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
    searchEditor.setVisible (tab == Tab::Find);
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

    for (int i = 0; i < 4; ++i)
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

    for (int i = 0; i < 4; ++i)
    {
        if (! tabBounds[(size_t) i].contains (position))
            continue;

        setTab (static_cast<Tab> (i));
        return;
    }

    // Activity rows open the player's profile.
    if (tab == Tab::Activity)
    {
        const auto area = getLocalBounds().reduced (8, 0).withTrimmedTop (52);
        const int rowH = 40;
        const int index = (position.y - area.getY()) / rowH;

        if (position.y >= area.getY()
            && juce::isPositiveAndBelow (index, static_cast<int> (activityEntries.size())))
        {
            const auto& entry = activityEntries[static_cast<size_t> (index)];

            if (entry.userId.isNotEmpty() && onOpenProfile != nullptr)
                onOpenProfile (entry.userId);
        }
    }
}
