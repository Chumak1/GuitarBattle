#include "UiKit.h"

#include <cmath>

namespace Ui
{
    //==========================================================================
    // Type
    //==========================================================================

    juce::Font font (float height, bool bold, float tracking)
    {
        return juce::Font (juce::FontOptions()
                               .withName ("Segoe UI")
                               .withHeight (height)
                               .withStyle (bold ? "Bold" : "Regular")
                               .withKerningFactor (tracking));
    }

    juce::Font semibold (float height, float tracking)
    {
        return juce::Font (juce::FontOptions()
                               .withName ("Segoe UI Semibold")
                               .withHeight (height)
                               .withStyle ("Semibold")
                               .withKerningFactor (tracking));
    }

    juce::Font eyebrow (float height)
    {
        return font (height, true, 0.14f);
    }

    //==========================================================================
    // Icons
    //==========================================================================

    void icon (juce::Graphics& g,
               Icon which,
               juce::Rectangle<float> area,
               juce::Colour colour,
               float thickness)
    {
        if (which == Icon::none || area.isEmpty())
            return;

        const float scale = juce::jmin (area.getWidth(), area.getHeight()) / 24.0f;

        auto at = [&area, scale] (float x, float y)
        {
            return juce::Point<float> (area.getCentreX() + (x - 12.0f) * scale,
                                       area.getCentreY() + (y - 12.0f) * scale);
        };

        juce::Path path;

        auto line = [&path, &at] (float x1, float y1, float x2, float y2)
        {
            path.startNewSubPath (at (x1, y1));
            path.lineTo (at (x2, y2));
        };

        auto poly = [&path, &at] (std::initializer_list<juce::Point<float>> points,
                                  bool close)
        {
            bool first = true;

            for (const auto& p : points)
            {
                const auto q = at (p.x, p.y);

                if (first)
                {
                    path.startNewSubPath (q);
                    first = false;
                }
                else
                {
                    path.lineTo (q);
                }
            }

            if (close)
                path.closeSubPath();
        };

        const float pi = juce::MathConstants<float>::pi;

        // Circular arc in grid degrees: 0 = 12 o'clock, growing clockwise.
        // Sampled through at() so it lives in the same space as every other
        // path point (juce::Path::addCentredArc uses absolute coordinates and
        // used to place these arcs outside the icon box).
        auto arc = [&path, &at, pi] (float cx, float cy, float radius,
                                     float startDegrees, float endDegrees)
        {
            const float start = startDegrees * pi / 180.0f;
            const float end = endDegrees * pi / 180.0f;
            const int steps = juce::jmax (10, juce::roundToInt (std::abs (end - start) * 10.0f));

            for (int i = 0; i <= steps; ++i)
            {
                const float t = start + (end - start) * static_cast<float> (i)
                                            / static_cast<float> (steps);
                const auto q = at (cx + radius * std::sin (t),
                                   cy - radius * std::cos (t));

                if (i == 0)
                    path.startNewSubPath (q);
                else
                    path.lineTo (q);
            }
        };

        switch (which)
        {
            case Icon::sword:
                // Two crossed swords: blade, guard and pommel each.
                line (8.0f, 17.0f, 19.0f, 6.0f);
                line (6.6f, 15.6f, 9.4f, 18.4f);
                line (8.0f, 17.0f, 6.0f, 19.0f);
                line (16.0f, 17.0f, 5.0f, 6.0f);
                line (14.6f, 15.6f, 17.4f, 18.4f);
                line (16.0f, 17.0f, 18.0f, 19.0f);
                break;

            case Icon::globe:
                path.addEllipse (at (4.0f, 4.0f).x, at (4.0f, 4.0f).y,
                                 16.0f * scale, 16.0f * scale);
                path.addEllipse (at (8.0f, 4.0f).x, at (8.0f, 4.0f).y,
                                 8.0f * scale, 16.0f * scale);
                line (4.0f, 12.0f, 20.0f, 12.0f);
                break;

            case Icon::cap:
                poly ({ { 12.0f, 5.0f }, { 21.0f, 9.2f }, { 12.0f, 13.4f },
                        { 3.0f, 9.2f } }, true);
                line (6.5f, 11.2f, 6.5f, 14.6f);
                line (17.5f, 11.2f, 17.5f, 14.6f);
                path.startNewSubPath (at (6.5f, 14.6f));
                path.quadraticTo (at (12.0f, 17.4f), at (17.5f, 14.6f));
                line (21.0f, 9.2f, 21.0f, 13.6f);
                break;

            case Icon::users:
                path.addEllipse (at (5.8f, 5.8f).x, at (5.8f, 5.8f).y,
                                 6.4f * scale, 6.4f * scale);
                path.startNewSubPath (at (3.4f, 19.4f));
                path.quadraticTo (at (9.0f, 13.6f), at (14.6f, 19.4f));
                path.addEllipse (at (13.9f, 5.4f).x, at (13.9f, 5.4f).y,
                                 5.2f * scale, 5.2f * scale);
                path.startNewSubPath (at (14.4f, 18.6f));
                path.quadraticTo (at (18.4f, 14.2f), at (20.6f, 18.6f));
                break;

            case Icon::tuner:
                line (9.0f, 4.0f, 9.0f, 10.4f);
                line (15.0f, 4.0f, 15.0f, 10.4f);
                path.startNewSubPath (at (9.0f, 10.4f));
                path.lineTo (at (9.0f, 12.4f));
                path.quadraticTo (at (12.0f, 15.6f), at (15.0f, 12.4f));
                path.lineTo (at (15.0f, 10.4f));
                line (12.0f, 15.4f, 12.0f, 20.0f);
                break;

            case Icon::waveform:
                poly ({ { 3.0f, 12.0f }, { 5.8f, 12.0f }, { 8.0f, 6.6f },
                        { 10.2f, 17.4f }, { 12.4f, 4.6f }, { 14.6f, 16.0f },
                        { 16.4f, 9.6f }, { 18.2f, 13.0f }, { 21.0f, 13.0f } },
                      false);
                break;

            case Icon::mail:
                path.addRoundedRectangle (at (3.0f, 6.0f).x, at (3.0f, 6.0f).y,
                                          18.0f * scale, 12.0f * scale,
                                          2.2f * scale);
                poly ({ { 4.4f, 7.8f }, { 12.0f, 13.0f }, { 19.6f, 7.8f } }, false);
                break;

            case Icon::bell:
                path.startNewSubPath (at (6.4f, 16.6f));
                path.lineTo (at (6.4f, 11.2f));
                path.quadraticTo (at (12.0f, 5.2f), at (17.6f, 11.2f));
                path.lineTo (at (17.6f, 16.6f));
                path.closeSubPath();
                line (12.0f, 4.0f, 12.0f, 5.4f);
                path.startNewSubPath (at (10.2f, 18.6f));
                path.quadraticTo (at (12.0f, 20.4f), at (13.8f, 18.6f));
                break;

            case Icon::gear:
            {
                // Cog: a toothed outline around a hole.
                const juce::Point<float> centre = at (12.0f, 12.0f);
                const float outer = 9.4f * scale;
                const float body = 7.2f * scale;
                const float hole = 2.9f * scale;

                for (int i = 0; i < 16; ++i)
                {
                    const float angle = pi * 2.0f * static_cast<float> (i) / 16.0f;
                    const float radius = (i % 2 == 0) ? outer : body;
                    const juce::Point<float> p (centre.x + std::sin (angle) * radius,
                                                centre.y - std::cos (angle) * radius);

                    if (i == 0)
                        path.startNewSubPath (p);
                    else
                        path.lineTo (p);
                }

                path.closeSubPath();
                path.addEllipse (centre.x - hole, centre.y - hole, hole * 2.0f, hole * 2.0f);
                break;
            }

            case Icon::power:
                // Power symbol: a ring with a clear gap at the top and a
                // vertical bar through it.
                arc (12.0f, 12.4f, 7.0f, 50.0f, 310.0f);
                line (12.0f, 4.2f, 12.0f, 11.2f);
                break;

            case Icon::chevronRight:
                poly ({ { 9.4f, 5.8f }, { 15.6f, 12.0f }, { 9.4f, 18.2f } }, false);
                break;

            case Icon::chevronDown:
                poly ({ { 5.8f, 9.4f }, { 12.0f, 15.6f }, { 18.2f, 9.4f } }, false);
                break;

            case Icon::arrowRight:
                line (4.6f, 12.0f, 18.4f, 12.0f);
                poly ({ { 12.8f, 6.6f }, { 18.4f, 12.0f }, { 12.8f, 17.4f } }, false);
                break;

            case Icon::trophy:
                poly ({ { 7.4f, 4.6f }, { 16.6f, 4.6f }, { 15.6f, 11.4f } }, false);
                path.startNewSubPath (at (15.6f, 11.4f));
                path.quadraticTo (at (12.0f, 14.0f), at (8.4f, 11.4f));
                line (8.4f, 11.4f, 7.4f, 4.6f);
                line (12.0f, 14.0f, 12.0f, 17.4f);
                line (8.2f, 19.6f, 15.8f, 19.6f);
                path.startNewSubPath (at (7.4f, 6.4f));
                path.quadraticTo (at (4.2f, 9.6f), at (8.6f, 12.2f));
                path.startNewSubPath (at (16.6f, 6.4f));
                path.quadraticTo (at (19.8f, 9.6f), at (15.4f, 12.2f));
                break;

            case Icon::star:
            {
                const juce::Point<float> centre = at (12.0f, 12.4f);

                for (int i = 0; i < 10; ++i)
                {
                    const float angle = -pi * 0.5f
                                        + pi * 2.0f * static_cast<float> (i) / 10.0f;
                    const float radius = (i % 2 == 0 ? 8.4f : 3.7f) * scale;
                    const juce::Point<float> p (centre.x + std::cos (angle) * radius,
                                                centre.y + std::sin (angle) * radius);

                    if (i == 0)
                        path.startNewSubPath (p);
                    else
                        path.lineTo (p);
                }

                path.closeSubPath();
                break;
            }

            case Icon::guitar:
                path.addEllipse (at (4.9f, 10.4f).x, at (4.9f, 10.4f).y,
                                 9.2f * scale, 9.2f * scale);
                line (12.6f, 11.8f, 19.2f, 5.2f);
                line (19.2f, 5.2f, 20.6f, 3.8f);
                path.addEllipse (at (8.4f, 13.9f).x, at (8.4f, 13.9f).y,
                                 2.2f * scale, 2.2f * scale);
                break;

            case Icon::crown:
                poly ({ { 4.4f, 18.2f }, { 6.0f, 7.6f }, { 9.6f, 12.2f },
                        { 12.0f, 5.4f }, { 14.4f, 12.2f }, { 18.0f, 7.6f },
                        { 19.6f, 18.2f } }, true);
                line (6.2f, 15.0f, 17.8f, 15.0f);
                break;

            case Icon::person:
                path.addEllipse (at (8.6f, 5.2f).x, at (8.6f, 5.2f).y,
                                 6.8f * scale, 6.8f * scale);
                path.startNewSubPath (at (5.0f, 19.6f));
                path.quadraticTo (at (12.0f, 13.2f), at (19.0f, 19.6f));
                break;

            case Icon::search:
                path.addEllipse (at (4.8f, 4.8f).x, at (4.8f, 4.8f).y,
                                 12.0f * scale, 12.0f * scale);
                line (15.2f, 15.2f, 19.6f, 19.6f);
                break;

            case Icon::plus:
                line (12.0f, 5.6f, 12.0f, 18.4f);
                line (5.6f, 12.0f, 18.4f, 12.0f);
                break;

            case Icon::close:
                line (6.6f, 6.6f, 17.4f, 17.4f);
                line (17.4f, 6.6f, 6.6f, 17.4f);
                break;

            case Icon::check:
                poly ({ { 5.4f, 12.6f }, { 10.0f, 17.0f }, { 18.6f, 7.4f } }, false);
                break;

            case Icon::play:
                poly ({ { 8.6f, 5.4f }, { 19.0f, 12.0f }, { 8.6f, 18.6f } }, true);
                break;

            case Icon::lock:
                path.addRoundedRectangle (at (5.4f, 11.0f).x, at (5.4f, 11.0f).y,
                                          13.2f * scale, 8.6f * scale,
                                          2.0f * scale);
                path.startNewSubPath (at (8.6f, 11.0f));
                path.lineTo (at (8.6f, 8.6f));
                path.quadraticTo (at (12.0f, 5.2f), at (15.4f, 8.6f));
                path.lineTo (at (15.4f, 11.0f));
                break;

            case Icon::refresh:
            {
                // Circular arrow: a ring with a gap at the top and a chevron
                // arrow head at the right end of the arc. Sized to stay legible
                // at 14-16 px inside a pill button.
                arc (12.0f, 12.0f, 6.8f, 58.0f, 312.0f);

                line (14.4f, 7.0f, 18.0f, 8.1f);
                line (18.0f, 8.1f, 17.0f, 4.2f);
                break;
            }

            case Icon::spark:
                poly ({ { 12.0f, 4.0f }, { 13.6f, 10.4f }, { 20.0f, 12.0f },
                        { 13.6f, 13.6f }, { 12.0f, 20.0f }, { 10.4f, 13.6f },
                        { 4.0f, 12.0f }, { 10.4f, 10.4f } }, true);
                break;

            case Icon::collapse:
                poly ({ { 10.6f, 7.0f }, { 5.8f, 12.0f }, { 10.6f, 17.0f } }, false);
                poly ({ { 18.2f, 7.0f }, { 13.4f, 12.0f }, { 18.2f, 17.0f } }, false);
                break;

            case Icon::none:
            default:
                return;
        }

        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (thickness * juce::jmax (0.85f, scale * 1.05f),
                                                  juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    }

    //==========================================================================
    // Surfaces
    //==========================================================================

    juce::Image iconImage (Icon which, int size, juce::Colour colour, float thickness)
    {
        if (size <= 0 || which == Icon::none)
            return {};

        juce::Image image (juce::Image::ARGB, size, size, true);
        juce::Graphics g (image);

        icon (g, which, { 0.0f, 0.0f, static_cast<float> (size),
                          static_cast<float> (size) },
              colour, thickness);

        return image;
    }

    void glow (juce::Graphics& g,
               juce::Rectangle<float> area,
               float radius,
               juce::Colour colour,
               float strength)
    {
        if (area.isEmpty() || strength <= 0.0f)
            return;

        const float expand[3] = { 10.0f, 6.0f, 3.0f };
        const float alpha[3] = { 0.035f, 0.055f, 0.08f };

        for (int i = 0; i < 3; ++i)
        {
            g.setColour (colour.withAlpha (alpha[i] * strength));
            g.fillRoundedRectangle (area.expanded (expand[i]), radius + expand[i]);
        }
    }

    void panel (juce::Graphics& g, juce::Rectangle<float> area, float radius,
                bool accentEdge)
    {
        if (area.isEmpty())
            return;

        // Soft drop shadow: three very light passes keep the edge from reading
        // as a hard cut against the page.
        for (int i = 3; i >= 1; --i)
        {
            g.setColour (juce::Colours::black.withAlpha (0.055f));
            g.fillRoundedRectangle (area.translated (0.0f, static_cast<float> (i) * 2.0f),
                                    radius + 1.0f);
        }

        juce::ColourGradient fill (Colours::surfaceRaised, area.getX(), area.getY(),
                                   Colours::surface, area.getX(), area.getBottom(),
                                   false);
        g.setGradientFill (fill);
        g.fillRoundedRectangle (area, radius);

        g.setColour (juce::Colours::white.withAlpha (0.028f));
        g.fillRoundedRectangle (area.withHeight (1.0f), 0.5f);

        g.setColour (accentEdge ? Colours::accent.withAlpha (0.34f) : Colours::border);
        g.drawRoundedRectangle (area.reduced (0.5f), radius, 1.0f);
    }

    void well (juce::Graphics& g, juce::Rectangle<float> area, float radius,
               juce::Colour fill)
    {
        if (area.isEmpty())
            return;

        g.setColour (fill);
        g.fillRoundedRectangle (area, radius);

        g.setColour (Colours::border.withAlpha (0.75f));
        g.drawRoundedRectangle (area.reduced (0.5f), radius, 1.0f);
    }

    void divider (juce::Graphics& g, float x1, float x2, float y)
    {
        g.setColour (Colours::border.withAlpha (0.85f));
        g.drawLine (x1, y, x2, y, 1.0f);
    }

    void verticalDivider (juce::Graphics& g, float x, float y1, float y2)
    {
        g.setColour (Colours::border.withAlpha (0.8f));
        g.drawLine (x, y1, x, y2, 1.0f);
    }

    void label (juce::Graphics& g, const juce::String& text,
                juce::Rectangle<float> area, const juce::Font& f,
                juce::Colour colour, juce::Justification justification,
                bool useEllipses)
    {
        g.setColour (colour);
        g.setFont (f);

        if (useEllipses)
            g.drawFittedText (text, area.toNearestInt(), justification, 1);
        else
            g.drawText (text, area, justification, false);
    }

    void eyebrow (juce::Graphics& g, const juce::String& text,
                  juce::Rectangle<float> area, juce::Colour colour,
                  juce::Justification justification)
    {
        label (g, text.toUpperCase(), area, Ui::eyebrow (Type::eyebrow), colour,
               justification);
    }
    void paragraph (juce::Graphics& g, const juce::String& text,
                    juce::Rectangle<float> area, const juce::Font& f,
                    juce::Colour colour, juce::Justification justification,
                    int maxLines)
    {
        g.setColour (colour);
        g.setFont (f);
        g.drawFittedText (text, area.toNearestInt(), justification,
                          juce::jmax (1, maxLines));
    }

    void progress (juce::Graphics& g, juce::Rectangle<float> area, float value,
                   juce::Colour fill, float trackAlpha)
    {
        if (area.isEmpty())
            return;

        const float radius = area.getHeight() * 0.5f;

        g.setColour (Colours::surfaceSunken.withAlpha (trackAlpha));
        g.fillRoundedRectangle (area, radius);

        g.setColour (Colours::border.withAlpha (0.9f * trackAlpha));
        g.drawRoundedRectangle (area.reduced (0.5f), radius, 1.0f);

        const float filled = area.getWidth()
                             * juce::jlimit (0.0f, 1.0f, value);

        if (filled <= 0.5f)
            return;

        juce::ColourGradient bar (fill.brighter (0.12f), area.getX(), area.getY(),
                                  fill, area.getRight(), area.getY(), false);
        g.setGradientFill (bar);
        g.fillRoundedRectangle (area.withWidth (juce::jmax (area.getHeight(), filled)),
                                radius);
    }

    void statusDot (juce::Graphics& g, juce::Point<float> centre, bool online)
    {
        const juce::Colour colour = online ? Colours::success : Colours::textMuted;

        g.setColour (colour.withAlpha (0.22f));
        g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (centre));

        g.setColour (colour);
        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (centre));
    }

    void chip (juce::Graphics& g, juce::Rectangle<float> area,
               const juce::String& text, juce::Colour colour, bool solid)
    {
        if (area.isEmpty())
            return;

        const float radius = area.getHeight() * 0.5f;

        if (solid)
        {
            g.setColour (colour);
            g.fillRoundedRectangle (area, radius);
        }
        else
        {
            g.setColour (colour.withAlpha (0.14f));
            g.fillRoundedRectangle (area, radius);
            g.setColour (colour.withAlpha (0.45f));
            g.drawRoundedRectangle (area.reduced (0.5f), radius, 1.0f);
        }

        label (g, text, area.reduced (9.0f, 0.0f), font (10.5f, true, 0.06f),
               solid ? Colours::onAccent : colour, juce::Justification::centred);
    }

    //==========================================================================
    // PillButton
    //==========================================================================

    PillButton::PillButton (juce::String textToUse, ButtonStyle styleToUse)
        : text (std::move (textToUse)), style (styleToUse)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setWantsKeyboardFocus (false);
    }

    void PillButton::setButtonText (juce::String newText)
    {
        text = std::move (newText);
        repaint();
    }

    void PillButton::setStyle (ButtonStyle newStyle)
    {
        style = newStyle;
        repaint();
    }

    void PillButton::animateTo (float target)
    {
        hoverTarget = target;

        if (! isTimerRunning())
            startTimerHz (60);
    }

    void PillButton::timerCallback()
    {
        const float delta = hoverTarget - hover;
        hover += delta * 0.28f;

        if (std::abs (delta) < 0.01f)
        {
            hover = hoverTarget;
            stopTimer();
        }

        repaint();
    }

    void PillButton::mouseEnter (const juce::MouseEvent&) { animateTo (1.0f); }
    void PillButton::mouseExit (const juce::MouseEvent&) { pressed = false; animateTo (0.0f); }

    void PillButton::mouseDown (const juce::MouseEvent&)
    {
        pressed = true;
        repaint();
    }

    void PillButton::mouseUp (const juce::MouseEvent& event)
    {
        const bool wasPressed = pressed;
        pressed = false;
        repaint();

        if (wasPressed && getLocalBounds().toFloat().contains (event.position)
            && onClick != nullptr)
            onClick();
    }

    void PillButton::paint (juce::Graphics& g)
    {
        const auto area = getLocalBounds().toFloat().reduced (0.5f);
        const float radius = area.getHeight() * 0.5f;

        juce::Colour fill;
        juce::Colour ink;
        juce::Colour border;

        switch (style)
        {
            case ButtonStyle::primary:
                fill = Colours::accent.interpolatedWith (Colours::accentBright, hover);
                ink = Colours::onAccent;
                border = juce::Colours::transparentBlack;
                break;

            case ButtonStyle::ghost:
                fill = Colours::accent.withAlpha (0.05f + 0.07f * hover);
                ink = Colours::accent;
                border = Colours::accent.withAlpha (0.55f + 0.35f * hover);
                break;

            case ButtonStyle::secondary:
                fill = Colours::surfaceRaised.interpolatedWith (Colours::surfaceHover, hover);
                ink = Colours::text;
                border = Colours::borderStrong;
                break;

            case ButtonStyle::quiet:
            default:
                fill = Colours::surfaceHover.withAlpha (0.85f * hover);
                ink = Colours::textSecondary.interpolatedWith (Colours::text, hover);
                border = juce::Colours::transparentBlack;
                break;
        }

        if (pressed)
            fill = fill.darker (0.12f);

        if (style == ButtonStyle::primary)
            glow (g, area, radius, Colours::accent, 0.55f + 0.45f * hover);

        g.setColour (fill);
        g.fillRoundedRectangle (area, radius);

        if (! border.isTransparent())
        {
            g.setColour (border);
            g.drawRoundedRectangle (area, radius, 1.0f);
        }

        const bool hasLeading = leading != Icon::none;
        const bool hasTrailing = trailing != Icon::none;
        const float iconSize = juce::jmin (18.0f, area.getHeight() * 0.5f);
        const auto textFont = font (fontHeight, true, 0.04f);
        const float textWidth = juce::GlyphArrangement::getStringWidth (textFont, text);
        const float iconGap = 8.0f;

        float contentW = textWidth;

        if (hasLeading)
            contentW += iconSize + iconGap;

        if (hasTrailing)
            contentW += iconSize + iconGap;

        float x = area.getCentreX() - contentW * 0.5f;

        if (hasLeading)
        {
            icon (g, leading, { x, area.getCentreY() - iconSize * 0.5f,
                                iconSize, iconSize }, ink, 1.7f);
            x += iconSize + iconGap;
        }

        label (g, text, { x, area.getY(), textWidth, area.getHeight() },
               textFont, ink, juce::Justification::centredLeft);

        if (hasTrailing)
            icon (g, trailing, { x + textWidth + iconGap,
                                 area.getCentreY() - iconSize * 0.5f,
                                 iconSize, iconSize }, ink, 1.7f);
    }

    //==========================================================================
    // IconButton
    //==========================================================================

    IconButton::IconButton (Icon iconToUse, juce::String tooltip)
        : which (iconToUse)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);

        if (tooltip.isNotEmpty())
            setName (tooltip);
    }

    void IconButton::setBadge (int count)
    {
        if (badge == count)
            return;

        badge = count;
        repaint();
    }

    void IconButton::animateTo (float target)
    {
        hoverTarget = target;

        if (! isTimerRunning())
            startTimerHz (60);
    }

    void IconButton::timerCallback()
    {
        const float delta = hoverTarget - hover;
        hover += delta * 0.3f;

        if (std::abs (delta) < 0.01f)
        {
            hover = hoverTarget;
            stopTimer();
        }

        repaint();
    }

    void IconButton::mouseEnter (const juce::MouseEvent&) { animateTo (1.0f); }

    void IconButton::mouseExit (const juce::MouseEvent&)
    {
        pressed = false;
        animateTo (0.0f);
    }

    void IconButton::mouseDown (const juce::MouseEvent&)
    {
        pressed = true;
        repaint();
    }

    void IconButton::mouseUp (const juce::MouseEvent& event)
    {
        const bool wasPressed = pressed;
        pressed = false;
        repaint();

        if (wasPressed && getLocalBounds().toFloat().contains (event.position)
            && onClick != nullptr)
            onClick();
    }

    void IconButton::paint (juce::Graphics& g)
    {
        const auto area = getLocalBounds().toFloat();
        const float radius = Radius::control;

        const float plateAlpha = active ? 0.16f : 0.075f * hover;

        if (plateAlpha > 0.001f)
        {
            g.setColour ((active ? Colours::accent : juce::Colours::white)
                             .withAlpha (plateAlpha));
            g.fillRoundedRectangle (area.reduced (1.0f), radius);
        }

        if (active)
        {
            g.setColour (Colours::accent.withAlpha (0.28f));
            g.drawRoundedRectangle (area.reduced (1.0f), radius, 1.0f);
        }

        const auto colour = (active ? Colours::accent
                                    : Colours::textSecondary.interpolatedWith (Colours::text, hover))
                                .withMultipliedBrightness (pressed ? 0.9f : 1.0f);

        icon (g, which, area.withSizeKeepingCentre (iconPixels, iconPixels), colour, 1.55f);

        if (badge > 0)
        {
            const auto dotArea = juce::Rectangle<float> (
                area.getRight() - 13.0f, area.getY() + 7.0f, 8.0f, 8.0f);

            g.setColour (Colours::shell);
            g.fillEllipse (dotArea.expanded (1.6f));
            g.setColour (Colours::accent);
            g.fillEllipse (dotArea);
        }
    }
}
