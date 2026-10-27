#pragma once

#include <JuceHeader.h>

#include <functional>

// Compact, reusable input monitor shown on the song screen, in the tuner and
// during gameplay. It owns the input-channel selector and the detector
// sensitivity slider, and paints the real RMS/peak meter fed by the audio
// thread. All state that comes from audio is read from atomics by the message
// thread; this component never touches the device or the audio callback
// directly.
class GuitarInputMonitor final : public juce::Component
{
public:
    explicit GuitarInputMonitor(juce::AudioDeviceManager& managerToUse);
    ~GuitarInputMonitor() override = default;

    // Compact mode is used by the gameplay HUD: same controls, less padding.
    void setCompact(bool shouldBeCompact);
    bool isCompact() const noexcept { return compact; }

    // Rebuilds the input list from the current device. A preferred hardware
    // channel of -1 selects "Auto (loudest input)".
    void refreshDevices(int preferredHardwareChannel);

    // Detector sensitivity, 0..1, 0.5 = neutral. Never affects the meter.
    void setSensitivityNormalised(float normalised);
    float getSensitivityNormalised() const;

    void setChannelSelectedCallback(std::function<void(int)> callback);
    void setSensitivityChangedCallback(std::function<void(float)> callback);

    // Called by the message thread at the UI tick rate with the latest audio
    // measurements. Levels are linear amplitudes (0..1).
    void updateMeter(float rmsLinear, float peakLinear, int clipEvents,
                     bool noteActive, const juce::String& noteName,
                     double frequencyHz);

    // Advances smoothing and peak-hold state; call from a 30 Hz timer.
    void tick();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    bool tightLayout() const { return compact || getHeight() < 118; }

    void applyState();

    juce::AudioDeviceManager& manager;
    juce::ComboBox channelCombo;
    juce::Slider sensitivitySlider;

    std::function<void(int)> channelCallback;
    std::function<void(float)> sensitivityCallback;

    bool compact = false;
    bool updatingFromCode = false;

    juce::String deviceName;
    bool inputOffline = false;
    int selectedHardware = -1;
    int activeHardware = -1;

    // Meter state (message thread only).
    float rmsShown = 0.0f;
    float peakShown = 0.0f;
    float heldPeak = 0.0f;
    int heldTicks = 0;
    int lastClipEvents = 0;
    int clipHoldTicks = 0;
    float rmsTarget = 0.0f;
    float peakTarget = 0.0f;

    juce::String noteText;
    double noteFrequency = 0.0;
    bool hasNote = false;

    juce::Rectangle<int> headerArea;
    juce::Rectangle<int> meterArea;
    juce::Rectangle<int> numbersArea;
    juce::Rectangle<int> sensLabelArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GuitarInputMonitor)
};
