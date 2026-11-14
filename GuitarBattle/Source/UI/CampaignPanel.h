#pragma once

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <vector>

// One stage of an admin-defined campaign ("tower riff"): a song plus the bot
// that plays it. The number of stages is exactly the number of songs the admin
// added to the campaign.
struct CampaignStageView
{
    juce::String opponent;   // character faced on the stage (bot name fallback)
    juce::String song;       // catalogue title
    juce::String slug;       // server song slug
    juce::String serverId;   // server song UUID
    int difficulty = 1;      // song difficulty (published value)
    int botLevel = 1;        // bot difficulty of this stage: 0 easy, 1 normal, 2 hard
};

// A server campaign as the client sees it.
struct CampaignView
{
    juce::String id;
    juce::String name;
    juce::String description;      // admin text, additive on the server
    int botLevel = 1;              // campaign default for stages without their own level
    std::vector<CampaignStageView> stages;

    // Reward for clearing the whole tower (public fields, may be empty).
    juce::String rewardName;
    juce::String rewardCondition;

    // Card artwork uploaded by the admin for this tower; empty = placeholder.
    juce::String coverImageUrl;
    juce::Image cover;

    // Cleared stages of the current run, mirrored from the server. The grid
    // shows every tower's own progress, not only the selected one.
    int bestStage = 0;
};

class CampaignPanel final : public juce::Component
{
public:
    CampaignPanel();
    ~CampaignPanel() override;

    std::function<void (const juce::String& campaignId)> onCampaignSelected;
    std::function<void()> onFight;
    std::function<void()> onReset;

    void setCampaigns (std::vector<CampaignView> newCampaigns,
                       const juce::String& selectedCampaignId,
                       int currentStage);
    void setLoading (bool isLoading);
    void setStatus (const juce::String& text);

    //: Level used for the tower access rule: Easy from 1, Normal from 4,
    //: Hard from 7. Higher levels keep access to the lower tiers.
    void setPlayerLevel (int level);
    int getPlayerLevel() const noexcept { return playerLevel; }

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;

private:
    class StageModel;
    friend class StageModel;

    const CampaignView* selectedCampaign() const;
    int stageCount() const;
    void styleButton (juce::TextButton& button, bool primary);
    void rebuildStageList();
    void openTower (const juce::String& campaignId);
    void closeTower();
    void layoutGrid();
    void layoutDetail();
    int cardColumns() const;
    juce::Rectangle<float> cardBounds (int index) const;
    void paintGrid (juce::Graphics& g);
    void paintDetail (juce::Graphics& g);
    void paintTowerCard (juce::Graphics& g, juce::Rectangle<float> area,
                         const CampaignView& campaign, int currentStageForTower);
    bool isCampaignLocked (const CampaignView& campaign) const;

    juce::TextButton fightButton { "FIGHT" };
    juce::TextButton resetButton { "RESET" };
    juce::TextButton backButton { "< ALL TOWERS" };
    std::unique_ptr<StageModel> stageModel;
    juce::ListBox stageList { "campaign stages", nullptr };

    std::vector<CampaignView> campaigns;
    juce::String selectedId;
    int currentStage = 0;
    int playerLevel = 1;
    bool loading = false;
    bool showingDetail = false;
    int hoveredCard = -1;
    juce::String statusText;

    juce::Rectangle<float> headerArea;
    juce::Rectangle<float> detailBanner;
    juce::Rectangle<float> detailBody;
    juce::Rectangle<float> stagesArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CampaignPanel)
};
