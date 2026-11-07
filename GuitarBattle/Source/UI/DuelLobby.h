#pragma once

#include <JuceHeader.h>

#include <array>
#include <functional>
#include <vector>

#include "../Network/DuelClient.h"

// Open online duel rooms, shown under the global rating on the song screen.
// The host creates a room with ONLINE MATCH; it appears here for everyone
// else, and clicking a row joins that room. Refresh is manual (the button)
// and the panel also refreshes on a timer from MainComponent.
class DuelLobbyComponent final : public juce::Component
{
public:
    DuelLobbyComponent();
    ~DuelLobbyComponent() override = default;

    std::function<void()> onRefresh;
    std::function<void (const juce::String& matchId)> onJoin;

    void setRooms (std::vector<DuelLobbyRoom> newRooms);
    void setStatus (const juce::String& text);
    void setSignedIn (bool isSignedIn);

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;

private:
    juce::TextButton refreshButton { "REFRESH" };

    std::vector<DuelLobbyRoom> rooms;
    juce::String status { "Sign in to see open duels." };
    bool signedIn = false;

    std::array<juce::Rectangle<int>, 6> rowBounds;
    int rowCount = 0;
    int hoveredRow = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DuelLobbyComponent)
};
