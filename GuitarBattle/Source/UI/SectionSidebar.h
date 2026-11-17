#pragma once

#include <JuceHeader.h>

#include <array>
#include <functional>

// Top-level destinations of the FretFight hub. Home is not a side-menu item:
// it is the landing screen with the global leaderboard and is reached from the
// FretFight wordmark at the top of the sidebar.
enum class AppSection
{
    Home,
    Profile,
    Campaign,
    Songs,
    OnlineDuel,
    Friends,
    Messages,
    Tuner,
    Vst,
    Settings
};

// Persistent left navigation for the hub. It only draws and reports clicks;
// MainComponent owns the section state and the actual screens.
class SectionSidebar final : public juce::Component
{
public:
    SectionSidebar();

    std::function<void (AppSection)> onSectionSelected;
    std::function<void()> onNotificationsSelected;

    void setActiveSection (AppSection section);
    void setUser (const juce::String& username, int level);
    void setUnreadNotifications (int count);
    void setUnreadMessages (int count);

    // Bell row in sidebar-local coordinates. The sidebar sits at the window
    // origin, so the shell uses this rectangle to anchor the notification
    // history panel.
    juce::Rectangle<int> getNotificationsBounds() const { return notificationsBounds; }

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;

private:
    struct Item
    {
        AppSection section;
        const char* label;
        juce::Rectangle<int> bounds;
    };

    void select (AppSection section);
    int rowAt (juce::Point<int> position) const;

    std::array<Item, 9> items {{
        { AppSection::Profile,    "PROFILE",     {} },
        { AppSection::Campaign,   "BATTLE",      {} },
        { AppSection::Songs,      "PRACTICE",    {} },
        { AppSection::OnlineDuel, "ONLINE",      {} },
        { AppSection::Friends,    "FRIENDS",     {} },
        { AppSection::Messages,   "MESSAGES",    {} },
        { AppSection::Tuner,      "TUNER",       {} },
        { AppSection::Vst,        "VST",         {} },
        { AppSection::Settings,   "SETTINGS",    {} }
    }};

    juce::Rectangle<int> brandBounds;
    juce::Rectangle<int> notificationsBounds;
    AppSection active = AppSection::Home;
    int hovered = -1;
    juce::String userName;
    int userLevel = 1;
    int unreadNotifications = 0;
    int unreadMessages = 0;
    bool notificationsHovered = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SectionSidebar)
};
