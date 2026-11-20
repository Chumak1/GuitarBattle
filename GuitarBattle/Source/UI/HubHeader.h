#pragma once

#include <JuceHeader.h>

#include "SectionSidebar.h"
#include "UiKit.h"

#include <functional>

//==============================================================================
// Top bar of the hub.
//
// Left: the name of the open section plus a one-line description.
// Right: messages, notifications, settings, the player chip and EXIT.
//
// Messages, notifications and settings live here (not in the sidebar) so the
// navigation can stay focused on the play modes. The player chip opens a small
// menu with the actions that no longer fit the header: profile, feedback,
// donation, log out and exit.
//
// The component only draws and reports intent; MainComponent owns the screens
// and the account state.
class HubHeader final : public juce::Component
{
public:
    HubHeader();
    ~HubHeader() override = default;

    std::function<void()> onMessages;
    std::function<void()> onNotifications;
    std::function<void()> onSettings;
    std::function<void()> onProfile;
    std::function<void()> onFeedback;
    std::function<void()> onDonation;
    std::function<void()> onLogout;
    std::function<void()> onExit;

    // The chip asks the shell to show the rounded dropdown; the shell owns the
    // overlay so it is never clipped by the header bounds.
    std::function<void (juce::Rectangle<int> anchorInHeader)> onPlayerMenu;

    void setSection (AppSection newSection);
    void setUser (const juce::String& username, int level);

    // Player picture for the chip. An invalid image keeps the initial letter.
    void setAvatarImage (const juce::Image& image);

    void setUnreadNotifications (int count);
    void setUnreadMessages (int count);

    // Bell rectangle in header-local coordinates. The shell adds the header
    // position to anchor the notification history panel.
    juce::Rectangle<int> getNotificationsBounds() const;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;

private:
    juce::String sectionTitle() const;
    juce::String sectionSubtitle() const;
    juce::String sectionEyebrow() const;
    void showPlayerMenu();

    Ui::IconButton mailButton { Ui::Icon::mail, "Messages" };
    Ui::IconButton bellButton { Ui::Icon::bell, "Notifications" };
    Ui::IconButton gearButton { Ui::Icon::gear, "Settings" };
    Ui::PillButton exitButton { "EXIT", Ui::ButtonStyle::quiet };

    juce::Rectangle<int> playerArea;
    int dividerX = 0;

    AppSection section = AppSection::Home;
    juce::String userName;
    juce::Image avatarImage;
    int userLevel = 1;
    bool playerHovered = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HubHeader)
};
