#include "SectionSidebar.h"

#include "BrandAssets.h"
#include "GameStyle.h"

SectionSidebar::SectionSidebar()
{
    setOpaque (false);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void SectionSidebar::setActiveSection (AppSection section)
{
    if (active == section)
        return;

    active = section;
    repaint();
}

void SectionSidebar::setUser (const juce::String& username, int level)
{
    if (userName == username && userLevel == juce::jmax (1, level))
        return;

    userName = username;
    userLevel = juce::jmax (1, level);
    repaint();
}

void SectionSidebar::setUnreadNotifications (int count)
{
    count = juce::jmax (0, count);

    if (unreadNotifications == count)
        return;

    unreadNotifications = count;
    repaint();
}

void SectionSidebar::setUnreadMessages (int count)
{
    count = juce::jmax (0, count);

    if (unreadMessages == count)
        return;

    unreadMessages = count;
    repaint();
}

void SectionSidebar::resized()
{
    const auto bounds = getLocalBounds();

    brandBounds = bounds.withHeight (96).reduced (10, 10);

    const int rowH = 46;
    const int gap = 4;
    int y = brandBounds.getBottom() + 18;

    for (auto& item : items)
    {
        item.bounds = juce::Rectangle<int> (12, y, bounds.getWidth() - 24, rowH);
        y += rowH + gap;
    }

    // The bell lives just above the signed-in footer, so notifications are one
    // click away from every section.
    notificationsBounds = juce::Rectangle<int> (12, bounds.getBottom() - 78 - 54,
                                                bounds.getWidth() - 24, 42);
}

int SectionSidebar::rowAt (juce::Point<int> position) const
{
    for (int i = 0; i < static_cast<int> (items.size()); ++i)
        if (items[static_cast<size_t> (i)].bounds.contains (position))
            return i;

    return -1;
}

void SectionSidebar::select (AppSection section)
{
    if (active != section)
    {
        active = section;
        repaint();
    }

    if (onSectionSelected != nullptr)
        onSectionSelected (section);
}

void SectionSidebar::mouseDown (const juce::MouseEvent& event)
{
    if (notificationsBounds.contains (event.getPosition()))
    {
        if (onNotificationsSelected != nullptr)
            onNotificationsSelected();

        return;
    }

    if (brandBounds.contains (event.getPosition()))
    {
        select (AppSection::Home);
        return;
    }

    const int row = rowAt (event.getPosition());

    if (row >= 0)
        select (items[static_cast<size_t> (row)].section);
}

void SectionSidebar::mouseMove (const juce::MouseEvent& event)
{
    const bool overNotifications = notificationsBounds.contains (event.getPosition());

    if (overNotifications != notificationsHovered)
    {
        notificationsHovered = overNotifications;
        repaint();
    }

    const int row = rowAt (event.getPosition());

    if (row == hovered)
        return;

    hovered = row;
    repaint();
}

void SectionSidebar::mouseExit (const juce::MouseEvent&)
{
    if (hovered < 0 && ! notificationsHovered)
        return;

    hovered = -1;
    notificationsHovered = false;
    repaint();
}

void SectionSidebar::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    // Deep panel with a soft accent glow behind the brand.
    g.setColour (UiColors::background);
    g.fillRect (bounds);

    juce::ColourGradient glow (UiColors::accent.withAlpha (0.10f),
                               bounds.getCentreX(), 20.0f,
                               juce::Colours::transparentBlack,
                               bounds.getCentreX(), bounds.getHeight() * 0.55f,
                               true);
    g.setGradientFill (glow);
    g.fillRect (bounds.withHeight (bounds.getHeight() * 0.55f));

    g.setColour (UiColors::border.withAlpha (0.65f));
    g.fillRect (bounds.getRight() - 1.0f, 0.0f, 1.0f, bounds.getHeight());

    // Brand / Home button.
    {
        const auto brand = brandBounds.toFloat();
        const bool homeActive = active == AppSection::Home;

        g.setColour (UiColors::accent.withAlpha (homeActive ? 0.16f : 0.08f));
        g.fillRoundedRectangle (brand, 16.0f);

        if (homeActive)
        {
            g.setColour (UiColors::accent.withAlpha (0.65f));
            g.drawRoundedRectangle (brand.reduced (0.5f), 16.0f, 1.0f);
        }

        const auto logo = juce::Rectangle<float> (brand.getX() + 10.0f,
                                                  brand.getY() + 16.0f,
                                                  38.0f, 38.0f);
        BrandAssets::drawLogo (g, logo);

        g.setColour (UiColors::text);
        g.setFont (GameUi::heading (17.0f));
        g.drawFittedText ("FRETFIGHT",
                          juce::Rectangle<int> (static_cast<int> (brand.getX() + 56.0f),
                                                static_cast<int> (brand.getY() + 22.0f),
                                                static_cast<int> (brand.getWidth() - 62.0f),
                                                28),
                          juce::Justification::centredLeft, 1);
    }

    g.setColour (UiColors::muted);
    g.setFont (GameUi::eyebrow (8.0f));
    g.drawText ("",
                juce::Rectangle<int> (20, items.front().bounds.getY() - 22,
                                      getWidth() - 40, 14),
                juce::Justification::centredLeft, false);

    for (int i = 0; i < static_cast<int> (items.size()); ++i)
    {
        const auto& item = items[static_cast<size_t> (i)];
        const auto row = item.bounds.toFloat();
        const bool isActive = item.section == active;
        const bool isHovered = i == hovered;

        if (isActive)
        {
            // Admin nav language: a warm gold gradient pill with dark ink.
            const juce::Colour top (0xFFF3C451);
            const juce::Colour bottom (0xFFE0A72C);

            g.setColour (top.withAlpha (0.12f));
            g.fillRoundedRectangle (row.expanded (2.0f), 12.0f);

            juce::ColourGradient gradient (top, row.getX(), row.getY(),
                                           bottom, row.getX(), row.getBottom(),
                                           false);
            g.setGradientFill (gradient);
            g.fillRoundedRectangle (row, 10.0f);
        }
        else if (isHovered)
        {
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.fillRoundedRectangle (row, 10.0f);
        }

        auto textBounds = item.bounds.reduced (16, 0);
        const auto textColour = isActive ? UiColors::onAccent
                                         : (isHovered ? UiColors::text
                                                      : UiColors::muted);

        g.setColour (textColour);
        g.setFont (GameUi::font (12.5f, true));
        g.drawFittedText (item.label,
                          textBounds,
                          juce::Justification::centredLeft, 1);

        // Unread direct messages: a compact gold counter on the right of the
        // MESSAGES row, drawn in the same ink as the active label.
        if (item.section == AppSection::Messages && unreadMessages > 0)
        {
            const auto badge = juce::Rectangle<float> (
                static_cast<float> (item.bounds.getRight()) - 44.0f,
                static_cast<float> (item.bounds.getCentreY()) - 9.0f,
                32.0f, 18.0f);

            g.setColour (isActive ? UiColors::onAccent.withAlpha (0.85f)
                                  : UiColors::accent);
            g.fillRoundedRectangle (badge, 9.0f);

            g.setColour (isActive ? UiColors::accent : UiColors::onAccent);
            g.setFont (GameUi::font (10.0f, true));
            g.drawText (juce::String (juce::jmin (unreadMessages, 99)), badge,
                        juce::Justification::centred, false);
        }
    }

    // Notifications bell with the unread badge.
    {
        const auto row = notificationsBounds.toFloat();

        if (notificationsHovered)
        {
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.fillRoundedRectangle (row, 10.0f);
        }

        const auto ink = unreadNotifications > 0 ? UiColors::accent
                                                 : (notificationsHovered
                                                        ? UiColors::text
                                                        : UiColors::muted);
        const float cx = row.getX() + 23.0f;
        const float cy = row.getCentreY();

        // Bell: dome, brim and clapper.
        g.setColour (ink);
        g.fillRoundedRectangle (cx - 6.5f, cy - 8.0f, 13.0f, 11.0f, 5.0f);
        g.fillRoundedRectangle (cx - 8.5f, cy + 1.0f, 17.0f, 3.0f, 1.5f);
        g.fillEllipse (cx - 2.0f, cy + 4.0f, 4.0f, 4.0f);

        g.setFont (GameUi::font (11.5f, true));
        g.setColour (unreadNotifications > 0 ? UiColors::text : UiColors::muted);
        g.drawFittedText ("NOTIFICATIONS",
                          notificationsBounds.withTrimmedLeft (44).withTrimmedRight (34),
                          juce::Justification::centredLeft, 1);

        if (unreadNotifications > 0)
        {
            const auto badge = juce::Rectangle<float> (
                row.getRight() - 30.0f, cy - 9.0f, 22.0f, 18.0f);

            g.setColour (UiColors::accent);
            g.fillRoundedRectangle (badge, badge.getHeight() * 0.5f);

            g.setColour (UiColors::onAccent);
            g.setFont (GameUi::font (10.0f, true));
            g.drawText (juce::String (juce::jmin (unreadNotifications, 99)),
                        badge, juce::Justification::centred, false);
        }
    }

    // Signed-in summary at the bottom. The full profile lives in Profile.
    auto footer = bounds.removeFromBottom (78.0f).reduced (16.0f, 10.0f);

    g.setColour (UiColors::border.withAlpha (0.5f));
    g.fillRect (footer.getX(), footer.getY() - 6.0f, footer.getWidth(), 1.0f);

    g.setColour (UiColors::muted);
    g.setFont (GameUi::eyebrow (8.0f));
    g.drawText (userName.isNotEmpty() ? "SIGNED IN AS" : "NOT SIGNED IN",
                footer.removeFromTop (14.0f),
                juce::Justification::centredLeft, false);

    if (userName.isNotEmpty())
    {
        g.setColour (UiColors::text);
        g.setFont (GameUi::font (12.0f, true));
        g.drawFittedText (userName, footer.removeFromTop (20.0f).toNearestInt(),
                          juce::Justification::centredLeft, 1);
    }

    // The running build is always visible at the bottom of the menu: it is the
    // number to quote in bug reports and the one the update check compares
    // against the server.
    g.setColour (UiColors::muted);
    g.setFont (GameUi::font (10.0f, true));
    g.drawText ("VERSION " + juce::String (ProjectInfo::versionString),
                footer.removeFromTop (16.0f),
                juce::Justification::centredLeft, false);
}
