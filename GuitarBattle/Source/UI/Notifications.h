#pragma once

#include <JuceHeader.h>

#include <functional>
#include <vector>

class ApiClient;

// Shared wording for the server notification kinds: friend_request,
// friend_accepted and activity_<kind>. Both the corner toasts and the history
// panel read the same text, so a pop-up and the list never disagree.
namespace NotificationText
{
    juce::String describe (const juce::String& kind, const juce::var& payload);
    juce::String kindLabel (const juce::String& kind);
    juce::Colour kindColour (const juce::String& kind);
    juce::String shortDate (const juce::String& iso);
}

// Corner stack of pop-up cards: the notification "window" of the client. A card
// appears for a few seconds, pauses while the mouse is over it and opens the
// player who caused it on click. The component sizes itself to the number of
// visible cards, so the rest of the window stays clickable.
class NotificationToasts final : public juce::Component,
                                 private juce::Timer
{
public:
    NotificationToasts();
    ~NotificationToasts() override;

    std::function<void (const juce::String& userId)> onOpenProfile;
    // The number of cards changed: the shell repositions the stack.
    std::function<void()> onSizeChanged;

    // Shows a card unless the same notification id is already on screen.
    void push (const juce::String& id, const juce::String& kind,
               const juce::String& text, const juce::String& userId);

    void dismissAll();

    // Height needed for the current cards; 0 when the stack is empty.
    int getPreferredHeight() const;

    void paint (juce::Graphics& g) override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;

private:
    struct Card
    {
        juce::String id, kind, text, userId;
        double remainingMs = 6500.0;
        double ageMs = 0.0;
        double lastTickMs = 0.0;
        bool hovered = false;
    };

    void timerCallback() override;
    void updateTimer();
    int cardAt (juce::Point<int> position) const;
    juce::Rectangle<float> cardBounds (int index) const;

    std::vector<Card> cards;
    int hoveredCard = -1;

    static constexpr int cardHeight = 68;
    static constexpr int cardGap = 10;
    static constexpr int maxCards = 3;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NotificationToasts)
};

// Small history panel: the latest server notifications with the unread ones
// highlighted. Opening it marks everything read; a row opens the player card.
class NotificationHistoryPanel final : public juce::Component,
                                       private juce::ListBoxModel
{
public:
    NotificationHistoryPanel (ApiClient& apiToUse, juce::String bearerToken);
    ~NotificationHistoryPanel() override;

    std::function<void (const juce::String& userId)> onOpenProfile;
    std::function<void()> onClose;
    std::function<void (int unread)> onUnreadChanged;

    void refresh();

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    struct Entry
    {
        juce::String id, kind, text, time, userId;
        bool read = false;
    };

    void markAllRead();

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics& g, int width, int height,
                           bool rowIsSelected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent& event) override;

    ApiClient& api;
    juce::String token;
    std::vector<Entry> entries;
    int unreadCount = 0;
    bool loading = true;

    juce::ListBox list { "Notifications", this };
    juce::TextButton closeButton { "CLOSE" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NotificationHistoryPanel)
};

// Transparent full-window layer that hosts the history panel next to the
// sidebar bell. A click outside the panel, Escape or CLOSE dismisses it, so the
// rest of the app keeps working while it is open.
class NotificationPopup final : public juce::Component
{
public:
    NotificationPopup (ApiClient& apiToUse, juce::String bearerToken,
                       juce::Rectangle<int> anchorInParent);

    std::function<void()> onDismiss;
    std::function<void (const juce::String& userId)> onOpenProfile;
    std::function<void (int unread)> onUnreadChanged;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& event) override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    void dismiss();

    NotificationHistoryPanel panel;
    juce::Rectangle<int> anchor;
    juce::Rectangle<int> panelBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NotificationPopup)
};
