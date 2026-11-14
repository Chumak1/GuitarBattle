#pragma once

#include <JuceHeader.h>

#include "PlayerCard.h"

#include <functional>
#include <memory>
#include <vector>

class ApiClient;

/*
    Friends section: friends, incoming/outgoing requests, player search, the
    friends' activity feed and the social notifications list.

    Every row is the universal PlayerCardComponent, so a friend looks exactly
    like a leaderboard player and a search result. All data comes from the
    /api/v1/social endpoints; nothing is stored only on the client.
*/
class FriendsPanel final : public juce::Component,
                           private juce::Timer
{
public:
    enum class Tab { Friends, Requests, Find, Activity };

    FriendsPanel (ApiClient& apiToUse, juce::String bearerToken);
    ~FriendsPanel() override;

    std::function<void (const juce::String& userId)> onOpenProfile;
    /** Notifies the shell so the sidebar bell badge stays in sync. */
    std::function<void (int unread)> onUnreadChanged;

    void refreshAll();
    void setTab (Tab newTab);
    Tab getTab() const { return tab; }

    /** Opens the notifications list (sidebar bell). */
    void openNotificationsView();

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;

private:
    struct Player
    {
        juce::String id, username, displayName, presence, relationship;
        int level = 1;
        int rating = 0;
    };

    enum class RowAction
    {
        None,
        AddFriend,
        CancelRequest,
        AcceptRequest,
        DeclineRequest,
        RemoveFriend,
        BlockUser
    };

    struct Row
    {
        Player player;
        RowAction action = RowAction::None;
        juce::String requestId;
    };

    void timerCallback() override;
    void loadFriends();
    void loadRequests();
    void loadSearch();
    void loadActivity();
    void loadNotifications();
    void rebuildCards();
    void postAction (const Row& row, PlayerCardComponent::Action picked);
    void markAllRead();
    void layoutTabBar();
    void paintTabBar (juce::Graphics& g);
    void paintNotifications (juce::Graphics& g);
    static Player parsePlayer (const juce::var& object);

    ApiClient& api;
    juce::String token;

    Tab tab = Tab::Friends;
    bool showNotifications = false;
    bool searchPending = false;
    juce::String searchQuery;
    double lastSearchMs = 0.0;

    std::vector<Row> rows;
    std::vector<std::unique_ptr<PlayerCardComponent>> cards;

    struct Notification
    {
        juce::String kind;
        juce::String text;
        juce::String time;
        juce::String userId;
        bool read = false;
    };

    std::vector<Notification> notifications;
    int unreadCount = 0;

    juce::TextEditor searchEditor;
    juce::Viewport viewport;
    juce::Component listContent;
    juce::TextButton notificationsButton { "NOTIFICATIONS" };

    std::array<juce::Rectangle<int>, 5> tabBounds {};
    int hoveredTab = -1;
    juce::String statusLine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FriendsPanel)
};
