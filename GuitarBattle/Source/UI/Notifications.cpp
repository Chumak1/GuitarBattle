#include "Notifications.h"

#include "GameStyle.h"
#include "../Network/ApiClient.h"

#include <algorithm>

//==============================================================================
// Wording shared by the toasts and the history panel.
//==============================================================================

namespace NotificationText
{
    namespace
    {
        juce::String humanKind (const juce::String& kind)
        {
            return kind.replaceCharacter ('_', ' ').toUpperCase();
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
    }

    juce::String describe (const juce::String& kind, const juce::var& payload)
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

    juce::String kindLabel (const juce::String& kind)
    {
        if (kind == "friend_request")   return "FRIEND REQUEST";
        if (kind == "friend_accepted")  return "FRIEND ADDED";

        if (kind.startsWith ("activity_"))
        {
            const auto inner = kind.substring (9);

            if (inner == "record")       return "NEW RECORD";
            if (inner == "level_up")     return "LEVEL UP";
            if (inner == "achievement")  return "ACHIEVEMENT";
            if (inner == "tower")        return "BATTLE TOWER";
            if (inner == "song")         return "NEW SONG";

            return humanKind (inner);
        }

        return humanKind (kind);
    }

    juce::Colour kindColour (const juce::String& kind)
    {
        if (kind == "friend_request")
            return UiColors::accent;

        if (kind == "friend_accepted")
            return UiColors::teal;

        return UiColors::electric;
    }

    juce::String shortDate (const juce::String& iso)
    {
        return iso.length() >= 16 ? iso.substring (0, 16).replaceCharacter ('T', ' ')
                                  : iso;
    }
}

//==============================================================================
// Toasts
//==============================================================================

NotificationToasts::NotificationToasts()
{
    setInterceptsMouseClicks (true, false);
    setAlwaysOnTop (false);
}

NotificationToasts::~NotificationToasts() = default;

int NotificationToasts::getPreferredHeight() const
{
    if (cards.empty())
        return 0;

    return static_cast<int> (cards.size()) * (cardHeight + cardGap) - cardGap;
}

void NotificationToasts::push (const juce::String& id, const juce::String& kind,
                               const juce::String& text, const juce::String& userId)
{
    if (text.isEmpty())
        return;

    if (id.isNotEmpty())
        for (const auto& card : cards)
            if (card.id == id)
                return;

    // Keep the corner readable: the oldest card makes room for the newest.
    while (static_cast<int> (cards.size()) >= maxCards)
        cards.erase (cards.begin());

    Card card;
    card.id = id;
    card.kind = kind;
    card.text = text;
    card.userId = userId;
    card.lastTickMs = juce::Time::getMillisecondCounterHiRes();
    cards.push_back (std::move (card));

    hoveredCard = -1;

    if (onSizeChanged != nullptr)
        onSizeChanged();

    updateTimer();
    repaint();
}

void NotificationToasts::dismissAll()
{
    if (cards.empty())
        return;

    cards.clear();
    hoveredCard = -1;
    stopTimer();

    if (onSizeChanged != nullptr)
        onSizeChanged();

    repaint();
}

void NotificationToasts::updateTimer()
{
    if (cards.empty())
    {
        stopTimer();
        return;
    }

    if (! isTimerRunning())
        startTimerHz (30);
}

void NotificationToasts::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();

    for (auto& card : cards)
    {
        const double delta = card.lastTickMs > 0.0 ? now - card.lastTickMs : 0.0;
        card.lastTickMs = now;
        card.ageMs += delta;

        if (! card.hovered)
            card.remainingMs -= delta;
    }

    const auto previousCount = cards.size();

    cards.erase (std::remove_if (cards.begin(), cards.end(),
                                 [] (const Card& card)
                                 {
                                     return card.remainingMs <= 0.0;
                                 }),
                 cards.end());

    if (cards.size() != previousCount)
    {
        hoveredCard = -1;

        if (onSizeChanged != nullptr)
            onSizeChanged();
    }

    // Fade-in and fade-out are the only animated parts; a card that is fully
    // visible does not force 30 fps repaints of the window.
    for (const auto& card : cards)
    {
        if (card.ageMs < 320.0 || card.remainingMs < 700.0)
        {
            repaint();
            break;
        }
    }

    updateTimer();
}

juce::Rectangle<float> NotificationToasts::cardBounds (int index) const
{
    const int count = static_cast<int> (cards.size());
    const int fromBottom = count - 1 - index;
    const float rowH = static_cast<float> (cardHeight + cardGap);

    return { 0.0f,
             static_cast<float> (getHeight()) - static_cast<float> (fromBottom + 1) * rowH
                 + static_cast<float> (cardGap),
             static_cast<float> (getWidth()),
             static_cast<float> (cardHeight) };
}

int NotificationToasts::cardAt (juce::Point<int> position) const
{
    for (int i = 0; i < static_cast<int> (cards.size()); ++i)
        if (cardBounds (i).contains (position.toFloat()))
            return i;

    return -1;
}

void NotificationToasts::paint (juce::Graphics& g)
{
    for (int i = 0; i < static_cast<int> (cards.size()); ++i)
    {
        const auto& card = cards[static_cast<size_t> (i)];
        const auto area = cardBounds (i);

        if (area.isEmpty())
            continue;

        float alpha = juce::jlimit (0.0f, 1.0f,
                                    static_cast<float> (card.ageMs / 260.0));

        if (card.remainingMs < 700.0)
            alpha = juce::jmin (alpha,
                                juce::jlimit (0.0f, 1.0f,
                                              static_cast<float> (card.remainingMs / 700.0)));

        if (alpha <= 0.0f)
            continue;

        juce::Graphics::ScopedSaveState save (g);
        g.setOpacity (alpha);

        GameUi::drawCard (g, area, 14.0f, card.hovered);

        // Kind stripe on the left edge: gold for requests, teal for accepted
        // friendships, blue for activity from friends.
        g.setColour (NotificationText::kindColour (card.kind));
        g.fillRoundedRectangle (area.withWidth (4.0f).reduced (0.0f, 10.0f), 2.0f);

        auto text = area.reduced (18.0f, 11.0f).withTrimmedLeft (4.0f);

        g.setColour (NotificationText::kindColour (card.kind));
        g.setFont (GameUi::eyebrow (8.5f));
        g.drawText (NotificationText::kindLabel (card.kind),
                    text.removeFromTop (13.0f).toNearestInt(),
                    juce::Justification::centredLeft, false);

        g.setColour (UiColors::text);
        g.setFont (GameUi::font (11.5f));
        g.drawFittedText (card.text, text.toNearestInt(),
                          juce::Justification::topLeft, 2, 0.9f);
    }
}

void NotificationToasts::mouseMove (const juce::MouseEvent& event)
{
    const int index = cardAt (event.getPosition());

    if (index == hoveredCard)
        return;

    hoveredCard = index;

    for (int i = 0; i < static_cast<int> (cards.size()); ++i)
        cards[static_cast<size_t> (i)].hovered = (i == index);

    setMouseCursor (index >= 0 && cards[static_cast<size_t> (index)].userId.isNotEmpty()
                        ? juce::MouseCursor::PointingHandCursor
                        : juce::MouseCursor::NormalCursor);

    repaint();
}

void NotificationToasts::mouseExit (const juce::MouseEvent&)
{
    if (hoveredCard < 0)
        return;

    hoveredCard = -1;

    for (auto& card : cards)
        card.hovered = false;

    setMouseCursor (juce::MouseCursor::NormalCursor);
    repaint();
}

void NotificationToasts::mouseUp (const juce::MouseEvent& event)
{
    const int index = cardAt (event.getPosition());

    if (index < 0)
        return;

    const auto card = cards[static_cast<size_t> (index)];

    cards.erase (cards.begin() + index);
    hoveredCard = -1;

    if (onSizeChanged != nullptr)
        onSizeChanged();

    updateTimer();
    repaint();

    if (card.userId.isNotEmpty() && onOpenProfile != nullptr)
        onOpenProfile (card.userId);
}

//==============================================================================
// History panel
//==============================================================================

NotificationHistoryPanel::NotificationHistoryPanel (ApiClient& apiToUse,
                                                    juce::String bearerToken)
    : api (apiToUse), token (std::move (bearerToken))
{
    list.setRowHeight (54);
    list.setOutlineThickness (0);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (list);

    closeButton.setColour (juce::TextButton::buttonColourId, UiColors::surface2);
    closeButton.setColour (juce::TextButton::textColourOffId, UiColors::muted);
    closeButton.setColour (juce::TextButton::textColourOnId, UiColors::text);
    closeButton.onClick = [this]
    {
        if (onClose != nullptr)
            onClose();
    };
    addAndMakeVisible (closeButton);

    refresh();
}

NotificationHistoryPanel::~NotificationHistoryPanel() = default;

void NotificationHistoryPanel::refresh()
{
    loading = true;
    repaint();

    juce::Component::SafePointer<NotificationHistoryPanel> safeThis (this);

    api.getJson ("/social/notifications?limit=20", token, 6000,
        [safeThis] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->loading = false;
            self->entries.clear();

            if (reply.transportOk && reply.json.isObject())
            {
                self->unreadCount =
                    static_cast<int> (reply.json.getProperty ("unread", 0));

                if (const auto* items =
                        reply.json.getProperty ("items", juce::var()).getArray())
                {
                    for (const auto& item : *items)
                    {
                        Entry entry;
                        const auto payload = item.getProperty ("payload", juce::var());
                        entry.id = item.getProperty ("id", juce::var()).toString();
                        entry.kind = item.getProperty ("kind", juce::var()).toString();
                        entry.text = NotificationText::describe (entry.kind, payload);
                        entry.time = NotificationText::shortDate (
                            item.getProperty ("created_at", juce::var()).toString());
                        entry.userId =
                            payload.getProperty ("user_id", juce::var()).toString();
                        entry.read = ! item.getProperty ("read_at", juce::var()).isVoid();
                        self->entries.push_back (std::move (entry));
                    }
                }
            }

            self->list.updateContent();
            self->list.repaint();
            self->repaint();

            // Opening the list is the moment the player sees the items: mark
            // them so the sidebar badge clears.
            self->markAllRead();
        });
}

void NotificationHistoryPanel::markAllRead()
{
    if (unreadCount <= 0)
        return;

    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty ("all", true);

    juce::Component::SafePointer<NotificationHistoryPanel> safeThis (this);

    api.postJson ("/social/notifications/read", juce::var (body.get()), token, 5000,
        [safeThis] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr || ! reply.transportOk)
                return;

            self->unreadCount = 0;

            for (auto& entry : self->entries)
                entry.read = true;

            if (self->onUnreadChanged != nullptr)
                self->onUnreadChanged (0);

            self->list.repaint();
            self->repaint();
        });
}

int NotificationHistoryPanel::getNumRows()
{
    return static_cast<int> (entries.size());
}

void NotificationHistoryPanel::paintListBoxItem (int row, juce::Graphics& g,
                                                 int width, int height,
                                                 bool rowIsSelected)
{
    juce::ignoreUnused (rowIsSelected);

    if (! juce::isPositiveAndBelow (row, static_cast<int> (entries.size())))
        return;

    const auto& entry = entries[static_cast<size_t> (row)];

    auto area = juce::Rectangle<float> (0.0f, 0.0f,
                                        static_cast<float> (width),
                                        static_cast<float> (height))
                    .reduced (2.0f, 2.0f);

    g.setColour (UiColors::surface2.withAlpha (0.55f));
    g.fillRoundedRectangle (area, 10.0f);

    g.setColour (NotificationText::kindColour (entry.kind)
                     .withAlpha (entry.read ? 0.30f : 1.0f));
    g.fillRoundedRectangle (area.withWidth (3.0f).reduced (0.0f, 8.0f), 1.5f);

    auto text = area.reduced (16.0f, 7.0f).withTrimmedLeft (2.0f);

    g.setColour (UiColors::muted);
    g.setFont (GameUi::eyebrow (8.0f));
    g.drawText (NotificationText::kindLabel (entry.kind),
                text.removeFromTop (12.0f).toNearestInt(),
                juce::Justification::centredLeft, false);

    g.setColour (entry.read ? UiColors::secondaryText : UiColors::text);
    g.setFont (GameUi::font (11.0f));
    g.drawFittedText (entry.text,
                      text.withTrimmedRight (78.0f).toNearestInt(),
                      juce::Justification::centredLeft, 2, 0.9f);

    if (entry.time.isNotEmpty())
    {
        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (9.0f));
        g.drawText (entry.time, area.withTrimmedRight (12.0f).toNearestInt(),
                    juce::Justification::centredRight, false);
    }
}

void NotificationHistoryPanel::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    if (! juce::isPositiveAndBelow (row, static_cast<int> (entries.size())))
        return;

    const auto& entry = entries[static_cast<size_t> (row)];

    if (entry.userId.isEmpty())
        return;

    if (onOpenProfile != nullptr)
        onOpenProfile (entry.userId);

    if (onClose != nullptr)
        onClose();
}

void NotificationHistoryPanel::paint (juce::Graphics& g)
{
    GameUi::drawCard (g, getLocalBounds().toFloat(), 16.0f, true);

    auto header = getLocalBounds().reduced (18, 14).removeFromTop (46);

    g.setColour (UiColors::accent);
    g.setFont (GameUi::eyebrow (9.5f));
    g.drawText ("NOTIFICATIONS", header.removeFromTop (16),
                juce::Justification::centredLeft, false);

    const auto summary = loading
                             ? juce::String ("Loading...")
                             : (entries.empty()
                                    ? juce::String ("Nothing yet")
                                    : juce::String (static_cast<int> (entries.size()))
                                          + " latest events");

    g.setColour (UiColors::muted);
    g.setFont (GameUi::font (10.0f));
    g.drawText (summary, header, juce::Justification::centredLeft, false);

    g.setColour (UiColors::border.withAlpha (0.6f));
    g.fillRect (18, 68, getWidth() - 36, 1);

    if (! loading && entries.empty())
    {
        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (12.0f));
        g.drawText ("No notifications yet.",
                    getLocalBounds().withTrimmedTop (120),
                    juce::Justification::centredTop, false);
    }
}

void NotificationHistoryPanel::resized()
{
    closeButton.setBounds (getWidth() - 18 - 64, 16, 64, 24);
    list.setBounds (10, 74, getWidth() - 20, juce::jmax (40, getHeight() - 84));
}

//==============================================================================
// Popup shell
//==============================================================================

NotificationPopup::NotificationPopup (ApiClient& apiToUse, juce::String bearerToken,
                                      juce::Rectangle<int> anchorInParent)
    : panel (apiToUse, std::move (bearerToken)), anchor (anchorInParent)
{
    setWantsKeyboardFocus (true);

    panel.onClose = [this]
    {
        dismiss();
    };
    panel.onOpenProfile = [this] (const juce::String& userId)
    {
        if (onOpenProfile != nullptr)
            onOpenProfile (userId);
    };
    panel.onUnreadChanged = [this] (int unread)
    {
        if (onUnreadChanged != nullptr)
            onUnreadChanged (unread);
    };

    addAndMakeVisible (panel);
}

void NotificationPopup::paint (juce::Graphics& g)
{
    // A soft scrim separates the small window from the screen behind it while
    // keeping the current section readable.
    g.fillAll (UiColors::background.withAlpha (0.42f));
}

void NotificationPopup::resized()
{
    const int width = juce::jmin (380, juce::jmax (280, getWidth() - 48));
    const int height = juce::jmin (470, juce::jmax (260, getHeight() - 64));

    int x = anchor.getRight() + 14;

    if (x + width > getWidth() - 16)
        x = juce::jmax (16, getWidth() - width - 16);

    int y = anchor.getCentreY() - height / 2;
    y = juce::jlimit (16, juce::jmax (16, getHeight() - height - 16), y);

    panel.setBounds (x, y, width, height);
    panelBounds = panel.getBounds();
}

void NotificationPopup::mouseDown (const juce::MouseEvent& event)
{
    if (! panelBounds.contains (event.getPosition()))
        dismiss();
}

bool NotificationPopup::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        dismiss();
        return true;
    }

    return false;
}

void NotificationPopup::dismiss()
{
    // The owner deletes this component; never do that inside its own event.
    if (onDismiss == nullptr)
        return;

    const auto callback = onDismiss;
    onDismiss = nullptr;

    juce::MessageManager::callAsync ([callback]
    {
        callback();
    });
}
