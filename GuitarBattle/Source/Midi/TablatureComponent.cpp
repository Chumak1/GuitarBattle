#include "TablatureComponent.h"

namespace
{
    constexpr float topMargin = 48.0f;
    constexpr float stringSpacing = 26.0f;

    constexpr float leftMargin = 45.0f;
    constexpr float rightMargin = 20.0f;

    constexpr float noteRadius = 16.0f;

    constexpr float playheadPosition = 0.50f;

    constexpr double beatsPerMeasure = 4.0;

    const juce::Colour background =
        juce::Colour::fromString("FFF8FAFC");

    const juce::Colour cardColour =
        juce::Colour::fromString("FFFFFFFF");

    const juce::Colour stringColour =
        juce::Colour::fromString("FFCBD5E1");

    const juce::Colour measureColour =
        juce::Colour::fromString("FF94A3B8");

    const juce::Colour beatGuideColour =
        juce::Colour::fromString("FFE2E8F0");

    const juce::Colour textColour =
        juce::Colour::fromString("FF1E293B");

    const juce::Colour secondaryTextColour =
        juce::Colour::fromString("FF64748B");

    const juce::Colour playheadColour =
        juce::Colour::fromString("FF2563EB");
}

//==============================================================================

TablatureComponent::TablatureComponent()
{
    setOpaque(true);

    startTimerHz(60);
}

//==============================================================================

void TablatureComponent::setNotes(
    const std::vector<TabNote>& newNotes)
{
    notes = newNotes;

    repaint();
}

//==============================================================================

void TablatureComponent::setBpm(double newBpm)
{
    if (newBpm <= 0.0)
        return;

    bpm = newBpm;
}

//==============================================================================

void TablatureComponent::setPlayheadBeat(double beat)
{
    syncedBeat = beat;
    playheadBeat = beat;

    lastSyncTime =
        juce::Time::getCurrentTime();

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
    case 1: return juce::Colour::fromString("FF2563EB"); // index: blue
    case 2: return juce::Colour::fromString("FF16A34A"); // middle: green
    case 3: return juce::Colour::fromString("FFF97316"); // ring: orange
    case 4: return juce::Colour::fromString("FFDC2626"); // pinky: red
    default: return juce::Colour::fromString("FF64748B"); // open string
    }
}

//==============================================================================

const char* TablatureComponent::getStringName(
    int stringIndex)
{
    switch (stringIndex)
    {
    case 0:
        return "E";

    case 1:
        return "A";

    case 2:
        return "D";

    case 3:
        return "G";

    case 4:
        return "B";

    case 5:
        return "E";

    default:
        return "?";
    }
}

//==============================================================================

void TablatureComponent::drawString(
    juce::Graphics& g,
    int stringIndex,
    float y)
{
    juce::ignoreUnused(stringIndex);

    g.setColour(
        stringColour.withAlpha(0.90f));

    g.fillRect(
        leftMargin,
        y - 0.7f,
        static_cast<float>(
            getWidth()
            - leftMargin
            - rightMargin),
        1.4f);
}

//==============================================================================

void TablatureComponent::drawMeasureLines(
    juce::Graphics& g,
    float playheadX)
{
    if (pixelsPerBeat <= 0.0)
        return;

    /*
        Рисуем тактовые черты вокруг playhead.

        Например при 4/4:

        |       |       |       |
        1       2       3       4
    */

    const double firstMeasure =
        std::floor(playheadBeat / beatsPerMeasure)
        - 2.0;

    const int firstMeasureIndex =
        static_cast<int>(firstMeasure);

    for (int measure = firstMeasureIndex;
        measure < firstMeasureIndex + 8;
        ++measure)
    {
        const double measureBeat =
            static_cast<double>(measure)
            * beatsPerMeasure;

        const double relativeBeat =
            measureBeat - playheadBeat;

        const float x =
            playheadX
            + static_cast<float>(
                relativeBeat * pixelsPerBeat);

        if (x < leftMargin
            || x > getWidth() - rightMargin)
        {
            continue;
        }

        g.setColour(
            measureColour.withAlpha(0.85f));

        g.fillRect(
            x,
            topMargin - 13.0f,
            1.5f,
            stringSpacing * 5.0f + 26.0f);
    }
}

//==============================================================================

void TablatureComponent::drawBeatGuides(
    juce::Graphics& g,
    float playheadX)
{
    if (pixelsPerBeat <= 0.0)
        return;

    const double firstBeat =
        std::floor(playheadBeat) - 4.0;

    const int firstBeatIndex =
        static_cast<int>(firstBeat);

    for (int beat = firstBeatIndex;
        beat < firstBeatIndex + 12;
        ++beat)
    {
        const double beatPosition =
            static_cast<double>(beat);

        /*
            Не рисуем тактовые границы повторно.
        */
        if (std::fmod(
            std::abs(beatPosition),
            beatsPerMeasure) < 0.001)
        {
            continue;
        }

        const double relativeBeat =
            beatPosition - playheadBeat;

        const float x =
            playheadX
            + static_cast<float>(
                relativeBeat * pixelsPerBeat);

        if (x < leftMargin
            || x > getWidth() - rightMargin)
        {
            continue;
        }

        g.setColour(
            beatGuideColour.withAlpha(0.65f));

        g.fillRect(
            x,
            topMargin - 5.0f,
            1.0f,
            stringSpacing * 5.0f + 10.0f);
    }
}

//==============================================================================

void TablatureComponent::drawMeasureNumbers(
    juce::Graphics& g,
    float playheadX)
{
    if (pixelsPerBeat <= 0.0)
        return;

    const double firstMeasure =
        std::floor(playheadBeat / beatsPerMeasure)
        - 2.0;

    const int firstMeasureIndex =
        static_cast<int>(firstMeasure);

    g.setFont(
        juce::FontOptions(
            11.0f,
            juce::Font::bold));

    for (int measure = firstMeasureIndex;
        measure < firstMeasureIndex + 8;
        ++measure)
    {
        const double measureBeat =
            static_cast<double>(measure)
            * beatsPerMeasure;

        const double relativeBeat =
            measureBeat - playheadBeat;

        const float x =
            playheadX
            + static_cast<float>(
                relativeBeat * pixelsPerBeat);

        if (x < leftMargin
            || x > getWidth() - rightMargin)
        {
            continue;
        }

        g.setColour(
            secondaryTextColour);

        g.drawText(
            juce::String(measure + 1),
            static_cast<int>(x + 5.0f),
            31,
            40,
            18,
            juce::Justification::left,
            false);
    }
}

//==============================================================================

void TablatureComponent::drawNote(
    juce::Graphics& g,
    const TabNote& note,
    float x,
    float y)
{
    const auto colour =
        getFingerColour(note.finger);

    const double distance =
        note.startBeat - playheadBeat;

    // A note remains active for its full written duration. The previous narrow
    // +/- 0.20 beat test made the highlight flash briefly and disappear while
    // the player was still holding a note.
    const bool isCurrent =
        distance <= 0.05 && distance > -juce::jmax(0.20, note.durationBeats);

    const bool isPast =
        distance <= -juce::jmax(0.20, note.durationBeats);

    //==========================================================================
    // A single filled circle per note; no finger-number badges or extra rings.

    // Состояние ноты

    if (isPast)
    {
        g.setColour(
            colour.withAlpha(0.28f));
    }
    else
    {
        g.setColour(colour);
    }

    g.fillEllipse(
        x - noteRadius,
        y - noteRadius,
        noteRadius * 2.0f,
        noteRadius * 2.0f);

    // The extra white/gold ring marks the note currently approaching the playhead.
    // It is separate from the finger-colour circle, so timing is visible at a glance.
    if (distance <= 0.10 && distance >= -0.35)
    {
        g.setColour(juce::Colour::fromString("FFFFC857"));
        g.drawEllipse(x - noteRadius - 5.0f, y - noteRadius - 5.0f,
                      (noteRadius + 5.0f) * 2.0f,
                      (noteRadius + 5.0f) * 2.0f, 3.5f);
    }

    // Show the fret/note value inside the coloured note marker; never show
    // a separate finger-number badge. Colour alone identifies the fingering.
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(juce::jmax(9.0f, noteRadius * 0.92f), juce::Font::bold));
    g.drawText(juce::String(note.fret),
               juce::Rectangle<int>(static_cast<int>(x - noteRadius),
                                     static_cast<int>(y - noteRadius),
                                     static_cast<int>(noteRadius * 2.0f),
                                     static_cast<int>(noteRadius * 2.0f)),
               juce::Justification::centred, false);

    //==========================================================================
    // Маленькая линия длительности

    if (note.durationBeats > 0.25)
    {
        const float durationWidth =
            static_cast<float>(
                note.durationBeats
                * pixelsPerBeat);

        if (durationWidth > 18.0f)
        {
            g.setColour(
                colour.withAlpha(
                    isPast ? 0.20f : 0.45f));

            g.fillRect(
                x + noteRadius - 2.0f,
                y - 1.0f,
                juce::jmin(
                    durationWidth - noteRadius,
                    80.0f),
                2.0f);
        }
    }
}

//==============================================================================

void TablatureComponent::paint(
    juce::Graphics& g)
{
    g.fillAll(background);

    if (getWidth() <= 0
        || getHeight() <= 0)
    {
        return;
    }

    //==========================================================================
    // Заголовок

    g.setColour(textColour);

    g.setFont(
        juce::FontOptions(
            16.0f,
            juce::Font::bold));

    g.drawText(
        "Guitar Tab",
        16,
        10,
        130,
        24,
        juce::Justification::left,
        false);

    g.setColour(
        secondaryTextColour);

    g.setFont(
        juce::FontOptions(12.0f));

    g.drawText(
        "Standard tuning",
        145,
        11,
        118,
        22,
        juce::Justification::left,
        false);

    // Compact legend: the number and colour identify the fretting-hand finger.
    const int legendX = 270;
    for (int finger = 1; finger <= 4; ++finger)
    {
        const float cx = static_cast<float>(legendX + (finger - 1) * 35 + 10);
        const float cy = 21.0f;
        g.setColour(getFingerColour(finger));
        g.fillEllipse(cx - 10.0f, cy - 10.0f, 20.0f, 20.0f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawText(juce::String(finger),
                   juce::Rectangle<int>((int)cx - 10, (int)cy - 10, 20, 20),
                   juce::Justification::centred, false);
    }

    //==========================================================================
    // Размеры

    const float firstStringY =
        topMargin;

    const float playheadX =
        static_cast<float>(getWidth())
        * playheadPosition;

    //==========================================================================
    // Направляющие ритма

    drawBeatGuides(
        g,
        playheadX);

    drawMeasureLines(
        g,
        playheadX);

    drawMeasureNumbers(
        g,
        playheadX);

    //==========================================================================
    // Струны

    for (int stringIndex = 0;
        stringIndex < numberOfStrings;
        ++stringIndex)
    {
        /*
            stringIndex:

            0 = 6-я струна
            5 = 1-я струна

            На экране:

            1-я E
            B
            G
            D
            A
            6-я E
        */

        const int displayRow =
            numberOfStrings - 1 - stringIndex;

        const float y =
            firstStringY
            + displayRow * stringSpacing;

        drawString(
            g,
            stringIndex,
            y);
    }

    //==========================================================================
    // Названия струн

    g.setFont(
        juce::FontOptions(
            13.0f,
            juce::Font::bold));

    for (int stringIndex = 0;
        stringIndex < numberOfStrings;
        ++stringIndex)
    {
        const int displayRow =
            numberOfStrings - 1 - stringIndex;

        const float y =
            firstStringY
            + displayRow * stringSpacing;

        // String names are neutral; the note badge and colour indicate the finger.
        g.setColour(secondaryTextColour);

        g.drawText(
            getStringName(stringIndex),
            8,
            static_cast<int>(y - 9.0f),
            28,
            18,
            juce::Justification::centred,
            false);
    }

    // Finger guidance is colour-only; no finger-number badges or legend are drawn.

        // Ноты

    for (const auto& note : notes)
    {
        const double relativeBeat =
            note.startBeat - playheadBeat;

        const float x =
            playheadX
            + static_cast<float>(
                relativeBeat * pixelsPerBeat);

        if (x < leftMargin - 30.0f
            || x > getWidth() + 30.0f)
        {
            continue;
        }

        const int displayRow =
            numberOfStrings - 1
            - note.stringIndex;

        const float y =
            firstStringY
            + displayRow * stringSpacing;

        drawNote(
            g,
            note,
            x,
            y);
    }

    //==========================================================================
    // Playhead

    g.setColour(playheadColour);

    g.fillRoundedRectangle(
        playheadX - 2.0f,
        topMargin - 19.0f,
        4.0f,
        stringSpacing * 5.0f + 38.0f,
        2.0f);

    juce::Path triangle;

    triangle.addTriangle(
        playheadX - 8.0f,
        topMargin - 20.0f,

        playheadX + 8.0f,
        topMargin - 20.0f,

        playheadX,
        topMargin - 8.0f);

    g.fillPath(triangle);

    // COUNT IN is rendered by the full-window overlay so the entire card can be
    // centered on the screen rather than inside the tablature area.

    //==========================================================================
    // Текущий beat

    g.setColour(
        playheadColour);

    g.setFont(
        juce::FontOptions(
            11.0f,
            juce::Font::bold));

    g.drawText(
        "BEAT "
        + juce::String(playheadBeat, 2),
        static_cast<int>(playheadX - 45.0f),
        getHeight() - 20,
        90,
        16,
        juce::Justification::centred,
        false);
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

    const auto now =
        juce::Time::getCurrentTime();

    const double elapsedSeconds =
        static_cast<double>(
            now.toMilliseconds()
            - lastSyncTime.toMilliseconds())
        / 1000.0;

    const double beatsPerSecond =
        bpm / 60.0;

    playheadBeat =
        syncedBeat
        + elapsedSeconds * beatsPerSecond;

    repaint();
}