#include "DuelLobby.h"

#include "GameStyle.h"

DuelLobbyComponent::DuelLobbyComponent()
{
    createButton.setColour (juce::TextButton::buttonColourId, UiColors::accent);
    createButton.setColour (juce::TextButton::buttonOnColourId, UiColors::accentStrong);
    createButton.setColour (juce::TextButton::textColourOffId, UiColors::onAccent);
    createButton.setColour (juce::TextButton::textColourOnId, UiColors::onAccent);
    createButton.setTooltip ("Create an online room with an optional password.");
    createButton.setEnabled (false);
    createButton.onClick = [this]
    {
        if (onCreate != nullptr)
            onCreate();
    };
    addAndMakeVisible (createButton);

    refreshButton.setColour (juce::TextButton::buttonColourId, UiColors::surface2);
    refreshButton.setColour (juce::TextButton::textColourOffId, UiColors::accent);
    refreshButton.setTooltip ("Refresh the list of open online rooms.");
    refreshButton.onClick = [this]
    {
        if (onRefresh != nullptr)
            onRefresh();
    };
    addAndMakeVisible (refreshButton);
}

void DuelLobbyComponent::setRooms (std::vector<DuelLobbyRoom> newRooms)
{
    rooms = std::move (newRooms);
    status = signedIn
        ? (rooms.empty()
               ? "No open rooms. Press CREATE LOBBY to host one."
               : juce::String (static_cast<int> (rooms.size()))
                     + (rooms.size() == 1 ? " OPEN ROOM - click to join"
                                          : " OPEN ROOMS - click to join"))
        : "Sign in to see open rooms.";

    hoveredRow = -1;
    resized();
    repaint();
}

void DuelLobbyComponent::setStatus (const juce::String& text)
{
    status = text;
    repaint();
}

void DuelLobbyComponent::setSignedIn (bool isSignedIn)
{
    if (signedIn == isSignedIn)
        return;

    signedIn = isSignedIn;
    createButton.setEnabled (isSignedIn);

    if (! signedIn)
    {
        rooms.clear();
        status = "Sign in to see open rooms.";
    }

    repaint();
}

void DuelLobbyComponent::resized()
{
    auto area = getLocalBounds().reduced (16, 14);
    auto header = area.removeFromTop (40);

    // CREATE LOBBY is the primary action next to REFRESH; the list below is
    // the shared lobby for every signed-in player.
    auto buttons = header.removeFromRight (juce::jmin (380, header.getWidth()));
    createButton.setBounds (buttons.removeFromRight (220).reduced (0, 1));
    buttons.removeFromRight (10);
    refreshButton.setBounds (buttons.removeFromRight (130).reduced (0, 1));

    // Status line under the header.
    area.removeFromTop (22);
    area.removeFromTop (4);

    // Never more rows than rooms: an empty lobby still has a height, and a
    // stale rowCount would index past the vector in paint().
    constexpr int rowH = 58;
    rowCount = juce::jmin (static_cast<int> (rooms.size()),
                           juce::jmin (static_cast<int> (rowBounds.size()),
                                       juce::jmax (0, area.getHeight() / rowH)));

    for (int i = 0; i < static_cast<int> (rowBounds.size()); ++i)
    {
        if (i < rowCount)
            rowBounds[static_cast<size_t> (i)] = area.removeFromTop (rowH).reduced (0, 1);
        else
            rowBounds[static_cast<size_t> (i)] = {};
    }
}

void DuelLobbyComponent::paint (juce::Graphics& g)
{
    GameUi::drawCard (g, getLocalBounds().toFloat(), 14.0f, true);

    auto header = getLocalBounds().reduced (16, 14).removeFromTop (40);

    g.setColour (UiColors::accent);
    g.setFont (GameUi::eyebrow (13.0f));
    g.drawText ("ONLINE LOBBY", header, juce::Justification::centredLeft, false);

    g.setColour (UiColors::muted);
    g.setFont (GameUi::font (12.0f));
    g.drawText (status,
                juce::Rectangle<int> (16, 14 + 40, getWidth() - 32, 22),
                juce::Justification::centredLeft, true);

    const int visibleRows = juce::jmin (rowCount, static_cast<int> (rooms.size()));

    for (int i = 0; i < visibleRows; ++i)
    {
        const auto row = rowBounds[static_cast<size_t> (i)];
        const auto& room = rooms[static_cast<size_t> (i)];

        g.setColour (i == hoveredRow ? UiColors::accent.withAlpha (0.16f)
                                     : UiColors::surface2.withAlpha (0.55f));
        g.fillRoundedRectangle (row.toFloat(), 10.0f);

        g.setColour (UiColors::text);
        g.setFont (GameUi::font (15.0f, true));
        g.drawText ((room.hasPassword ? "[LOCK]  " : "") + room.hostName,
                    row.withTrimmedLeft (14).withWidth (row.getWidth() / 2 - 14),
                    juce::Justification::centredLeft, true);

        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (13.0f));
        g.drawText (room.songTitle,
                    juce::Rectangle<int> (row.getX() + row.getWidth() / 2,
                                          row.getY(), row.getWidth() / 2 - 120,
                                          row.getHeight()),
                    juce::Justification::centredLeft, true);

        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (12.5f, true));
        g.drawText (juce::String (room.playerCount) + "/2",
                    row.withTrimmedRight (130).removeFromRight (44),
                    juce::Justification::centredRight, false);

        g.setColour (UiColors::accent);
        g.setFont (GameUi::font (14.0f, true));
        g.drawText ("JOIN",
                    row.withTrimmedRight (16).removeFromRight (110),
                    juce::Justification::centredRight, false);
    }
}

void DuelLobbyComponent::mouseDown (const juce::MouseEvent& event)
{
    if (onJoin == nullptr)
        return;

    for (int i = 0; i < rowCount; ++i)
    {
        if (rowBounds[static_cast<size_t> (i)].contains (event.getPosition()))
        {
            if (i < static_cast<int> (rooms.size()))
                onJoin (rooms[static_cast<size_t> (i)].matchId,
                        rooms[static_cast<size_t> (i)].hasPassword);

            return;
        }
    }
}

void DuelLobbyComponent::mouseMove (const juce::MouseEvent& event)
{
    int hover = -1;

    for (int i = 0; i < rowCount; ++i)
        if (rowBounds[static_cast<size_t> (i)].contains (event.getPosition()))
            hover = i;

    if (hover != hoveredRow)
    {
        hoveredRow = hover;
        repaint();
    }
}

void DuelLobbyComponent::mouseExit (const juce::MouseEvent&)
{
    if (hoveredRow != -1)
    {
        hoveredRow = -1;
        repaint();
    }
}
