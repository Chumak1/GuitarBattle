#include "GuitarInputMonitor.h"

#include "GameStyle.h"

#include <cmath>

namespace
{
    constexpr float meterFloorDb = -60.0f;
    constexpr float yellowDb = -18.0f;
    constexpr float redDb = -3.0f;

    float linearToDb(float linear)
    {
        return linear > 0.000001f
            ? 20.0f * std::log10(juce::jmax(1.0e-6f, linear))
            : -100.0f;
    }

    float dbToNormalised(float db)
    {
        return juce::jlimit(0.0f, 1.0f, (db - meterFloorDb) / -meterFloorDb);
    }

    juce::String formatDb(float linear)
    {
        const float db = linearToDb(linear);

        if (db <= -99.0f)
            return "-inf";

        return juce::String(db, 1);
    }
}

GuitarInputMonitor::GuitarInputMonitor(juce::AudioDeviceManager& managerToUse)
    : manager(managerToUse)
{
    channelCombo.setColour(juce::ComboBox::backgroundColourId, UiColors::surface2);
    channelCombo.setColour(juce::ComboBox::textColourId, UiColors::text);
    channelCombo.setColour(juce::ComboBox::outlineColourId, UiColors::borderStrong);
    channelCombo.setColour(juce::ComboBox::arrowColourId, UiColors::accent);
    channelCombo.setColour(juce::ComboBox::focusedOutlineColourId, UiColors::accent);
    channelCombo.setTooltip("Audio input used for pitch detection and the tuner");

    channelCombo.onChange = [this]
    {
        if (updatingFromCode)
            return;

        const int id = channelCombo.getSelectedId();
        selectedHardware = id > 1 ? id - 2 : -1;

        if (channelCallback != nullptr)
            channelCallback(selectedHardware);
    };

    sensitivitySlider.setSliderStyle(juce::Slider::LinearHorizontal);
    sensitivitySlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 46, 20);
    sensitivitySlider.setRange(0.0, 100.0, 1.0);
    sensitivitySlider.setValue(50.0, juce::dontSendNotification);
    sensitivitySlider.setColour(juce::Slider::trackColourId, UiColors::accent);
    sensitivitySlider.setColour(juce::Slider::backgroundColourId, UiColors::border);
    sensitivitySlider.setColour(juce::Slider::thumbColourId, UiColors::accent);
    sensitivitySlider.setColour(juce::Slider::textBoxTextColourId, UiColors::text);
    sensitivitySlider.setColour(juce::Slider::textBoxBackgroundColourId,
                                juce::Colours::transparentBlack);
    sensitivitySlider.setColour(juce::Slider::textBoxOutlineColourId,
                                juce::Colours::transparentBlack);
    sensitivitySlider.setTooltip(
        "Detector sensitivity. 50% keeps the original note-detection thresholds.");

    sensitivitySlider.onValueChange = [this]
    {
        if (updatingFromCode)
            return;

        if (sensitivityCallback != nullptr)
            sensitivityCallback(static_cast<float>(sensitivitySlider.getValue() / 100.0));
    };

    addAndMakeVisible(channelCombo);
    addAndMakeVisible(sensitivitySlider);
}

void GuitarInputMonitor::setCompact(bool shouldBeCompact)
{
    if (compact == shouldBeCompact)
        return;

    compact = shouldBeCompact;
    resized();
    repaint();
}

void GuitarInputMonitor::setChannelSelectedCallback(std::function<void(int)> callback)
{
    channelCallback = std::move(callback);
}

void GuitarInputMonitor::setSensitivityChangedCallback(std::function<void(float)> callback)
{
    sensitivityCallback = std::move(callback);
}

void GuitarInputMonitor::refreshDevices(int preferredHardwareChannel)
{
    updatingFromCode = true;

    deviceName = "No audio device";
    inputOffline = false;
    activeHardware = -1;

    channelCombo.clear(juce::dontSendNotification);
    channelCombo.addItem("Auto (loudest input)", 1);

    if (auto* device = manager.getCurrentAudioDevice())
    {
        deviceName = device->getName().trim();

        const auto names = device->getInputChannelNames();
        const auto active = device->getActiveInputChannels();
        const int maxChannels = juce::jmin(names.size(), 32);

        for (int hardware = 0; hardware < maxChannels; ++hardware)
        {
            auto name = names[hardware].trim();

            if (name.isEmpty())
                name = "Input " + juce::String(hardware + 1);

            if (! active[hardware])
                name << " (inactive)";

            if (active[hardware])
                activeHardware = hardware;

            channelCombo.addItem(name, hardware + 2);
        }

        if (preferredHardwareChannel >= 0)
        {
            const bool available =
                preferredHardwareChannel < maxChannels
                && active[preferredHardwareChannel];

            inputOffline = ! available;
        }
    }
    else
    {
        inputOffline = true;
    }

    const int requestedId = preferredHardwareChannel < 0
        ? 1
        : preferredHardwareChannel + 2;

    channelCombo.setSelectedId(
        channelCombo.indexOfItemId(requestedId) >= 0 ? requestedId : 1,
        juce::dontSendNotification);

    if (requestedId > 1 && channelCombo.getSelectedId() != requestedId)
        inputOffline = true;

    selectedHardware = preferredHardwareChannel;

    updatingFromCode = false;
    repaint();
}

void GuitarInputMonitor::setSensitivityNormalised(float normalised)
{
    updatingFromCode = true;
    sensitivitySlider.setValue(juce::jlimit(0.0f, 1.0f, normalised) * 100.0,
                               juce::dontSendNotification);
    updatingFromCode = false;
}

float GuitarInputMonitor::getSensitivityNormalised() const
{
    return static_cast<float>(sensitivitySlider.getValue() / 100.0);
}

void GuitarInputMonitor::updateMeter(float rmsLinear,
                                     float peakLinear,
                                     int clipEvents,
                                     bool noteActive,
                                     const juce::String& noteName,
                                     double frequencyHz)
{
    rmsTarget = juce::jlimit(0.0f, 2.0f, rmsLinear);
    peakTarget = juce::jlimit(0.0f, 2.0f, peakLinear);

    hasNote = noteActive;
    noteText = noteActive ? noteName : juce::String();
    noteFrequency = noteActive ? frequencyHz : 0.0;

    if (clipEvents != lastClipEvents)
    {
        lastClipEvents = clipEvents;
        clipHoldTicks = 45; // 1.5 s at the 30 Hz UI tick.
    }
}

void GuitarInputMonitor::tick()
{
    // Fast attack, gentle release, so the bar follows the playing without
    // flickering on every audio block.
    const float attack = 0.55f;
    const float release = 0.14f;

    auto follow = [](float current, float target, float up, float down)
    {
        const float factor = target > current ? up : down;
        return current + (target - current) * factor;
    };

    rmsShown = follow(rmsShown, rmsTarget, attack, release);
    peakShown = follow(peakShown, peakTarget, attack, release);

    if (peakShown >= heldPeak)
    {
        heldPeak = peakShown;
        heldTicks = 36; // 1.2 s hold.
    }
    else if (heldTicks > 0)
    {
        --heldTicks;
    }
    else
    {
        heldPeak *= 0.94f;

        if (heldPeak < 0.00001f)
            heldPeak = 0.0f;
    }

    if (clipHoldTicks > 0)
        --clipHoldTicks;

    repaint();
}

void GuitarInputMonitor::resized()
{
    const bool tight = tightLayout();

    auto area = getLocalBounds().reduced(tight ? 10 : 16, tight ? 8 : 12);

    headerArea = area.removeFromTop(tight ? 12 : 16);
    area.removeFromTop(tight ? 2 : 6);
    channelCombo.setBounds(area.removeFromTop(tight ? 20 : 26));
    area.removeFromTop(tight ? 3 : 8);
    meterArea = area.removeFromTop(tight ? 10 : 16);
    area.removeFromTop(tight ? 3 : 6);

    auto bottom = area.removeFromTop(tight ? 18 : 22);
    auto sensArea = bottom.removeFromRight(
        tight ? juce::jlimit(140, 200, area.getWidth() / 2) : 240);

    if (tight)
    {
        sensLabelArea = {};
    }
    else
    {
        sensLabelArea = sensArea.removeFromLeft(110);
    }

    sensitivitySlider.setBounds(sensArea);
    numbersArea = bottom;
}

void GuitarInputMonitor::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const bool tight = tightLayout();

    g.setColour(UiColors::surface.withAlpha(0.92f));
    g.fillRoundedRectangle(bounds, 16.0f);
    g.setColour(UiColors::border);
    g.drawRoundedRectangle(bounds.reduced(0.5f), 16.0f, 1.0f);

    const float rmsDb = linearToDb(rmsShown);
    const float peakDb = linearToDb(peakShown);
    const float heldDb = linearToDb(heldPeak);

    const bool clipping = clipHoldTicks > 0;

    juce::String statusText;
    juce::Colour statusColour;

    if (inputOffline)
    {
        statusText = "INPUT OFFLINE";
        statusColour = UiColors::red;
    }
    else if (clipping)
    {
        statusText = "CLIPPING";
        statusColour = UiColors::red;
    }
    else if (peakDb >= redDb)
    {
        statusText = "PEAK";
        statusColour = UiColors::orange;
    }
    else if (rmsDb > -50.0f)
    {
        statusText = hasNote ? "OK - NOTE DETECTED" : "SIGNAL OK - NO NOTE";
        statusColour = UiColors::green;
    }
    else
    {
        statusText = "NO SIGNAL";
        statusColour = UiColors::muted;
    }

    // Header: title on the left, device + status on the right.
    g.setColour(UiColors::accent);
    g.setFont(GameUi::eyebrow(tight ? 9.0f : 10.0f));
    g.drawText("GUITAR INPUT", headerArea, juce::Justification::centredLeft, false);

    {
        g.setFont(GameUi::font(tight ? 9.0f : 10.0f, true));
        g.setColour(statusColour);
        g.drawText(statusText, headerArea, juce::Justification::centredRight, false);
    }

    // Meter: fixed dB scale with green/yellow/red zones and a peak-hold marker.
    if (meterArea.getWidth() > 40 && meterArea.getHeight() > 4)
    {
        const auto bar = meterArea.toFloat();

        g.setColour(UiColors::border);
        g.fillRoundedRectangle(bar, bar.getHeight() * 0.5f);

        const float yellowX = bar.getX() + bar.getWidth() * dbToNormalised(yellowDb);
        const float redX = bar.getX() + bar.getWidth() * dbToNormalised(redDb);

        g.setColour(UiColors::green.withAlpha(0.22f));
        g.fillRect(juce::Rectangle<float>(bar.getX(), bar.getY(),
                                          yellowX - bar.getX(), bar.getHeight()));
        g.setColour(UiColors::orange.withAlpha(0.24f));
        g.fillRect(juce::Rectangle<float>(yellowX, bar.getY(),
                                          redX - yellowX, bar.getHeight()));
        g.setColour(UiColors::red.withAlpha(0.26f));
        g.fillRect(juce::Rectangle<float>(redX, bar.getY(),
                                          bar.getRight() - redX, bar.getHeight()));

        // Instantaneous level, drawn with the classic green->yellow->red ramp.
        const float levelW = bar.getWidth() * dbToNormalised(peakDb);

        if (levelW > 1.0f)
        {
            juce::ColourGradient gradient(
                UiColors::green, bar.getX(), bar.getY(),
                UiColors::red, bar.getRight(), bar.getY(), false);
            gradient.addColour(0.70, UiColors::green);
            gradient.addColour(juce::jlimit(0.71, 0.99,
                                            static_cast<double>(dbToNormalised(yellowDb))),
                               UiColors::orange);

            g.setGradientFill(gradient);
            g.fillRoundedRectangle(bar.withWidth(levelW), bar.getHeight() * 0.5f);
        }

        // Zone boundaries.
        g.setColour(UiColors::background.withAlpha(0.45f));
        g.drawVerticalLine(juce::roundToInt(yellowX), bar.getY(), bar.getBottom());
        g.drawVerticalLine(juce::roundToInt(redX), bar.getY(), bar.getBottom());

        // Peak hold marker.
        if (heldPeak > 0.00001f)
        {
            const float holdX = bar.getX() + bar.getWidth() * dbToNormalised(heldDb);
            g.setColour(heldDb >= redDb ? UiColors::red
                                        : (heldDb >= yellowDb ? UiColors::orange
                                                              : UiColors::text));
            g.fillRect(juce::Rectangle<float>(holdX - 1.5f, bar.getY(), 3.0f,
                                              bar.getHeight()));
        }
    }

    // Numbers row: RMS, held peak and the detected note.
    {
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(tight ? 9.5f : 11.0f));

        juce::String numbers = "RMS " + formatDb(rmsShown) + " dBFS   PEAK "
            + formatDb(heldPeak) + " dBFS";

        if (hasNote)
        {
            numbers << "   " << noteText;

            if (noteFrequency > 0.0)
                numbers << " " << juce::String(noteFrequency, 1) << " Hz";
        }

        g.drawFittedText(numbers, numbersArea, juce::Justification::centredLeft, 1);

        if (! tight)
        {
            g.setColour(UiColors::muted);
            g.setFont(GameUi::eyebrow(8.5f));
            g.drawFittedText("SENSITIVITY",
                             sensLabelArea, juce::Justification::centredRight, 1);
        }
    }
}
