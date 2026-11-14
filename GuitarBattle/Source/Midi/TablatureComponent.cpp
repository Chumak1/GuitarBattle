#include "TablatureComponent.h"
#include "../UI/GameStyle.h"

#include <algorithm>

namespace
{
    constexpr float leftMargin = 62.0f;
    constexpr float rightMargin = 22.0f;

    constexpr float playheadPosition = 0.50f;

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
    // The tablature is drawn straight over the duel stage artwork, so the
    // component itself must stay transparent.
    setOpaque(false);
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

void TablatureComponent::setTuning(const GuitarTuning::Tuning& newTuning,
                                   const juce::String& name)
{
    tuning = newTuning;
    tuning.count = juce::jlimit(1, maxStrings, tuning.count);
    stringCount = tuning.count;
    tuningName = name.isNotEmpty() ? name : juce::String("CUSTOM");
    staticLayerDirty = true;
    repaint();
}

void TablatureComponent::resized()
{
    staticLayerDirty = true;
    repaint();
}

//==============================================================================

void TablatureComponent::setBpm(double newBpm)
{
    if (newBpm > 0.0)
        bpm = newBpm;
}

//==============================================================================

void TablatureComponent::setTimeSignature(int numerator, int denominator)
{
    timeSignatureNumerator = juce::jlimit(1, 32, numerator);
    timeSignatureDenominator = denominator > 0 ? denominator : 4;

    // Bars are measured in quarter notes because the playhead and the MIDI
    // note positions are: 4/4 = 4.0, 3/4 = 3.0, 7/8 = 3.5.
    beatsPerMeasure = static_cast<double>(timeSignatureNumerator)
        * 4.0
        / static_cast<double>(timeSignatureDenominator);

    staticLayerDirty = true;
    repaint();
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
    if (stringIndex < 0 || stringIndex >= stringCount)
        return "?";

    if (! isStringActive(stringIndex))
        return "-";

    return GuitarTuning::noteName(
        tuning.notes[static_cast<size_t>(stringIndex)], false);
}

//==============================================================================

bool TablatureComponent::isStringActive(int stringIndex) const
{
    return GuitarTuning::isActive(tuning, stringIndex);
}

//==============================================================================

void TablatureComponent::drawString(juce::Graphics& g, int stringIndex, float y)
{
    // Bass strings read slightly heavier than the treble strings.
    const float thickness = 1.3f + static_cast<float>(stringCount - 1 - stringIndex) * 0.34f;

    // A string the player marked as empty keeps its place in the diagram but
    // is drawn faintly: it exists on the instrument, yet no note may land on it.
    const float alpha = isStringActive(stringIndex)
        ? 0.62f + static_cast<float>(stringCount - 1 - stringIndex) * 0.05f
        : 0.18f;

    g.setColour(UiColors::text.withAlpha(juce::jmin(1.0f, alpha)));
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
    const float bottom = layoutFirstStringY + layoutStringSpacing * static_cast<float>(juce::jmax(1, stringCount - 1)) + 14.0f;

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

    // Guides follow the meter's beat unit: a 7/8 bar shows seven eighth-note
    // guides instead of four quarter-note ones. Bar lines are drawn by
    // drawMeasureLines(), so the first click of every bar is skipped here.
    const int beatsPerBar = juce::jmax(1, timeSignatureNumerator);

    const double clickStep =
        beatsPerMeasure / static_cast<double>(beatsPerBar);

    if (clickStep <= 0.0)
        return;

    const int firstIndex =
        static_cast<int>(std::floor(playheadBeat / clickStep)) - 8;

    const float top = layoutFirstStringY - 5.0f;
    const float bottom = layoutFirstStringY + layoutStringSpacing * static_cast<float>(juce::jmax(1, stringCount - 1)) + 7.0f;

    for (int index = firstIndex; index < firstIndex + 16; ++index)
    {
        if (index % beatsPerBar == 0)
            continue;

        const double beatPosition = static_cast<double>(index) * clickStep;
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

    // The fret digit is pre-rendered once per radius: laying out text for every
    // visible note on every frame dominated the paint on dense, fast charts.
    if (fretGlyphRadius != noteRadius)
    {
        for (auto& image : fretGlyphs)
            image = {};

        fretGlyphRadius = noteRadius;
    }

    const int glyphIndex = juce::jlimit(0,
                                        static_cast<int>(fretGlyphs.size()) - 1,
                                        note.fret);
    auto& glyph = fretGlyphs[static_cast<size_t>(glyphIndex)];

    if (! glyph.isValid())
    {
        const int glyphSize = juce::jmax(1, juce::roundToInt(noteRadius * 2.0f));
        glyph = juce::Image(juce::Image::ARGB, glyphSize, glyphSize, true);
        juce::Graphics glyphGraphics(glyph);
        glyphGraphics.setColour(UiColors::background);
        glyphGraphics.setFont(GameUi::heading(juce::jmax(10.0f, noteRadius * 1.05f)));
        glyphGraphics.drawText(juce::String(note.fret),
                               glyph.getBounds().toFloat(),
                               juce::Justification::centred, false);
    }

    g.drawImageAt(glyph,
                  juce::roundToInt(x - noteRadius),
                  juce::roundToInt(y - noteRadius));

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
    const auto paintStartMs = juce::Time::getMillisecondCounterHiRes();

    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    const auto card = getLocalBounds().toFloat().reduced(1.0f);

    // The card background, header and finger legend are static: render them
    // into an image once per resize instead of on every 60 Hz frame.
    if (staticLayer.isNull()
        || staticLayer.getWidth() != getWidth()
        || staticLayer.getHeight() != getHeight()
        || staticLayerDirty)
    {
        staticLayer = juce::Image(juce::Image::ARGB, getWidth(), getHeight(), true);
        juce::Graphics sg(staticLayer);

        // The stage artwork stays visible under the tabs, but with a soft
        // vertical fade: at the low strings (bottom) the card is filled almost
        // solid, and toward the high strings (top) it opens up to the same
        // glass tint the controls use.
        {
            juce::ColourGradient fade(
                UiColors::surface.withAlpha(0.56f), card.getX(), card.getY(),
                UiColors::surface.withAlpha(0.98f), card.getX(), card.getBottom(),
                false);
            sg.setGradientFill(fade);
            sg.fillRoundedRectangle(card, 16.0f);
        }

        // Hairline border and the accent tick define the area.
        sg.setColour(UiColors::border.withAlpha(0.55f));
        sg.drawRoundedRectangle(card, 16.0f, 1.0f);

        sg.setColour(UiColors::accent);
        sg.fillRoundedRectangle(card.getX() + 22.0f, card.getY() + 1.0f,
                                56.0f, 2.0f, 1.0f);

        sg.reduceClipRegion(card.toNearestInt());

        // Header
        sg.setColour(UiColors::accent);
        sg.setFont(GameUi::eyebrow(10.0f));
        sg.drawText("TABLATURE",
                    juce::Rectangle<float>(24.0f, 12.0f, 150.0f, 18.0f),
                    juce::Justification::centredLeft, false);

        sg.setColour(UiColors::muted);
        sg.setFont(GameUi::font(10.5f));
        sg.drawText(tuningName + " TUNING  /  "
                        + GuitarTuning::label(tuning, false)
                        + "  /  " + juce::String(timeSignatureNumerator)
                        + "/" + juce::String(timeSignatureDenominator),
                    juce::Rectangle<float>(180.0f, 12.0f, 420.0f, 18.0f),
                    juce::Justification::centredLeft, false);

        const float legendX = static_cast<float>(getWidth()) - 212.0f;
        sg.setColour(UiColors::muted);
        sg.setFont(GameUi::eyebrow(7.5f));
        sg.drawText("FINGERING",
                    juce::Rectangle<float>(legendX - 118.0f, 13.0f, 110.0f, 14.0f),
                    juce::Justification::centredRight, false);

        for (int finger = 1; finger <= 4; ++finger)
        {
            const float cx = legendX + 8.0f + static_cast<float>(finger - 1) * 28.0f;
            const float cy = 20.0f;
            sg.setColour(getFingerColour(finger));
            sg.fillEllipse(cx - 8.0f, cy - 8.0f, 16.0f, 16.0f);
            sg.setColour(UiColors::background);
            sg.setFont(GameUi::font(9.0f, true));
            sg.drawText(juce::String(finger),
                        juce::Rectangle<int>(static_cast<int>(cx - 8.0f),
                                             static_cast<int>(cy - 8.0f), 16, 16),
                        juce::Justification::centred, false);
        }

        staticLayerDirty = false;
    }

    g.drawImageAt(staticLayer, 0, 0);
    g.reduceClipRegion(card.toNearestInt());

    //==========================================================================
    // Adaptive string layout

    // Use the full height of the enlarged tablature card: centre the staff
    // between the header and the footer beat pill, and let the strings
    // breathe much further apart than the old fixed 30 px ceiling. The
    // instrument may have more or fewer than six strings, so the spacing is
    // derived from the actual gap count.
    const float headerHeight = 56.0f;
    const float footerHeight = 38.0f;
    const float available = juce::jmax(0.0f, static_cast<float>(getHeight())
                                             - headerHeight - footerHeight);
    const int gapCount = juce::jmax(1, stringCount - 1);
    const float gapCountF = static_cast<float>(gapCount);

    layoutStringSpacing = juce::jlimit(stringCount > 6 ? 14.0f : 18.0f,
                                       56.0f,
                                       available / gapCountF);

    const float staffHeight = layoutStringSpacing * gapCountF;
    layoutFirstStringY = juce::jmax(58.0f,
                                    headerHeight + (available - staffHeight) * 0.5f);

    const float playheadX = static_cast<float>(getWidth()) * playheadPosition;

    drawBeatGuides(g, playheadX);
    drawMeasureLines(g, playheadX);
    drawMeasureNumbers(g, playheadX);

    for (int stringIndex = 0; stringIndex < stringCount; ++stringIndex)
    {
        const int displayRow = stringCount - 1 - stringIndex;
        const float y = layoutFirstStringY + displayRow * layoutStringSpacing;
        drawString(g, stringIndex, y);

        g.setColour(UiColors::text.withAlpha(isStringActive(stringIndex) ? 0.95f : 0.38f));
        g.setFont(GameUi::font(11.0f, true));
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

            // A chart parsed for an older tuning can briefly hold a string
            // index that no longer exists; keep it inside the staff instead of
            // drawing the note above the card.
            const int displayRow = stringCount - 1
                - juce::jlimit(0, stringCount - 1, it->stringIndex);
            const float y = layoutFirstStringY + displayRow * layoutStringSpacing;
            drawNote(g, *it, x, y, noteRadius);
        }
    }

    //==========================================================================
    // Playhead

    const float playheadTop = layoutFirstStringY - 12.0f;
    const float playheadHeight = layoutStringSpacing * static_cast<float>(juce::jmax(1, stringCount - 1)) + 24.0f;

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
            + layoutStringSpacing * static_cast<float>(juce::jmax(1, stringCount - 1)) + 8.0f;

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

    {
        const double paintMs =
            juce::Time::getMillisecondCounterHiRes() - paintStartMs;
        double previousMax = maxPaintMs.load(std::memory_order_relaxed);

        while (paintMs > previousMax
               && ! maxPaintMs.compare_exchange_weak(previousMax, paintMs,
                                                     std::memory_order_relaxed))
        {
        }
    }
}

//==============================================================================

void TablatureComponent::timerCallback()
{
    if (!playheadRunning)
        return;

    // Adaptive repaint rate. The static-layer and fret-glyph caches made a
    // frame cheap enough for 120 Hz on this class of hardware; the faster
    // cadence halves the visible scroll step on 190+ BPM charts. The rate drops
    // back to 60 Hz when a frame consistently costs too much.
    if (++rateCheckTicks >= 60)
    {
        rateCheckTicks = 0;

        const double worstPaintMs =
            maxPaintMs.exchange(0.0, std::memory_order_relaxed);

        if (timerHz < 120 && worstPaintMs > 0.0 && worstPaintMs < 6.0)
        {
            timerHz = 120;
            startTimerHz(timerHz);
        }
        else if (timerHz > 60 && worstPaintMs > 12.0)
        {
            timerHz = 60;
            startTimerHz(timerHz);
        }
    }

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
