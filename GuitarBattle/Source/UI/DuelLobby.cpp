#include "DuelLobby.h"

#include "GameStyle.h"

DuelLobbyComponent::DuelLobbyComponent()
{
    refreshButton.setColour (juce::TextButton::buttonColourId, UiColors::surface2);
    refreshButton.setColour (juce::TextButton::textColourOffId, UiColors::accent);
    refreshButton.setTooltip ("Refresh the list of open duel rooms.");
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
               ? "No open rooms. Press DUEL - ONLINE MATCH to host one."
               : juce::String (static_cast<int> (rooms.size()))
                     + (rooms.size() == 1 ? " OPEN ROOM - click to join"
                                          : " OPEN ROOMS - click to join"))
        : "Sign in to see open duels.";

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

    if (! signedIn)
    {
        rooms.clear();
        status = "Sign in to see open duels.";
    }

    repaint();
}

void DuelLobbyComponent::resized()
{
    auto area = getLocalBounds().reduced (14, 12);
    auto header = area.removeFromTop (26);

    refreshButton.setBounds (header.removeFromRight (juce::jmin (92, header.getWidth() / 2))
                                   .reduced (0, 1));

    area.removeFromTop (4);
    // Never more rows than rooms: an empty lobby still has a height, and a
    // stale rowCount would index past the vector in paint().
    rowCount = juce::jmin (static_cast<int> (rooms.size()),
                           juce::jmin (static_cast<int> (rowBounds.size()),
                                       juce::jmax (0, area.getHeight() / 26)));

    for (int i = 0; i < static_cast<int> (rowBounds.size()); ++i)
    {
        if (i < rowCount)
            rowBounds[static_cast<size_t> (i)] = area.removeFromTop (26).reduced (0, 1);
        else
            rowBounds[static_cast<size_t> (i)] = {};
    }
}

void DuelLobbyComponent::paint (juce::Graphics& g)
{
    GameUi::drawCard (g, getLocalBounds().toFloat(), 14.0f, true);

    auto header = getLocalBounds().reduced (14, 12).removeFromTop (26);

    g.setColour (UiColors::accent);
    g.setFont (GameUi::eyebrow (10.0f));
    g.drawText ("DUEL LOBBY", header, juce::Justification::centredLeft, false);

    g.setColour (UiColors::muted);
    g.setFont (GameUi::font (10.0f));
    g.drawText (status,
                juce::Rectangle<int> (14, 38, getWidth() - 28, 16),
                juce::Justification::centredLeft, true);

    const int visibleRows = juce::jmin (rowCount, static_cast<int> (rooms.size()));

    for (int i = 0; i < visibleRows; ++i)
    {
        const auto row = rowBounds[static_cast<size_t> (i)];
        const auto& room = rooms[static_cast<size_t> (i)];

        g.setColour (i == hoveredRow ? UiColors::accent.withAlpha (0.16f)
                                     : UiColors::surface2.withAlpha (0.55f));
        g.fillRoundedRectangle (row.toFloat(), 6.0f);

        g.setColour (UiColors::text);
        g.setFont (GameUi::font (11.5f, true));
        g.drawText (room.songTitle,
                    row.withTrimmedLeft (10).withWidth (row.getWidth() / 2 - 10),
                    juce::Justification::centredLeft, true);

        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (10.5f));
        g.drawText (room.hostName + "  " + juce::String (room.hostRating),
                    juce::Rectangle<int> (row.getX() + row.getWidth() / 2,
                                          row.getY(), row.getWidth() / 2 - 110,
                                          row.getHeight()),
                    juce::Justification::centredLeft, true);

        g.setColour (UiColors::accent);
        g.setFont (GameUi::font (10.5f, true));
        g.drawText ("JOIN  " + juce::String (room.waitingSeconds) + "s",
                    row.withTrimmedRight (10).removeFromLeft (row.getWidth())
                       .removeFromRight (96),
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
                onJoin (rooms[static_cast<size_t> (i)].matchId);

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
