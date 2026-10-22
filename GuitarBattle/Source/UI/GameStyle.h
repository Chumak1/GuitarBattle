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
    extern const juce::Colour accent;       // #f3c451
    extern const juce::Colour accentStrong; // #ffb703
    extern const juce::Colour onAccent;     // dark ink on gold
}

namespace GameUi
{
    juce::Font font(float height, bool bold = false, float tracking = 0.0f);
    juce::Font heading(float height);
    juce::Font eyebrow(float height = 11.0f);

    // Layered, rounded surface with a thin highlight and soft drop shadow.
    void drawCard(juce::Graphics& g,
                  juce::Rectangle<float> area,
                  float radius = 18.0f,
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
