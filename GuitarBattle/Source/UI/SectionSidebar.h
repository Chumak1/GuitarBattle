#pragma once

#include <JuceHeader.h>

#include "Localisation.h"
#include "UiKit.h"

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

// Persistent left navigation for the hub.
//
// The rows are grouped by intent (PLAY / TRAINING / COMMUNITY / TOOLS) so the
// two game modes read as the primary destinations and the tools sit at the
// bottom. Messages, notifications and settings are not here: they live in the
// top bar. The component only draws and reports clicks; MainComponent owns the
// section state and the actual screens.
class SectionSidebar final : public juce::Component
{
public:
    SectionSidebar();

    std::function<void (AppSection)> onSectionSelected;
    std::function<void (I18n::Language)> onLanguageSelected;

    void setActiveSection (AppSection section);
    void setUser (const juce::String& username, int level);
    void setLanguage (I18n::Language language);

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
        Ui::Icon icon;
        juce::Rectangle<int> bounds;
    };

    struct Group
    {
        const char* label;
        int firstItem;
        int itemCount;
        int labelY = 0;
    };

    void select (AppSection section);
    int rowAt (juce::Point<int> position) const;
    void paintItem (juce::Graphics& g, const Item& item, bool active, bool hovered);

    std::array<Item, 6> items {{
        { AppSection::Campaign,   "BATTLE",   Ui::Icon::sword,    {} },
        { AppSection::OnlineDuel, "ONLINE",   Ui::Icon::globe,    {} },
        { AppSection::Songs,      "PRACTICE", Ui::Icon::cap,      {} },
        { AppSection::Friends,    "FRIENDS",  Ui::Icon::users,    {} },
        { AppSection::Tuner,      "TUNER",    Ui::Icon::tuner,    {} },
        { AppSection::Vst,        "VST",      Ui::Icon::waveform, {} }
    }};

    std::array<Group, 4> groups {{
        { "PLAY",      0, 2, 0 },
        { "TRAINING",  2, 1, 0 },
        { "COMMUNITY", 3, 1, 0 },
        { "TOOLS",     4, 2, 0 }
    }};

    juce::Rectangle<int> brandBounds;
    std::array<juce::Rectangle<int>, 2> languageBounds {};
    AppSection active = AppSection::Home;
    I18n::Language language = I18n::Language::english;
    int hovered = -1;
    juce::String userName;
    int userLevel = 1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SectionSidebar)
};
