#pragma once

#include <JuceHeader.h>

#include <functional>

//==============================================================================
// FretFight design system (v3).
//
// One place for the tokens and primitives the redesigned hub screens are built
// from: a neutral graphite surface ramp, the FretFight gold accent, a compact
// type scale, line icons and the shared surface/button/state painters.
//
// The gameplay screen keeps using UiColors/GameUi on purpose: the play HUD is
// not part of the redesign. Hub screens migrate to this kit section by section.
namespace Ui
{
    //==========================================================================
    // Tokens
    //==========================================================================

    namespace Colours
    {
        // Page and shell.
        inline const juce::Colour background    { 0xff0a0b0d };
        inline const juce::Colour shell         { 0xff0c0e11 };
        inline const juce::Colour shellRaised   { 0xff101318 };

        // Layered surfaces.
        inline const juce::Colour surface       { 0xff11141a };
        inline const juce::Colour surfaceRaised { 0xff171a21 };
        inline const juce::Colour surfaceHover  { 0xff1d212a };
        inline const juce::Colour surfaceSunken { 0xff0e1015 };

        // Hairlines.
        inline const juce::Colour border        { 0xff23262e };
        inline const juce::Colour borderStrong  { 0xff32363f };

        // Type.
        inline const juce::Colour text          { 0xfff4f5f7 };
        inline const juce::Colour textSecondary { 0xff9ba2ad };
        inline const juce::Colour textMuted     { 0xff6b727c };

        // Brand accent: FretFight gold. Never replace with another hue.
        inline const juce::Colour accent        { 0xfff6c445 };
        inline const juce::Colour accentBright  { 0xffffd766 };
        inline const juce::Colour accentDeep    { 0xffc99a1c };
        inline const juce::Colour onAccent      { 0xff14150f };

        // Status.
        inline const juce::Colour success       { 0xff4ad295 };
        inline const juce::Colour danger        { 0xffff5f6d };
        inline const juce::Colour info          { 0xff63a4ff };
    }

    namespace Radius
    {
        constexpr float card    = 18.0f;
        constexpr float panel   = 14.0f;
        constexpr float control = 11.0f;
        constexpr float tile    = 12.0f;
        constexpr float pill    = 999.0f;
    }

    namespace Space
    {
        constexpr float xs = 4.0f;
        constexpr float sm = 8.0f;
        constexpr float md = 12.0f;
        constexpr float lg = 16.0f;
        constexpr float xl = 24.0f;
        constexpr float xxl = 32.0f;
    }

    namespace Type
    {
        constexpr float display    = 34.0f; // hero titles (BATTLE / ONLINE)
        constexpr float pageTitle  = 30.0f; // header page name
        constexpr float section    = 18.0f; // card headers
        constexpr float cardTitle  = 14.5f; // list rows, small cards
        constexpr float body       = 13.0f;
        constexpr float caption    = 11.5f;
        constexpr float eyebrow    = 10.0f; // tracked ALL-CAPS captions
        constexpr float metric     = 26.0f; // numbers, levels
        constexpr float heroMetric = 30.0f;
    }

    //==========================================================================
    // Type
    //==========================================================================

    // Sizes are final pixels (no global multiplier): the hub layout is designed
    // on a fixed grid, so text must not drift with the gameplay font scale.
    juce::Font font (float height, bool bold = false, float tracking = 0.0f);
    juce::Font semibold (float height, float tracking = 0.0f);
    juce::Font eyebrow (float height = Type::eyebrow);

    //==========================================================================
    // Icons
    //==========================================================================

    enum class Icon
    {
        none,
        sword,
        globe,
        cap,
        users,
        tuner,
        waveform,
        mail,
        bell,
        gear,
        power,
        chevronRight,
        chevronDown,
        arrowRight,
        trophy,
        star,
        guitar,
        crown,
        person,
        search,
        plus,
        close,
        check,
        play,
        lock,
        refresh,
        spark,
        collapse
    };

    // Draws a line icon centred in `area`, preserving the 1:1 aspect.
    void icon (juce::Graphics& g,
               Icon which,
               juce::Rectangle<float> area,
               juce::Colour colour,
               float thickness = 1.6f);

    // Rasterises an icon into a square ARGB image (popup menus, buttons that
    // take a juce::Image).
    juce::Image iconImage (Icon which, int size, juce::Colour colour,
                           float thickness = 1.6f);

    //==========================================================================
    // Surfaces and decorations
    //==========================================================================

    // Soft gold halo behind a surface. Cheap enough for static paint.
    void glow (juce::Graphics& g,
               juce::Rectangle<float> area,
               float radius,
               juce::Colour colour,
               float strength = 1.0f);

    // Base card: layered graphite gradient, hairline border and soft shadow.
    void panel (juce::Graphics& g,
                juce::Rectangle<float> area,
                float radius = Radius::card,
                bool accentEdge = false);

    // Slightly lifted surface used for inner blocks (rows, tiles, wells).
    void well (juce::Graphics& g,
               juce::Rectangle<float> area,
               float radius = Radius::panel,
               juce::Colour fill = Colours::surfaceRaised);

    void divider (juce::Graphics& g, float x1, float x2, float y);
    void verticalDivider (juce::Graphics& g, float x, float y1, float y2);

    void label (juce::Graphics& g,
                const juce::String& text,
                juce::Rectangle<float> area,
                const juce::Font& f,
                juce::Colour colour,
                juce::Justification justification = juce::Justification::centredLeft,
                bool useEllipses = false);

    // Wrapped multi-line text block (up to maxLines lines, clipped).
    void paragraph (juce::Graphics& g,
                    const juce::String& text,
                    juce::Rectangle<float> area,
                    const juce::Font& f,
                    juce::Colour colour,
                    juce::Justification justification = juce::Justification::topLeft,
                    int maxLines = 3);

    // Tracked ALL-CAPS caption.
    void eyebrow (juce::Graphics& g,
                  const juce::String& text,
                  juce::Rectangle<float> area,
                  juce::Colour colour = Colours::textMuted,
                  juce::Justification justification = juce::Justification::centredLeft);

    // Rounded progress track with an optional animated fill (0..1).
    void progress (juce::Graphics& g,
                   juce::Rectangle<float> area,
                   float value,
                   juce::Colour fill = Colours::accent,
                   float trackAlpha = 1.0f);

    void statusDot (juce::Graphics& g, juce::Point<float> centre, bool online);

    // Small pill: optional solid fill, otherwise a tinted outline.
    void chip (juce::Graphics& g,
               juce::Rectangle<float> area,
               const juce::String& text,
               juce::Colour colour,
               bool solid);

    //==========================================================================
    // Buttons
    //==========================================================================

    enum class ButtonStyle
    {
        primary,   // gold fill, dark ink
        ghost,     // gold outline, gold text
        secondary, // graphite fill, hairline border
        quiet      // no fill until hovered
    };

    // Flat pill button with a label, optional leading icon and hover fade.
    class PillButton final : public juce::Component,
                             private juce::Timer
    {
    public:
        explicit PillButton (juce::String text, ButtonStyle style = ButtonStyle::primary);
        ~PillButton() override = default;

        std::function<void()> onClick;

        void setButtonText (juce::String text);
        void setStyle (ButtonStyle newStyle);
        void setIcon (Icon newIcon) { leading = newIcon; repaint(); }
        void setTrailingIcon (Icon newIcon) { trailing = newIcon; repaint(); }
        void setFontHeight (float height) { fontHeight = height; repaint(); }
        void setAccentColour (juce::Colour colour) { accent = colour; repaint(); }

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

    private:
        void timerCallback() override;
        void animateTo (float target);

        juce::String text;
        ButtonStyle style;
        Icon leading = Icon::none;
        Icon trailing = Icon::none;
        float hover = 0.0f;
        float hoverTarget = 0.0f;
        float fontHeight = Type::caption;
        bool pressed = false;
        juce::Colour accent = Colours::accent;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PillButton)
    };

    // Square icon button for the header: hover plate, optional unread badge.
    class IconButton final : public juce::Component,
                             private juce::Timer
    {
    public:
        explicit IconButton (Icon iconToUse, juce::String tooltip = {});
        ~IconButton() override = default;

        std::function<void()> onClick;

        void setIcon (Icon iconToUse) { which = iconToUse; repaint(); }
        void setBadge (int count);
        void setActive (bool shouldBeActive) { active = shouldBeActive; repaint(); }
        void setSizePixels (float pixels) { iconPixels = pixels; repaint(); }

        void paint (juce::Graphics&) override;
        void mouseEnter (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

    private:
        void timerCallback() override;
        void animateTo (float target);

        Icon which;
        int badge = 0;
        bool active = false;
        bool pressed = false;
        float hover = 0.0f;
        float hoverTarget = 0.0f;
        float iconPixels = 19.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconButton)
    };
}
