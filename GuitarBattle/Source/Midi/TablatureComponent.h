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

    void setBpm(double newBpm);

    void paint(juce::Graphics& g) override;

private:
    std::vector<TabNote> notes;

    double playheadBeat = 0.0;
    double bpm = 120.0;

    double syncedBeat = 0.0;
    juce::Time lastSyncTime;

    double pixelsPerBeat = 90.0;

    static constexpr int numberOfStrings = 6;

    static juce::Colour getStringColour(int stringIndex);
    static const char* getStringName(int stringIndex);

    void drawString(
        juce::Graphics& g,
        int stringIndex,
        float y);

    void drawNote(
        juce::Graphics& g,
        const TabNote& note,
        float x,
        float y);

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