#include "HubHeader.h"

#include "Localisation.h"

#include <cmath>

namespace
{
    juce::String initialsFor (const juce::String& name)
    {
        const auto trimmed = name.trim();

        if (trimmed.isEmpty())
            return "?";

        return trimmed.substring (0, 1).toUpperCase();
    }
}

HubHeader::HubHeader()
{
    setOpaque (false);

    mailButton.onClick = [this] { if (onMessages != nullptr) onMessages(); };
    bellButton.onClick = [this] { if (onNotifications != nullptr) onNotifications(); };
    gearButton.onClick = [this] { if (onSettings != nullptr) onSettings(); };

    exitButton.setIcon (Ui::Icon::power);
    exitButton.setFontHeight (10.5f);
    exitButton.setButtonText (I18n::tr ("EXIT"));
    exitButton.onClick = [this] { if (onExit != nullptr) onExit(); };

    I18n::addLanguageListener ([safe = juce::Component::SafePointer<HubHeader> (this)]
    {
        if (auto* self = safe.getComponent())
        {
            self->exitButton.setButtonText (I18n::tr ("EXIT"));
            self->repaint();
        }
    });

    addAndMakeVisible (mailButton);
    addAndMakeVisible (bellButton);
    addAndMakeVisible (gearButton);
    addAndMakeVisible (exitButton);
}

void HubHeader::setSection (AppSection newSection)
{
    if (section == newSection)
        return;

    section = newSection;
    repaint();
}

void HubHeader::setUser (const juce::String& username, int level)
{
    const int safeLevel = juce::jmax (1, level);

    if (userName == username && userLevel == safeLevel)
        return;

    userName = username;
    userLevel = safeLevel;
    repaint();
}

void HubHeader::setAvatarImage (const juce::Image& image)
{
    if (avatarImage == image)
        return;

    avatarImage = image;
    repaint (playerArea);
}

void HubHeader::setUnreadNotifications (int count)
{
    bellButton.setBadge (juce::jmax (0, count));
}

void HubHeader::setUnreadMessages (int count)
{
    mailButton.setBadge (juce::jmax (0, count));
}

juce::Rectangle<int> HubHeader::getNotificationsBounds() const
{
    return bellButton.getBounds();
}

juce::String HubHeader::sectionTitle() const
{
    switch (section)
    {
        case AppSection::Profile:    return I18n::tr ("PROFILE");
        case AppSection::Campaign:   return I18n::tr ("BATTLE");
        case AppSection::Songs:      return I18n::tr ("PRACTICE");
        case AppSection::OnlineDuel: return I18n::tr ("ONLINE");
        case AppSection::Friends:    return I18n::tr ("FRIENDS");
        case AppSection::Messages:   return I18n::tr ("MESSAGES");
        case AppSection::Tuner:      return I18n::tr ("TUNER");
        case AppSection::Vst:        return "VST";
        case AppSection::Settings:   return I18n::tr ("SETTINGS");
        case AppSection::Home:
        default:                     return I18n::tr ("HOME");
    }
}

juce::String HubHeader::sectionSubtitle() const
{
    switch (section)
    {
        case AppSection::Home:
            return userName.isNotEmpty()
                       ? I18n::tr ("Welcome back") + ", " + userName + "!"
                       : I18n::tr ("Welcome to FretFight. Sign in to save your progress.");

        case AppSection::Profile:
            return I18n::tr ("Your account, characters and play history.");
        case AppSection::Campaign:
            return I18n::tr ("Climb the towers and take down the house band.");
        case AppSection::Songs:
            return I18n::tr ("Pick a song from the catalogue and drill it.");
        case AppSection::OnlineDuel:
            return I18n::tr ("Real-time duels against other players.");
        case AppSection::Friends:
            return I18n::tr ("Your crew, requests and activity.");
        case AppSection::Messages:
            return I18n::tr ("Direct messages with other players.");
        case AppSection::Tuner:
            return I18n::tr ("Tune up before the next run.");
        case AppSection::Vst:
            return I18n::tr ("Host and monitor your guitar plugins.");
        case AppSection::Settings:
            return I18n::tr ("Audio, account and application settings.");
        default:
            return {};
    }
}

juce::String HubHeader::sectionEyebrow() const
{
    switch (section)
    {
        case AppSection::Home:       return I18n::tr ("Welcome back");
        case AppSection::Campaign:
        case AppSection::OnlineDuel: return I18n::tr ("Play");
        case AppSection::Songs:      return I18n::tr ("TRAINING");
        case AppSection::Friends:    return I18n::tr ("COMMUNITY");
        case AppSection::Tuner:
        case AppSection::Vst:        return I18n::tr ("TOOLS");
        case AppSection::Profile:
        case AppSection::Messages:
        case AppSection::Settings:
        default:                     return "FretFight";
    }
}

void HubHeader::resized()
{
    const int centreY = getHeight() / 2;
    const int iconSize = 36;
    const int iconGap = 4;

    int right = getWidth() - 24;

    exitButton.setBounds (right - 84, centreY - 15, 84, 30);
    right -= 84 + 12;

    playerArea = { right - 196, centreY - 18, 196, 36 };
    right -= 196 + 14;

    dividerX = right;
    right -= 14;

    gearButton.setBounds (right - iconSize, centreY - iconSize / 2, iconSize, iconSize);
    right -= iconSize + iconGap;
    bellButton.setBounds (right - iconSize, centreY - iconSize / 2, iconSize, iconSize);
    right -= iconSize + iconGap;
    mailButton.setBounds (right - iconSize, centreY - iconSize / 2, iconSize, iconSize);
}

void HubHeader::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    Ui::panel (g, bounds, 16.0f);

    // Section name and description on the left.
    const float textWidth = juce::jmax (120.0f,
                                        static_cast<float> (dividerX) - 48.0f);

    const float textTop = juce::jmax (8.0f,
                                      (static_cast<float> (getHeight()) - 56.0f) * 0.5f);

    Ui::eyebrow (g, sectionEyebrow(),
                 { 24.0f, textTop, textWidth, 11.0f },
                 Ui::Colours::textMuted);

    Ui::label (g, sectionTitle(),
               { 24.0f, textTop + 11.0f, textWidth, 28.0f },
               Ui::semibold (24.0f, 0.01f), Ui::Colours::text,
               juce::Justification::centredLeft, true);

    Ui::label (g, sectionSubtitle(),
               { 24.0f, textTop + 38.0f, textWidth, 16.0f },
               Ui::font (11.5f), Ui::Colours::textSecondary,
               juce::Justification::centredLeft, true);

    // Separator between the action icons and the player chip.
    Ui::verticalDivider (g, static_cast<float> (dividerX),
                         20.0f, static_cast<float> (getHeight()) - 20.0f);

    // Player chip: avatar, name, level and a chevron that hints at the menu.
    const auto chip = playerArea.toFloat();

    if (playerHovered)
    {
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.fillRoundedRectangle (chip, Ui::Radius::control);
    }

    const float avatarSize = 32.0f;
    const juce::Rectangle<float> avatar (chip.getX() + 2.0f,
                                         chip.getCentreY() - avatarSize * 0.5f,
                                         avatarSize, avatarSize);

    juce::ColourGradient avatarFill (Ui::Colours::accentBright, avatar.getX(), avatar.getY(),
                                     Ui::Colours::accentDeep, avatar.getRight(), avatar.getBottom(),
                                     false);
    g.setGradientFill (avatarFill);
    g.fillEllipse (avatar);

    if (avatarImage.isValid())
    {
        juce::Graphics::ScopedSaveState state (g);

        juce::Path circle;
        circle.addEllipse (avatar);
        g.reduceClipRegion (circle);

        g.setColour (juce::Colours::white);
        g.drawImage (avatarImage, avatar,
                     juce::RectanglePlacement::fillDestination, true);

        g.setColour (Ui::Colours::accent.withAlpha (0.5f));
        g.drawEllipse (avatar.reduced (0.5f), 1.2f);
    }
    else
    {
        Ui::label (g, initialsFor (userName), avatar,
                   Ui::semibold (13.0f), Ui::Colours::onAccent,
                   juce::Justification::centred);
    }

    const float textX = avatar.getRight() + 10.0f;
    const float textW = chip.getRight() - textX - 22.0f;

    Ui::label (g, userName.isNotEmpty() ? userName : juce::String ("GUEST"),
               { textX, chip.getY() + 4.0f, textW, 16.0f },
               Ui::semibold (12.0f), Ui::Colours::text,
               juce::Justification::centredLeft, true);

    Ui::label (g, userName.isNotEmpty()
                      ? I18n::tr ("LEVEL") + " " + juce::String (userLevel)
                      : I18n::tr ("NOT SIGNED IN"),
               { textX, chip.getY() + 20.0f, textW, 13.0f },
               Ui::font (9.5f, false, 0.08f), Ui::Colours::textMuted,
               juce::Justification::centredLeft, true);

    Ui::icon (g, Ui::Icon::chevronDown,
              { chip.getRight() - 22.0f, chip.getCentreY() - 8.0f, 16.0f, 16.0f },
              playerHovered ? Ui::Colours::text : Ui::Colours::textMuted, 1.6f);
}

void HubHeader::mouseMove (const juce::MouseEvent& event)
{
    const bool overPlayer = playerArea.contains (event.getPosition());

    if (overPlayer == playerHovered)
        return;

    playerHovered = overPlayer;
    setMouseCursor (overPlayer ? juce::MouseCursor::PointingHandCursor
                               : juce::MouseCursor::NormalCursor);
    repaint();
}

void HubHeader::mouseExit (const juce::MouseEvent&)
{
    if (! playerHovered)
        return;

    playerHovered = false;
    repaint();
}

void HubHeader::mouseUp (const juce::MouseEvent& event)
{
    if (playerArea.contains (event.getPosition()))
        showPlayerMenu();
}

void HubHeader::showPlayerMenu()
{
    if (onPlayerMenu != nullptr)
        onPlayerMenu (playerArea);
}
