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

    /** Open-string MIDI notes, low string first, plus the display name
        ("STANDARD", "DROP D", "CUSTOM"). The notes themselves come from the
        parser, which is re-run with the same tuning.
    */
    void setTuning(const std::array<int, 6>& newOpenMidiNotes,
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

    static constexpr int numberOfStrings = 6;

    std::array<int, 6> openMidiNotes = GuitarTuning::standard;
    juce::String tuningName = "STANDARD";

    static juce::Colour getFingerColour(int finger);
    juce::String getStringName(int stringIndex) const;

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