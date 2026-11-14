#pragma once

#include <JuceHeader.h>

#include <functional>
#include <vector>

/*
    Universal FretFight player card.

    One component renders any player in any screen: friends, search results,
    requests, lobby members, leaderboards, duel results. Screens pick a Mode and
    supply the same PlayerCardData, so a player looks the same everywhere and no
    screen grows its own private card.

    The data comes from the public server sources:
      * GET /duels/players/{id}  - level, rating, duel totals, recent matches
      * GET /profile/users/{id}  - achievements, songs mastered
    A card with unknown fields shows "-" instead of a fake zero.
*/

struct PlayerCardData
{
    enum class Presence { Unknown, Offline, Online, InLobby, InMatch };

    juce::String userId;
    juce::String username;
    juce::String displayName;

    int level = 0;
    int xpRemaining = -1;     // -1 = unknown
    int xpForNext = -1;       // -1 = unknown
    int rating = 0;           // 0 = unranked
    int peakRating = 0;

    int duelsOnline = -1;
    int wins = -1;
    int losses = -1;
    double winRate = -1.0;
    double accuracy = -1.0;

    int songsMastered = -1;
    int achievementsUnlocked = -1;
    int achievementsTotal = -1;
    int towersCompleted = -1;
    int bestScore = -1;

    juce::String characterName;
    juce::String characterSheetUrl;
    juce::Image characterSheet;

    Presence presence = Presence::Unknown;

    bool valid = false;

    juce::String name() const
    {
        if (displayName.isNotEmpty())
            return displayName;

        return username.isNotEmpty() ? username : juce::String ("Player");
    }
};

enum class PlayerRelation
{
    None,             // no relationship: Add Friend
    Self,             // this is the signed-in player
    Friend,           // Friends / Challenge / Compare
    IncomingRequest,  // Accept / Decline
    OutgoingRequest,  // Request sent / Cancel
    Blocked           // no interaction
};

class PlayerCardComponent final : public juce::Component
{
public:
    enum class Mode { Compact, Standard, Expanded, Preview };

    enum class Action
    {
        None,
        ViewProfile,
        AddFriend,
        CancelRequest,
        AcceptRequest,
        DeclineRequest,
        RemoveFriend,
        Challenge,
        Compare,
        Block,
        EditProfile
    };

    PlayerCardComponent();

    void setMode (Mode newMode);
    void setData (const PlayerCardData& newData);
    void setRelation (PlayerRelation newRelation);
    void setChallengeEnabled (bool enabled);
    void setActionHandler (std::function<void (Action)> handler);

    static int preferredHeight (Mode mode);

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;

private:
    struct ActionChip
    {
        Action action = Action::None;
        juce::String label;
        bool primary = false;
        juce::Rectangle<int> bounds;
    };

    std::vector<ActionChip> buildActions() const;
    void layoutActionChips (std::vector<ActionChip>& chips,
                            juce::Rectangle<int> area) const;
    void paintAvatar (juce::Graphics& g, juce::Rectangle<float> area) const;
    void paintChip (juce::Graphics& g, const ActionChip& chip) const;
    void paintLevel (juce::Graphics& g, juce::Rectangle<float> area) const;
    void paintRating (juce::Graphics& g, juce::Rectangle<float> area) const;
    void paintStatus (juce::Graphics& g, juce::Rectangle<float> area) const;
    void paintStat (juce::Graphics& g, juce::Rectangle<float> area,
                    const juce::String& caption, const juce::String& value) const;

    Mode mode = Mode::Compact;
    PlayerCardData data;
    PlayerRelation relation = PlayerRelation::None;
    bool challengeEnabled = false;
    std::function<void (Action)> onAction;
    std::vector<ActionChip> chips;
    int hoveredChip = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlayerCardComponent)
};
