#include "PlayerMenu.h"

#include "Localisation.h"

PlayerMenuOverlay::PlayerMenuOverlay (juce::String headerText, bool signedIn)
    : title (std::move (headerText))
{
    setOpaque (false);
    setWantsKeyboardFocus (true);

    items = {
        { "PROFILE",  Ui::Icon::person, 1 },
        { "FEEDBACK", Ui::Icon::mail,   2 },
        { "DONATION", Ui::Icon::star,   3 }
    };

    if (signedIn)
        items.push_back ({ "LOG OUT", Ui::Icon::power, 4 });
}

void PlayerMenuOverlay::setAnchor (juce::Rectangle<int> anchorInParent)
{
    anchorArea = anchorInParent;
    resized();
}

void PlayerMenuOverlay::resized()
{
    const int separator = items.size() > 3 ? separatorHeight : 0;
    const int height = headerHeight
                       + static_cast<int> (items.size()) * itemHeight
                       + separator + bottomPadding;

    int x = anchorArea.getRight() - cardWidth;
    int y = anchorArea.getBottom() + 10;

    // Keep the card inside the window.
    x = juce::jlimit (8, juce::jmax (8, getWidth() - cardWidth - 8), x);
    y = juce::jlimit (8, juce::jmax (8, getHeight() - height - 8), y);

    cardArea = { x, y, cardWidth, height };
}

int PlayerMenuOverlay::itemAt (juce::Point<int> position) const
{
    for (int i = 0; i < static_cast<int> (items.size()); ++i)
        if (itemBounds (i).contains (position))
            return i;

    return -1;
}

juce::Rectangle<int> PlayerMenuOverlay::itemBounds (int index) const
{
    int y = cardArea.getY() + headerHeight;

    for (int i = 0; i < index; ++i)
    {
        y += itemHeight;

        // A separator sits between the last action and LOG OUT.
        if (i == 2 && static_cast<int> (items.size()) > 3)
            y += separatorHeight;
    }

    return { cardArea.getX() + 8, y, cardArea.getWidth() - 16, itemHeight };
}

void PlayerMenuOverlay::paint (juce::Graphics& g)
{
    if (cardArea.isEmpty())
        return;

    const auto card = cardArea.toFloat();

    Ui::panel (g, card, 16.0f);

    // Header: who is signed in.
    Ui::eyebrow (g, title,
                 { card.getX() + 16.0f, card.getY() + 14.0f,
                   card.getWidth() - 32.0f, 12.0f },
                 Ui::Colours::textMuted, juce::Justification::centredLeft);

    Ui::divider (g, card.getX() + 14.0f, card.getRight() - 14.0f,
                 static_cast<float> (cardArea.getY() + headerHeight - 1));

    for (int i = 0; i < static_cast<int> (items.size()); ++i)
    {
        const auto& item = items[static_cast<size_t> (i)];
        const auto row = itemBounds (i).toFloat().reduced (0.0f, 2.0f);
        const bool isHovered = i == hovered;
        const bool isDanger = item.id == 4;

        if (isHovered)
        {
            g.setColour (isDanger ? Ui::Colours::danger.withAlpha (0.12f)
                                  : Ui::Colours::accent.withAlpha (0.12f));
            g.fillRoundedRectangle (row, 10.0f);
        }

        const auto ink = isDanger ? Ui::Colours::danger
                                  : (isHovered ? Ui::Colours::text
                                               : Ui::Colours::textSecondary);

        Ui::icon (g, item.icon,
                  { row.getX() + 12.0f, row.getCentreY() - 9.0f, 18.0f, 18.0f },
                  ink, 1.6f);

        Ui::label (g, I18n::tr (item.label),
                   { row.getX() + 44.0f, row.getY(),
                     row.getWidth() - 54.0f, row.getHeight() },
                   Ui::semibold (11.5f, 0.06f), ink,
                   juce::Justification::centredLeft, true);
    }

    // Separator above LOG OUT.
    if (items.size() > 3)
    {
        const float y = static_cast<float> (itemBounds (3).getY()) - 5.0f;
        Ui::divider (g, card.getX() + 14.0f, card.getRight() - 14.0f, y);
    }
}

void PlayerMenuOverlay::mouseMove (const juce::MouseEvent& event)
{
    const int item = itemAt (event.getPosition());

    if (item == hovered)
        return;

    hovered = item;
    setMouseCursor (item >= 0 ? juce::MouseCursor::PointingHandCursor
                              : juce::MouseCursor::NormalCursor);
    repaint();
}

void PlayerMenuOverlay::mouseExit (const juce::MouseEvent&)
{
    if (hovered < 0)
        return;

    hovered = -1;
    repaint();
}

void PlayerMenuOverlay::mouseUp (const juce::MouseEvent& event)
{
    const int item = itemAt (event.getPosition());

    if (item < 0)
    {
        if (! cardArea.contains (event.getPosition()))
            dismiss();

        return;
    }

    // The action may rebuild the shell and destroy this overlay (for example
    // opening PROFILE changes the section). Copy the callbacks first so nothing
    // touches a dead object afterwards.
    std::function<void()> action;

    switch (items[static_cast<size_t> (item)].id)
    {
        case 1: action = onProfile;  break;
        case 2: action = onFeedback; break;
        case 3: action = onDonation; break;
        case 4: action = onLogout;   break;
        default: break;
    }

    const auto dismissCallback = onDismiss;

    if (action != nullptr)
        action();

    if (dismissCallback != nullptr)
        dismissCallback();
}

bool PlayerMenuOverlay::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        dismiss();
        return true;
    }

    return false;
}

void PlayerMenuOverlay::dismiss()
{
    if (onDismiss != nullptr)
        onDismiss();
}
