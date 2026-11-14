#include "PlayerCard.h"

#include "GameStyle.h"

#include <cmath>

namespace
{
    juce::String statValue (int value)
    {
        return value >= 0 ? juce::String (value) : juce::String ("-");
    }

    juce::String statValue (double value, int decimals)
    {
        return value >= 0.0 ? juce::String (value, decimals) : juce::String ("-");
    }

    juce::String initials (const juce::String& name)
    {
        auto trimmed = name.trim();

        if (trimmed.isEmpty())
            return "?";

        return trimmed.substring (0, 1).toUpperCase();
    }
}

PlayerCardComponent::PlayerCardComponent()
{
    setOpaque (false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void PlayerCardComponent::setMode (Mode newMode)
{
    if (mode == newMode)
        return;

    mode = newMode;
    resized();
    repaint();
}

void PlayerCardComponent::setData (const PlayerCardData& newData)
{
    data = newData;
    resized();
    repaint();
}

void PlayerCardComponent::setRelation (PlayerRelation newRelation)
{
    relation = newRelation;

    if (relation == PlayerRelation::Self)
        challengeEnabled = false;

    resized();
    repaint();
}

void PlayerCardComponent::setChallengeEnabled (bool enabled)
{
    challengeEnabled = enabled;
    resized();
    repaint();
}

void PlayerCardComponent::setActionHandler (std::function<void (Action)> handler)
{
    onAction = std::move (handler);
}

int PlayerCardComponent::preferredHeight (Mode mode)
{
    switch (mode)
    {
        case Mode::Compact:  return 92;
        case Mode::Standard: return 152;
        case Mode::Expanded: return 252;
        case Mode::Preview:  return 332;
    }

    return 88;
}

std::vector<PlayerCardComponent::ActionChip> PlayerCardComponent::buildActions() const
{
    std::vector<ActionChip> result;

    auto add = [&result] (Action action, juce::String label, bool primary)
    {
        ActionChip chip;
        chip.action = action;
        chip.label = std::move (label);
        chip.primary = primary;
        result.push_back (std::move (chip));
    };

    // Only the actions that are valid for the current relationship: the card
    // never offers "Add Friend" to a friend or "Challenge" to a stranger.
    switch (relation)
    {
        case PlayerRelation::Self:
            add (mode == Mode::Preview ? Action::ViewProfile : Action::EditProfile,
                 mode == Mode::Preview ? "VIEW MY PROFILE" : "EDIT PROFILE", true);
            break;

        case PlayerRelation::Blocked:
            break;

        case PlayerRelation::IncomingRequest:
            add (Action::AcceptRequest, "ACCEPT", true);
            add (Action::DeclineRequest, "DECLINE", false);

            if (mode != Mode::Compact)
                add (Action::ViewProfile, "VIEW PROFILE", false);
            break;

        case PlayerRelation::OutgoingRequest:
            add (Action::CancelRequest, "CANCEL REQUEST", false);
            break;

        case PlayerRelation::Friend:
            add (Action::Challenge, "CHALLENGE", true);
            add (Action::Compare, "COMPARE", false);

            if (mode == Mode::Expanded || mode == Mode::Preview)
                add (Action::RemoveFriend, "REMOVE", false);
            break;

        case PlayerRelation::None:
            add (Action::AddFriend, "ADD FRIEND", true);

            if (mode != Mode::Compact)
                add (Action::ViewProfile, "VIEW PROFILE", false);
            break;
    }

    if (mode == Mode::Preview && relation != PlayerRelation::Self
        && relation != PlayerRelation::Blocked)
        add (Action::ViewProfile, "OPEN FULL PROFILE", ! result.empty() ? false : true);

    if (mode == Mode::Expanded && relation != PlayerRelation::Blocked
        && relation != PlayerRelation::Self)
        add (Action::Block, "BLOCK", false);

    if (mode == Mode::Compact && result.size() > 1)
        result.resize (1);

    return result;
}

void PlayerCardComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    GameUi::drawCard (g, bounds, 16.0f, mode != Mode::Compact);

    // Accessibility / profile-view highlight: the whole card is clickable.
    g.setColour (UiColors::accent.withAlpha (hoveredChip >= 0 ? 0.26f : 0.16f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 16.0f, 1.0f);

    auto area = getLocalBounds().reduced (mode == Mode::Compact ? 12 : 16);
    const int avatarSize = mode == Mode::Compact ? 52
                          : (mode == Mode::Standard ? 72 : 92);
    auto avatar = area.removeFromLeft (avatarSize).withHeight (avatarSize).toFloat();
    paintAvatar (g, avatar);

    auto right = area.withTrimmedLeft (mode == Mode::Compact ? 12 : 16);

    // --- Name + relationship badge ---
    const juce::String name = data.name();
    auto nameRow = right.removeFromTop (name.isEmpty() ? 0 : (mode == Mode::Compact ? 20 : 24));

    g.setColour (UiColors::text);
    g.setFont (mode == Mode::Compact ? GameUi::font (15.0f, true)
                                     : GameUi::heading (19.0f));
    g.drawFittedText (name, nameRow.withWidth (nameRow.getWidth() - 4),
                      juce::Justification::centredLeft, 1, 1.0f);

    if (data.username.isNotEmpty() && data.username != name && mode != Mode::Compact)
    {
        auto handleRow = right.removeFromTop (14);
        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (10.0f));
        g.drawText ("@" + data.username, handleRow, juce::Justification::centredLeft, false);
    }

    // --- Status + level + rating row ---
    auto metaRow = right.removeFromTop (mode == Mode::Compact ? 18 : 22);
    auto statusArea = metaRow.removeFromLeft (juce::jmin (110, metaRow.getWidth() / 3));
    paintStatus (g, statusArea.toFloat());

    metaRow.removeFromLeft (8);
    auto levelArea = metaRow.removeFromLeft (juce::jmin (86, metaRow.getWidth() / 2));
    paintLevel (g, levelArea.toFloat());

    paintRating (g, metaRow.toFloat());

    // --- Mode specific body ---
    if (mode != Mode::Compact)
    {
        right.removeFromTop (mode == Mode::Standard ? 8 : 10);

        if (mode == Mode::Preview)
        {
            auto characterRow = right.removeFromTop (18);

            g.setColour (UiColors::muted);
            g.setFont (GameUi::font (10.0f));
            g.drawText (data.characterName.isNotEmpty()
                            ? juce::String ("CHARACTER  ") + data.characterName
                            : juce::String ("CHARACTER  -"),
                        characterRow, juce::Justification::centredLeft, false);
        }

        const int statCount = mode == Mode::Standard ? 4 : 6;
        const int columns = mode == Mode::Standard ? 4 : 6;
        auto statsRow = right.removeFromTop (mode == Mode::Standard ? 42 : 46);
        const float cellW = static_cast<float> (statsRow.getWidth())
                            / static_cast<float> (juce::jmax (1, columns));

        struct StatEntry
        {
            const char* caption;
            juce::String value;
        };

        const StatEntry entries[] = {
            { "WINS", statValue (data.wins) },
            { "LOSSES", statValue (data.losses) },
            { "WIN RATE", data.winRate >= 0.0
                              ? juce::String (juce::roundToInt (data.winRate * 100.0)) + "%"
                              : juce::String ("-") },
            { "DUELS", statValue (data.duelsOnline) },
            { "ACCURACY", data.accuracy >= 0.0
                              ? juce::String (juce::roundToInt (data.accuracy)) + "%"
                              : juce::String ("-") },
            { "ACHIEVEMENTS", data.achievementsUnlocked >= 0
                                  ? juce::String (data.achievementsUnlocked)
                                        + (data.achievementsTotal > 0
                                               ? "/" + juce::String (data.achievementsTotal)
                                               : juce::String())
                                  : juce::String ("-") },
        };

        for (int i = 0; i < statCount; ++i)
        {
            const auto cell = juce::Rectangle<float> (
                statsRow.getX() + cellW * static_cast<float> (i),
                static_cast<float> (statsRow.getY()), cellW,
                static_cast<float> (statsRow.getHeight()));
            paintStat (g, cell, entries[i].caption, entries[i].value);
        }

        if (mode == Mode::Expanded || mode == Mode::Preview)
        {
            auto extraRow = right.removeFromTop (34);
            const float cellW2 = static_cast<float> (extraRow.getWidth()) / 3.0f;

            paintStat (g, { static_cast<float> (extraRow.getX()),
                            static_cast<float> (extraRow.getY()),
                            cellW2, static_cast<float> (extraRow.getHeight()) },
                       "SONGS MASTERED", statValue (data.songsMastered));
            paintStat (g, { static_cast<float> (extraRow.getX()) + cellW2,
                            static_cast<float> (extraRow.getY()),
                            cellW2, static_cast<float> (extraRow.getHeight()) },
                       "TOWERS", statValue (data.towersCompleted));
            paintStat (g, { static_cast<float> (extraRow.getX()) + cellW2 * 2.0f,
                            static_cast<float> (extraRow.getY()),
                            cellW2, static_cast<float> (extraRow.getHeight()) },
                       "PEAK RATING", data.peakRating > 0
                                          ? juce::String (data.peakRating)
                                          : juce::String ("-"));
        }
    }

    // --- Action chips (bottom row) ---
    for (const auto& chip : chips)
        paintChip (g, chip);
}

void PlayerCardComponent::paintAvatar (juce::Graphics& g, juce::Rectangle<float> area) const
{
    g.setColour (UiColors::surface2.withAlpha (0.8f));
    g.fillRoundedRectangle (area, 12.0f);

    if (data.characterSheet.isValid())
    {
        const auto& sheet = data.characterSheet;
        const int fw = sheet.getWidth() / 4;
        const int fh = sheet.getHeight() / 2;

        if (fw > 0 && fh > 0)
        {
            const float targetW = area.getWidth() - 8.0f;
            const float targetH = area.getHeight() - 8.0f;
            const auto target = area.withSizeKeepingCentre (targetW, targetH);
            g.drawImage (sheet, (int) target.getX(), (int) target.getY(),
                         (int) target.getWidth(), (int) target.getHeight(),
                         0, 0, fw, fh);
            return;
        }
    }

    // Placeholder: the first letter of the nickname on a soft accent disc.
    auto disc = area.withSizeKeepingCentre (area.getWidth() * 0.58f,
                                            area.getHeight() * 0.58f);
    g.setColour (UiColors::accent.withAlpha (0.16f));
    g.fillEllipse (disc);
    g.setColour (UiColors::accent);
    g.setFont (GameUi::heading (area.getHeight() * 0.42f));
    g.drawText (initials (data.name()), disc.toNearestInt(),
                juce::Justification::centred, false);

    g.setColour (UiColors::border);
    g.drawRoundedRectangle (area.reduced (0.5f), 12.0f, 1.0f);
}

void PlayerCardComponent::paintStatus (juce::Graphics& g, juce::Rectangle<float> area) const
{
    if (data.presence == PlayerCardData::Presence::Unknown)
        return;

    juce::String label;
    juce::Colour colour = UiColors::muted;

    switch (data.presence)
    {
        case PlayerCardData::Presence::Online:
            label = "ONLINE";
            colour = UiColors::green;
            break;
        case PlayerCardData::Presence::InLobby:
            label = "IN LOBBY";
            colour = UiColors::accent;
            break;
        case PlayerCardData::Presence::InMatch:
            label = "IN MATCH";
            colour = UiColors::orange;
            break;
        case PlayerCardData::Presence::Offline:
            label = "OFFLINE";
            colour = UiColors::muted;
            break;
        case PlayerCardData::Presence::Unknown:
            return;
    }

    const float dot = 6.0f;
    g.setColour (colour);
    g.fillEllipse (area.getX(), area.getCentreY() - dot * 0.5f, dot, dot);

    g.setColour (colour);
    g.setFont (GameUi::eyebrow (8.5f));
    g.drawText (label, area.withTrimmedLeft (dot + 6.0f),
                juce::Justification::centredLeft, false);
}

void PlayerCardComponent::paintLevel (juce::Graphics& g, juce::Rectangle<float> area) const
{
    if (data.level <= 0)
        return;

    g.setColour (UiColors::muted);
    g.setFont (GameUi::eyebrow (8.5f));
    g.drawText ("LV", area, juce::Justification::centredLeft, false);

    g.setColour (UiColors::text);
    g.setFont (GameUi::font (12.0f, true));
    g.drawText (juce::String (data.level), area.withTrimmedLeft (20.0f),
                juce::Justification::centredLeft, false);
}

void PlayerCardComponent::paintRating (juce::Graphics& g, juce::Rectangle<float> area) const
{
    if (area.getWidth() < 40.0f)
        return;

    g.setColour (UiColors::muted);
    g.setFont (GameUi::eyebrow (8.5f));
    g.drawText ("RATING", area, juce::Justification::centredLeft, false);

    const float captionW = 46.0f;
    g.setColour (data.rating > 0 ? UiColors::accent : UiColors::muted);
    g.setFont (GameUi::font (12.0f, true));
    g.drawText (data.rating > 0 ? juce::String (data.rating)
                                : juce::String ("UNRANKED"),
                area.withTrimmedLeft (captionW), juce::Justification::centredLeft,
                false);
}

void PlayerCardComponent::paintStat (juce::Graphics& g, juce::Rectangle<float> area,
                                     const juce::String& caption,
                                     const juce::String& value) const
{
    auto inner = area.reduced (6.0f, 2.0f);
    g.setColour (UiColors::surface2.withAlpha (0.45f));
    g.fillRoundedRectangle (inner, 8.0f);

    g.setColour (UiColors::muted);
    g.setFont (GameUi::eyebrow (7.5f));
    g.drawText (caption, inner.reduced (8.0f, 0.0f).withTrimmedTop (5.0f)
                                  .withHeight (11.0f),
                juce::Justification::centredLeft, false);

    g.setColour (value == "-" ? UiColors::muted : UiColors::text);
    g.setFont (GameUi::font (13.0f, true));
    g.drawText (value, inner.reduced (8.0f, 0.0f).withTrimmedTop (16.0f),
                juce::Justification::centredLeft, false);
}

void PlayerCardComponent::paintChip (juce::Graphics& g, const ActionChip& chip) const
{
    const bool hovered = chip.bounds.contains (getMouseXYRelative());
    const bool disabled = chip.action == Action::Challenge && ! challengeEnabled;

    auto area = chip.bounds.toFloat();

    juce::Colour fill = chip.primary ? UiColors::accent : UiColors::surface2;
    juce::Colour ink = chip.primary ? UiColors::onAccent : UiColors::text;

    if (disabled)
    {
        fill = UiColors::surface2.withAlpha (0.5f);
        ink = UiColors::muted;
    }
    else if (hovered)
    {
        fill = chip.primary ? UiColors::accentStrong : UiColors::surface2.brighter (0.12f);
    }

    g.setColour (fill);
    g.fillRoundedRectangle (area, 9.0f);
    g.setColour (ink);
    g.setFont (GameUi::font (10.5f, true));
    g.drawFittedText (chip.label, chip.bounds, juce::Justification::centred, 1, 1.0f);
}

void PlayerCardComponent::layoutActionChips (std::vector<ActionChip>& target,
                                             juce::Rectangle<int> area) const
{
    if (target.empty())
        return;

    const int gap = 8;
    const int height = 26;
    auto row = area.removeFromBottom (height);

    // Right-aligned chips; the primary action ends up rightmost.
    int totalWidth = 0;

    for (const auto& chip : target)
        totalWidth += juce::jlimit (72, 170, 16 + (int) chip.label.length() * 7) + gap;

    totalWidth = juce::jmin (totalWidth - gap, row.getWidth());
    int x = row.getRight() - totalWidth - 0;

    for (auto& chip : target)
    {
        const int width = juce::jlimit (72, 170, 16 + (int) chip.label.length() * 7);
        chip.bounds = { x, row.getY(), width, height };
        x += width + gap;
    }
}

void PlayerCardComponent::resized()
{
    chips = buildActions();

    if (chips.empty())
        return;

    auto area = getLocalBounds().reduced (mode == Mode::Compact ? 12 : 16);
    layoutActionChips (chips, area);
}

void PlayerCardComponent::mouseMove (const juce::MouseEvent& event)
{
    int newHovered = -1;

    for (int i = 0; i < static_cast<int> (chips.size()); ++i)
        if (chips[static_cast<size_t> (i)].bounds.contains (event.getPosition()))
            newHovered = i;

    if (newHovered != hoveredChip)
    {
        hoveredChip = newHovered;
        repaint();
    }
}

void PlayerCardComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoveredChip >= 0)
    {
        hoveredChip = -1;
        repaint();
    }
}

void PlayerCardComponent::mouseUp (const juce::MouseEvent& event)
{
    for (const auto& chip : chips)
    {
        if (! chip.bounds.contains (event.getPosition()))
            continue;

        if (chip.action == Action::Challenge && ! challengeEnabled)
            return;

        if (onAction != nullptr)
            onAction (chip.action);

        return;
    }

    // Clicking anywhere else opens the profile (accessibility requirement:
    // the card itself is the profile affordance).
    if (onAction != nullptr && relation != PlayerRelation::Blocked)
        onAction (Action::ViewProfile);
}
