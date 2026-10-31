#pragma once

#include <JuceHeader.h>

//==============================================================================
// FretFight brand mark.
//
// The artwork lives in Source/UI/Assets/LogoDark.svg and is compiled into the
// binary by the Projucer resource step. The dark-surface variant uses solid
// dark-gold accent stripes, so it reads correctly on the #070b10 app
// background without the translucency of the legacy export.

namespace BrandAssets
{
    // Returns the shared drawable, or nullptr if the SVG could not be parsed.
    // Message thread only: the drawable is created lazily on first use.
    const juce::Drawable* getLogo();

    // Draws the mark into `area`, preserving its square aspect ratio.
    void drawLogo (juce::Graphics& g, juce::Rectangle<float> area);

    // Rasterises the mark for the window/taskbar icon.
    juce::Image createLogoImage (int size);

    // The square app icon (brand mark on its own dark plate), rasterised once
    // at 256 px. Used for the window icon and wherever a song has no artwork.
    const juce::Image& getAppIconImage();
}
