#pragma once

#include <JuceHeader.h>

#include "UiKit.h"

#include <functional>
#include <vector>

//==============================================================================
// Player chip dropdown.
//
// A full-window transparent layer that hosts one rounded card under the
// header chip. It replaces juce::PopupMenu on purpose: the system popup paints
// a square window, while this card has the same 16 px radius, hairline border
// and shadow as every other surface of the hub.
//
// A click outside the card, Escape or picking an item closes it.
class PlayerMenuOverlay final : public juce::Component
{
public:
    PlayerMenuOverlay (juce::String headerText, bool signedIn);

    std::function<void()> onProfile;
    std::function<void()> onFeedback;
    std::function<void()> onDonation;
    std::function<void()> onLogout;
    std::function<void()> onDismiss;

    // Anchor is the chip rectangle in this component's coordinates.
    void setAnchor (juce::Rectangle<int> anchorInParent);

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    struct Item
    {
        const char* label;
        Ui::Icon icon;
        int id;
    };

    void dismiss();
    int itemAt (juce::Point<int> position) const;
    juce::Rectangle<int> itemBounds (int index) const;

    static constexpr int cardWidth = 236;
    static constexpr int headerHeight = 52;
    static constexpr int itemHeight = 38;
    static constexpr int separatorHeight = 9;
    static constexpr int bottomPadding = 8;

    juce::String title;
    std::vector<Item> items;
    juce::Rectangle<int> anchorArea;
    juce::Rectangle<int> cardArea;
    int hovered = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlayerMenuOverlay)
};
