#include "GameStyle.h"
#include <cmath>

namespace UiColors
{
    const juce::Colour background = juce::Colour::fromString("FF070B10");
    const juce::Colour bgSoft = juce::Colour::fromString("FF0B1119");
    const juce::Colour surface = juce::Colour::fromString("FF101821");
    const juce::Colour surface2 = juce::Colour::fromString("FF141E29");
    const juce::Colour card = surface;
    const juce::Colour border = juce::Colour::fromString("1AFFFFFF");
    const juce::Colour borderStrong = juce::Colour::fromString("2EFFFFFF");
    const juce::Colour text = juce::Colour::fromString("FFE9EFF6");
    const juce::Colour secondaryText = juce::Colour::fromString("FF8D9CAB");
    const juce::Colour muted = secondaryText;
    const juce::Colour blue = juce::Colour::fromString("FFF3C451");
    const juce::Colour blueDark = juce::Colour::fromString("FF0B1119");
    const juce::Colour blueLight = surface2;
    const juce::Colour orange = juce::Colour::fromString("FFFFB703");
    const juce::Colour orangeLight = juce::Colour::fromString("FF1B2430");
    const juce::Colour green = juce::Colour::fromString("FF42D392");
    const juce::Colour red = juce::Colour::fromString("FFFF7272");
    const juce::Colour info = juce::Colour::fromString("FF6DB7FF");
    const juce::Colour accent = juce::Colour::fromString("FFF3C451");
    const juce::Colour accentStrong = juce::Colour::fromString("FFFFB703");
    const juce::Colour onAccent = juce::Colour::fromString("FF0B1119");
}

namespace GameUi
{
    // One tracking value for every label in the client: headings, eyebrow
    // captions, pills, buttons and body copy all share the same rhythm.
    static constexpr float textTracking = 0.0f;

    juce::Font font(float height, bool bold, float tracking)
    {
        return juce::Font(juce::FontOptions()
            .withName("Segoe UI")
            .withHeight(height)
            .withStyle(bold ? "Bold" : "Regular")
            .withKerningFactor(tracking));
    }

    juce::Font heading(float height)
    {
        return font(height, true, textTracking);
    }

    juce::Font eyebrow(float height)
    {
        return font(height, true, textTracking);
    }

    void drawCard(juce::Graphics& g,
                  juce::Rectangle<float> area,
                  float radius,
                  bool glow)
    {
        if (area.isEmpty())
            return;

        if (glow)
        {
            g.setColour(UiColors::accent.withAlpha(0.035f));
            g.fillRoundedRectangle(area.expanded(3.0f), radius + 3.0f);
        }

        g.setColour(juce::Colours::black.withAlpha(0.28f));
        g.fillRoundedRectangle(area.translated(0.0f, 3.0f), radius);

        juce::ColourGradient fill(
            UiColors::surface2.withAlpha(0.98f), area.getX(), area.getY(),
            UiColors::surface.withAlpha(0.98f), area.getX(), area.getBottom(),
            false);
        g.setGradientFill(fill);
        g.fillRoundedRectangle(area, radius);

        g.setColour(UiColors::border);
        g.drawRoundedRectangle(area, radius, 1.0f);

        g.setColour(juce::Colours::white.withAlpha(0.035f));
        g.drawLine(area.getX() + radius,
                   area.getY() + 1.0f,
                   area.getRight() - radius,
                   area.getY() + 1.0f,
                   1.0f);
    }

    void drawPill(juce::Graphics& g,
                  juce::Rectangle<float> area,
                  const juce::String& label,
                  juce::Colour colour,
                  bool filled)
    {
        if (area.isEmpty())
            return;

        g.setColour(colour.withAlpha(filled ? 0.20f : 0.08f));
        g.fillRoundedRectangle(area, area.getHeight() * 0.5f);
        g.setColour(colour.withAlpha(filled ? 0.60f : 0.32f));
        g.drawRoundedRectangle(area, area.getHeight() * 0.5f, 1.0f);

        g.setColour(colour);
        g.setFont(font(juce::jmax(8.5f, area.getHeight() * 0.34f), true, textTracking));
        g.drawFittedText(label,
                         area.reduced(area.getHeight() * 0.38f, 0.0f).toNearestInt(),
                         juce::Justification::centred, 1, 0.78f);
    }

    void drawEyebrow(juce::Graphics& g,
                     const juce::String& label,
                     juce::Rectangle<float> area,
                     juce::Colour colour)
    {
        g.setColour(colour);
        g.setFont(eyebrow(11.0f));
        g.drawText(label, area, juce::Justification::centredLeft, false);
    }

    void drawDivider(juce::Graphics& g,
                     float x1, float x2, float y,
                     float thickness)
    {
        g.setColour(UiColors::border);
        g.fillRect(juce::Rectangle<float>(x1, y, x2 - x1, thickness));
    }
}

//==============================================================================
GameLookAndFeel::GameLookAndFeel()
{
    setColourScheme(getDarkColourScheme());

    setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
    setColour(juce::TextButton::textColourOffId, UiColors::text);
    setColour(juce::TextButton::textColourOnId, UiColors::onAccent);

    setColour(juce::Label::textColourId, UiColors::text);
    setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);

    setColour(juce::ComboBox::backgroundColourId, UiColors::surface2);
    setColour(juce::ComboBox::textColourId, UiColors::text);
    setColour(juce::ComboBox::outlineColourId, UiColors::border);
    setColour(juce::ComboBox::arrowColourId, UiColors::accent);
    setColour(juce::ComboBox::buttonColourId, UiColors::surface2);

    setColour(juce::PopupMenu::backgroundColourId, UiColors::surface);
    setColour(juce::PopupMenu::textColourId, UiColors::text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId,
              UiColors::accent.withAlpha(0.14f));
    setColour(juce::PopupMenu::highlightedTextColourId, UiColors::accent);
    setColour(juce::PopupMenu::headerTextColourId, UiColors::muted);

    setColour(juce::Slider::backgroundColourId, UiColors::borderStrong);
    setColour(juce::Slider::trackColourId, UiColors::accent);
    setColour(juce::Slider::thumbColourId, UiColors::accent);
    setColour(juce::Slider::rotarySliderFillColourId, UiColors::accent);
    setColour(juce::Slider::rotarySliderOutlineColourId, UiColors::surface2);
    setColour(juce::Slider::textBoxTextColourId, UiColors::text);
    setColour(juce::Slider::textBoxBackgroundColourId, UiColors::surface2);
    setColour(juce::Slider::textBoxOutlineColourId, UiColors::border);
    setColour(juce::Slider::textBoxHighlightColourId,
              UiColors::accent.withAlpha(0.22f));

    setColour(juce::TextEditor::backgroundColourId, UiColors::surface2);
    setColour(juce::TextEditor::textColourId, UiColors::text);
    setColour(juce::TextEditor::outlineColourId, UiColors::border);
    setColour(juce::TextEditor::focusedOutlineColourId, UiColors::accent);
    setColour(juce::TextEditor::highlightColourId,
              UiColors::accent.withAlpha(0.25f));
    setColour(juce::TextEditor::highlightedTextColourId, UiColors::text);
    setColour(juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
    setColour(juce::CaretComponent::caretColourId, UiColors::accent);

    setColour(juce::AlertWindow::backgroundColourId, UiColors::surface);
    setColour(juce::AlertWindow::textColourId, UiColors::text);
    setColour(juce::AlertWindow::outlineColourId, UiColors::border);

    setColour(juce::ScrollBar::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ScrollBar::trackColourId, UiColors::surface2);
    setColour(juce::ScrollBar::thumbColourId, UiColors::borderStrong);

    setColour(juce::TooltipWindow::backgroundColourId, UiColors::surface);
    setColour(juce::TooltipWindow::textColourId, UiColors::text);
    setColour(juce::TooltipWindow::outlineColourId, UiColors::border);

    setColour(juce::ResizableWindow::backgroundColourId, UiColors::background);
    setColour(juce::DocumentWindow::backgroundColourId, UiColors::background);
    setColour(juce::ListBox::backgroundColourId, UiColors::surface);
    setColour(juce::ListBox::outlineColourId, UiColors::border);
    setColour(juce::ListBox::textColourId, UiColors::text);
}

juce::Font GameLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return GameUi::font(juce::jlimit(9.5f, 15.0f, static_cast<float>(buttonHeight) * 0.40f),
                        true, GameUi::textTracking);
}

juce::Font GameLookAndFeel::getLabelFont(juce::Label& label)
{
    const auto configured = label.getFont();
    if (configured.getHeight() > 0.0f)
        return configured;

    return GameUi::font(13.0f);
}

juce::Font GameLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return GameUi::font(13.0f);
}

juce::Font GameLookAndFeel::getPopupMenuFont()
{
    return GameUi::font(14.0f);
}

juce::Font GameLookAndFeel::getAlertWindowFont()
{
    return GameUi::font(14.0f);
}

juce::Font GameLookAndFeel::getAlertWindowTitleFont()
{
    return GameUi::heading(22.0f);
}

juce::Font GameLookAndFeel::getAlertWindowMessageFont()
{
    return GameUi::font(14.0f);
}

juce::Font GameLookAndFeel::getSliderPopupFont(juce::Slider&)
{
    return GameUi::font(12.0f, true);
}

void GameLookAndFeel::drawButtonBackground(juce::Graphics& g,
                                           juce::Button& button,
                                           const juce::Colour& backgroundColour,
                                           bool shouldDrawButtonAsHighlighted,
                                           bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    if (bounds.isEmpty())
        return;

    const float corner = juce::jmin(14.0f, bounds.getHeight() * 0.30f);
    const bool ghost = backgroundColour.getAlpha() < 8
        && !button.getToggleState();
    auto fill = button.getToggleState()
        ? button.findColour(juce::TextButton::buttonOnColourId)
        : backgroundColour;

    if (fill.getAlpha() < 8)
        fill = UiColors::surface2;

    if (shouldDrawButtonAsDown)
        fill = fill.darker(0.16f);
    else if (shouldDrawButtonAsHighlighted)
        fill = fill.brighter(0.07f);

    if (!button.isEnabled())
        fill = fill.withMultipliedAlpha(0.55f);

    if (ghost && !shouldDrawButtonAsHighlighted && !shouldDrawButtonAsDown)
    {
        // Ghost buttons keep a faint outline so secondary actions stay visible.
        g.setColour(UiColors::surface2.withAlpha(0.18f));
        g.fillRoundedRectangle(bounds, corner);
        g.setColour(UiColors::border.withAlpha(0.68f));
        g.drawRoundedRectangle(bounds, corner, 1.0f);
        return;
    }

    const float brightness = fill.getPerceivedBrightness();
    const bool bright = brightness > 0.52f;

    if (!bright)
    {
        g.setColour(juce::Colours::black.withAlpha(0.30f));
        g.fillRoundedRectangle(bounds.translated(0.0f, 2.0f), corner);
    }
    else
    {
        g.setColour(fill.withAlpha(0.13f));
        g.fillRoundedRectangle(bounds.expanded(2.0f), corner + 2.0f);
    }

    juce::ColourGradient gradient(
        fill.brighter(bright ? 0.05f : 0.08f), bounds.getX(), bounds.getY(),
        fill.darker(bright ? 0.08f : 0.05f), bounds.getX(), bounds.getBottom(),
        false);
    g.setGradientFill(gradient);
    g.fillRoundedRectangle(bounds, corner);

    g.setColour(bright ? fill.brighter(0.20f) : UiColors::borderStrong);
    g.drawRoundedRectangle(bounds, corner, 1.0f);

    if (button.getToggleState())
    {
        g.setColour(fill.brighter(0.50f).withAlpha(0.92f));
        g.drawRoundedRectangle(bounds.reduced(1.0f), corner, 1.0f);
        g.fillRoundedRectangle(bounds.getX() + 1.5f,
                               bounds.getY() + 4.0f,
                               2.5f,
                               juce::jmax(4.0f, bounds.getHeight() - 8.0f),
                               1.25f);
    }

    g.setColour(juce::Colours::white.withAlpha(bright ? 0.22f : 0.045f));
    g.drawLine(bounds.getX() + corner,
               bounds.getY() + 1.0f,
               bounds.getRight() - corner,
               bounds.getY() + 1.0f,
               1.0f);
}

void GameLookAndFeel::drawButtonText(juce::Graphics& g,
                                     juce::TextButton& button,
                                     bool,
                                     bool)
{
    if (button.getButtonText().isEmpty())
        return;

    const auto background = button.getToggleState()
        ? button.findColour(juce::TextButton::buttonOnColourId)
        : button.findColour(juce::TextButton::buttonColourId);
    const bool bright = background.getPerceivedBrightness() > 0.52f;

    auto colour = bright ? UiColors::onAccent : UiColors::text;
    if (!button.isEnabled())
        colour = UiColors::muted.withAlpha(0.65f);

    g.setColour(colour);
    g.setFont(getTextButtonFont(button, button.getHeight()));
    g.drawFittedText(button.getButtonText(),
                     button.getLocalBounds().reduced(7, 1),
                     juce::Justification::centred,
                     1,
                     0.76f);
}

void GameLookAndFeel::drawComboBox(juce::Graphics& g,
                                   int width, int height,
                                   bool isButtonDown,
                                   int, int, int, int,
                                   juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f,
                                         static_cast<float>(width),
                                         static_cast<float>(height)).reduced(0.5f);
    auto fill = box.findColour(juce::ComboBox::backgroundColourId);
    if (isButtonDown)
        fill = fill.darker(0.12f);
    else if (box.isMouseOver())
        fill = fill.brighter(0.05f);

    g.setColour(fill);
    g.fillRoundedRectangle(bounds, 10.0f);
    g.setColour(box.findColour(juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle(bounds, 10.0f, 1.0f);

    juce::Path arrow;
    const float centreX = static_cast<float>(width) - 16.0f;
    const float centreY = static_cast<float>(height) * 0.5f;
    arrow.addTriangle(centreX - 4.5f, centreY - 2.0f,
                      centreX + 4.5f, centreY - 2.0f,
                      centreX, centreY + 3.0f);
    g.setColour(box.findColour(juce::ComboBox::arrowColourId));
    g.fillPath(arrow);
}

void GameLookAndFeel::positionComboBoxText(juce::ComboBox& box,
                                           juce::Label& label)
{
    label.setBounds(12, 1, box.getWidth() - 34, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
    label.setJustificationType(juce::Justification::centredLeft);
}

int GameLookAndFeel::getSliderThumbRadius(juce::Slider&)
{
    return 7;
}

void GameLookAndFeel::drawLinearSlider(juce::Graphics& g,
                                       int x, int y, int width, int height,
                                       float sliderPos,
                                       float minSliderPos,
                                       float maxSliderPos,
                                       juce::Slider::SliderStyle style,
                                       juce::Slider& slider)
{
    juce::ignoreUnused(minSliderPos, maxSliderPos);

    const auto trackColour = slider.findColour(juce::Slider::backgroundColourId);
    const auto fillColour = slider.findColour(juce::Slider::trackColourId);
    const auto thumbColour = slider.findColour(juce::Slider::thumbColourId);

    if (style == juce::Slider::LinearHorizontal
        || style == juce::Slider::LinearBar)
    {
        const float trackY = static_cast<float>(y)
            + static_cast<float>(height) * 0.5f;
        const float trackHeight = 4.0f;
        const float trackLeft = static_cast<float>(x);
        const float trackWidth = static_cast<float>(width);

        g.setColour(trackColour);
        g.fillRoundedRectangle(trackLeft, trackY - trackHeight * 0.5f,
                               trackWidth, trackHeight, trackHeight * 0.5f);

        const float filled = juce::jlimit(0.0f, trackWidth,
                                          sliderPos - trackLeft);
        g.setColour(fillColour);
        g.fillRoundedRectangle(trackLeft, trackY - trackHeight * 0.5f,
                               filled, trackHeight, trackHeight * 0.5f);

        const float diameter = slider.isEnabled() ? 14.0f : 10.0f;
        const float left = sliderPos - diameter * 0.5f;
        const float top = trackY - diameter * 0.5f;
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillEllipse(left + 0.8f, top + 1.2f, diameter, diameter);
        g.setColour(thumbColour.withAlpha(slider.isEnabled() ? 1.0f : 0.45f));
        g.fillEllipse(left, top, diameter, diameter);
        g.setColour(UiColors::background.withAlpha(0.52f));
        g.fillEllipse(left + diameter * 0.36f, top + diameter * 0.36f,
                      diameter * 0.28f, diameter * 0.28f);
        return;
    }

    if (style == juce::Slider::LinearVertical)
    {
        const float trackX = static_cast<float>(x)
            + static_cast<float>(width) * 0.5f;
        const float trackWidth = 4.0f;
        const float trackTop = static_cast<float>(y);
        const float trackHeight = static_cast<float>(height);

        g.setColour(trackColour);
        g.fillRoundedRectangle(trackX - trackWidth * 0.5f, trackTop,
                               trackWidth, trackHeight, trackWidth * 0.5f);

        const float filled = juce::jlimit(0.0f, trackHeight,
                                          trackTop + trackHeight - sliderPos);
        g.setColour(fillColour);
        g.fillRoundedRectangle(trackX - trackWidth * 0.5f,
                               sliderPos, trackWidth, filled,
                               trackWidth * 0.5f);

        const float diameter = slider.isEnabled() ? 14.0f : 10.0f;
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillEllipse(trackX - diameter * 0.5f + 0.8f,
                      sliderPos - diameter * 0.5f + 1.2f,
                      diameter, diameter);
        g.setColour(thumbColour.withAlpha(slider.isEnabled() ? 1.0f : 0.45f));
        g.fillEllipse(trackX - diameter * 0.5f,
                      sliderPos - diameter * 0.5f,
                      diameter, diameter);
        return;
    }

    LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos,
                                     minSliderPos, maxSliderPos, style, slider);
}

void GameLookAndFeel::drawRotarySlider(juce::Graphics& g,
                                       int x, int y, int width, int height,
                                       float sliderPosProportional,
                                       float rotaryStartAngle,
                                       float rotaryEndAngle,
                                       juce::Slider& slider)
{
    const float radius = static_cast<float>(juce::jmin(width, height)) * 0.5f - 7.0f;
    const float centreX = static_cast<float>(x) + static_cast<float>(width) * 0.5f;
    const float centreY = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
    const float angle = rotaryStartAngle
        + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

    g.setColour(slider.findColour(juce::Slider::rotarySliderOutlineColourId));
    g.fillEllipse(centreX - radius - 2.0f, centreY - radius - 2.0f,
                  (radius + 2.0f) * 2.0f, (radius + 2.0f) * 2.0f);

    g.setColour(UiColors::surface2);
    g.fillEllipse(centreX - radius, centreY - radius,
                  radius * 2.0f, radius * 2.0f);
    g.setColour(UiColors::border);
    g.drawEllipse(centreX - radius, centreY - radius,
                  radius * 2.0f, radius * 2.0f, 1.0f);

    juce::Path track;
    track.addCentredArc(centreX, centreY, radius - 3.0f, radius - 3.0f,
                        0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(UiColors::borderStrong);
    g.strokePath(track, juce::PathStrokeType(3.0f,
                                             juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));

    if (angle > rotaryStartAngle + 0.01f)
    {
        juce::Path valueArc;
        valueArc.addCentredArc(centreX, centreY, radius - 3.0f, radius - 3.0f,
                               0.0f, rotaryStartAngle, angle, true);
        g.setColour(slider.findColour(juce::Slider::rotarySliderFillColourId));
        g.strokePath(valueArc, juce::PathStrokeType(3.0f,
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
    }

    const juce::Point<float> thumb(
        centreX + (radius - 3.0f) * std::cos(angle),
        centreY + (radius - 3.0f) * std::sin(angle));

    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillEllipse(thumb.x - 7.0f, thumb.y - 6.0f, 14.0f, 14.0f);
    g.setColour(slider.findColour(juce::Slider::thumbColourId));
    g.fillEllipse(thumb.x - 5.5f, thumb.y - 5.5f, 11.0f, 11.0f);

    g.setColour(UiColors::background.withAlpha(0.55f));
    g.fillEllipse(centreX - 2.0f, centreY - 2.0f, 4.0f, 4.0f);
}

int GameLookAndFeel::getDefaultScrollbarWidth()
{
    return 9;
}

void GameLookAndFeel::drawScrollbar(juce::Graphics& g,
                                    juce::ScrollBar&,
                                    int x, int y, int width, int height,
                                    bool isScrollbarVertical,
                                    int thumbStartPosition, int thumbSize,
                                    bool isMouseOver, bool isMouseDown)
{
    if (isScrollbarVertical)
    {
        const float trackX = static_cast<float>(x)
            + static_cast<float>(width) * 0.5f - 2.0f;
        g.setColour(UiColors::surface2);
        g.fillRoundedRectangle(trackX, static_cast<float>(y) + 2.0f,
                               4.0f, static_cast<float>(height) - 4.0f, 2.0f);
        g.setColour(UiColors::borderStrong.withAlpha(
            isMouseDown ? 0.95f : (isMouseOver ? 0.80f : 0.62f)));
        g.fillRoundedRectangle(trackX,
                               static_cast<float>(thumbStartPosition),
                               4.0f, static_cast<float>(thumbSize), 2.0f);
    }
    else
    {
        const float trackY = static_cast<float>(y)
            + static_cast<float>(height) * 0.5f - 2.0f;
        g.setColour(UiColors::surface2);
        g.fillRoundedRectangle(static_cast<float>(x) + 2.0f, trackY,
                               static_cast<float>(width) - 4.0f, 4.0f, 2.0f);
        g.setColour(UiColors::borderStrong.withAlpha(
            isMouseDown ? 0.95f : (isMouseOver ? 0.80f : 0.62f)));
        g.fillRoundedRectangle(static_cast<float>(thumbStartPosition),
                               trackY, static_cast<float>(thumbSize), 4.0f, 2.0f);
    }
}

void GameLookAndFeel::fillTextEditorBackground(juce::Graphics& g,
                                               int width, int height,
                                               juce::TextEditor& editor)
{
    g.setColour(editor.findColour(juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle(juce::Rectangle<float>(
        0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(1.0f),
        10.0f);
}

void GameLookAndFeel::drawTextEditorOutline(juce::Graphics& g,
                                            int width, int height,
                                            juce::TextEditor& editor)
{
    const bool focused = editor.hasKeyboardFocus(true);
    g.setColour(focused
        ? editor.findColour(juce::TextEditor::focusedOutlineColourId)
        : editor.findColour(juce::TextEditor::outlineColourId));
    g.drawRoundedRectangle(juce::Rectangle<float>(
        0.5f, 0.5f, static_cast<float>(width) - 1.0f,
        static_cast<float>(height) - 1.0f),
        10.0f, focused ? 1.5f : 1.0f);
}

void GameLookAndFeel::drawPopupMenuBackground(juce::Graphics& g,
                                              int width, int height)
{
    g.fillAll(UiColors::surface);
    g.setColour(UiColors::border);
    g.drawRect(0, 0, width, height);
}

void GameLookAndFeel::drawTooltip(juce::Graphics& g,
                                  const juce::String& text,
                                  int width, int height)
{
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f,
                                         static_cast<float>(width),
                                         static_cast<float>(height)).reduced(0.5f);
    g.setColour(UiColors::surface);
    g.fillRoundedRectangle(bounds, 8.0f);
    g.setColour(UiColors::border);
    g.drawRoundedRectangle(bounds, 8.0f, 1.0f);
    g.setColour(UiColors::text);
    g.setFont(GameUi::font(12.0f));
    g.drawFittedText(text, bounds.toNearestInt().reduced(8, 3),
                     juce::Justification::centredLeft, 3);
}

int GameLookAndFeel::getAlertWindowButtonHeight()
{
    return 36;
}

void GameLookAndFeel::drawAlertBox(juce::Graphics& g,
                                   juce::AlertWindow& alert,
                                   const juce::Rectangle<int>& textArea,
                                   juce::TextLayout& textLayout)
{
    auto bounds = alert.getLocalBounds().toFloat();
    g.fillAll(UiColors::background);

    auto card = bounds.reduced(1.0f);
    g.setColour(UiColors::surface);
    g.fillRoundedRectangle(card, 18.0f);
    g.setColour(UiColors::borderStrong);
    g.drawRoundedRectangle(card, 18.0f, 1.0f);

    g.setColour(UiColors::accent);
    g.fillRoundedRectangle(card.getX() + 22.0f,
                           card.getY() + 22.0f,
                           4.0f, 26.0f, 2.0f);

    g.setColour(alert.findColour(juce::AlertWindow::textColourId));
    textLayout.draw(g, textArea.toFloat().reduced(10.0f, 5.0f));
}
