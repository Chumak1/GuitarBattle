#pragma once

#include <JuceHeader.h>

#include "UiKit.h"

#include <array>
#include <functional>
#include <vector>

//==============================================================================
// HOME.
//
// Two large mode cards (BATTLE / ONLINE), a compact player strip
// (level + XP, experience, guitar skills, duel wins) and the global
// leaderboard with a top-three podium and the full table.
//
// The component paints the cards and the chrome; the leaderboard table and its
// refresh button stay real components owned by MainComponent and are laid out
// here, so row clicks, sorting and the network refresh keep working unchanged.
//
// All numbers come from the existing account/progression/leaderboard data.
// Nothing is estimated: a value the server has not sent shows as "-".
class HomeView final : public juce::Component
{
public:
    struct Stats
    {
        juce::String username;
        int level = 1;
        int xpIntoLevel = 0;
        int xpForNext = 0;
        int xpRemaining = 0;
        float levelRatio = 0.0f;
        juce::String totalXpText { "-" };
        juce::String skillsText { "-" };
        juce::String duelsText { "-" };
        int ownRank = 0;
    };

    struct TopPlayer
    {
        juce::String userId;
        juce::String name;
        int rank = 0;
        int level = 1;
        juce::String skillsText { "-" };
        int positionChange = 0;
        bool hasPositionChange = false;
    };

    HomeView();
    ~HomeView() override = default;

    std::function<void()> onBattle;
    std::function<void()> onOnline;
    std::function<void()> onRefresh;
    std::function<void (const juce::String& userId)> onOpenProfile;

    // The table and its status/empty label are laid out by this component but
    // remain owned by MainComponent. The refresh action is a PillButton here so
    // it matches the rest of the dashboard.
    void setLeaderboardComponents (juce::TableListBox& table,
                                   juce::Component& statusLabel);

    void setStats (const Stats& newStats);
    void setTopPlayers (std::vector<TopPlayer> newPlayers);

    // Admin artwork for the two mode cards. An invalid image keeps the
    // procedural fallback, so an empty admin slot never breaks the page.
    void setHeroBackgrounds (const juce::Image& battleImage,
                             const juce::Image& duelImage);

    // Resolves a player picture for the podium tiles. Return an invalid image
    // to fall back to the initial letter.
    void setAvatarLookup (std::function<juce::Image (const juce::String& userId)> lookup);

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;

private:
    void paintHero (juce::Graphics& g, juce::Rectangle<float> area, bool online,
                    bool hovered);
    void paintTowerArt (juce::Graphics& g, juce::Rectangle<float> area);
    void paintDuelArt (juce::Graphics& g, juce::Rectangle<float> area);
    void paintStats (juce::Graphics& g);
    void paintBoard (juce::Graphics& g);
    void paintPodiumTile (juce::Graphics& g, int slot);
    void paintOwnStrip (juce::Graphics& g);
    void updateLocalisedText();
    void updatePodium();

    static bool sameStats (const Stats& a, const Stats& b);
    static bool samePlayers (const std::vector<TopPlayer>& a,
                             const std::vector<TopPlayer>& b);

    juce::TableListBox* table = nullptr;
    juce::Component* statusLabel = nullptr;
    Ui::PillButton refreshPill { "REFRESH", Ui::ButtonStyle::secondary };

    Stats stats;
    std::vector<TopPlayer> players;
    juce::Image battleBackground;
    juce::Image duelBackground;
    std::function<juce::Image (const juce::String&)> avatarLookup;

    juce::Rectangle<int> heroBattle;
    juce::Rectangle<int> heroOnline;
    juce::Rectangle<int> statsArea;
    juce::Rectangle<int> boardArea;
    juce::Rectangle<int> refreshArea;
    juce::Rectangle<int> statusArea;
    juce::Rectangle<int> tableArea;
    juce::Rectangle<int> ownArea;
    std::array<juce::Rectangle<int>, 3> podiumTiles {};
    std::array<int, 3> podiumPlayerIndex { -1, -1, -1 };

    int hoveredHero = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HomeView)
};
