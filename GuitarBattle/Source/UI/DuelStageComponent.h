#pragma once

#include <JuceHeader.h>

#include <atomic>

// Pixel-art backdrop for Guitar Duels. It sits above the full-width
// tablature: the stage background plus the two guitarists.
//
// Animation:
//   * idle  - the two standing poses (frames 0 and 1) alternate every 0.8 s;
//   * hit   - all eight poses play once at 0.5 s per pose (4 s total), then
//             the guitarist returns to idle.
// Poses never cross-fade: every frame is drawn fully opaque, so the pixel art
// stays crisp instead of showing semi-transparent ghost frames.
// The left guitarist reacts to the player's hits, the right one to the
// opponent's. Note resolution happens on the pitch-analysis thread, so the
// trigger methods only touch atomics; the message-thread timer starts the
// animation.
class DuelStageComponent final : public juce::Component,
                                 private juce::Timer
{
public:
    DuelStageComponent();
    ~DuelStageComponent() override = default;

    void setActive (bool shouldBeActive);
    bool isActive() const noexcept { return active; }

    // Stage artwork and the opponent spritesheet are downloaded from the
    // server for the selected song (admin-assigned stage/character), exactly
    // like the song assets. No duel artwork is embedded in the executable.
    void setStageBackground (const juce::MemoryBlock& pngData);
    void clearStageBackground();
    void setOpponentSheet (const juce::MemoryBlock& pngData);
    void clearOpponentSheet();

    // Duel/campaign gameplay uses the full window as the stage backdrop; the
    // guitarists stand between these two local Y coordinates (above the tabs).
    void setGameplayBaseline (int top, int bottom);
    void clearGameplayBaseline();

    // Optional player character chosen in Profile. The sheet is a 4x2 PNG with
    // eight poses; it replaces the opponent sheet on the left side when the
    // player has not picked a character yet.
    void setPlayerSheet (const juce::MemoryBlock& pngData);
    void clearPlayerSheet();

    // Bottom edge (the bottom border of the tablature) the background picture
    // is anchored to: the art is cover-filled down to this line.
    void setPictureBottom (int bottom);

    // Thread-safe. Called from the pitch-analysis thread and the online
    // exchange callback; the timer consumes the requests on the message thread.
    void triggerPlayerHit();
    void triggerOpponentHit();

    void paint (juce::Graphics&) override;

private:
    struct AnimState
    {
        int idleFrame = 0;      // standing poses 0 and 1 alternate
        int idleTicks = 0;
        bool hitting = false;
        int hitFrame = 0;       // current pose 0..7
        int hitTicks = 0;       // ticks spent on the current pose
    };

    void timerCallback() override;

    bool advance (AnimState& state);

    bool active = false;
    int gameplayTop = -1;
    int gameplayBottom = -1;
    int pictureBottom = -1;

    std::atomic<int> playerHitRequests { 0 };
    std::atomic<int> opponentHitRequests { 0 };

    AnimState leftAnim;
    AnimState rightAnim;

    juce::Image background;
    juce::Image rightSheet;
    juce::Image playerSheet;

    // The stage art fades in only in its bottom third: the top 2/3 of the
    // visible picture is fully transparent, then the alpha ramps to 100% at
    // the picture bottom. The masked copy is cached and rebuilt only when the
    // art or the stage geometry changes.
    juce::Image maskedBackground;
    int maskedWidth = -1;
    int maskedHeight = -1;
    int maskedCoverHeight = -1;
    void rebuildMaskedBackground (float coverHeight);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DuelStageComponent)
};
