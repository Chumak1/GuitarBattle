#include "DuelStageComponent.h"

#include <vector>

namespace
{
    // The supplied sheets are 4 columns x 2 rows = 8 frames of 384x512.
    constexpr int kFrameColumns = 4;
    constexpr int kFrameRows = 2;
    constexpr int kFrameCount = kFrameColumns * kFrameRows;
    constexpr int kFrameRateHz = 60;
    constexpr int kHitTicksPerFrame = 30;  // 0.5 s per pose -> 4 s for 8 poses
    constexpr int kIdleTicksPerFrame = 48; // 0.8 s at 60 Hz, poses 0 and 1

    juce::Image loadPng (const void* data, int size)
    {
        if (data == nullptr || size <= 0)
            return {};

        return juce::ImageFileFormat::loadFrom (data, static_cast<size_t> (size));
    }

    // The supplied sheets keep a soft glow / anti-aliased halo in their alpha
    // channel, and some of the inner shading is semi-transparent too. The game
    // field needs solid pixel art, so:
    //   * pixels already opaque enough stay opaque;
    //   * fully transparent areas connected to the border (the outer glow) are
    //     dropped;
    //   * transparent "holes" enclosed by the silhouette are filled in, so the
    //     stage never shows through the character.
    juce::Image makeSolidAlpha (const juce::Image& source, float brightness = 1.0f)
    {
        if (! source.isValid())
            return {};

        const int width = source.getWidth();
        const int height = source.getHeight();

        juce::Image result (juce::Image::ARGB, width, height, true);
        juce::Image::BitmapData src (source, juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData dst (result, juce::Image::BitmapData::writeOnly);

        const int total = width * height;
        std::vector<juce::uint8> solid (static_cast<size_t> (total), 0);
        std::vector<juce::uint8> outside (static_cast<size_t> (total), 0);

        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                if (src.getPixelColour (x, y).getAlpha() >= 16)
                    solid[static_cast<size_t> (y * width + x)] = 1;

        // Flood fill the transparent area that is reachable from the border:
        // everything it cannot reach is an enclosed hole inside the sprite.
        std::vector<int> stack;
        stack.reserve (static_cast<size_t> (total / 8 + 16));

        auto pushIfOutside = [&] (int x, int y)
        {
            const int index = y * width + x;

            if (solid[static_cast<size_t> (index)] == 0
                && outside[static_cast<size_t> (index)] == 0)
            {
                outside[static_cast<size_t> (index)] = 1;
                stack.push_back (index);
            }
        };

        for (int x = 0; x < width; ++x)
        {
            pushIfOutside (x, 0);
            pushIfOutside (x, height - 1);
        }

        for (int y = 0; y < height; ++y)
        {
            pushIfOutside (0, y);
            pushIfOutside (width - 1, y);
        }

        while (! stack.empty())
        {
            const int index = stack.back();
            stack.pop_back();

            const int x = index % width;
            const int y = index / width;

            if (x > 0)          pushIfOutside (x - 1, y);
            if (x + 1 < width)  pushIfOutside (x + 1, y);
            if (y > 0)          pushIfOutside (x, y - 1);
            if (y + 1 < height) pushIfOutside (x, y + 1);
        }

        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const int index = y * width + x;
                const bool keep = solid[static_cast<size_t> (index)] != 0
                                  || outside[static_cast<size_t> (index)] == 0;

                if (! keep)
                    continue;

                const auto colour = src.getPixelColour (x, y);
                dst.setPixelColour (x, y,
                                    colour.withMultipliedBrightness (brightness)
                                          .withAlpha ((juce::uint8) 255));
            }

        return result;
    }
}

DuelStageComponent::DuelStageComponent()
{
    setInterceptsMouseClicks (false, false);
    setOpaque (false);
}

void DuelStageComponent::setActive (bool shouldBeActive)
{
    if (active == shouldBeActive)
        return;

    active = shouldBeActive;

    if (! active)
    {
        // Do not replay hits that happened while the stage was hidden.
        playerHitRequests.store (0, std::memory_order_relaxed);
        opponentHitRequests.store (0, std::memory_order_relaxed);
        leftAnim = {};
        rightAnim = {};
        stopTimer();
    }
    else
    {
        startTimerHz (kFrameRateHz);
    }

    setVisible (active);
    repaint();
}

void DuelStageComponent::triggerPlayerHit()
{
    playerHitRequests.fetch_add (1, std::memory_order_relaxed);
}

void DuelStageComponent::triggerOpponentHit()
{
    opponentHitRequests.fetch_add (1, std::memory_order_relaxed);
}

void DuelStageComponent::setGameplayBaseline (int top, int bottom)
{
    if (gameplayTop == top && gameplayBottom == bottom)
        return;

    gameplayTop = top;
    gameplayBottom = bottom;
    repaint();
}

void DuelStageComponent::setStageBackground (const juce::MemoryBlock& pngData)
{
    if (pngData.getSize() == 0)
    {
        clearStageBackground();
        return;
    }

    auto image = loadPng (pngData.getData(),
                          static_cast<int> (pngData.getSize()));

    if (! image.isValid())
    {
        clearStageBackground();
        return;
    }

    background = image;
    maskedBackground = {};
    repaint();
}

void DuelStageComponent::clearStageBackground()
{
    if (! background.isValid())
        return;

    background = {};
    repaint();
}

void DuelStageComponent::setOpponentSheet (const juce::MemoryBlock& pngData)
{
    if (pngData.getSize() == 0)
    {
        clearOpponentSheet();
        return;
    }

    auto sheetImage = loadPng (pngData.getData(),
                               static_cast<int> (pngData.getSize()));

    if (! sheetImage.isValid())
    {
        clearOpponentSheet();
        return;
    }

    rightSheet = makeSolidAlpha (sheetImage, 1.2f);
    sheetImage = {};
    repaint();
}

void DuelStageComponent::clearOpponentSheet()
{
    if (! rightSheet.isValid())
        return;

    rightSheet = {};
    repaint();
}

void DuelStageComponent::setPlayerSheet (const juce::MemoryBlock& pngData)
{
    if (pngData.getSize() == 0)
    {
        clearPlayerSheet();
        return;
    }

    auto sheetImage = loadPng (pngData.getData(), static_cast<int> (pngData.getSize()));

    if (! sheetImage.isValid())
    {
        clearPlayerSheet();
        return;
    }

    playerSheet = makeSolidAlpha (sheetImage, 1.2f);
    sheetImage = {};
    repaint();
}

void DuelStageComponent::clearPlayerSheet()
{
    if (! playerSheet.isValid())
        return;

    playerSheet = {};
    repaint();
}

void DuelStageComponent::setPictureBottom (int bottom)
{
    if (pictureBottom == bottom)
        return;

    pictureBottom = bottom;
    repaint();
}

void DuelStageComponent::clearGameplayBaseline()
{
    if (gameplayTop < 0 && gameplayBottom < 0 && pictureBottom < 0)
        return;

    gameplayTop = -1;
    gameplayBottom = -1;
    pictureBottom = -1;
    repaint();
}

bool DuelStageComponent::advance (AnimState& state)
{
    if (state.hitting)
    {
        if (++state.hitTicks < kHitTicksPerFrame)
            return false;

        state.hitTicks = 0;

        if (++state.hitFrame >= kFrameCount)
        {
            state.hitting = false;
            state.hitFrame = 0;
            state.idleFrame = 0;
            state.idleTicks = 0;
        }

        // Repaint on every pose change; frames never blend into each other.
        return true;
    }

    if (++state.idleTicks < kIdleTicksPerFrame)
        return false;

    state.idleTicks = 0;
    state.idleFrame = state.idleFrame == 0 ? 1 : 0;
    return true;
}

void DuelStageComponent::timerCallback()
{
    if (! active)
        return;

    bool dirty = false;

    // A hit that arrives while the previous animation is still playing is
    // ignored: the running animation finishes, then the next hit starts a new
    // one. This keeps the motion smooth instead of restarting mid-pose.
    if (playerHitRequests.exchange (0, std::memory_order_relaxed) > 0
        && ! leftAnim.hitting)
    {
        leftAnim.hitting = true;
        leftAnim.hitFrame = 0;
        leftAnim.hitTicks = 0;
        dirty = true;
    }

    if (opponentHitRequests.exchange (0, std::memory_order_relaxed) > 0
        && ! rightAnim.hitting)
    {
        rightAnim.hitting = true;
        rightAnim.hitFrame = 0;
        rightAnim.hitTicks = 0;
        dirty = true;
    }

    dirty = advance (leftAnim) || dirty;
    dirty = advance (rightAnim) || dirty;

    if (dirty)
        repaint();
}

void DuelStageComponent::paint (juce::Graphics& g)
{
    if (! active || getWidth() <= 0 || getHeight() <= 0)
        return;

    // Pixel art: never smooth the sprites or the background.
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);

    const auto bounds = getLocalBounds().toFloat();

    // Rounded play-field card, matching the tablature below.
    juce::Path clip;
    clip.addRoundedRectangle (bounds, 14.0f);
    g.reduceClipRegion (clip);

    // Duel/campaign gameplay: the background covers the whole window and the
    // guitarists stand in the stage block above the tabs. Other duel screens
    // keep the centred strip.
    const bool gameplay = gameplayBottom > gameplayTop;
    const float floorY = gameplay
        ? static_cast<float> (gameplayBottom)
        : bounds.getHeight() * 0.86f;

    if (background.isValid())
    {
        const float coverHeight = gameplay && pictureBottom > 0
            ? static_cast<float> (pictureBottom)
            : bounds.getHeight();

        if (! maskedBackground.isValid()
            || maskedWidth != getWidth()
            || maskedHeight != getHeight()
            || maskedCoverHeight != juce::roundToInt (coverHeight))
            rebuildMaskedBackground (coverHeight);

        if (maskedBackground.isValid())
            g.drawImageAt (maskedBackground, 0, 0);
    }

    // A dark wash keeps the HUD readable without hiding the stage art.
    g.setColour (juce::Colours::black.withAlpha (0.26f));
    g.fillRect (bounds);

    // setColour() also sets the context opacity to the colour's alpha. Reset it
    // before drawing the sprites, otherwise every image is composited at 26%.
    g.setOpacity (1.0f);

    auto drawDuelist = [this, &g, &bounds, floorY, gameplay] (const juce::Image& sheet,
                                                    bool mirror,
                                                    bool leftSide,
                                                    int frameIndex)
    {
        if (! sheet.isValid())
            return;

        const int fw = sheet.getWidth() / kFrameColumns;
        const int fh = sheet.getHeight() / kFrameRows;

        if (fw <= 0 || fh <= 0)
            return;

        const int col = frameIndex % kFrameColumns;
        const int row = frameIndex / kFrameColumns;
        const auto source = juce::Rectangle<int> (col * fw, row * fh, fw, fh);
        const auto frameImage = sheet.getClippedImage (source);

        // Two guitarists in the side columns of the stage, feet on the floor.
        const float sideW = bounds.getWidth() * 0.20f;
        const float maxH = juce::jmin (
            gameplay
                ? static_cast<float> (juce::jmax (40, gameplayBottom - gameplayTop - 4))
                : bounds.getHeight() * 0.84f,
            sideW * static_cast<float> (fh) / static_cast<float> (fw));
        const float scale = maxH / static_cast<float> (fh);
        const float w = static_cast<float> (fw) * scale;
        const float h = static_cast<float> (fh) * scale;

        const float y = floorY - h;
        const float columnX = leftSide ? 0.0f : bounds.getWidth() - sideW;
        const float x = columnX + (sideW - w) * 0.5f;

        const auto transform = mirror
            ? juce::AffineTransform::scale (-scale, scale)
                  .translated (x + w, y)
            : juce::AffineTransform::scale (scale, scale)
                  .translated (x, y);

        g.drawImageTransformed (frameImage, transform, false);
    };

    // One pose at a time, no cross-fade: the 8-frame sequences read as crisp
    // pixel-art steps. The idle loop alternates the two standing poses; a hit
    // plays all eight poses.
    auto drawAnimated = [&] (const juce::Image& sheet, bool mirror, bool leftSide,
                             const AnimState& state)
    {
        drawDuelist (sheet, mirror, leftSide,
                     state.hitting ? state.hitFrame : state.idleFrame);
    };

    // The left guitarist is the character chosen in Profile; until the player
    // picks one, fall back to the song's character so the stage never shows a
    // blank side. The right one is the song's admin-assigned opponent.
    const auto& playerSprite = playerSheet.isValid() ? playerSheet : rightSheet;
    drawAnimated (playerSprite, false, true, leftAnim);
    drawAnimated (rightSheet, true, false, rightAnim);
}

void DuelStageComponent::rebuildMaskedBackground (float coverHeight)
{
    maskedWidth = getWidth();
    maskedHeight = getHeight();
    maskedCoverHeight = juce::roundToInt (coverHeight);

    if (maskedWidth <= 0 || maskedHeight <= 0 || ! background.isValid())
    {
        maskedBackground = {};
        return;
    }

    maskedBackground = juce::Image (juce::Image::ARGB,
                                    maskedWidth, maskedHeight, true);
    juce::Graphics ig (maskedBackground);
    ig.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);

    // Cover-fill exactly like the old direct draw: scale to cover the field,
    // anchor the bottom at the picture bottom and crop the excess.
    const auto src = background.getBounds().toFloat();
    const auto scale = juce::jmax (
        static_cast<float> (maskedWidth) / src.getWidth(),
        coverHeight / src.getHeight());
    const float w = src.getWidth() * scale;
    const float h = src.getHeight() * scale;
    const float destX = (static_cast<float> (maskedWidth) - w) * 0.5f;
    const float destY = coverHeight - h;
    ig.drawImage (background, juce::Rectangle<float> (destX, destY, w, h));

    // Alpha ramp: the top 2/3 of the visible picture is fully transparent,
    // then the alpha smoothly reaches 100% at the picture bottom.
    const int coverEnd = juce::jlimit (0, maskedHeight,
                                       juce::roundToInt (coverHeight));
    const int fadeStart = juce::jlimit (
        0, coverEnd, juce::roundToInt (coverHeight * 2.0f / 3.0f));
    const int fadeLength = juce::jmax (1, coverEnd - fadeStart);

    juce::Image::BitmapData data (maskedBackground,
                                  juce::Image::BitmapData::readWrite);

    for (int y = 0; y < maskedHeight; ++y)
    {
        auto* line = reinterpret_cast<juce::PixelARGB*> (data.getLinePointer (y));

        if (y >= fadeStart && y < coverEnd)
        {
            float t = static_cast<float> (y - fadeStart)
                      / static_cast<float> (fadeLength);
            t = juce::jlimit (0.0f, 1.0f, t);
            t = t * t * (3.0f - 2.0f * t); // smooth dissolve

            for (int x = 0; x < maskedWidth; ++x)
                line[x].multiplyAlpha (t);
        }
        else
        {
            // Top 2/3 and everything below the picture: fully transparent.
            juce::zeromem (line, static_cast<size_t> (maskedWidth)
                                    * sizeof (juce::PixelARGB));
        }
    }
}
