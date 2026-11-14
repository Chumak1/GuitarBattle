#pragma once

#include <JuceHeader.h>

#include <functional>

class ApiClient;

/*
    Full public profile of another player, opened from any player card (friends,
    requests, search, leaderboards, duel results).

    Everything comes from the social endpoints: /social/players/{id} returns the
    card (level, rating, character, presence, relationship, stats) and the
    relationship-aware actions post to /social/friends, /social/requests and
    /social/blocks. /duels/me supplies the signed-in player's numbers for the
    Compare Stats view.
*/
class PublicProfilePanel final : public juce::Component
{
public:
    PublicProfilePanel (ApiClient& apiToUse, juce::String bearerToken);
    ~PublicProfilePanel() override;

    /** Back to the section the profile was opened from. */
    std::function<void()> onBack;

    /** Challenge: duels are lobby based, so this opens the Online section. */
    std::function<void (const juce::String& userId)> onChallenge;

    /** Opens the direct-message thread with this player. */
    std::function<void (const juce::String& userId, const juce::String& name)> onMessage;

    /** "Edit my profile" for the Self relation. */
    std::function<void()> onEditOwnProfile;

    /** Loads (or reloads) the profile of @p userId. */
    void open (const juce::String& userId);

    const juce::String& getUserId() const noexcept { return data.userId; }

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    struct Player
    {
        juce::String userId;
        juce::String username;
        juce::String displayName;
        juce::String characterName;
        juce::String characterSheetUrl;
        juce::String presence = "offline";
        juce::String relationship = "none";

        int level = 1;
        int xpTotal = 0;
        int rating = 1000;
        int peakRating = 1000;

        // Guitar Skills: the competitive metric shown first. -1 means the
        // server did not send it (older build), so the panel falls back to the
        // duel rating.
        double guitarSkills = -1.0;
        int guitarSkillsRank = 0;
        bool provisional = false;
        //: Mean accuracy of the counted Guitar Skills window, in percent. Used
        //: when the social card has no best-accuracy value yet.
        double recentAccuracy = -1.0;

        // Per-component detail from /leaderboard/global/{id}. Loaded in a
        // second request so the card appears immediately.
        struct SkillComponent
        {
            double value = -1.0;
            int events = 0;
            double confidence = 0.0;
        };

        SkillComponent accuracySkill;
        SkillComponent challengeSkill;
        SkillComponent scoreSkill;
        SkillComponent duelSkill;
        SkillComponent progressionSkill;
        double skillsConfidence = 0.0;
        bool skillsDetailLoaded = false;

        bool statsVisible = false;
        bool canChallenge = true;

        int duels = 0;
        int wins = 0;
        int losses = 0;
        int draws = 0;
        double winRate = 0.0;
        double bestAccuracy = 0.0;
        int songsMastered = 0;
        int achievementsUnlocked = 0;
        int achievementsTotal = 0;
        int towersCompleted = 0;

        juce::String name() const
        {
            if (displayName.isNotEmpty())
                return displayName;

            return username.isNotEmpty() ? username : juce::String ("PLAYER");
        }
    };

    enum class PrimaryAction
    {
        None,
        AddFriend,
        CancelRequest,
        AcceptRequest,
        RemoveFriend,
        Unblock,
        EditProfile
    };

    struct Layout
    {
        juce::Rectangle<int> header;
        juce::Rectangle<int> character;
        juce::Rectangle<int> info;
        juce::Rectangle<int> stats;
        juce::Rectangle<int> progress;
    };

    Layout computeLayout() const;

    void loadCard();
    void loadSkillsDetail();
    void loadPendingRequest();
    void loadOwnStats();
    void loadCharacterImage (const juce::String& relativeUrl);
    void rebuildActions();
    void performPrimary();
    void post (const juce::String& path, bool httpDelete, const juce::String& statusText);
    void setStatus (const juce::String& text);

    void paintHeader (juce::Graphics& g, juce::Rectangle<int> area);
    void paintCharacter (juce::Graphics& g, juce::Rectangle<int> area);
    void paintInfo (juce::Graphics& g, juce::Rectangle<int> area);
    void paintStats (juce::Graphics& g, juce::Rectangle<int> area);
    void paintProgress (juce::Graphics& g, juce::Rectangle<int> area);

    void styleSecondaryButton (juce::TextButton& button);
    void stylePrimaryButton (juce::TextButton& button);

    ApiClient& api;
    juce::String token;

    Player data;
    Player self;

    bool cardLoaded = false;
    bool compareVisible = false;
    bool comparePending = false;
    juce::String pendingRequestId;
    juce::String statusLine;
    int imageGeneration = 0;
    int skillsRequestId = 0;

    juce::Image characterSheet;

    juce::TextButton backButton { "BACK" };
    juce::TextButton primaryButton;
    juce::TextButton messageButton { "MESSAGE" };
    juce::TextButton declineButton { "DECLINE" };
    juce::TextButton challengeButton { "CHALLENGE" };
    juce::TextButton compareButton { "COMPARE STATS" };
    juce::TextButton blockButton { "BLOCK USER" };

    PrimaryAction primaryAction = PrimaryAction::None;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PublicProfilePanel)
};
