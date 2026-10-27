#pragma once

#include <JuceHeader.h>
#include "TabNote.h"

class TablatureComponent : public juce::Component,
    private juce::Timer
{
public:
    TablatureComponent();

    void setNotes(const std::vector<TabNote>& newNotes);

    void setPlayheadBeat(double beat);
    void setPlayheadRunning(bool shouldRun);
    void setCountInDisplay(bool active, int count);

    void setBpm(double newBpm);

    void paint(juce::Graphics& g) override;

private:
    std::vector<TabNote> notes;

    double playheadBeat = 0.0;
    bool playheadRunning = false;
    bool countInDisplayActive = false;
    int countInDisplayCount = 4;
    double bpm = 120.0;

    double syncedBeat = 0.0;
    juce::Time lastSyncTime;

    double pixelsPerBeat = 160.0;
    float layoutStringSpacing = 26.0f;
    float layoutFirstStringY = 60.0f;

    static constexpr int numberOfStrings = 6;

    static juce::Colour getFingerColour(int finger);
    static const char* getStringName(int stringIndex);

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