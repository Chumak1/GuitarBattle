#include "SectionSidebar.h"

#include "BrandAssets.h"
#include "Localisation.h"

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
    const int safeLevel = juce::jmax (1, level);

    if (userName == username && userLevel == safeLevel)
        return;

    userName = username;
    userLevel = safeLevel;
    repaint();
}

void SectionSidebar::setLanguage (I18n::Language newLanguage)
{
    if (language == newLanguage)
        return;

    language = newLanguage;
    repaint();
}

void SectionSidebar::resized()
{
    const auto bounds = getLocalBounds();

    brandBounds = { 10, 18, bounds.getWidth() - 20, 56 };

    const int rowH = 42;
    const int rowGap = 4;
    int y = brandBounds.getBottom() + 16;

    for (auto& group : groups)
    {
        group.labelY = y;
        y += 24;

        for (int i = 0; i < group.itemCount; ++i)
        {
            auto& item = items[static_cast<size_t> (group.firstItem + i)];
            item.bounds = { 10, y, bounds.getWidth() - 20, rowH };
            y += rowH + rowGap;
        }

        y += 10;
    }

    // Language switcher above the version line.
    const auto bottom = bounds.getBottom();
    const int chipW = 52;
    const int chipH = 26;
    const int chipsY = bottom - 60;

    languageBounds[0] = { 22, chipsY, chipW, chipH };
    languageBounds[1] = { 22 + chipW + 6, chipsY, chipW, chipH };
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
    for (int i = 0; i < 2; ++i)
    {
        if (! languageBounds[static_cast<size_t> (i)].contains (event.getPosition()))
            continue;

        const auto chosen = i == 0 ? I18n::Language::english
                                   : I18n::Language::russian;

        if (chosen == language)
            return;

        setLanguage (chosen);
        I18n::setLanguage (chosen);

        if (onLanguageSelected != nullptr)
            onLanguageSelected (chosen);

        if (auto* top = getTopLevelComponent())
            top->repaint();

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
    const int row = rowAt (event.getPosition());

    if (row == hovered)
        return;

    hovered = row;
    repaint();
}

void SectionSidebar::mouseExit (const juce::MouseEvent&)
{
    if (hovered < 0)
        return;

    hovered = -1;
    repaint();
}

void SectionSidebar::paintItem (juce::Graphics& g, const Item& item,
                                bool isActive, bool hoveredItem)
{
    const auto row = item.bounds.toFloat();

    if (isActive)
    {
        // Warm gold pill: the only saturated surface in the navigation.
        juce::ColourGradient gradient (Ui::Colours::accent,
                                       row.getX(), row.getY(),
                                       Ui::Colours::accentDeep,
                                       row.getX(), row.getBottom(),
                                       false);
        g.setGradientFill (gradient);
        g.fillRoundedRectangle (row, Ui::Radius::control);

        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.drawRoundedRectangle (row.reduced (0.5f), Ui::Radius::control, 1.0f);
    }
    else if (hoveredItem)
    {
        g.setColour (juce::Colours::white.withAlpha (0.045f));
        g.fillRoundedRectangle (row, Ui::Radius::control);
    }

    const auto ink = isActive ? Ui::Colours::onAccent
                              : (hoveredItem ? Ui::Colours::text
                                             : Ui::Colours::textSecondary);

    const float iconSize = 19.0f;
    Ui::icon (g, item.icon,
              { row.getX() + 13.0f, row.getCentreY() - iconSize * 0.5f,
                iconSize, iconSize },
              isActive ? ink : ink.withAlpha (hoveredItem ? 0.95f : 0.8f),
              1.65f);

    Ui::label (g, I18n::tr (item.label),
               { row.getX() + 45.0f, row.getY(),
                 row.getWidth() - 45.0f - 30.0f, row.getHeight() },
               Ui::semibold (13.5f, 0.04f), ink,
               juce::Justification::centredLeft, true);

    if (isActive || hoveredItem)
    {
        Ui::icon (g, Ui::Icon::chevronRight,
                  { row.getRight() - 26.0f, row.getCentreY() - 7.0f, 14.0f, 14.0f },
                  isActive ? ink.withAlpha (0.75f) : Ui::Colours::accent.withAlpha (0.7f),
                  1.7f);
    }
}

void SectionSidebar::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    g.setColour (Ui::Colours::shell);
    g.fillRect (bounds);

    // Very soft gold wash behind the brand, so the panel is not flat black.
    {
        juce::ColourGradient wash (Ui::Colours::accent.withAlpha (0.07f),
                                   bounds.getCentreX(), 0.0f,
                                   juce::Colours::transparentBlack,
                                   bounds.getCentreX(), bounds.getHeight() * 0.42f,
                                   true);
        g.setGradientFill (wash);
        g.fillRect (bounds.withHeight (bounds.getHeight() * 0.42f));
    }

    g.setColour (Ui::Colours::border);
    g.fillRect (bounds.getRight() - 1.0f, 0.0f, 1.0f, bounds.getHeight());

    // Brand / home button.
    {
        const auto brand = brandBounds.toFloat();
        const bool homeActive = active == AppSection::Home;

        if (homeActive)
        {
            g.setColour (Ui::Colours::accent.withAlpha (0.10f));
            g.fillRoundedRectangle (brand, Ui::Radius::control);
            g.setColour (Ui::Colours::accent.withAlpha (0.35f));
            g.drawRoundedRectangle (brand.reduced (0.5f), Ui::Radius::control, 1.0f);
        }

        const auto logo = juce::Rectangle<float> (brand.getX() + 10.0f,
                                                  brand.getCentreY() - 16.0f,
                                                  32.0f, 32.0f);
        BrandAssets::drawLogo (g, logo);

        Ui::label (g, "FRETFIGHT",
                   { brand.getX() + 52.0f, brand.getCentreY() - 17.0f,
                     brand.getWidth() - 58.0f, 20.0f },
                   Ui::semibold (15.5f, 0.08f), Ui::Colours::text,
                   juce::Justification::centredLeft, true);

        Ui::label (g, I18n::tr ("GUITAR BATTLES"),
                   { brand.getX() + 52.0f, brand.getCentreY() + 1.0f,
                     brand.getWidth() - 58.0f, 13.0f },
                   Ui::font (8.5f, false, 0.14f), Ui::Colours::textMuted,
                   juce::Justification::centredLeft, true);
    }

    // Grouped navigation.
    for (const auto& group : groups)
    {
        Ui::eyebrow (g, I18n::tr (group.label),
                     { 22.0f, static_cast<float> (group.labelY),
                       bounds.getWidth() - 32.0f, 12.0f },
                     Ui::Colours::textMuted.withAlpha (0.75f));

        for (int i = 0; i < group.itemCount; ++i)
        {
            const int index = group.firstItem + i;
            const auto& item = items[static_cast<size_t> (index)];
            paintItem (g, item, item.section == active, index == hovered);
        }
    }

    // Language switcher: EN / RU, the active one is the gold pill.
    Ui::eyebrow (g, I18n::tr ("Language"),
                 { 22.0f, static_cast<float> (languageBounds[0].getY()) - 16.0f,
                   140.0f, 11.0f },
                 Ui::Colours::textMuted.withAlpha (0.75f));

    for (int i = 0; i < 2; ++i)
    {
        const bool isActive = (i == 0) == (language == I18n::Language::english);
        const auto chip = languageBounds[static_cast<size_t> (i)].toFloat();

        if (isActive)
        {
            g.setColour (Ui::Colours::accent);
            g.fillRoundedRectangle (chip, 9.0f);
        }
        else
        {
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.fillRoundedRectangle (chip, 9.0f);
            g.setColour (Ui::Colours::borderStrong);
            g.drawRoundedRectangle (chip.reduced (0.5f), 9.0f, 1.0f);
        }

        Ui::label (g, i == 0 ? "EN" : "RU", chip,
                   Ui::semibold (10.5f, 0.1f),
                   isActive ? Ui::Colours::onAccent : Ui::Colours::textSecondary,
                   juce::Justification::centred);
    }

    // Version footer. The signed-in identity moved to the header chip, so the
    // menu ends with the build number only.
    auto footer = bounds.removeFromBottom (22.0f).reduced (22.0f, 0.0f);

    Ui::label (g, I18n::tr ("VERSION") + " " + juce::String (ProjectInfo::versionString),
               { footer.getX(), footer.getY(), footer.getWidth(), 12.0f },
               Ui::font (9.0f, false, 0.08f), Ui::Colours::textMuted.withAlpha (0.8f),
               juce::Justification::centredLeft, true);
}
