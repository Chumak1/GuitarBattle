#pragma once

#include <JuceHeader.h>

// Shared visual language for the player client. The values mirror the dark
// navy/gold admin panel so every screen of the application feels like one
// product: near-black backgrounds, layered navy surfaces, warm gold accents
// and restrained status colours.
namespace UiColors
{
    extern const juce::Colour background;   // #070b10
    extern const juce::Colour bgSoft;       // #0b1119
    extern const juce::Colour surface;      // #101821
    extern const juce::Colour surface2;     // #141e29
    extern const juce::Colour card;         // legacy alias for surface
    extern const juce::Colour border;
    extern const juce::Colour borderStrong;
    extern const juce::Colour text;         // #e9eff6
    extern const juce::Colour secondaryText;// #8d9cab
    extern const juce::Colour muted;        // #8d9cab
    extern const juce::Colour blue;         // legacy alias for accent
    extern const juce::Colour blueDark;     // on-accent ink
    extern const juce::Colour blueLight;    // legacy alias for surface2
    extern const juce::Colour orange;       // strong gold
    extern const juce::Colour orangeLight;  // warm tinted surface
    extern const juce::Colour green;
    extern const juce::Colour red;
    extern const juce::Colour info;        // cool blue for "good" judgements
    extern const juce::Colour accent;       // #8B5CF6 neon violet
    extern const juce::Colour accentStrong; // #A78BFA
    extern const juce::Colour onAccent;     // dark ink on accent

    // Design system v2: neon accent ramp. Electric blue is the secondary
    // accent, teal marks connected/positive states and gold is kept for
    // rewards and competitive ratings only.
    extern const juce::Colour violet;
    extern const juce::Colour violetSoft;
    extern const juce::Colour electric;
    extern const juce::Colour teal;
    extern const juce::Colour gold;
}

namespace GameUi
{
    // Shared layout grid derived from the player-facing web cabinet
    // (app/static/app/styles.css). Every screen uses these values instead of
    // ad-hoc numbers so the desktop client and the site stay one product.
    namespace Metrics
    {
        constexpr float radiusCard = 16.0f;     // --radius
        constexpr float radiusControl = 10.0f;  // inputs, combos, buttons
        constexpr float radiusSmall = 9.0f;     // tabs, chips
        constexpr float cardPadding = 24.0f;    // .section padding
        constexpr float screenMargin = 24.0f;   // shell gutter
        constexpr float cardGap = 14.0f;        // .grid gap
        constexpr float buttonHeight = 44.0f;   // .btn
        constexpr float controlHeight = 32.0f;  // inputs
    }

    constexpr float screenMargin = Metrics::screenMargin;
    constexpr float cardGap = Metrics::cardGap;
    constexpr float cardPadding = Metrics::cardPadding;

    // One typography scale for every screen. Sizes are the design-system
    // values; GameUi::font() applies the global font scale to all of them.
    namespace Type
    {
        constexpr float display = 34.0f;   // screen hero titles
        constexpr float pageTitle = 26.0f; // section headers
        constexpr float section = 19.0f;   // card headers
        constexpr float cardTitle = 16.0f; // list rows / small cards
        constexpr float body = 12.0f;      // default reading size
        constexpr float caption = 10.0f;   // secondary lines
        constexpr float eyebrow = 9.0f;    // ALL-CAPS captions
        constexpr float metric = 24.0f;    // numbers, levels, ratings
    }

    // Motion durations (ms): short enough to feel instant, long enough to read.
    namespace Motion
    {
        constexpr int hover = 120;
        constexpr int screen = 180;
        constexpr int modal = 160;
    }

    // Soft, layered neon halo behind a rounded surface. Three expanding strokes
    // with low alpha read as a real glow instead of a flat outline.
    void drawNeonGlow(juce::Graphics& g,
                      juce::Rectangle<float> area,
                      juce::Colour colour,
                      float radius,
                      float strength = 1.0f);

    juce::Font font(float height, bool bold = false, float tracking = 0.0f);
    juce::Font heading(float height);
    juce::Font eyebrow(float height = 11.0f);

    // Layered, rounded surface with a thin highlight and soft drop shadow.
    void drawCard(juce::Graphics& g,
                  juce::Rectangle<float> area,
                  float radius = Metrics::radiusCard,
                  bool glow = false);

    // Gameplay variant: the same rounded surface with a much lower opacity so
    // the stage artwork behind the HUD stays visible.
    void drawGlassCard(juce::Graphics& g,
                       juce::Rectangle<float> area,
                       float radius = Metrics::radiusCard,
                       bool glow = false);

    void drawPill(juce::Graphics& g,
                  juce::Rectangle<float> area,
                  const juce::String& label,
                  juce::Colour colour,
                  bool filled = false);

    void drawEyebrow(juce::Graphics& g,
                     const juce::String& label,
                     juce::Rectangle<float> area,
                     juce::Colour colour = UiColors::accent);

    void drawDivider(juce::Graphics& g,
                     float x1, float x2, float y,
                     float thickness = 1.0f);
}

// Dark navy/gold LookAndFeel used by the whole application. It styles buttons,
// sliders, combo boxes, text editors, scrollbars and alert windows so that
// screens built from standard JUCE controls stay visually consistent.
class GameLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    GameLookAndFeel();
    ~GameLookAndFeel() override = default;

    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont(juce::Label&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getAlertWindowFont() override;
    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getSliderPopupFont(juce::Slider&) override;

    void drawButtonBackground(juce::Graphics&, juce::Button&,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&,
                        bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override;

    void drawComboBox(juce::Graphics&, int width, int height,
                      bool isButtonDown, int buttonX, int buttonY,
                      int buttonW, int buttonH, juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;

    int getSliderThumbRadius(juce::Slider&) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;
    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider&) override;

    int getDefaultScrollbarWidth() override;
    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y,
                       int width, int height, bool isScrollbarVertical,
                       int thumbStartPosition, int thumbSize,
                       bool isMouseOver, bool isMouseDown) override;

    void fillTextEditorBackground(juce::Graphics&, int width, int height,
                                  juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int width, int height,
                               juce::TextEditor&) override;

    void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
    void drawTooltip(juce::Graphics&, const juce::String& text,
                     int width, int height) override;

    int getAlertWindowButtonHeight() override;
    void drawAlertBox(juce::Graphics&, juce::AlertWindow&,
                      const juce::Rectangle<int>& textArea,
                      juce::TextLayout&) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GameLookAndFeel)
};
