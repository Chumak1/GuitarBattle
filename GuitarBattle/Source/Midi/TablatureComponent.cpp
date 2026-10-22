#include "TablatureComponent.h"
#include "../UI/GameStyle.h"

namespace
{
    constexpr float leftMargin = 62.0f;
    constexpr float rightMargin = 22.0f;

    constexpr float playheadPosition = 0.50f;
    constexpr double beatsPerMeasure = 4.0;
}

//==============================================================================

TablatureComponent::TablatureComponent()
{
    setOpaque(true);
    startTimerHz(60);
}

//==============================================================================

void TablatureComponent::setNotes(const std::vector<TabNote>& newNotes)
{
    notes = newNotes;
    repaint();
}

//==============================================================================

void TablatureComponent::setBpm(double newBpm)
{
    if (newBpm > 0.0)
        bpm = newBpm;
}

//==============================================================================

void TablatureComponent::setPlayheadBeat(double beat)
{
    syncedBeat = beat;
    playheadBeat = beat;
    lastSyncTime = juce::Time::getCurrentTime();
    repaint();
}

// Freeze the visual timeline during count-in and while the game is paused.
void TablatureComponent::setPlayheadRunning(bool shouldRun)
{
    playheadRunning = shouldRun;
    lastSyncTime = juce::Time::getCurrentTime();
    repaint();
}

void TablatureComponent::setCountInDisplay(bool active, int count)
{
    countInDisplayActive = active;
    countInDisplayCount = juce::jmax(1, count);
    repaint();
}

//==============================================================================

juce::Colour TablatureComponent::getFingerColour(int finger)
{
    // Tab note colours encode the recommended fretting-hand finger, not the string.
    switch (finger)
    {
    case 1: return UiColors::info;   // index
    case 2: return UiColors::green;  // middle
    case 3: return UiColors::accent; // ring
    case 4: return UiColors::red;    // pinky
    default: return UiColors::muted; // open string
    }
}

//==============================================================================

const char* TablatureComponent::getStringName(int stringIndex)
{
    switch (stringIndex)
    {
    case 0: return "E";
    case 1: return "A";
    case 2: return "D";
    case 3: return "G";
    case 4: return "B";
    case 5: return "E";
    default: return "?";
    }
}

//==============================================================================

void TablatureComponent::drawString(juce::Graphics& g, int stringIndex, float y)
{
    // Bass strings read slightly heavier than the treble strings.
    const float thickness = 1.0f + static_cast<float>(numberOfStrings - 1 - stringIndex) * 0.28f;
    const float alpha = 0.30f + static_cast<float>(numberOfStrings - 1 - stringIndex) * 0.04f;

    g.setColour(UiColors::borderStrong.withAlpha(alpha));
    g.fillRect(leftMargin, y - thickness * 0.5f,
               static_cast<float>(getWidth()) - leftMargin - rightMargin,
               thickness);
}

//==============================================================================

void TablatureComponent::drawMeasureLines(juce::Graphics& g, float playheadX)
{
    if (pixelsPerBeat <= 0.0)
        return;

    const double firstMeasure = std::floor(playheadBeat / beatsPerMeasure) - 2.0;
    const int firstMeasureIndex = static_cast<int>(firstMeasure);
    const float top = layoutFirstStringY - 14.0f;
    const float bottom = layoutFirstStringY + layoutStringSpacing * 5.0f + 14.0f;

    for (int measure = firstMeasureIndex; measure < firstMeasureIndex + 8; ++measure)
    {
        const double measureBeat = static_cast<double>(measure) * beatsPerMeasure;
        const double relativeBeat = measureBeat - playheadBeat;
        const float x = playheadX + static_cast<float>(relativeBeat * pixelsPerBeat);

        if (x < leftMargin || x > getWidth() - rightMargin)
            continue;

        g.setColour(UiColors::borderStrong.withAlpha(0.70f));
        g.fillRect(x, top, 1.0f, bottom - top);
    }
}

//==============================================================================

void TablatureComponent::drawBeatGuides(juce::Graphics& g, float playheadX)
{
    if (pixelsPerBeat <= 0.0)
        return;

    const double firstBeat = std::floor(playheadBeat) - 4.0;
    const int firstBeatIndex = static_cast<int>(firstBeat);
    const float top = layoutFirstStringY - 5.0f;
    const float bottom = layoutFirstStringY + layoutStringSpacing * 5.0f + 7.0f;

    for (int beat = firstBeatIndex; beat < firstBeatIndex + 12; ++beat)
    {
        const double beatPosition = static_cast<double>(beat);

        if (std::fmod(std::abs(beatPosition), beatsPerMeasure) < 0.001)
            continue;

        const double relativeBeat = beatPosition - playheadBeat;
        const float x = playheadX + static_cast<float>(relativeBeat * pixelsPerBeat);

        if (x < leftMargin || x > getWidth() - rightMargin)
            continue;

        g.setColour(UiColors::border.withAlpha(0.44f));
        g.fillRect(x, top, 1.0f, bottom - top);
    }
}

//==============================================================================

void TablatureComponent::drawMeasureNumbers(juce::Graphics& g, float playheadX)
{
    if (pixelsPerBeat <= 0.0)
        return;

    const double firstMeasure = std::floor(playheadBeat / beatsPerMeasure) - 2.0;
    const int firstMeasureIndex = static_cast<int>(firstMeasure);

    g.setFont(GameUi::eyebrow(8.5f));

    for (int measure = firstMeasureIndex; measure < firstMeasureIndex + 8; ++measure)
    {
        const double measureBeat = static_cast<double>(measure) * beatsPerMeasure;
        const double relativeBeat = measureBeat - playheadBeat;
        const float x = playheadX + static_cast<float>(relativeBeat * pixelsPerBeat);

        if (x < leftMargin || x > getWidth() - rightMargin)
            continue;

        g.setColour(UiColors::muted.withAlpha(0.78f));
        g.drawText(juce::String(measure + 1),
                   static_cast<int>(x + 5.0f), 39, 38, 14,
                   juce::Justification::left, false);
    }
}

//==============================================================================

void TablatureComponent::drawNote(juce::Graphics& g,
                                  const TabNote& note,
                                  float x,
                                  float y)
{
    const auto colour = getFingerColour(note.finger);
    const float noteRadius = juce::jlimit(9.0f, 14.0f, layoutStringSpacing * 0.44f);

    const double distance = note.startBeat - playheadBeat;

    // A note remains active for its full written duration. The previous narrow
    // +/- 0.20 beat test made the highlight flash briefly and disappear while
    // the player was still holding a note.
    const bool isCurrent = distance <= 0.05
        && distance > -juce::jmax(0.20, note.durationBeats);
    const bool isPast = distance <= -juce::jmax(0.20, note.durationBeats);

    if (isCurrent)
    {
        g.setColour(colour.withAlpha(0.22f));
        g.fillEllipse(x - noteRadius - 5.0f, y - noteRadius - 5.0f,
                      (noteRadius + 5.0f) * 2.0f, (noteRadius + 5.0f) * 2.0f);
    }

    g.setColour(isPast ? colour.withAlpha(0.25f) : colour);
    g.fillEllipse(x - noteRadius, y - noteRadius,
                  noteRadius * 2.0f, noteRadius * 2.0f);

    // The gold ring marks the note currently approaching or sitting under the
    // playhead, independently of the finger-colour fill.
    if (distance <= 0.10 && distance >= -0.35)
    {
        g.setColour(UiColors::accent);
        g.drawEllipse(x - noteRadius - 3.0f, y - noteRadius - 3.0f,
                      (noteRadius + 3.0f) * 2.0f,
                      (noteRadius + 3.0f) * 2.0f, 2.0f);
    }

    g.setColour(UiColors::background);
    g.setFont(GameUi::heading(juce::jmax(9.0f, noteRadius * 0.94f)));
    g.drawText(juce::String(note.fret),
               juce::Rectangle<int>(static_cast<int>(x - noteRadius),
                                    static_cast<int>(y - noteRadius),
                                    static_cast<int>(noteRadius * 2.0f),
                                    static_cast<int>(noteRadius * 2.0f)),
               juce::Justification::centred, false);

    if (note.durationBeats > 0.25)
    {
        const float durationWidth = static_cast<float>(note.durationBeats * pixelsPerBeat);

        if (durationWidth > 18.0f)
        {
            g.setColour(colour.withAlpha(isPast ? 0.16f : 0.40f));
            g.fillRect(x + noteRadius - 2.0f, y - 1.0f,
                       juce::jmin(durationWidth - noteRadius, 90.0f), 2.0f);
        }
    }
}

//==============================================================================

void TablatureComponent::paint(juce::Graphics& g)
{
    g.fillAll(UiColors::background);

    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    const auto card = getLocalBounds().toFloat().reduced(1.0f);
    juce::ColourGradient cardFill(
        UiColors::surface2.withAlpha(0.98f), card.getX(), card.getY(),
        UiColors::surface.withAlpha(0.98f), card.getX(), card.getBottom(),
        false);
    g.setGradientFill(cardFill);
    g.fillRoundedRectangle(card, 16.0f);
    g.setColour(UiColors::border);
    g.drawRoundedRectangle(card, 16.0f, 1.0f);

    g.setColour(UiColors::accent);
    g.fillRoundedRectangle(card.getX() + 22.0f, card.getY() + 1.0f,
                           56.0f, 2.0f, 1.0f);

    g.reduceClipRegion(card.toNearestInt());

    //==========================================================================
    // Header

    g.setColour(UiColors::accent);
    g.setFont(GameUi::eyebrow(10.0f));
    g.drawText("TABLATURE",
               juce::Rectangle<float>(24.0f, 12.0f, 150.0f, 18.0f),
               juce::Justification::centredLeft, false);

    g.setColour(UiColors::muted);
    g.setFont(GameUi::font(10.5f));
    g.drawText("STANDARD TUNING  /  E  A  D  G  B  E",
               juce::Rectangle<float>(180.0f, 12.0f, 340.0f, 18.0f),
               juce::Justification::centredLeft, false);

    const float legendX = static_cast<float>(getWidth()) - 212.0f;
    g.setColour(UiColors::muted);
    g.setFont(GameUi::eyebrow(7.5f));
    g.drawText("FINGERING",
               juce::Rectangle<float>(legendX - 118.0f, 13.0f, 110.0f, 14.0f),
               juce::Justification::centredRight, false);

    for (int finger = 1; finger <= 4; ++finger)
    {
        const float cx = legendX + 8.0f + static_cast<float>(finger - 1) * 28.0f;
        const float cy = 20.0f;
        g.setColour(getFingerColour(finger));
        g.fillEllipse(cx - 8.0f, cy - 8.0f, 16.0f, 16.0f);
        g.setColour(UiColors::background);
        g.setFont(GameUi::font(9.0f, true));
        g.drawText(juce::String(finger),
                   juce::Rectangle<int>(static_cast<int>(cx - 8.0f),
                                        static_cast<int>(cy - 8.0f), 16, 16),
                   juce::Justification::centred, false);
    }

    //==========================================================================
    // Adaptive string layout

    layoutFirstStringY = 60.0f;
    const float available = static_cast<float>(getHeight()) - 94.0f;
    layoutStringSpacing = juce::jlimit(18.0f, 30.0f, available / 5.0f);

    const float playheadX = static_cast<float>(getWidth()) * playheadPosition;

    drawBeatGuides(g, playheadX);
    drawMeasureLines(g, playheadX);
    drawMeasureNumbers(g, playheadX);

    for (int stringIndex = 0; stringIndex < numberOfStrings; ++stringIndex)
    {
        const int displayRow = numberOfStrings - 1 - stringIndex;
        const float y = layoutFirstStringY + displayRow * layoutStringSpacing;
        drawString(g, stringIndex, y);

        g.setColour(UiColors::muted.withAlpha(0.82f));
        g.setFont(GameUi::font(10.0f, true));
        g.drawText(getStringName(stringIndex),
                   juce::Rectangle<float>(8.0f, y - 9.0f, 30.0f, 18.0f),
                   juce::Justification::centred, false);
    }

    //==========================================================================
    // Notes

    for (const auto& note : notes)
    {
        const double relativeBeat = note.startBeat - playheadBeat;
        const float x = playheadX + static_cast<float>(relativeBeat * pixelsPerBeat);

        if (x < leftMargin || x > static_cast<float>(getWidth()) + 20.0f)
            continue;

        const int displayRow = numberOfStrings - 1 - note.stringIndex;
        const float y = layoutFirstStringY + displayRow * layoutStringSpacing;
        drawNote(g, note, x, y);
    }

    //==========================================================================
    // Playhead

    const float playheadTop = layoutFirstStringY - 12.0f;
    const float playheadHeight = layoutStringSpacing * 5.0f + 24.0f;

    g.setColour(UiColors::accent.withAlpha(0.055f));
    g.fillRoundedRectangle(playheadX - 8.0f, playheadTop,
                           16.0f, playheadHeight, 8.0f);
    g.setColour(UiColors::accent.withAlpha(0.92f));
    g.fillRoundedRectangle(playheadX - 1.5f, playheadTop,
                           3.0f, playheadHeight, 1.5f);

    juce::Path triangle;
    triangle.addTriangle(playheadX - 7.0f, playheadTop - 7.0f,
                         playheadX + 7.0f, playheadTop - 7.0f,
                         playheadX, playheadTop + 4.0f);
    g.setColour(UiColors::accent);
    g.fillPath(triangle);

    //==========================================================================
    // Footer beat pill

    if (getHeight() >= 180)
    {
        const juce::Rectangle<float> beatPill(
            playheadX - 54.0f, static_cast<float>(getHeight()) - 30.0f,
            108.0f, 20.0f);
        GameUi::drawPill(g, beatPill,
                         "BEAT " + juce::String(playheadBeat, 2),
                         UiColors::accent,
                         true);
    }

    juce::ignoreUnused(countInDisplayActive, countInDisplayCount);
}

//==============================================================================

void TablatureComponent::timerCallback()
{
    if (!playheadRunning)
        return;

    if (bpm <= 0.0)
        return;

    if (lastSyncTime.toMilliseconds() <= 0)
        return;

    const auto now = juce::Time::getCurrentTime();
    const double elapsedSeconds =
        static_cast<double>(now.toMilliseconds() - lastSyncTime.toMilliseconds()) / 1000.0;
    const double beatsPerSecond = bpm / 60.0;

    playheadBeat = syncedBeat + elapsedSeconds * beatsPerSecond;
    repaint();
}
