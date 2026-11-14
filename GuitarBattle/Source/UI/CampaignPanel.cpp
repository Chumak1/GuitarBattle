#include "CampaignPanel.h"

#include "GameStyle.h"

#include <algorithm>
#include <cmath>

namespace
{
    juce::String towerStatus (int best, int count)
    {
        if (count <= 0)
            return "NO OPPONENTS";

        if (best >= count)
            return "COMPLETE";

        return best > 0 ? "IN PROGRESS" : "NOT STARTED";
    }

    juce::Colour towerStatusColour (int best, int count)
    {
        if (count > 0 && best >= count)
            return UiColors::green;

        return best > 0 ? UiColors::accent : UiColors::muted;
    }

    // Admin-defined tower difficulty (campaigns.bot_level).
    juce::String towerDifficultyLabel (int botLevel)
    {
        if (botLevel <= 0)
            return "EASY";

        return botLevel >= 2 ? "HARD" : "NORMAL";
    }

    juce::Colour towerDifficultyColour (int botLevel)
    {
        if (botLevel <= 0)
            return UiColors::green;

        return botLevel >= 2 ? UiColors::orange : UiColors::accent;
    }

    // Player level required by a tower: Easy from 1, Normal from 4, Hard from
    // 7. A higher-level player keeps access to the lower tiers.
    int towerRequiredLevel (int botLevel)
    {
        if (botLevel <= 0)
            return 1;

        return botLevel >= 2 ? 7 : 4;
    }
}

class CampaignPanel::StageModel final : public juce::ListBoxModel
{
public:
    explicit StageModel (CampaignPanel& ownerToUse) : owner (ownerToUse) {}

    int getNumRows() override
    {
        const auto* campaign = owner.selectedCampaign();
        return campaign != nullptr
                   ? static_cast<int> (campaign->stages.size())
                   : 0;
    }

    void paintListBoxItem (int rowNumber, juce::Graphics& g,
                           int width, int height, bool) override
    {
        const auto* campaign = owner.selectedCampaign();

        if (campaign == nullptr
            || rowNumber < 0
            || rowNumber >= static_cast<int> (campaign->stages.size()))
            return;

        const auto& stage = campaign->stages[static_cast<size_t> (rowNumber)];
        const bool isCurrent = rowNumber == owner.currentStage;
        const bool isCompleted = rowNumber < owner.currentStage;

        auto row = juce::Rectangle<float> (0.0f, 1.0f,
                                           static_cast<float> (width),
                                           static_cast<float> (height) - 2.0f);

        g.setColour (isCurrent ? UiColors::accent.withAlpha (0.14f)
                               : UiColors::surface.withAlpha (isCompleted ? 0.65f : 0.38f));
        g.fillRoundedRectangle (row, 12.0f);

        if (isCurrent)
        {
            g.setColour (UiColors::accent.withAlpha (0.75f));
            g.drawRoundedRectangle (row.reduced (0.5f), 12.0f, 1.0f);
        }

        auto number = row.removeFromLeft (58.0f).reduced (10.0f, 8.0f);
        g.setColour (isCurrent ? UiColors::accent
                               : (isCompleted ? UiColors::green : UiColors::muted));
        g.setFont (GameUi::heading (20.0f));
        g.drawText (juce::String (rowNumber + 1), number,
                    juce::Justification::centred, false);

        auto text = row.reduced (4.0f, 6.0f);
        g.setColour (isCompleted ? UiColors::text : UiColors::muted);
        g.setFont (GameUi::font (12.0f, true));
        g.drawFittedText (stage.song, text.removeFromTop (18.0f).toNearestInt(),
                          juce::Justification::centredLeft, 1);

        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (10.0f));
        g.drawFittedText (stage.opponent,
                          text.removeFromTop (16.0f).toNearestInt(),
                          juce::Justification::centredLeft, 1);

        const char* status = isCompleted ? "CLEARED"
                                         : (isCurrent ? "CURRENT" : "LOCKED");
        const auto statusColour = isCompleted ? UiColors::green
                                              : (isCurrent ? UiColors::accent
                                                           : UiColors::muted);
        g.setColour (statusColour.withAlpha (0.16f));
        const juce::Rectangle<float> pill (row.getRight() - 108.0f,
                                           row.getY() + 13.0f, 92.0f, 22.0f);
        g.fillRoundedRectangle (pill, 11.0f);
        g.setColour (statusColour);
        g.setFont (GameUi::eyebrow (8.0f));
        g.drawText (status, pill, juce::Justification::centred, false);
    }

private:
    CampaignPanel& owner;
};

CampaignPanel::~CampaignPanel() = default;

CampaignPanel::CampaignPanel()
{
    stageModel = std::make_unique<StageModel> (*this);

    styleButton (fightButton, true);
    styleButton (resetButton, false);
    styleButton (backButton, false);

    fightButton.onClick = [this] { if (onFight != nullptr) onFight(); };
    resetButton.onClick = [this] { if (onReset != nullptr) onReset(); };
    backButton.onClick = [this] { closeTower(); };

    addAndMakeVisible (fightButton);
    addAndMakeVisible (resetButton);
    addAndMakeVisible (backButton);

    stageList.setModel (stageModel.get());
    stageList.setRowHeight (58);
    stageList.setColour (juce::ListBox::backgroundColourId,
                         juce::Colours::transparentBlack);
    stageList.setColour (juce::ListBox::outlineColourId,
                         juce::Colours::transparentBlack);
    stageList.setOutlineThickness (0);
    addAndMakeVisible (stageList);

    setInterceptsMouseClicks (true, true);

    closeTower();
}

void CampaignPanel::styleButton (juce::TextButton& button, bool primary)
{
    button.setColour (juce::TextButton::buttonColourId,
                      primary ? UiColors::accent : UiColors::surface2);
    button.setColour (juce::TextButton::buttonOnColourId,
                      primary ? UiColors::accentStrong : UiColors::accent);
    button.setColour (juce::TextButton::textColourOffId,
                      primary ? UiColors::onAccent : UiColors::text);
    button.setColour (juce::TextButton::textColourOnId, UiColors::onAccent);
    button.setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

const CampaignView* CampaignPanel::selectedCampaign() const
{
    for (const auto& campaign : campaigns)
        if (campaign.id == selectedId)
            return &campaign;

    return nullptr;
}

int CampaignPanel::stageCount() const
{
    const auto* campaign = selectedCampaign();
    return campaign != nullptr ? static_cast<int> (campaign->stages.size()) : 0;
}

void CampaignPanel::setCampaigns (std::vector<CampaignView> newCampaigns,
                                  const juce::String& selectedCampaignId,
                                  int newCurrentStage)
{
    campaigns = std::move (newCampaigns);
    selectedId = selectedCampaignId;
    currentStage = juce::jmax (0, newCurrentStage);

    if (selectedId.isEmpty() && ! campaigns.empty() && showingDetail)
        selectedId = campaigns.front().id;

    if (showingDetail && selectedCampaign() == nullptr)
        showingDetail = false;

    hoveredCard = -1;
    resized();
    repaint();
}

void CampaignPanel::setLoading (bool isLoading)
{
    loading = isLoading;
    fightButton.setEnabled (! loading && stageCount() > 0 && currentStage < stageCount());
    resetButton.setEnabled (! loading && currentStage > 0);
    repaint();
}

void CampaignPanel::setStatus (const juce::String& text)
{
    statusText = text;
    repaint();
}

void CampaignPanel::setPlayerLevel (int level)
{
    const int normalised = juce::jmax (1, level);

    if (normalised == playerLevel)
        return;

    playerLevel = normalised;

    if (showingDetail)
    {
        const auto* campaign = selectedCampaign();

        // The tower may have been opened before the progression poll arrived;
        // never leave an inaccessible tower open with an enabled fight button.
        if (campaign != nullptr && isCampaignLocked (*campaign))
            closeTower();
    }

    repaint();
}

bool CampaignPanel::isCampaignLocked (const CampaignView& campaign) const
{
    return juce::jmax (1, playerLevel) < towerRequiredLevel (campaign.botLevel);
}

void CampaignPanel::openTower (const juce::String& campaignId)
{
    const CampaignView* target = nullptr;

    for (const auto& campaign : campaigns)
        if (campaign.id == campaignId)
        {
            target = &campaign;
            break;
        }

    if (target == nullptr || isCampaignLocked (*target))
        return;

    selectedId = campaignId;
    showingDetail = true;
    currentStage = 0;

    if (const auto* campaign = selectedCampaign())
        currentStage = juce::jlimit (0, static_cast<int> (campaign->stages.size()),
                                     campaign->bestStage);

    if (onCampaignSelected != nullptr)
        onCampaignSelected (campaignId);

    resized();
    repaint();
}

void CampaignPanel::closeTower()
{
    showingDetail = false;
    hoveredCard = -1;

    stageList.setVisible (false);
    fightButton.setVisible (false);
    resetButton.setVisible (false);
    backButton.setVisible (false);

    resized();
    repaint();
}

int CampaignPanel::cardColumns() const
{
    const float width = static_cast<float> (getWidth());

    if (width >= 1080.0f)
        return 3;

    if (width >= 700.0f)
        return 2;

    return 1;
}

juce::Rectangle<float> CampaignPanel::cardBounds (int index) const
{
    const int columns = cardColumns();
    const float gap = 16.0f;
    const float width = static_cast<float> (getWidth());
    const float cardW = (width - gap * static_cast<float> (columns - 1))
                        / static_cast<float> (columns);
    const int totalCards = juce::jmax (1, static_cast<int> (campaigns.size()));
    const int rows = juce::jmax (1, (totalCards + columns - 1) / columns);
    const float top = 66.0f;
    const float available = juce::jmax (160.0f,
                                        static_cast<float> (getHeight()) - top - 8.0f);
    const float maxH = (available - gap * static_cast<float> (rows - 1))
                       / static_cast<float> (rows);
    // Most admin covers are 16:9. The card is slightly taller than that ratio
    // so the artwork gets a real hero area above the information panel.
    const float idealH = cardW / 1.65f;
    const float cardH = juce::jlimit (150.0f, 280.0f,
                                      juce::jmin (idealH, maxH));
    const int column = index % columns;
    const int row = index / columns;

    return { static_cast<float> (column) * (cardW + gap),
             top + static_cast<float> (row) * (cardH + gap),
             cardW, cardH };
}

void CampaignPanel::resized()
{
    if (showingDetail)
        layoutDetail();
    else
        layoutGrid();
}

void CampaignPanel::layoutGrid()
{
    stageList.setVisible (false);
    fightButton.setVisible (false);
    resetButton.setVisible (false);
    backButton.setVisible (false);

    headerArea = { 0.0f, 0.0f, static_cast<float> (getWidth()), 46.0f };
}

void CampaignPanel::layoutDetail()
{
    const float width = static_cast<float> (getWidth());
    const float height = static_cast<float> (getHeight());

    backButton.setVisible (true);
    backButton.setBounds (0, 0, 132, 28);

    detailBanner = { 0.0f, 40.0f, width, 150.0f };
    detailBody = { 0.0f, 200.0f, width, 66.0f };

    const int listTop = 274;
    const int listHeight = juce::jmax (60, static_cast<int> (height) - listTop - 64);
    stagesArea = { 0.0f, static_cast<float> (listTop), width,
                   static_cast<float> (listHeight) };

    stageList.setVisible (true);
    stageList.setBounds (0, listTop, static_cast<int> (width), listHeight);

    const int buttonWidth = juce::jmin (230, (static_cast<int> (width) - 16) / 2);
    const int buttonsY = static_cast<int> (height) - 48;
    fightButton.setVisible (true);
    resetButton.setVisible (true);
    fightButton.setBounds (static_cast<int> (width) - buttonWidth, buttonsY,
                           buttonWidth, 40);
    resetButton.setBounds (static_cast<int> (width) - buttonWidth * 2 - 12,
                           buttonsY, buttonWidth, 40);

    rebuildStageList();
}

void CampaignPanel::rebuildStageList()
{
    const int count = stageCount();
    const bool complete = count > 0 && currentStage >= count;
    const auto* campaign = selectedCampaign();
    const int requiredLevel = campaign != nullptr
                                  ? towerRequiredLevel (campaign->botLevel)
                                  : 1;
    const bool locked = campaign != nullptr
                        && juce::jmax (1, playerLevel) < requiredLevel;

    if (locked)
    {
        fightButton.setButtonText ("REQUIRES LEVEL "
                                   + juce::String (requiredLevel));
        fightButton.setEnabled (false);
        resetButton.setEnabled (false);
        stageList.updateContent();
        stageList.repaint();
        return;
    }

    if (count <= 0)
    {
        fightButton.setButtonText ("NO STAGES");
        fightButton.setEnabled (false);
    }
    else if (complete)
    {
        fightButton.setButtonText ("BATTLE COMPLETE");
        fightButton.setEnabled (false);
    }
    else
    {
        fightButton.setButtonText ("FIGHT STAGE " + juce::String (currentStage + 1));
        fightButton.setEnabled (! loading);
    }

    resetButton.setEnabled (currentStage > 0 && ! loading);

    stageList.updateContent();
    stageList.repaint();
}

void CampaignPanel::paint (juce::Graphics& g)
{
    // The panel manages its own card layout and text, so it must clear stale
    // pixels after a resize; otherwise the previous card positions ghost
    // through the transparent sections between cards.
    g.fillAll (UiColors::background);

    if (showingDetail)
        paintDetail (g);
    else
        paintGrid (g);
}

void CampaignPanel::paintGrid (juce::Graphics& g)
{
    const float width = static_cast<float> (getWidth());

    g.setColour (UiColors::accent);
    g.setFont (GameUi::eyebrow (9.0f));
    g.drawText ("BATTLES", juce::Rectangle<float> (0.0f, 0.0f, width, 12.0f),
                juce::Justification::centredLeft, false);

    g.setColour (UiColors::text);
    g.setFont (GameUi::heading (22.0f));
    g.drawText ("TOWER OF RIFFS",
                juce::Rectangle<float> (0.0f, 12.0f, width, 28.0f),
                juce::Justification::centredLeft, false);

    g.setColour (UiColors::muted);
    g.setFont (GameUi::font (10.5f));
    g.drawText (campaigns.empty() && statusText.isNotEmpty()
                    ? statusText
                    : juce::String ("Pick a tower: every fight inside is a song."),
                juce::Rectangle<float> (0.0f, 42.0f, width, 14.0f),
                juce::Justification::centredLeft, true);

    if (campaigns.empty())
    {
        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (13.0f));
        g.drawText (loading ? "Loading towers..." : "No towers yet.",
                    getLocalBounds().withTrimmedTop (120),
                    juce::Justification::centredTop, false);
        return;
    }

    for (int i = 0; i < static_cast<int> (campaigns.size()); ++i)
    {
        const auto area = cardBounds (i);

        if (! area.intersects (getLocalBounds().toFloat()))
            continue;

        paintTowerCard (g, area, campaigns[static_cast<size_t> (i)],
                        campaigns[static_cast<size_t> (i)].bestStage);
    }
}

void CampaignPanel::paintTowerCard (juce::Graphics& g, juce::Rectangle<float> area,
                                    const CampaignView& campaign,
                                    int bestStageForTower)
{
    const int count = static_cast<int> (campaign.stages.size());
    const bool complete = count > 0 && bestStageForTower >= count;
    const bool started = bestStageForTower > 0 && ! complete;
    const int requiredLevel = towerRequiredLevel (campaign.botLevel);
    const bool locked = juce::jmax (1, playerLevel) < requiredLevel;
    const bool isHovered = hoveredCard >= 0
                           && &campaign
                                  == &campaigns[static_cast<size_t> (hoveredCard)];

    GameUi::drawCard (g, area, 18.0f, isHovered);

    const auto inner = area.reduced (1.0f);
    const auto coverArea = inner.withBottom (inner.getBottom() - 100.0f);

    // Cover image with a dark gradient at the bottom so the text always reads.
    {
        juce::Graphics::ScopedSaveState save (g);
        juce::Path clip;
        clip.addRoundedRectangle (inner, 18.0f);
        g.reduceClipRegion (clip);

        if (campaign.cover.isValid())
        {
            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
            const auto& image = campaign.cover;

            // Normal cover crop, anchored to the top edge: the guitarist's
            // head is near the top of the artwork and must never be cut off.
            const float scale = juce::jmax (coverArea.getWidth() / (float) image.getWidth(),
                                            coverArea.getHeight() / (float) image.getHeight());
            const float w = (float) image.getWidth() * scale;
            const float h = (float) image.getHeight() * scale;

            g.setOpacity (1.0f);
            g.drawImage (image, coverArea.getCentreX() - w * 0.5f,
                         coverArea.getY(), w, h,
                         0, 0, image.getWidth(), image.getHeight());
        }
        else
        {
            juce::ColourGradient fallback (UiColors::surface2.brighter (0.06f),
                                           coverArea.getX(), coverArea.getY(),
                                           UiColors::background, coverArea.getRight(),
                                           coverArea.getBottom(), false);
            g.setGradientFill (fallback);
            g.fillRect (coverArea);

            // Branded placeholder: the tower initial on a soft disc.
            const auto disc = juce::Rectangle<float> (coverArea.getCentreX() - 34.0f,
                                                      coverArea.getY() + 34.0f, 68.0f, 68.0f);
            g.setColour (UiColors::accent.withAlpha (0.16f));
            g.fillEllipse (disc);
            g.setColour (UiColors::accent);
            g.setFont (GameUi::heading (30.0f));
            g.drawText (campaign.name.substring (0, 1).toUpperCase(),
                        disc.toNearestInt(), juce::Justification::centred, false);
        }

        // The title now lives on the solid panel below, so the cover needs no
        // darkening gradient at all. The status pill has its own background.
        const auto infoPanel = inner.withTop (inner.getBottom() - 100.0f);
        g.setColour (UiColors::background.withAlpha (0.96f));
        g.fillRect (infoPanel);
        g.setColour (UiColors::border.withAlpha (0.55f));
        g.fillRect (infoPanel.withHeight (1.0f));
    }

    // Frame: accent for the started tower, green for a cleared one, orange for
    // a tower the player has not unlocked yet.
    g.setColour (locked ? UiColors::orange.withAlpha (isHovered ? 0.55f : 0.38f)
                        : (complete ? UiColors::green.withAlpha (0.55f)
                                    : (started ? UiColors::accent.withAlpha (isHovered ? 0.9f : 0.55f)
                                               : UiColors::border.withAlpha (isHovered ? 0.8f : 0.45f))));
    g.drawRoundedRectangle (inner.reduced (0.5f), 18.0f, isHovered ? 2.0f : 1.0f);

    auto text = area.reduced (16.0f, 12.0f);

    // Difficulty plate in the top-left corner of the artwork.
    GameUi::drawPill (g, { text.getX(), text.getY(), 76.0f, 20.0f },
                      towerDifficultyLabel (campaign.botLevel),
                      towerDifficultyColour (campaign.botLevel), false);

    // Status pill floats over the artwork; it carries its own dark background.
    const auto status = locked ? juce::String ("LOCKED")
                               : towerStatus (bestStageForTower, count);
    const auto statusColour = locked ? UiColors::orange
                                     : towerStatusColour (bestStageForTower, count);
    const float pillW = 104.0f;
    GameUi::drawPill (g, { text.getRight() - pillW, text.getY(), pillW, 20.0f },
                      status, statusColour, complete && ! locked);

    // Everything else lives on the solid panel below; the cover above is fully
    // visible and is never covered by the title or a dark gradient.
    const auto panel = inner.withTop (inner.getBottom() - 100.0f);
    const float panelX = panel.getX() + 16.0f;
    const float panelW = panel.getWidth() - 32.0f;

    auto titleRow = juce::Rectangle<float> (panelX, panel.getY() + 8.0f,
                                            panelW, 24.0f);

    if (locked)
    {
        auto lockArea = titleRow.removeFromRight (150.0f);
        g.setColour (UiColors::orange);
        g.setFont (GameUi::eyebrow (8.5f));
        g.drawText ("REQUIRES LEVEL " + juce::String (requiredLevel),
                    lockArea.toNearestInt(),
                    juce::Justification::centredRight, false);
    }

    g.setColour (locked ? UiColors::text.withAlpha (0.78f) : UiColors::text);
    g.setFont (GameUi::heading (18.0f));
    g.drawFittedText (campaign.name, titleRow.toNearestInt(),
                      juce::Justification::centredLeft, 1, 1.0f);

    g.setColour (locked ? UiColors::muted.withAlpha (0.85f) : UiColors::muted);
    g.setFont (GameUi::font (10.5f));
    g.drawFittedText (campaign.description.isNotEmpty()
                          ? campaign.description
                          : juce::String ("Climb the tower, one opponent at a time."),
                      juce::Rectangle<float> (panelX, panel.getY() + 32.0f,
                                              panelW, 26.0f).toNearestInt(),
                      juce::Justification::topLeft, 2, 1.0f);

    // Bottom row: reward on the left, cleared-stage count on the right.
    if (campaign.rewardName.isNotEmpty())
    {
        g.setColour (locked ? UiColors::muted
                            : UiColors::accent.withAlpha (0.85f));
        g.setFont (GameUi::font (9.5f));
        g.drawFittedText ("REWARD  " + campaign.rewardName,
                          juce::Rectangle<float> (panelX, panel.getY() + 64.0f,
                                                  panelW - 90.0f, 14.0f)
                              .toNearestInt(),
                          juce::Justification::centredLeft, 1, 1.0f);
    }

    g.setColour (locked ? UiColors::muted
                        : (complete ? UiColors::green : UiColors::accent));
    g.setFont (GameUi::font (11.0f, true));
    g.drawText (juce::String (bestStageForTower) + " / " + juce::String (count),
                juce::Rectangle<float> (panel.getRight() - 96.0f,
                                        panel.getY() + 64.0f, 80.0f, 14.0f),
                juce::Justification::centredRight, false);

    const juce::Rectangle<float> bar (panelX, panel.getY() + 86.0f,
                                      panelW, 6.0f);
    g.setColour (UiColors::surface2);
    g.fillRoundedRectangle (bar, 3.0f);

    if (count > 0)
    {
        const float ratio = juce::jlimit (0.0f, 1.0f,
                                          (float) bestStageForTower / (float) count);
        g.setColour (locked ? UiColors::muted.withAlpha (0.5f)
                            : (complete ? UiColors::green : UiColors::accent));
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * ratio), 3.0f);
    }
}

void CampaignPanel::paintDetail (juce::Graphics& g)
{
    const auto* campaign = selectedCampaign();

    if (campaign == nullptr)
        return;

    const int count = static_cast<int> (campaign->stages.size());
    const bool complete = count > 0 && currentStage >= count;
    const int requiredLevel = towerRequiredLevel (campaign->botLevel);
    const bool locked = juce::jmax (1, playerLevel) < requiredLevel;
    const auto statusColour = locked ? UiColors::orange
                                     : towerStatusColour (currentStage, count);

    // Banner without the cover image: at this height the crop hides most of
    // the artwork, so the detail view uses a clean branded gradient instead.
    {
        juce::Graphics::ScopedSaveState save (g);
        juce::Path clip;
        clip.addRoundedRectangle (detailBanner, 18.0f);
        g.reduceClipRegion (clip);

        juce::ColourGradient fallback (UiColors::surface2.brighter (0.06f),
                                       detailBanner.getX(), detailBanner.getY(),
                                       UiColors::background, detailBanner.getRight(),
                                       detailBanner.getBottom(), false);
        g.setGradientFill (fallback);
        g.fillRect (detailBanner);

        juce::ColourGradient shade (UiColors::background.withAlpha (0.15f), 0.0f,
                                    detailBanner.getY(),
                                    UiColors::background.withAlpha (0.92f), 0.0f,
                                    detailBanner.getBottom(), false);
        g.setGradientFill (shade);
        g.fillRect (detailBanner);
    }

    g.setColour (complete ? UiColors::green : UiColors::accent.withAlpha (0.5f));
    g.drawRoundedRectangle (detailBanner.reduced (0.5f), 18.0f, 1.0f);

    auto titleArea = detailBanner.reduced (20.0f, 16.0f);

    g.setColour (UiColors::muted);
    g.setFont (GameUi::eyebrow (9.0f));
    g.drawText ("TOWER", titleArea.removeFromTop (13.0f),
                juce::Justification::centredLeft, false);

    g.setColour (UiColors::text);
    g.setFont (GameUi::heading (26.0f));
    g.drawFittedText (campaign->name, titleArea.removeFromTop (34.0f).toNearestInt(),
                      juce::Justification::centredLeft, 1, 1.0f);

    g.setColour (UiColors::muted);
    g.setFont (GameUi::font (11.0f));
    g.drawFittedText (campaign->description.isNotEmpty()
                          ? campaign->description
                          : juce::String ("Win each fight to unlock the next opponent."),
                      titleArea.removeFromTop (34.0f).toNearestInt(),
                      juce::Justification::topLeft, 2, 1.0f);

    const auto pillW = 112.0f;
    GameUi::drawPill (g, { detailBanner.getRight() - 20.0f - pillW,
                           detailBanner.getY() + 18.0f, pillW, 20.0f },
                      locked ? juce::String ("LOCKED")
                             : towerStatus (currentStage, count),
                      statusColour, complete && ! locked);

    // Admin difficulty of the tower.
    GameUi::drawPill (g, { detailBanner.getRight() - 20.0f - pillW,
                           detailBanner.getY() + 44.0f, pillW, 20.0f },
                      towerDifficultyLabel (campaign->botLevel),
                      towerDifficultyColour (campaign->botLevel), false);

    // Progress + reward row under the banner.
    auto body = detailBody;
    g.setColour (UiColors::muted);
    g.setFont (GameUi::eyebrow (9.0f));
    g.drawText ("PROGRESS",
                body.removeFromTop (12.0f), juce::Justification::centredLeft, false);
    g.setColour (UiColors::text);
    g.setFont (GameUi::font (14.0f, true));
    g.drawText (juce::String (currentStage) + " / " + juce::String (count)
                    + " opponents cleared",
                body.removeFromTop (20.0f), juce::Justification::centredLeft, false);

    const juce::Rectangle<float> bar (body.getX(), body.getY(),
                                      body.getWidth() * 0.55f, 6.0f);
    g.setColour (UiColors::surface2);
    g.fillRoundedRectangle (bar, 3.0f);

    if (count > 0)
    {
        g.setColour (complete ? UiColors::green : UiColors::accent);
        g.fillRoundedRectangle (
            bar.withWidth (bar.getWidth()
                           * juce::jlimit (0.0f, 1.0f,
                                           (float) currentStage / (float) count)),
            3.0f);
    }

    if (campaign->rewardName.isNotEmpty())
    {
        g.setColour (UiColors::accent);
        g.setFont (GameUi::font (11.0f, true));
        g.drawFittedText ("REWARD  " + campaign->rewardName
                              + (campaign->rewardCondition.isNotEmpty()
                                     ? "  -  " + campaign->rewardCondition
                                     : juce::String()),
                          body.withWidth (body.getWidth() * 0.42f)
                              .withX (body.getRight() - body.getWidth() * 0.42f)
                              .toNearestInt(),
                          juce::Justification::centredRight, 1, 1.0f);
    }
}

void CampaignPanel::mouseMove (const juce::MouseEvent& event)
{
    if (showingDetail)
        return;

    int hovered = -1;

    for (int i = 0; i < static_cast<int> (campaigns.size()); ++i)
        if (cardBounds (i).contains (event.getPosition().toFloat()))
            hovered = i;

    if (hovered != hoveredCard)
    {
        hoveredCard = hovered;
        const bool locked = hovered >= 0
                            && isCampaignLocked (
                                campaigns[static_cast<size_t> (hovered)]);
        setMouseCursor (hovered >= 0 && ! locked
                            ? juce::MouseCursor::PointingHandCursor
                            : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void CampaignPanel::mouseExit (const juce::MouseEvent&)
{
    if (hoveredCard >= 0)
    {
        hoveredCard = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void CampaignPanel::mouseUp (const juce::MouseEvent& event)
{
    if (showingDetail || event.mods.isPopupMenu())
        return;

    for (int i = 0; i < static_cast<int> (campaigns.size()); ++i)
    {
        if (! cardBounds (i).contains (event.getPosition().toFloat()))
            continue;

        const auto& campaign = campaigns[static_cast<size_t> (i)];

        if (isCampaignLocked (campaign))
            return;

        openTower (campaign.id);
        return;
    }
}
