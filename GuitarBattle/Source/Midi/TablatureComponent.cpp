#include "TablatureComponent.h"
#include "../UI/GameStyle.h"

#include <algorithm>

namespace
{
    constexpr float leftMargin = 62.0f;
    constexpr float rightMargin = 22.0f;

    constexpr float playheadPosition = 0.50f;
    constexpr double beatsPerMeasure = 4.0;

    juce::String formatClock(double seconds)
    {
        const int whole = juce::jmax(0, juce::roundToInt(seconds));
        const int minutes = whole / 60;
        const int secs = whole % 60;

        return juce::String(minutes) + ":"
            + (secs < 10 ? "0" : "") + juce::String(secs);
    }
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
    // Notes are addressed by start beat in paint(); sorting once keeps the
    // per-frame work proportional to the visible window instead of the whole
    // song, which matters for dense MIDI.
    std::stable_sort(notes.begin(), notes.end(),
                     [](const TabNote& a, const TabNote& b)
                     {
                         return a.startBeat < b.startBeat;
                     });

    totalBeats = 0.0;

    for (const auto& note : notes)
        totalBeats = juce::jmax(totalBeats,
                                note.startBeat + juce::jmax(0.0, note.durationBeats));

    if (totalBeats <= 0.0 && ! notes.empty())
        totalBeats = notes.back().startBeat + 1.0;

    repaint();
}

//==============================================================================

void TablatureComponent::setTuning(const std::array<int, 6>& newOpenMidiNotes,
                                   const juce::String& name)
{
    openMidiNotes = newOpenMidiNotes;
    tuningName = name.isNotEmpty() ? name : juce::String("CUSTOM");
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
    const auto now = juce::Time::getCurrentTime();

    // The audio engine reports the beat at the start of the audio block, which
    // is a little behind the wall-clock position the 60 Hz timer has already
    // extrapolated. Snapping back to it produced a visible one-frame stutter
    // on every 30 Hz sync, especially on dense tablature.
    if (playheadRunning
        && lastSyncTime.toMilliseconds() > 0
        && bpm > 0.0)
    {
        const double elapsedSeconds =
            static_cast<double>(now.toMilliseconds()
                                - lastSyncTime.toMilliseconds()) / 1000.0;
        const double extrapolated =
            syncedBeat + elapsedSeconds * bpm / 60.0;

        if (extrapolated > beat && extrapolated - beat < 0.5)
            beat = extrapolated;
    }

    syncedBeat = beat;
    playheadBeat = beat;
    lastSyncTime = now;

    // While the playhead is running the 60 Hz timer repaints continuously;
    // repainting here as well would double the paint rate for every 30 Hz
    // audio sync.
    if (! playheadRunning)
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

juce::String TablatureComponent::getStringName(int stringIndex) const
{
    if (stringIndex < 0 || stringIndex >= numberOfStrings)
        return "?";

    return GuitarTuning::noteName(
        openMidiNotes[static_cast<size_t>(stringIndex)], false);
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

    // Keep the measure numbers attached to the staff instead of a fixed
    // header offset; the staff is vertically centred in the larger card.
    const int measureNumberY = static_cast<int>(layoutFirstStringY) - 21;

    for (int measure = firstMeasureIndex; measure < firstMeasureIndex + 8; ++measure)
    {
        const double measureBeat = static_cast<double>(measure) * beatsPerMeasure;
        const double relativeBeat = measureBeat - playheadBeat;
        const float x = playheadX + static_cast<float>(relativeBeat * pixelsPerBeat);

        if (x < leftMargin || x > getWidth() - rightMargin)
            continue;

        g.setColour(UiColors::muted.withAlpha(0.78f));
        g.drawText(juce::String(measure + 1),
                   static_cast<int>(x + 5.0f), measureNumberY, 38, 14,
                   juce::Justification::left, false);
    }
}

//==============================================================================

void TablatureComponent::drawNote(juce::Graphics& g,
                                  const TabNote& note,
                                  float x,
                                  float y,
                                  float noteRadius)
{
    const auto colour = getFingerColour(note.finger);

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
                       juce::jmin(durationWidth - noteRadius, 180.0f), 2.0f);
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
    g.drawText(tuningName + " TUNING  /  "
                   + GuitarTuning::label(openMidiNotes, false),
               juce::Rectangle<float>(180.0f, 12.0f, 420.0f, 18.0f),
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

    // Use the full height of the enlarged tablature card: centre the six-string
    // staff between the header and the footer beat pill, and let the strings
    // breathe much further apart than the old fixed 30 px ceiling.
    const float headerHeight = 56.0f;
    const float footerHeight = 38.0f;
    const float available = juce::jmax(0.0f, static_cast<float>(getHeight())
                                             - headerHeight - footerHeight);
    layoutStringSpacing = juce::jlimit(18.0f, 56.0f, available / 5.0f);

    const float staffHeight = layoutStringSpacing * 5.0f;
    layoutFirstStringY = juce::jmax(58.0f,
                                    headerHeight + (available - staffHeight) * 0.5f);

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

    // Only the notes inside the visible beat window are drawn. The vector is
    // sorted by start beat, so the scan starts at the first visible note and
    // stops as soon as notes leave the right edge. The fret font is created
    // once per paint instead of once per note.
    const float noteRadius =
        juce::jlimit(9.0f, 22.0f, layoutStringSpacing * 0.40f);

    g.setFont(GameUi::heading(juce::jmax(10.0f, noteRadius * 1.05f)));

    if (pixelsPerBeat > 0.0 && ! notes.empty())
    {
        const double firstVisibleBeat =
            playheadBeat
            + (static_cast<double>(leftMargin) - static_cast<double>(playheadX))
                / pixelsPerBeat
            - 1.0;

        auto it = std::lower_bound(
            notes.begin(), notes.end(), firstVisibleBeat,
            [](const TabNote& note, double beat)
            {
                return note.startBeat < beat;
            });

        for (; it != notes.end(); ++it)
        {
            const double relativeBeat = it->startBeat - playheadBeat;
            const float x = playheadX
                + static_cast<float>(relativeBeat * pixelsPerBeat);

            if (x > static_cast<float>(getWidth()) + 40.0f)
                break;

            if (x < leftMargin - 20.0f)
                continue;

            const int displayRow = numberOfStrings - 1 - it->stringIndex;
            const float y = layoutFirstStringY + displayRow * layoutStringSpacing;
            drawNote(g, *it, x, y, noteRadius);
        }
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
    // Song progress: filled bar with elapsed / total time

    if (getHeight() >= 140)
    {
        const float barX = 24.0f;
        const float barW = static_cast<float>(getWidth()) - 48.0f;
        const float barY = static_cast<float>(getHeight()) - 28.0f;
        const float barH = 6.0f;

        const float staffBottom = layoutFirstStringY
            + layoutStringSpacing * 5.0f + 8.0f;

        if (barW > 40.0f && barY > staffBottom + 2.0f)
        {
            const double clampedTotal = totalBeats > 0.0 ? totalBeats : 1.0;
            const double progress =
                juce::jlimit(0.0, 1.0, playheadBeat / clampedTotal);

            const double safeBpm = juce::jmax(1.0, bpm);
            const double elapsedSeconds =
                juce::jmax(0.0, playheadBeat) * 60.0 / safeBpm;
            const double totalSeconds = clampedTotal * 60.0 / safeBpm;

            g.setColour(UiColors::border);
            g.fillRoundedRectangle(barX, barY, barW, barH, barH * 0.5f);

            if (progress > 0.0)
            {
                g.setColour(UiColors::accent);
                g.fillRoundedRectangle(barX, barY,
                                       static_cast<float>(barW * progress),
                                       barH, barH * 0.5f);
            }

            g.setFont(GameUi::font(10.0f, true));

            g.setColour(UiColors::accent);
            g.drawText("BEAT " + juce::String(playheadBeat, 2),
                       juce::Rectangle<float>(barX, barY - 17.0f, barW, 14.0f),
                       juce::Justification::centredLeft, false);

            g.setColour(UiColors::muted);
            g.drawText(formatClock(elapsedSeconds)
                           + " / " + formatClock(totalSeconds)
                           + "   "
                           + juce::String(juce::roundToInt(progress * 100.0))
                           + "%",
                       juce::Rectangle<float>(barX, barY - 17.0f, barW, 14.0f),
                       juce::Justification::centredRight, false);
        }
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
