#pragma once

#include <JuceHeader.h>
#include <array>

#include "TabNote.h"
#include "TabMapper.h"

class TablatureComponent : public juce::Component,
    private juce::Timer
{
public:
    TablatureComponent();

    void setNotes(const std::vector<TabNote>& newNotes);

    /** Open strings of the selected tuning: a chart may be written for an
        instrument with more or fewer than six strings, and a slot may be
        marked as empty. The notes themselves come from the parser, which is
        re-run with the same tuning.
    */
    void setTuning(const GuitarTuning::Tuning& newTuning,
                   const juce::String& name);

    void setPlayheadBeat(double beat);
    void setPlayheadRunning(bool shouldRun);
    void setCountInDisplay(bool active, int count);

    /** Meter of the loaded song. The values are already normalised by the
        metronome; the tablature only needs them to place bar lines and to
        label the header. ``beatsPerMeasure`` is measured in quarter notes,
        so 7/8 gives 3.5.
    */
    void setTimeSignature(int numerator, int denominator);

    void setBpm(double newBpm);

    void paint(juce::Graphics& g) override;

private:
    std::vector<TabNote> notes;

    double playheadBeat = 0.0;
    bool playheadRunning = false;
    bool countInDisplayActive = false;
    int countInDisplayCount = 4;
    double bpm = 120.0;

    // End of the chart in beats; drives the progress bar.
    double totalBeats = 0.0;

    double syncedBeat = 0.0;
    juce::Time lastSyncTime;

    // Song meter. ``beatsPerMeasure`` is in quarter notes, so 4/4 = 4.0 and
    // 7/8 = 3.5. Bar lines, measure numbers and beat guides follow it.
    int timeSignatureNumerator = 4;
    int timeSignatureDenominator = 4;
    double beatsPerMeasure = 4.0;

    double pixelsPerBeat = 160.0;
    float layoutStringSpacing = 26.0f;
    float layoutFirstStringY = 60.0f;

    static constexpr int maxStrings = GuitarTuning::maxStrings;

    //: Number of string lines, including the slots marked as empty.
    int stringCount = 6;

    GuitarTuning::Tuning tuning = GuitarTuning::standardTuning();
    juce::String tuningName = "STANDARD";

    static juce::Colour getFingerColour(int finger);
    juce::String getStringName(int stringIndex) const;
    bool isStringActive(int stringIndex) const;

    void drawString(
        juce::Graphics& g,
        int stringIndex,
        float y);

    void drawNote(
        juce::Graphics& g,
        const TabNote& note,
        float x,
        float y,
        float noteRadius);

    void drawMeasureLines(
        juce::Graphics& g,
        float playheadX);

    void drawBeatGuides(
        juce::Graphics& g,
        float playheadX);

    void drawMeasureNumbers(
        juce::Graphics& g,
        float playheadX);

    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
        TablatureComponent)
};