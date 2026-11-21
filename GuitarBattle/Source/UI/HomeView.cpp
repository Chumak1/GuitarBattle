#include "HomeView.h"

#include "Localisation.h"

#include <cmath>

namespace
{
    constexpr int sectionGap = 16;
}

HomeView::HomeView()
{
    setOpaque (false);

    refreshPill.setFontHeight (10.0f);
    refreshPill.setIcon (Ui::Icon::refresh);
    refreshPill.onClick = [this] { if (onRefresh != nullptr) onRefresh(); };
    addAndMakeVisible (refreshPill);

    // Text set at construction must follow the language switcher.
    I18n::addLanguageListener ([safe = juce::Component::SafePointer<HomeView> (this)]
    {
        if (auto* self = safe.getComponent())
            self->updateLocalisedText();
    });

    updateLocalisedText();
}

void HomeView::updateLocalisedText()
{
    refreshPill.setButtonText (I18n::tr ("REFRESH"));
    repaint();
}

void HomeView::setLeaderboardComponents (juce::TableListBox& newTable,
                                         juce::Component& newStatus)
{
    table = &newTable;
    statusLabel = &newStatus;

    // Reparent: the table and the status label now live inside the leaderboard
    // card and follow its visibility.
    addAndMakeVisible (*table);
    addAndMakeVisible (*statusLabel);

    resized();
}

bool HomeView::sameStats (const Stats& a, const Stats& b)
{
    return a.username == b.username
           && a.level == b.level
           && a.xpIntoLevel == b.xpIntoLevel
           && a.xpForNext == b.xpForNext
           && a.xpRemaining == b.xpRemaining
           && std::abs (a.levelRatio - b.levelRatio) < 0.001f
           && a.totalXpText == b.totalXpText
           && a.skillsText == b.skillsText
           && a.duelsText == b.duelsText
           && a.ownRank == b.ownRank;
}

bool HomeView::samePlayers (const std::vector<TopPlayer>& a,
                            const std::vector<TopPlayer>& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].userId != b[i].userId || a[i].name != b[i].name
            || a[i].rank != b[i].rank || a[i].level != b[i].level
            || a[i].skillsText != b[i].skillsText
            || a[i].positionChange != b[i].positionChange
            || a[i].hasPositionChange != b[i].hasPositionChange)
            return false;
    }

    return true;
}

void HomeView::setStats (const Stats& newStats)
{
    if (sameStats (stats, newStats))
        return;

    stats = newStats;
    repaint();
}

void HomeView::setTopPlayers (std::vector<TopPlayer> newPlayers)
{
    if (samePlayers (players, newPlayers))
        return;

    players = std::move (newPlayers);
    updatePodium();
    repaint();
}

void HomeView::setHeroBackgrounds (const juce::Image& battleImage,
                                   const juce::Image& duelImage)
{
    bool changed = false;

    if (! (battleBackground == battleImage))
    {
        battleBackground = battleImage;
        changed = true;
    }

    if (! (duelBackground == duelImage))
    {
        duelBackground = duelImage;
        changed = true;
    }

    if (! changed)
        return;

    repaint (heroBattle);
    repaint (heroOnline);
}

void HomeView::setAvatarLookup (std::function<juce::Image (const juce::String&)> lookup)
{
    avatarLookup = std::move (lookup);
    repaint();
}

void HomeView::updatePodium()
{
    // Podium order: silver (rank 2), gold (rank 1), bronze (rank 3).
    const int order[3] = { 1, 0, 2 };

    for (int slot = 0; slot < 3; ++slot)
    {
        const int index = order[slot];
        podiumPlayerIndex[static_cast<size_t> (slot)] =
            index < static_cast<int> (players.size()) ? index : -1;
    }
}

void HomeView::resized()
{
    const auto bounds = getLocalBounds();

    if (bounds.isEmpty())
        return;

    const int heroH = juce::jlimit (150, 340,
                                    juce::roundToInt (bounds.getHeight() * 0.37f));
    const int statsH = juce::jlimit (86, 124,
                                     juce::roundToInt (bounds.getHeight() * 0.145f));
    const int heroW = (bounds.getWidth() - sectionGap) / 2;

    heroBattle = { bounds.getX(), bounds.getY(), heroW, heroH };
    heroOnline = { bounds.getX() + heroW + sectionGap, bounds.getY(),
                   bounds.getWidth() - heroW - sectionGap, heroH };

    statsArea = { bounds.getX(), heroBattle.getBottom() + sectionGap,
                  bounds.getWidth(), statsH };

    boardArea = { bounds.getX(), statsArea.getBottom() + sectionGap,
                  bounds.getWidth(),
                  juce::jmax (180, bounds.getBottom() - statsArea.getBottom() - sectionGap) };

    const auto inner = boardArea.reduced (20, 0);

    refreshArea = { inner.getRight() - 112, boardArea.getY() + 14, 112, 28 };    statusArea = { inner.getX() + 44, boardArea.getY() + 36,
                   juce::jmax (120, inner.getWidth() - 64), 13 };

    const int podiumTop = boardArea.getY() + 56;
    const int podiumBottom = boardArea.getBottom() - 14;
    const int contentH = juce::jmax (120, podiumBottom - podiumTop);

    // Wide windows: podium on the left, table on the right (as in the design
    // reference). Narrow windows stack them so the table keeps usable rows.
    const bool sideBySide = inner.getWidth() >= 880;

    if (sideBySide)
    {
        const int podiumW = juce::roundToInt (inner.getWidth() * 0.42f);
        const int tileH = juce::jmin (contentH, 300);
        const int tileW = (podiumW - 24) / 3;

        for (int slot = 0; slot < 3; ++slot)
            podiumTiles[static_cast<size_t> (slot)] = {
                inner.getX() + slot * (tileW + 12), podiumTop, tileW, tileH };

        const int tableX = inner.getX() + podiumW + 24;
        const int tableW = inner.getRight() - tableX;

        // The signed-in player's own line is anchored to the bottom of the
        // card, so the empty part of a short table never looks unfinished.
        const int ownH = contentH >= 340 ? 74 : 0;
        ownArea = ownH > 0 ? juce::Rectangle<int> (tableX, podiumBottom - ownH,
                                                   tableW, ownH)
                           : juce::Rectangle<int>();

        tableArea = { tableX, podiumTop - 4, tableW,
                      contentH + 8 - (ownH > 0 ? ownH + 12 : 0) };
    }
    else
    {
        const int podiumH = juce::jlimit (84, 112, contentH - 126);

        const int tileW = (inner.getWidth() - 24) / 3;

        for (int slot = 0; slot < 3; ++slot)
            podiumTiles[static_cast<size_t> (slot)] = {
                inner.getX() + slot * (tileW + 12), podiumTop, tileW, podiumH };

        ownArea = {};

        tableArea = { inner.getX(), podiumTop + podiumH + 14, inner.getWidth(),
                      juce::jmax (80, podiumBottom - (podiumTop + podiumH + 14)) };
    }

    if (table != nullptr)
    {
        table->setBounds (tableArea);

        // Column widths follow the card instead of the header's stretch mode:
        // the player name takes the remaining space, the metrics stay compact.
        auto& header = table->getHeader();
        const int tableW = tableArea.getWidth();

        int rankW = 46, lvlW = 74, skillsW = 78, duelsW = 132, statusW = 96;

        if (tableW - (rankW + lvlW + skillsW + duelsW + statusW) < 130)
        {
            rankW = 42;
            lvlW = 66;
            skillsW = 70;
            duelsW = 110;
            statusW = 84;
        }

        const int playerW = juce::jmax (90, tableW
                                             - (rankW + lvlW + skillsW
                                                + duelsW + statusW) - 2);

        header.setColumnWidth (1, rankW);
        header.setColumnWidth (2, playerW);
        header.setColumnWidth (3, lvlW);
        header.setColumnWidth (4, skillsW);
        header.setColumnWidth (5, duelsW);
        header.setColumnWidth (6, statusW);
    }

    refreshPill.setBounds (refreshArea);

    if (statusLabel != nullptr)
        statusLabel->setBounds (statusArea);
}

//==============================================================================
// Hero cards
//==============================================================================

void HomeView::paintTowerArt (juce::Graphics& g, juce::Rectangle<float> area)
{
    juce::ColourGradient sky (juce::Colour (0xff231b0e), area.getCentreX(), area.getY(),
                              juce::Colour (0xff0a0b0d), area.getCentreX(), area.getBottom(),
                              false);
    g.setGradientFill (sky);
    g.fillRect (area);

    // Moon halo behind the tower.
    const auto halo = juce::Rectangle<float> (area.getCentreX() - 60.0f,
                                              area.getY() + area.getHeight() * 0.08f,
                                              120.0f, 120.0f);
    g.setColour (Ui::Colours::accent.withAlpha (0.09f));
    g.fillEllipse (halo);
    g.setColour (Ui::Colours::accent.withAlpha (0.15f));
    g.fillEllipse (halo.reduced (30.0f));

    // Ground haze.
    {
        juce::ColourGradient haze (juce::Colours::transparentBlack,
                                   area.getCentreX(), area.getBottom() - 80.0f,
                                   Ui::Colours::accent.withAlpha (0.14f),
                                   area.getCentreX(), area.getBottom(), false);
        g.setGradientFill (haze);
        g.fillRect (area.withTop (area.getBottom() - 80.0f));
    }

    // Stars.
    {
        const float stars[][2] = { { 0.18f, 0.16f }, { 0.72f, 0.10f }, { 0.86f, 0.30f },
                                   { 0.30f, 0.34f }, { 0.60f, 0.24f }, { 0.44f, 0.12f } };

        g.setColour (juce::Colours::white.withAlpha (0.35f));

        for (const auto& star : stars)
            g.fillEllipse (area.getX() + area.getWidth() * star[0],
                           area.getY() + area.getHeight() * star[1], 2.0f, 2.0f);
    }

    const float cx = area.getCentreX();
    const float base = area.getBottom() - 6.0f;

    struct Block { float width; float height; };

    const Block blocks[] = { { 150.0f, 38.0f }, { 122.0f, 40.0f }, { 98.0f, 36.0f },
                             { 76.0f, 32.0f }, { 54.0f, 26.0f } };

    float y = base;

    for (const auto& block : blocks)
    {
        const juce::Rectangle<float> shape (cx - block.width * 0.5f,
                                            y - block.height,
                                            block.width, block.height);

        g.setColour (juce::Colour (0xff13161b));
        g.fillRect (shape);
        g.setColour (juce::Colour (0xff1f242c));
        g.fillRect (shape.withWidth (2.0f));
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.fillRect (shape.withTrimmedLeft (block.width - 7.0f));

        y -= block.height + 2.0f;
    }

    // Battlements on the top block.
    {
        const float topY = y + 2.0f;

        g.setColour (juce::Colour (0xff13161b));

        for (int i = 0; i < 3; ++i)
            g.fillRect (cx - 24.0f + static_cast<float> (i) * 17.0f, topY - 7.0f, 10.0f, 8.0f);
    }

    // Lit windows.
    {
        const float windows[][2] = { { -40.0f, -16.0f }, { -12.0f, -18.0f }, { 16.0f, -16.0f },
                                     { 40.0f, -18.0f }, { -30.0f, -52.0f }, { -2.0f, -54.0f },
                                     { 26.0f, -52.0f }, { -20.0f, -88.0f }, { 8.0f, -90.0f },
                                     { 32.0f, -88.0f }, { -8.0f, -124.0f }, { 14.0f, -126.0f } };

        for (const auto& window : windows)
        {
            g.setColour (Ui::Colours::accent.withAlpha (0.22f));
            g.fillRoundedRectangle (cx + window[0] - 1.0f, base + window[1] - 1.0f,
                                    9.0f, 12.0f, 2.5f);
            g.setColour (Ui::Colours::accent.withAlpha (0.72f));
            g.fillRoundedRectangle (cx + window[0], base + window[1], 7.0f, 10.0f, 1.5f);
        }
    }

    // Pennant.
    {
        juce::Path flag;
        const float flagX = cx + 2.0f;
        const float flagY = y - 6.0f;
        flag.startNewSubPath (flagX, flagY);
        flag.lineTo (flagX + 20.0f, flagY + 5.0f);
        flag.lineTo (flagX, flagY + 11.0f);
        flag.closeSubPath();
        g.setColour (Ui::Colours::accent);
        g.fillPath (flag);
        g.setColour (Ui::Colours::accent.withAlpha (0.7f));
        g.fillRect (flagX - 1.5f, flagY, 1.5f, 30.0f);
    }
}

void HomeView::paintDuelArt (juce::Graphics& g, juce::Rectangle<float> area)
{
    juce::ColourGradient sky (juce::Colour (0xff1d1a11), area.getCentreX(), area.getY(),
                              juce::Colour (0xff0a0b0d), area.getCentreX(), area.getBottom(),
                              false);
    g.setGradientFill (sky);
    g.fillRect (area);

    // Two light cones from the top corners.
    for (int side = 0; side < 2; ++side)
    {
        const bool left = side == 0;
        juce::Path cone;
        const float originX = left ? area.getX() + 14.0f
                                   : area.getRight() - 48.0f;

        cone.startNewSubPath (originX, area.getY());
        cone.lineTo (originX + 34.0f, area.getY());
        cone.lineTo (area.getCentreX() + (left ? 24.0f : -24.0f), area.getBottom());
        cone.lineTo (area.getCentreX() + (left ? -80.0f : 80.0f), area.getBottom());
        cone.closeSubPath();

        g.setColour (Ui::Colours::accent.withAlpha (0.05f));
        g.fillPath (cone);
    }

    const float scale = juce::jlimit (0.62f, 1.15f, area.getWidth() / 330.0f);
    const float base = area.getBottom() - 10.0f;

    auto drawPlayer = [&g, base, scale] (float centreX, bool facingRight)
    {
        const juce::Colour body (0xff0f1216);
        const juce::Colour rim (Ui::Colours::accent.withAlpha (0.32f));
        const float dir = facingRight ? 1.0f : -1.0f;

        // Torso.
        juce::Path torso;
        torso.startNewSubPath (centreX - 26.0f * scale, base);
        torso.lineTo (centreX - 19.0f * scale, base - 74.0f * scale);
        torso.quadraticTo (centreX, base - 98.0f * scale,
                           centreX + 19.0f * scale, base - 74.0f * scale);
        torso.lineTo (centreX + 26.0f * scale, base);
        torso.closeSubPath();

        g.setColour (body);
        g.fillPath (torso);
        g.setColour (rim);
        g.strokePath (torso, juce::PathStrokeType (1.0f));

        // Head.
        const float headR = 13.0f * scale;
        const juce::Rectangle<float> head (centreX - headR, base - 108.0f * scale,
                                           headR * 2.0f, headR * 2.0f);
        g.setColour (body);
        g.fillEllipse (head);
        g.setColour (rim);
        g.drawEllipse (head, 1.0f);

        // Guitar body and neck.
        const juce::Rectangle<float> guitar (centreX + dir * 6.0f - 15.0f * scale,
                                             base - 64.0f * scale,
                                             30.0f * scale, 26.0f * scale);
        g.setColour (Ui::Colours::accent.withAlpha (0.5f));
        g.fillEllipse (guitar);
        g.setColour (Ui::Colours::accent.withAlpha (0.78f));
        g.drawLine (guitar.getCentreX(), guitar.getY() + 6.0f * scale,
                    guitar.getCentreX() + dir * 36.0f * scale,
                    guitar.getY() - 16.0f * scale,
                    juce::jmax (1.5f, 3.0f * scale));
    };

    drawPlayer (area.getCentreX() - 58.0f * scale, true);
    drawPlayer (area.getCentreX() + 58.0f * scale, false);

    // VS medallion between them.
    const juce::Rectangle<float> vs (area.getCentreX() - 24.0f,
                                     base - 84.0f * scale, 48.0f, 48.0f);
    g.setColour (Ui::Colours::accent.withAlpha (0.12f));
    g.fillEllipse (vs);
    g.setColour (Ui::Colours::accent.withAlpha (0.45f));
    g.drawEllipse (vs, 1.2f);

    Ui::label (g, "VS", vs, Ui::semibold (18.0f, 0.04f),
               Ui::Colours::accent, juce::Justification::centred);

    // Stage floor.
    g.setColour (Ui::Colours::accent.withAlpha (0.10f));
    g.fillRect (area.getX(), area.getBottom() - 3.0f, area.getWidth(), 3.0f);
}

void HomeView::paintHero (juce::Graphics& g, juce::Rectangle<float> area, bool online,
                          bool hovered)
{
    Ui::panel (g, area, Ui::Radius::card, hovered);

    const auto art = juce::Rectangle<float> (area.getX() + area.getWidth() * 0.54f,
                                             area.getY(),
                                             area.getWidth() * 0.46f,
                                             area.getHeight());

    {
        juce::Graphics::ScopedSaveState state (g);

        juce::Path clip;
        clip.addRoundedRectangle (area, Ui::Radius::card);
        g.reduceClipRegion (clip);

        const auto& artwork = online ? duelBackground : battleBackground;

        if (artwork.isValid())
        {
            // Admin artwork covers the whole card; a left-to-right shade keeps
            // the text column readable and blends into the panel colour.
            g.setColour (juce::Colours::white);
            g.drawImage (artwork, area,
                         juce::RectanglePlacement::fillDestination);

            juce::ColourGradient shade (Ui::Colours::surface.withAlpha (0.95f),
                                        area.getX(), 0.0f,
                                        Ui::Colours::surface.withAlpha (0.0f),
                                        area.getX() + area.getWidth() * 0.74f,
                                        0.0f, false);
            shade.addColour (0.42, Ui::Colours::surface.withAlpha (0.62f));
            g.setGradientFill (shade);
            g.fillRect (area);

            juce::ColourGradient vignette (juce::Colours::transparentBlack,
                                           area.getCentreX(), area.getY(),
                                           juce::Colours::black.withAlpha (0.32f),
                                           area.getCentreX(), area.getBottom(),
                                           false);
            g.setGradientFill (vignette);
            g.fillRect (area);
        }
        else if (online)
        {
            paintDuelArt (g, art);
        }
        else
        {
            paintTowerArt (g, art);
        }

        if (! artwork.isValid())
        {
            // Blend the procedural artwork into the text side of the card.
            juce::ColourGradient fade (Ui::Colours::surface,
                                       art.getX(), 0.0f,
                                       Ui::Colours::surface.withAlpha (0.0f),
                                       art.getX() + 96.0f, 0.0f, false);
            g.setGradientFill (fade);
            g.fillRect (art.withWidth (96.0f));
        }
    }

    const juce::Rectangle<float> textColumn (area.getX() + 22.0f, area.getY(),
                                             area.getWidth() * 0.54f - 40.0f,
                                             area.getHeight());

    const bool roomy = area.getHeight() >= 200.0f;
    const bool showTagline = area.getHeight() >= 158.0f;

    // Mode icon, as in the design reference.
    const float iconSize = 24.0f;
    const juce::Rectangle<float> iconPlate (textColumn.getX(), area.getY() + 16.0f,
                                            iconSize, iconSize);
    Ui::icon (g, online ? Ui::Icon::globe : Ui::Icon::sword, iconPlate,
              Ui::Colours::accent, 1.7f);

    const float titleY = area.getY() + 44.0f;

    Ui::label (g, I18n::tr (online ? "ONLINE" : "BATTLE"),
               { textColumn.getX(), titleY, textColumn.getWidth(), 38.0f },
               Ui::semibold (roomy ? 32.0f : 28.0f, 0.02f), Ui::Colours::text,
               juce::Justification::centredLeft, true);

    const float ctaH = 34.0f;
    const float ctaY = area.getBottom() - 16.0f - ctaH;

    float cursorY = titleY + (roomy ? 38.0f : 34.0f);

    if (showTagline)
    {
        Ui::label (g, I18n::tr (online ? "Duel real players" : "Climb the towers"),
                   { textColumn.getX(), cursorY, textColumn.getWidth(), 18.0f },
                   Ui::semibold (13.0f, 0.01f), Ui::Colours::accent,
                   juce::Justification::centredLeft, true);
        cursorY += 22.0f;
    }

    if (roomy)
    {
        Ui::paragraph (g, I18n::tr (online
                                        ? "Create a lobby or jump into a live duel and put "
                                          "your skills against other guitarists."
                                        : "Beat the house band stage by stage and unlock "
                                          "the harder towers."),
                       { textColumn.getX(), cursorY, textColumn.getWidth(),
                         juce::jmax (14.0f, ctaY - 10.0f - cursorY) },
                       Ui::font (11.5f), Ui::Colours::textSecondary,
                       juce::Justification::topLeft, 3);
    }

    // Call to action.
    const juce::Rectangle<float> cta (textColumn.getX(), ctaY, 158.0f, ctaH);

    if (online)
    {
        g.setColour (Ui::Colours::accent.withAlpha (0.06f + (hovered ? 0.06f : 0.0f)));
        g.fillRoundedRectangle (cta, ctaH * 0.5f);
        g.setColour (Ui::Colours::accent.withAlpha (0.55f));
        g.drawRoundedRectangle (cta.reduced (0.5f), ctaH * 0.5f, 1.0f);
    }
    else
    {
        g.setColour (Ui::Colours::accent);
        g.fillRoundedRectangle (cta, ctaH * 0.5f);
    }

    const auto ctaInk = online ? Ui::Colours::accent : Ui::Colours::onAccent;

    Ui::label (g, I18n::tr (online ? "START DUEL" : "ENTER BATTLE"),
               { cta.getX() + 18.0f, cta.getY(), cta.getWidth() - 50.0f, cta.getHeight() },
               Ui::font (10.5f, true, 0.1f), ctaInk,
               juce::Justification::centredLeft);

    Ui::icon (g, Ui::Icon::arrowRight,
              { cta.getRight() - 30.0f, cta.getCentreY() - 7.0f, 14.0f, 14.0f },
              ctaInk, 1.8f);
}

//==============================================================================
// Player strip
//==============================================================================

void HomeView::paintStats (juce::Graphics& g)
{
    const auto area = statsArea.toFloat();

    if (area.isEmpty())
        return;

    Ui::panel (g, area, Ui::Radius::panel);

    const float levelW = juce::jmax (300.0f, area.getWidth() * 0.42f);
    const float restW = (area.getWidth() - levelW) * 0.5f;

    // Content block is centred vertically so the strip keeps its rhythm from
    // 86 px (1366x768) up to 124 px (1440p).
    const float top = area.getY()
                      + juce::jmax (4.0f, (area.getHeight() - 58.0f) * 0.5f);

    // --- Level ------------------------------------------------------------
    {
        const juce::Rectangle<float> plate (area.getX() + 20.0f,
                                            area.getCentreY() - 23.0f, 46.0f, 46.0f);
        g.setColour (Ui::Colours::accent.withAlpha (0.08f));
        g.fillRoundedRectangle (plate, 12.0f);
        g.setColour (Ui::Colours::accent.withAlpha (0.55f));
        g.drawRoundedRectangle (plate.reduced (0.5f), 12.0f, 1.0f);

        Ui::label (g, "LV", plate, Ui::font (11.0f, true, 0.12f),
                   Ui::Colours::accent, juce::Justification::centred);

        const float textX = plate.getRight() + 14.0f;

        Ui::eyebrow (g, I18n::tr ("LEVEL"),
                     { textX, top, 200.0f, 11.0f },
                     Ui::Colours::textMuted);

        Ui::label (g, juce::String (stats.level),
                   { textX, top + 11.0f, 60.0f, 30.0f },
                   Ui::semibold (23.0f, 0.01f), Ui::Colours::text,
                   juce::Justification::centredLeft, true);

        const float barX = textX + 58.0f;
        const float barW = juce::jmax (60.0f,
                                       juce::jmin (300.0f,
                                                   levelW - (barX - area.getX()) - 40.0f));

        if (stats.xpForNext > 0)
        {
            Ui::progress (g, { barX, top + 18.0f, barW, 6.0f },
                          stats.levelRatio);

            Ui::label (g, juce::String (stats.xpIntoLevel) + " / "
                              + juce::String (stats.xpForNext) + " XP",
                       { barX, top + 27.0f, barW, 12.0f },
                       Ui::font (9.0f, false, 0.06f), Ui::Colours::textMuted,
                       juce::Justification::centredLeft, true);
        }

        Ui::label (g, (stats.totalXpText.isNotEmpty() && stats.totalXpText != "-"
                           ? I18n::tr ("TOTAL") + " " + stats.totalXpText + " XP"
                           : I18n::tr ("NO XP DATA")),
                   { textX, top + 44.0f, levelW - 60.0f, 14.0f },
                   Ui::font (9.0f, false, 0.06f), Ui::Colours::textMuted,
                   juce::Justification::centredLeft, true);
    }

    // --- Guitar skills / duel record --------------------------------------
    struct Block
    {
        const char* label;
        juce::String value;
        juce::String note;
        Ui::Icon icon;
    };

    const Block blocks[2] = {
        { "GUITAR SKILLS", stats.skillsText, "0-100 SCALE", Ui::Icon::guitar },
        { "DUEL WINS", stats.duelsText, "WIN - LOSS RECORD", Ui::Icon::star }
    };
    for (int i = 0; i < 2; ++i)
    {
        const float x = area.getX() + levelW + restW * static_cast<float> (i);

        Ui::verticalDivider (g, x, area.getY() + 16.0f, area.getBottom() - 16.0f);

        const auto& block = blocks[i];

        const juce::Rectangle<float> iconPlate (x + 22.0f,
                                                area.getCentreY() - 17.0f,
                                                34.0f, 34.0f);
        g.setColour (juce::Colours::white.withAlpha (0.04f));
        g.fillEllipse (iconPlate);
        Ui::icon (g, block.icon, iconPlate.reduced (8.0f), Ui::Colours::accent, 1.5f);

        const float textX = iconPlate.getRight() + 12.0f;

        Ui::eyebrow (g, I18n::tr (block.label),
                     { textX, top, restW - 80.0f, 11.0f },
                     Ui::Colours::textMuted);

        Ui::label (g, block.value,
                   { textX, top + 11.0f, restW - 80.0f, 28.0f },
                   Ui::semibold (21.0f, 0.01f), Ui::Colours::text,
                   juce::Justification::centredLeft, true);

        Ui::label (g, I18n::tr (block.note),
                   { textX, top + 40.0f, restW - 80.0f, 12.0f },
                   Ui::font (8.5f, false, 0.06f), Ui::Colours::textMuted,
                   juce::Justification::centredLeft, true);
    }
}

//==============================================================================
// Leaderboard
//==============================================================================

void HomeView::paintPodiumTile (juce::Graphics& g, int slot)
{
    const auto tile = podiumTiles[static_cast<size_t> (slot)].toFloat();

    if (tile.isEmpty())
        return;

    const int index = podiumPlayerIndex[static_cast<size_t> (slot)];
    const bool isFirst = slot == 1;

    const juce::Colour medal = isFirst ? Ui::Colours::accent
                                       : (slot == 0 ? juce::Colour (0xffc0c6cf)
                                                    : juce::Colour (0xffc58b5a));

    Ui::panel (g, tile, Ui::Radius::tile, isFirst);

    if (isFirst)
    {
        g.setColour (Ui::Colours::accent.withAlpha (0.07f));
        g.fillRoundedRectangle (tile.reduced (1.0f), Ui::Radius::tile);
    }

    const float baseH = tile.getHeight() >= 190.0f ? 46.0f : 0.0f;
    const float topPad = isFirst ? 28.0f : 12.0f;

    if (isFirst)
        Ui::icon (g, Ui::Icon::crown,
                  { tile.getCentreX() - 11.0f, tile.getY() + 7.0f, 22.0f, 18.0f },
                  Ui::Colours::accent, 1.5f);

    if (index < 0 || index >= static_cast<int> (players.size()))
        return;

    const auto& player = players[static_cast<size_t> (index)];
    const auto name = player.name.isNotEmpty() ? player.name : juce::String ("?");

    const float avatarSize = juce::jlimit (36.0f, 52.0f, tile.getWidth() * 0.26f);
    const float nameH = 18.0f;
    const float lvlH = 13.0f;
    const float skillsH = 26.0f;

    const float blockH = avatarSize + 8.0f + nameH + lvlH + 4.0f + skillsH;
    const float bandTop = tile.getY() + topPad;
    const float bandH = tile.getHeight() - baseH - topPad - 10.0f;

    float y = bandTop + juce::jmax (0.0f, (bandH - blockH) * 0.5f);

    const juce::Rectangle<float> avatar (tile.getCentreX() - avatarSize * 0.5f, y,
                                         avatarSize, avatarSize);

    g.setColour (medal.withAlpha (0.14f));
    g.fillEllipse (avatar);
    g.setColour (medal.withAlpha (0.55f));
    g.drawEllipse (avatar, 1.0f);

    const juce::Image face = avatarLookup != nullptr
                                 ? avatarLookup (player.userId)
                                 : juce::Image();

    if (face.isValid())
    {
        juce::Graphics::ScopedSaveState state (g);

        juce::Path faceClip;
        faceClip.addEllipse (avatar);
        g.reduceClipRegion (faceClip);

        g.setColour (juce::Colours::white);
        g.drawImage (face, avatar, juce::RectanglePlacement::fillDestination);
    }
    else
    {
        Ui::label (g, name.substring (0, 1).toUpperCase(), avatar,
                   Ui::semibold (juce::jmax (13.0f, avatarSize * 0.38f)), medal,
                   juce::Justification::centred);
    }

    y = avatar.getBottom() + 8.0f;

    const float nameW = tile.getWidth() - 16.0f;

    Ui::label (g, name, { tile.getX() + 8.0f, y, nameW, nameH },
               Ui::semibold (12.0f), Ui::Colours::text,
               juce::Justification::centred, true);

    y += nameH;

    Ui::label (g, "LVL " + juce::String (player.level),
               { tile.getX() + 8.0f, y, nameW, lvlH },
               Ui::font (9.0f, false, 0.08f), Ui::Colours::textMuted,
               juce::Justification::centred, true);

    y += lvlH + 4.0f;

    Ui::label (g, player.skillsText,
               { tile.getX() + 8.0f, y, nameW, skillsH },
               Ui::semibold (19.0f), medal,
               juce::Justification::centred, true);

    if (player.hasPositionChange && player.positionChange != 0)
    {
        const bool movedUp = player.positionChange > 0;

        Ui::label (g, (movedUp ? "+" : "") + juce::String (player.positionChange),
                   { tile.getX() + 6.0f, tile.getBottom() - baseH - 18.0f,
                     nameW, 14.0f },
                   Ui::font (10.0f, true),
                   movedUp ? Ui::Colours::success : Ui::Colours::danger,
                   juce::Justification::centredRight);
    }

    // Wide rank base, as in the design reference.
    if (baseH > 0.0f)
    {
        juce::Graphics::ScopedSaveState state (g);

        juce::Path clip;
        clip.addRoundedRectangle (tile, Ui::Radius::tile);
        g.reduceClipRegion (clip);

        const juce::Rectangle<float> base (tile.getX(), tile.getBottom() - baseH,
                                            tile.getWidth(), baseH);
        juce::Path basePath;
        basePath.addRoundedRectangle (base.getX(), base.getY(), base.getWidth(),
                                      base.getHeight() + 18.0f,
                                      Ui::Radius::tile, Ui::Radius::tile,
                                      true, true, false, false);
        g.setColour (medal.withAlpha (isFirst ? 0.22f : 0.12f));
        g.fillPath (basePath);

        Ui::label (g, juce::String (player.rank > 0 ? player.rank : index + 1), base,
                   Ui::semibold (20.0f), medal.withAlpha (0.85f),
                   juce::Justification::centred);
    }
}

void HomeView::paintBoard (juce::Graphics& g)
{
    const auto area = boardArea.toFloat();

    if (area.isEmpty())
        return;

    Ui::panel (g, area, Ui::Radius::card);

    // Trophy + title on the left, as in the reference.
    Ui::icon (g, Ui::Icon::trophy,
              { area.getX() + 20.0f, area.getY() + 15.0f, 20.0f, 20.0f },
              Ui::Colours::accent, 1.6f);

    Ui::label (g, I18n::tr ("GLOBAL LEADERBOARD"),
               { area.getX() + 48.0f, area.getY() + 14.0f, 320.0f, 22.0f },
               Ui::semibold (16.0f, 0.02f), Ui::Colours::text,
               juce::Justification::centredLeft, true);

    for (int slot = 0; slot < 3; ++slot)
        paintPodiumTile (g, slot);
}

void HomeView::paintOwnStrip (juce::Graphics& g)
{
    const auto area = ownArea.toFloat();

    if (area.isEmpty())
        return;

    Ui::well (g, area, Ui::Radius::tile, Ui::Colours::surfaceSunken.withAlpha (0.65f));

    const bool ranked = stats.ownRank > 0;

    g.setColour (Ui::Colours::accent.withAlpha (0.65f));
    g.fillRoundedRectangle (area.getX() + 1.0f, area.getY() + 12.0f,
                            3.0f, area.getHeight() - 24.0f, 1.5f);

    Ui::eyebrow (g, I18n::tr ("YOUR POSITION"),
                 { area.getX() + 16.0f, area.getY() + 12.0f, 220.0f, 11.0f },
                 Ui::Colours::textMuted);

    Ui::label (g, ranked ? "#" + juce::String (stats.ownRank) : juce::String ("-"),
               { area.getX() + 16.0f, area.getY() + 26.0f, 60.0f, 26.0f },
               Ui::semibold (19.0f, 0.01f),
               ranked ? Ui::Colours::accent : Ui::Colours::textMuted,
               juce::Justification::centredLeft, true);

    Ui::label (g, stats.username.isNotEmpty()
                      ? stats.username
                      : I18n::tr ("Not signed in"),
               { area.getX() + 62.0f, area.getY() + 30.0f,
                 juce::jmax (60.0f, area.getWidth() - 330.0f), 18.0f },
               Ui::semibold (12.5f), Ui::Colours::text,
               juce::Justification::centredLeft, true);

    struct Metric
    {
        const char* label;
        juce::String value;
    };

    const Metric metrics[2] = {
        { "SKILLS", stats.skillsText },
        { "DUELS", stats.duelsText }
    };
    for (int i = 0; i < 2; ++i)
    {
        const float x = area.getRight() - 210.0f + static_cast<float> (i) * 100.0f;

        Ui::eyebrow (g, I18n::tr (metrics[i].label),
                     { x, area.getY() + 14.0f, 90.0f, 11.0f },
                     Ui::Colours::textMuted);

        Ui::label (g, metrics[i].value,
                   { x, area.getY() + 28.0f, 90.0f, 24.0f },
                   Ui::semibold (15.0f, 0.01f), Ui::Colours::text,
                   juce::Justification::centredLeft, true);
    }
}

void HomeView::paint (juce::Graphics& g)
{
    if (heroBattle.isEmpty())
        return;

    paintHero (g, heroBattle.toFloat(), false, hoveredHero == 0);
    paintHero (g, heroOnline.toFloat(), true, hoveredHero == 1);
    paintStats (g);
    paintBoard (g);
    paintOwnStrip (g);
}

//==============================================================================
// Interaction
//==============================================================================

void HomeView::mouseMove (const juce::MouseEvent& event)
{
    int hero = -1;

    if (heroBattle.contains (event.getPosition()))
        hero = 0;
    else if (heroOnline.contains (event.getPosition()))
        hero = 1;
    else
    {
        for (int slot = 0; slot < 3; ++slot)
        {
            if (podiumPlayerIndex[static_cast<size_t> (slot)] >= 0
                && podiumTiles[static_cast<size_t> (slot)].contains (event.getPosition()))
            {
                hero = -2;
                break;
            }
        }
    }

    setMouseCursor (hero != -1 ? juce::MouseCursor::PointingHandCursor
                               : juce::MouseCursor::NormalCursor);

    if (hero < 0)
        hero = -1;

    if (hero == hoveredHero)
        return;

    hoveredHero = hero;
    repaint (heroBattle);
    repaint (heroOnline);
}

void HomeView::mouseExit (const juce::MouseEvent&)
{
    if (hoveredHero == -1)
        return;

    hoveredHero = -1;
    repaint (heroBattle);
    repaint (heroOnline);
}

void HomeView::mouseUp (const juce::MouseEvent& event)
{
    const auto position = event.getPosition();

    if (heroBattle.contains (position))
    {
        if (onBattle != nullptr)
            onBattle();

        return;
    }

    if (heroOnline.contains (position))
    {
        if (onOnline != nullptr)
            onOnline();

        return;
    }

    for (int slot = 0; slot < 3; ++slot)
    {
        const int index = podiumPlayerIndex[static_cast<size_t> (slot)];

        if (index < 0 || index >= static_cast<int> (players.size()))
            continue;

        if (! podiumTiles[static_cast<size_t> (slot)].contains (position))
            continue;

        const auto userId = players[static_cast<size_t> (index)].userId;

        if (userId.isNotEmpty() && onOpenProfile != nullptr)
            onOpenProfile (userId);

        return;
    }
}
