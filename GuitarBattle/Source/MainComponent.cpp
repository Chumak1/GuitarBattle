#include "MainComponent.h"

// Keep this implementation compiled by the existing Source-only Visual Studio project.
// If MidiMelodyGenerator.cpp is later registered as a separate Projucer source file,
// remove this include to avoid compiling its implementation twice.
#include "Generation/MidiMelodyGenerator.cpp"
#include "Generation/SongArrangement.cpp"
#include "Audio/PitchDetectionThread.h"

#include <vector>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <sstream>
#include <string>

#include "Midi/MidiParser.h"

#include "UI/GameStyle.h"

// Forward declaration: the implementation is located later in this file.
static juce::AudioBuffer<float> makePitchPreservingTimeStretch(
    const juce::AudioBuffer<float>& source, double speedRatio);


//==============================================================================
// Branded audio settings panel used by the Audio Settings dialog.
// Keeps JUCE's native device selector and its existing deviceManager bindings,
// while placing it inside the app's dark navy/gold visual system.
class AudioSettingsPanel final : public juce::Component,
                                 private juce::ChangeListener
{
public:
    AudioSettingsPanel(juce::AudioDeviceManager& manager,
                       int initialInputChannel,
                       std::function<void(int)> onInputChannelChanged)
        : selector(manager, 0, 32, 0, 2, true, true, true, false),
          deviceManager(manager),
          inputChannelChanged(onInputChannelChanged)
    {
        setSize(760, 600);
        addAndMakeVisible(selector);
        addAndMakeVisible(inputChannelLabel);
        addAndMakeVisible(inputChannelBox);

        // Keep the native selector readable on the dark theme.
        std::function<void(juce::Component&)> applyReadableAudioColours;
        applyReadableAudioColours = [&applyReadableAudioColours](juce::Component& component)
        {
            if (auto* label = dynamic_cast<juce::Label*>(&component))
                label->setColour(juce::Label::textColourId, UiColors::text);

            if (auto* combo = dynamic_cast<juce::ComboBox*>(&component))
            {
                combo->setColour(juce::ComboBox::textColourId, UiColors::text);
                combo->setColour(juce::ComboBox::backgroundColourId, UiColors::card);
                combo->setColour(juce::ComboBox::outlineColourId, UiColors::border);
                combo->setColour(juce::ComboBox::arrowColourId, UiColors::accent);
            }

            if (auto* button = dynamic_cast<juce::Button*>(&component))
                button->setColour(juce::TextButton::textColourOffId, UiColors::text);

            for (int i = 0; i < component.getNumChildComponents(); ++i)
                applyReadableAudioColours(*component.getChildComponent(i));
        };
        applyReadableAudioColours(selector);

        inputChannelLabel.setText("GUITAR INPUT", juce::dontSendNotification);
        inputChannelLabel.setJustificationType(juce::Justification::centredLeft);
        inputChannelLabel.setColour(juce::Label::textColourId, UiColors::text);
        inputChannelLabel.setFont(juce::FontOptions().withHeight(12.0f).withStyle("Bold"));

        inputChannelBox.setColour(juce::ComboBox::textColourId, UiColors::text);
        inputChannelBox.setColour(juce::ComboBox::backgroundColourId, UiColors::card);
        inputChannelBox.setColour(juce::ComboBox::outlineColourId, UiColors::border);
        inputChannelBox.setColour(juce::ComboBox::arrowColourId, UiColors::accent);
        inputChannelBox.setTooltip("Choose the input channel that carries your guitar. "
                                   "Auto follows the loudest active input.");
        inputChannelBox.onChange = [this]
        {
            if (inputChannelChanged)
                inputChannelChanged(inputChannelBox.getSelectedId() - 2);
        };

        deviceManager.addChangeListener(this);
        rebuildInputChannelChoices(initialInputChannel);
    }

    ~AudioSettingsPanel() override
    {
        deviceManager.removeChangeListener(this);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UiColors::background);

        auto bounds = getLocalBounds().toFloat();
        GameUi::drawCard(g, bounds.reduced(12.0f), 20.0f, true);

        g.setColour(UiColors::accent);
        g.fillRoundedRectangle(juce::Rectangle<float>(30.0f, 29.0f, 5.0f, 36.0f), 2.5f);

        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(25.0f));
        g.drawText("AUDIO SETTINGS", 48, 26, getWidth() - 160, 34,
                   juce::Justification::centredLeft);

        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(13.0f));
        g.drawText("Choose your guitar input and configure audio performance.",
                   48, 59, getWidth() - 80, 24, juce::Justification::centredLeft);

        GameUi::drawPill(g,
                         juce::Rectangle<float>(getWidth() - 134.0f, 31.0f, 96.0f, 22.0f),
                         "AUDIO I/O",
                         UiColors::green,
                         true);

        g.setColour(UiColors::border);
        g.drawLine(30.0f, 96.0f, static_cast<float>(getWidth() - 30), 96.0f, 1.0f);
        g.drawLine(30.0f, static_cast<float>(getHeight() - 92),
                   static_cast<float>(getWidth() - 30),
                   static_cast<float>(getHeight() - 92), 1.0f);

        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(10.5f));
        g.drawText("Auto follows the loudest active input. Enable extra inputs above for other channels.",
                   30, getHeight() - 44, getWidth() - 60, 20,
                   juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        selector.setBounds(30, 108, getWidth() - 60,
                           juce::jmax(80, getHeight() - 204));
        inputChannelLabel.setBounds(30, getHeight() - 76, 130, 24);
        inputChannelBox.setBounds(168, getHeight() - 78, 320, 28);
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        rebuildInputChannelChoices(-1);
    }

    void rebuildInputChannelChoices(int initialChannel)
    {
        int selected = (inputChannelBox.getSelectedId() > 0)
            ? inputChannelBox.getSelectedId() - 2
            : initialChannel;

        inputChannelBox.clear(juce::dontSendNotification);
        inputChannelBox.addItem("Auto (any active input)", 1);

        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            const auto names = device->getInputChannelNames();
            const auto active = device->getActiveInputChannels();

            for (int hardware = 0; hardware < names.size(); ++hardware)
            {
                if (!active[hardware])
                    continue;

                const auto name = names[hardware].trim();
                inputChannelBox.addItem(name.isNotEmpty()
                                            ? name
                                            : "Input " + juce::String(hardware + 1),
                                        hardware + 2);
            }
        }

        int id = selected < 0 ? 1 : selected + 2;
        if (inputChannelBox.indexOfItemId(id) < 0)
            id = 1;

        inputChannelBox.setSelectedId(id, juce::dontSendNotification);
    }

    juce::AudioDeviceSelectorComponent selector;
    juce::AudioDeviceManager& deviceManager;
    std::function<void(int)> inputChannelChanged;
    juce::Label inputChannelLabel;
    juce::ComboBox inputChannelBox;
};

//==============================================================================
// Constructor
//==============================================================================

MainComponent::MainComponent()
    : overlayComponent(*this)
{
    // Apply the admin-panel visual language to every JUCE control, including
    // alert windows, dialog boxes and popup menus.
    juce::LookAndFeel::setDefaultLookAndFeel(&gameLookAndFeel);
    setLookAndFeel(&gameLookAndFeel);

    pitchThread = std::make_unique<PitchDetectionThread>(*this);
    // Configure persistent settings before loading songs or querying best scores.
    juce::PropertiesFile::Options options;
    options.applicationName = "GuitarBattle";
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    options.folderName = "GuitarBattle";
    options.commonToAllUsers = false;
    appProperties.setStorageParameters(options);

    if (auto* settings = appProperties.getUserSettings())
        preferredInputChannel.store(
            settings->getIntValue("InputChannel", -1),
            std::memory_order_relaxed);

    setSize(
        1000,
        950);

    startTimerHz(30);

    overlayComponent.setBounds(getLocalBounds());
    addAndMakeVisible(overlayComponent);

    addAndMakeVisible(
        tablatureComponent);

    //==========================================================================
    // Tuner UI

    tunerTitleLabel.setText(
        "LIVE INPUT",
        juce::dontSendNotification);

    tunerTitleLabel.setFont(
        GameUi::eyebrow(11.0f));

    tunerTitleLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    tunerTitleLabel.setJustificationType(
        juce::Justification::centredLeft);

    tunerNoteLabel.setText(
        "---",
        juce::dontSendNotification);

    tunerNoteLabel.setFont(
        GameUi::heading(72.0f));

    tunerNoteLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    tunerNoteLabel.setJustificationType(
        juce::Justification::centred);

    tunerFrequencyLabel.setText(
        "--- Hz",
        juce::dontSendNotification);

    tunerFrequencyLabel.setFont(
        GameUi::font(18.0f, true));

    tunerFrequencyLabel.setColour(
        juce::Label::textColourId,
        UiColors::muted);

    tunerFrequencyLabel.setJustificationType(
        juce::Justification::centred);

    tunerStringLabel.setText(
        "---",
        juce::dontSendNotification);

    tunerStringLabel.setFont(
        GameUi::font(19.0f, true));

    tunerStringLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    tunerStringLabel.setJustificationType(
        juce::Justification::centred);

    tunerCentsLabel.setText(
        "--- cents",
        juce::dontSendNotification);

    tunerCentsLabel.setFont(
        GameUi::font(19.0f, true));

    tunerCentsLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    tunerCentsLabel.setJustificationType(
        juce::Justification::centred);

    tunerStatusLabel.setText(
        "PLAY A STRING",
        juce::dontSendNotification);

    tunerStatusLabel.setFont(
        GameUi::font(13.5f, true, 0.8f));

    tunerStatusLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    tunerStatusLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(tunerTitleLabel);
    addAndMakeVisible(tunerNoteLabel);
    addAndMakeVisible(tunerFrequencyLabel);
    addAndMakeVisible(tunerStringLabel);
    addAndMakeVisible(tunerCentsLabel);
    addAndMakeVisible(tunerStatusLabel);

    tunerStartButton.setEnabled(false);
    tunerStartButton.setColour(juce::TextButton::buttonColourId, UiColors::accent);
    tunerStartButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accentStrong);
    tunerStartButton.setColour(juce::TextButton::textColourOffId, UiColors::onAccent);
    tunerStartButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);

    addAndMakeVisible(tunerStartButton);

    tunerSkipButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    tunerSkipButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
    tunerSkipButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    tunerSkipButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    addAndMakeVisible(tunerSkipButton);
    tunerSkipButton.onClick = [this]
    {
        // Treat setup as complete and reuse the normal game-start path.
        tuner.markAllStringsTuned();
        tunerStartButton.onClick();
    };

    tunerStartButton.onClick =
        [this]
        {
            if (!tuner.areAllStringsTuned())
            {
                return;
            }

            if (gameSession.getMidiNotes().empty())
            {
                DBG("START BLOCKED: MIDI is empty");
                return;
            }

            // Open a server session for online songs when the player is signed in.
            // If the server is unavailable, local gameplay remains available.
            startOnlineGameSession();

            //==============================================================
 // Reset game session

            gameSession.resetGameplay();
            // resetGameplay() resets its own tempo defaults, so restore the
            // selected song's original BPM and the currently selected speed.
            const double selectedBpm = baseBpm * speedPercent / 100.0;
            gameSession.setBaseBpm(baseBpm);
            gameSession.setSpeedPercent(speedPercent);
            gameSession.setBpm(selectedBpm);
            bpm.store(selectedBpm, std::memory_order_relaxed);
            audioPlaybackRate.store(speedPercent / 100.0, std::memory_order_relaxed);
            metronome.setBpm(selectedBpm);
            tablatureComponent.setBpm(selectedBpm);
            bpmLabel.setText(
                juce::String(static_cast<int>(std::round(selectedBpm))),
                juce::dontSendNotification);
            gameplayStats.reset();

            gameSessionLabel.setText(
                "0",
                juce::dontSendNotification);

            hitLabel.setText(
                "READY",
                juce::dontSendNotification);

            //==============================================================
            // Reset gameplay state

            tunerActive.store(
                false,
                std::memory_order_relaxed);

            gameSession.setTargetHit(false);
            gameSession.resetResolvedTarget();
            gameSession.setLastHitSample(0);

            lastOnsetSample.store(
                0,
                std::memory_order_relaxed);

            lastNoteOnSample.store(
                0,
                std::memory_order_relaxed);


            // Reset onset detection for the first gameplay note.
            noteOn.store(
                false,
                std::memory_order_relaxed);

            previousRmsValue = 0.0;
            previousRmsValue2 = 0.0;

            pausedSamples.store(
                0,
                std::memory_order_relaxed);

            pauseStartSample.store(
                0,
                std::memory_order_relaxed);

            //==============================================================
            // REAL START SAMPLE
            //
            // No artificial MIDI shift.
            // MIDI startBeat remains unchanged.

            const auto currentSample =
                audioSampleCounter.load(
                    std::memory_order_relaxed);

            const double beatLengthSamples =
                metronome.getBeatLengthSamples();

            // Give the player a full bar of four audible metronome clicks.
            // The song timeline starts on the beat immediately after click 4.
            // Use the fractional beat length so the count-in and the song start
            // stay locked to the same timeline as the MIDI backing.
            countInRemaining.store(4, std::memory_order_relaxed);
            countInActive.store(true, std::memory_order_release);
            tablatureComponent.setPlayheadRunning(false);

            const auto countInSamples =
                static_cast<uint64_t>(
                    std::llround(beatLengthSamples * 4.0));

            gameplayStartSample.store(
                currentSample + countInSamples,
                std::memory_order_relaxed);

            currentPlayheadBeat.store(
                0.0,
                std::memory_order_relaxed);
            tablatureComponent.setPlayheadBeat(0.0);

            //==============================================================
 // Reset metronome timeline
 //
 // First metronome beat happens at START.

            metronome.resetScheduler(
                currentSample);

            metronomeSamplesRemaining = 0;
            metronomePlaybackPosition = 0;

            //==============================================================
            // First MIDI target

            gameSession.setTargetNoteIndex(0);

            setTargetFromMidiIndex(0);

            gameState =
                GameState::Playing;

            gameStartButton.setButtonText("START");
            menuButton.setButtonText("MAIN MENU");

            DBG("START -> PLAYING");
            DBG("GAMEPLAY START SAMPLE = "
                + juce::String(
                    static_cast<juce::int64>(
                        currentSample)));

            tunerStartButton.setEnabled(false);

            pauseButton.setButtonText(
                "PAUSE");

            hitLabel.setText(
                "COUNT-IN 4",
                juce::dontSendNotification);

            resized();
            repaint();
        };

    //==========================================================================
    // Game buttons

    gameStartButton.setColour(
        juce::TextButton::buttonColourId,
        UiColors::accent);
    gameStartButton.setColour(
        juce::TextButton::buttonOnColourId,
        UiColors::accentStrong);
    gameStartButton.setColour(
        juce::TextButton::textColourOffId,
        UiColors::onAccent);
    gameStartButton.setColour(
        juce::TextButton::textColourOnId,
        UiColors::onAccent);
    gameStartButton.setLookAndFeel(&getLookAndFeel());

    pauseButton.setColour(
        juce::TextButton::buttonColourId,
        UiColors::surface2);
    pauseButton.setColour(
        juce::TextButton::buttonOnColourId,
        UiColors::accent);
    pauseButton.setColour(
        juce::TextButton::textColourOffId,
        UiColors::text);
    pauseButton.setColour(
        juce::TextButton::textColourOnId,
        UiColors::onAccent);

    menuButton.setColour(
        juce::TextButton::buttonColourId,
        juce::Colours::transparentBlack);
    menuButton.setColour(
        juce::TextButton::buttonOnColourId,
        UiColors::surface2);
    menuButton.setColour(
        juce::TextButton::textColourOffId,
        UiColors::muted);
    menuButton.setColour(
        juce::TextButton::textColourOnId,
        UiColors::text);

    // Consistent typography across the primary gameplay controls.
    for (auto* button : { &gameStartButton, &pauseButton, &menuButton,
                          &speed50Button, &speed100Button, &speed120Button,
                          &audioSettingsButton, &tunerStartButton, &tunerSkipButton })
        button->setLookAndFeel(&getLookAndFeel());

    addAndMakeVisible(gameStartButton);
    addAndMakeVisible(pauseButton);
    addAndMakeVisible(menuButton);

    // Compact stompbox-style distortion pedal controls.
    distortionButton.setClickingTogglesState(false);
    distortionButton.setColour(juce::TextButton::buttonColourId,
                               UiColors::red.darker(0.58f));
    distortionButton.setColour(juce::TextButton::buttonOnColourId,
                               UiColors::red);
    distortionButton.setColour(juce::TextButton::textColourOffId,
                               UiColors::text);
    distortionButton.setColour(juce::TextButton::textColourOnId,
                               UiColors::background);
    distortionButton.setTooltip("Switch the distortion pedal on or off.");
    addAndMakeVisible(distortionButton);
    midiBackingButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    midiBackingButton.setColour(juce::TextButton::buttonOnColourId,
                                UiColors::surface2.interpolatedWith(UiColors::green, 0.20f));
    midiBackingButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    midiBackingButton.setColour(juce::TextButton::textColourOnId, UiColors::background);
    midiBackingButton.setToggleState(true, juce::dontSendNotification);
    midiBackingButton.setButtonText("SOLO GUITAR ON");
    midiBackingButton.setTooltip("Toggle the MIDI solo-guitar part generated by FretFight.");
    addAndMakeVisible(midiBackingButton);
    midiBackingButton.onClick = [this]
    {
        const bool enabled = !midiBackingEnabled.load(std::memory_order_relaxed);
        midiBackingEnabled.store(enabled, std::memory_order_relaxed);
        midiBackingButton.setButtonText(enabled ? "SOLO GUITAR ON" : "SOLO GUITAR OFF");
        midiBackingButton.setToggleState(enabled, juce::dontSendNotification);
    };
    midiBackingVolumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    midiBackingVolumeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 20);
    midiBackingVolumeSlider.setColour(juce::Slider::textBoxTextColourId, UiColors::text);
    midiBackingVolumeSlider.setColour(juce::Slider::textBoxBackgroundColourId, UiColors::card);
    midiBackingVolumeSlider.setColour(juce::Slider::textBoxOutlineColourId, UiColors::border);
    midiBackingVolumeSlider.setRange(0.0, 0.8, 0.01);
    midiBackingVolumeSlider.setValue(0.30, juce::dontSendNotification);
    midiBackingVolumeSlider.setTooltip("Solo guitar MIDI volume");
    midiBackingVolumeSlider.onValueChange = [this]
    {
        midiBackingVolume.store(static_cast<float>(midiBackingVolumeSlider.getValue()),
                                std::memory_order_relaxed);
    };
    addAndMakeVisible(midiBackingVolumeSlider);
    midiBackingVolumeLabel.setText("SOLO VOL", juce::dontSendNotification);
    midiBackingVolumeLabel.setJustificationType(juce::Justification::centredLeft);
    midiBackingVolumeLabel.setColour(juce::Label::textColourId, UiColors::text);
    midiBackingVolumeLabel.setFont(juce::FontOptions().withHeight(12.0f).withStyle("Bold"));
    addAndMakeVisible(midiBackingVolumeLabel);

    midiRhythmGuitarButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    midiRhythmGuitarButton.setColour(juce::TextButton::buttonOnColourId,
                                     UiColors::surface2.interpolatedWith(UiColors::green, 0.20f));
    midiRhythmGuitarButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    midiRhythmGuitarButton.setColour(juce::TextButton::textColourOnId, UiColors::background);
    midiRhythmGuitarButton.setToggleState(true, juce::dontSendNotification);
    midiRhythmGuitarButton.setTooltip("Toggle the rhythm-guitar WAV stem.");
    addAndMakeVisible(midiRhythmGuitarButton);
    midiRhythmGuitarButton.onClick = [this]
    {
        const bool enabled = !midiRhythmGuitarEnabled.load(std::memory_order_relaxed);
        midiRhythmGuitarEnabled.store(enabled, std::memory_order_relaxed);
        midiRhythmGuitarButton.setButtonText(enabled ? "RHYTHM GUITAR ON" : "RHYTHM GUITAR OFF");
        midiRhythmGuitarButton.setToggleState(enabled, juce::dontSendNotification);
    };
    midiRhythmGuitarVolumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    midiRhythmGuitarVolumeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 42, 18);
    midiRhythmGuitarVolumeSlider.setColour(juce::Slider::textBoxTextColourId, UiColors::text);
    midiRhythmGuitarVolumeSlider.setColour(juce::Slider::textBoxBackgroundColourId, UiColors::card);
    midiRhythmGuitarVolumeSlider.setColour(juce::Slider::textBoxOutlineColourId, UiColors::border);
    midiRhythmGuitarVolumeSlider.setRange(0.0, 0.8, 0.01);
    midiRhythmGuitarVolumeSlider.setValue(0.20, juce::dontSendNotification);
    midiRhythmGuitarVolumeSlider.setTooltip("Guitar stem volume");
    midiRhythmGuitarVolumeSlider.onValueChange = [this]
    { midiRhythmGuitarVolume.store(static_cast<float>(midiRhythmGuitarVolumeSlider.getValue()), std::memory_order_relaxed); };
    addAndMakeVisible(midiRhythmGuitarVolumeSlider);
    midiRhythmGuitarVolumeLabel.setText("GUITAR VOL", juce::dontSendNotification);
    midiRhythmGuitarVolumeLabel.setJustificationType(juce::Justification::centredLeft);
    midiRhythmGuitarVolumeLabel.setColour(juce::Label::textColourId, UiColors::text);
    midiRhythmGuitarVolumeLabel.setFont(juce::FontOptions().withHeight(10.5f).withStyle("Bold"));
    addAndMakeVisible(midiRhythmGuitarVolumeLabel);

    auto setupBackingChannel = [this](juce::TextButton& button, juce::Slider& slider,
                                      juce::Label& label, std::atomic<bool>& enabled,
                                      std::atomic<float>& volume, const juce::String& name,
                                      float initialVolume)
    {
        button.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
        button.setColour(juce::TextButton::buttonOnColourId,
                         UiColors::surface2.interpolatedWith(UiColors::green, 0.20f));
        button.setColour(juce::TextButton::textColourOffId, UiColors::text);
        button.setColour(juce::TextButton::textColourOnId, UiColors::background);
        button.setToggleState(true, juce::dontSendNotification);
        button.setTooltip("Toggle the " + name + " audio channel.");
        addAndMakeVisible(button);
        auto* buttonPtr = &button;
        auto* enabledPtr = &enabled;
        button.onClick = [buttonPtr, enabledPtr, name]()
        {
            const bool next = !enabledPtr->load(std::memory_order_relaxed);
            enabledPtr->store(next, std::memory_order_relaxed);
            buttonPtr->setButtonText(name + (next ? " ON" : " OFF"));
            buttonPtr->setToggleState(next, juce::dontSendNotification);
        };
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 42, 18);
        slider.setColour(juce::Slider::textBoxTextColourId, UiColors::text);
        slider.setColour(juce::Slider::textBoxBackgroundColourId, UiColors::card);
        slider.setColour(juce::Slider::textBoxOutlineColourId, UiColors::border);
        slider.setRange(0.0, 0.8, 0.01);
        slider.setValue(initialVolume, juce::dontSendNotification);
        auto* sliderPtr = &slider;
        auto* volumePtr = &volume;
        slider.onValueChange = [sliderPtr, volumePtr]()
        { volumePtr->store(static_cast<float>(sliderPtr->getValue()), std::memory_order_relaxed); };
        addAndMakeVisible(slider);
        label.setText(name + " VOL", juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centredLeft);
        label.setColour(juce::Label::textColourId, UiColors::text);
        label.setFont(juce::FontOptions().withHeight(10.5f).withStyle("Bold"));
        addAndMakeVisible(label);
    };
    setupBackingChannel(midiBassButton, midiBassVolumeSlider, midiBassVolumeLabel,
                        midiBassEnabled, midiBassVolume, "BASS", 0.45f);
    setupBackingChannel(midiDrumsButton, midiDrumsVolumeSlider, midiDrumsVolumeLabel,
                        midiDrumsEnabled, midiDrumsVolume, "DRUMS", 0.45f);
    setupBackingChannel(midiPianoButton, midiPianoVolumeSlider, midiPianoVolumeLabel,
                        midiPianoEnabled, midiPianoVolume, "OTHERS", 0.30f);
    setupBackingChannel(metronomeMixerButton, metronomeMixerVolumeSlider,
                        metronomeMixerVolumeLabel, metronomeMixerEnabled,
                        metronomeMixerVolume, "METRONOME", 0.70f);
    metronomeMixerVolumeSlider.setRange(0.0, 1.0, 0.01);
    metronomeMixerVolumeSlider.setTooltip("Metronome click volume");

    distortionButton.onClick = [this]
    {
        const bool enabled = !distortionEnabled.load(std::memory_order_relaxed);
        distortionEnabled.store(enabled, std::memory_order_relaxed);
        distortionButton.setButtonText(enabled ? "DISTORTION  ON" : "DISTORTION  OFF");
        distortionButton.setToggleState(enabled, juce::dontSendNotification);
    };

    auto setupPedalKnob = [this](juce::Slider& knob, juce::Label& label,
                                  const juce::String& name, double minimum,
                                  double maximum, double initial)
    {
        knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 48, 15);
        knob.setRange(minimum, maximum, 0.01);
        knob.setValue(initial, juce::dontSendNotification);
        knob.setColour(juce::Slider::rotarySliderFillColourId, UiColors::red);
        knob.setColour(juce::Slider::rotarySliderOutlineColourId,
                       UiColors::red.darker(0.72f));
        knob.setColour(juce::Slider::thumbColourId, UiColors::text);
        knob.setColour(juce::Slider::textBoxTextColourId, UiColors::text);
        knob.setColour(juce::Slider::textBoxBackgroundColourId, UiColors::surface2);
        knob.setColour(juce::Slider::textBoxOutlineColourId, UiColors::border);
        addAndMakeVisible(knob);
        label.setText(name, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centred);
        label.setColour(juce::Label::textColourId, UiColors::text);
        label.setFont(juce::FontOptions().withHeight(12.0f).withStyle("Bold"));
        addAndMakeVisible(label);
    };
    setupPedalKnob(distortionDriveSlider, distortionDriveLabel, "DRIVE", 1.0, 8.0, 2.5);
    setupPedalKnob(distortionToneSlider, distortionToneLabel, "TONE", 0.0, 1.0, 0.65);
    distortionDriveSlider.onValueChange = [this]
    { distortionDrive.store((float) distortionDriveSlider.getValue(), std::memory_order_relaxed); };
    distortionToneSlider.onValueChange = [this]
    { distortionTone.store((float) distortionToneSlider.getValue(), std::memory_order_relaxed); };

    gameStartButton.onClick =
        [this]
        {
            // START always begins a fresh session from MIDI note zero.
            // Reuse the same reset path as the tuner-complete start button
            // so scoring, onset detection, timeline and metronome are reset
            // consistently.
            if (gameState == GameState::Playing
                || gameState == GameState::Paused
                || gameState == GameState::Finished)
            {
                if (tuner.areAllStringsTuned())
                    tunerStartButton.onClick();
            }
        };

    pauseButton.onClick =
        [this]
        {
            if (gameState == GameState::Playing)
            {
                pauseGame();
            }
            else if (gameState == GameState::Paused)
            {
                resumeGame();
            }
        };

    menuButton.onClick =
        [this]
        {
            returnToSongSelect();
        };

    //==========================================================================
    // Register WAV/MP3 decoders before online song assets are loaded.
    audioFormatManager.registerBasicFormats();

    // The catalogue is server-only. Local Songs/ folders are intentionally
    // ignored; published songs arrive asynchronously from refreshOnlineCatalog().
    songPridumayButton.setButtonText("START SONG");
    rebuildCatalogRows(juce::var());

    //==========================================================================
    // Pitch / combo HUD labels

    pitchLabel.setText(
        "---",
        juce::dontSendNotification);

    pitchLabel.setFont(
        GameUi::font(20.0f, true));

    pitchLabel.setColour(
        juce::Label::textColourId,
        UiColors::green);

    pitchLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        pitchLabel);

    comboLabel.setText(
        "x0",
        juce::dontSendNotification);

    comboLabel.setFont(
        GameUi::heading(30.0f));

    comboLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    comboLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        comboLabel);

    //==========================================================================
    // Beat label

    beatLabel.setText(
        "0",
        juce::dontSendNotification);

    beatLabel.setFont(
        GameUi::font(14.0f, true));

    beatLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    beatLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        beatLabel);

    //==========================================================================
    // Bar beat label

    barBeatLabel.setText(
        "0",
        juce::dontSendNotification);

    barBeatLabel.setFont(
        GameUi::font(14.0f, true));

    barBeatLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    barBeatLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        barBeatLabel);

    //==========================================================================
    // BPM

    bpmLabel.setText(
        juce::String(static_cast<int>(std::round(baseBpm * speedPercent / 100.0))),
        juce::dontSendNotification);

    bpmLabel.setFont(
        GameUi::font(13.0f, true));

    bpmLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    bpmLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        bpmLabel);

    //==========================================================================
    // Speed buttons
    // Explicitly add all tempo buttons to the component hierarchy.
    // Without addAndMakeVisible(), setVisible(true) in resized() is not enough
    // to make these buttons appear.

    addAndMakeVisible(speed50Button);
    addAndMakeVisible(speed100Button);
    addAndMakeVisible(speed120Button);

    speed50Button.setClickingTogglesState(true);
    speed100Button.setClickingTogglesState(true);
    speed120Button.setClickingTogglesState(true);

    speed50Button.setRadioGroupId(2001);
    speed100Button.setRadioGroupId(2001);
    speed120Button.setRadioGroupId(2001);

    speed50Button.setToggleState(
        false,
        juce::dontSendNotification);

    speed100Button.setToggleState(
        true,
        juce::dontSendNotification);

    speed120Button.setToggleState(
        false,
        juce::dontSendNotification);

    auto setSpeed =
        [this](double percent)
        {
            speedPercent = percent;

            const double actualBpm =
                baseBpm
                * speedPercent
                / 100.0;

            bpm.store(
                actualBpm,
                std::memory_order_relaxed);
            audioPlaybackRate.store(speedPercent / 100.0, std::memory_order_relaxed);
            prepareExternalAudioForSpeed(speedPercent);

            metronome.setBpm(
                actualBpm);
            gameSession.setBaseBpm(baseBpm);
            gameSession.setSpeedPercent(speedPercent);
            gameSession.setBpm(actualBpm);

            bpmLabel.setText(
                juce::String(
                    static_cast<int>(
                        std::round(
                            actualBpm))),
                juce::dontSendNotification);

            tablatureComponent.setBpm(
                actualBpm);
        };

    speed50Button.onClick =
        [this, setSpeed]
        {
            setSpeed(50.0);
        };

    speed100Button.onClick =
        [this, setSpeed]
        {
            setSpeed(100.0);
        };

    speed120Button.onClick =
        [this, setSpeed]
        {
            setSpeed(120.0);
        };

    for (auto* button :
        { &speed50Button,
          &speed100Button,
          &speed120Button })
    {
        button->setColour(
            juce::TextButton::buttonColourId,
            UiColors::surface2);

        button->setColour(
            juce::TextButton::buttonOnColourId,
            UiColors::accent);

        button->setColour(
            juce::TextButton::textColourOffId,
            UiColors::muted);

        button->setColour(
            juce::TextButton::textColourOnId,
            UiColors::onAccent);
    }

    //==========================================================================
    // Song select

    songTitleLabel.setText("Choose your next challenge", juce::dontSendNotification);
    songTitleLabel.setFont(GameUi::heading(32.0f));
    songTitleLabel.setColour(juce::Label::textColourId, UiColors::text);
    songTitleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(songTitleLabel);

    // Current song title shown in the gameplay header.
    gameSongTitleLabel.setText("FretFight", juce::dontSendNotification);
    gameSongTitleLabel.setFont(GameUi::heading(23.0f));
    gameSongTitleLabel.setColour(juce::Label::textColourId, UiColors::text);
    gameSongTitleLabel.setJustificationType(juce::Justification::centredLeft);
    gameSongTitleLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(gameSongTitleLabel);

    songPridumayButton.setColour(juce::TextButton::buttonColourId, UiColors::accent);
    songPridumayButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accentStrong);
    songPridumayButton.setColour(juce::TextButton::textColourOffId, UiColors::onAccent);
    songPridumayButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    addAndMakeVisible(songPridumayButton);
    onlineCatalogButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    onlineCatalogButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
    onlineCatalogButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    onlineCatalogButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    addAndMakeVisible(onlineCatalogButton);
    onlineCatalogStatusLabel.setFont(GameUi::font(12.0f));
    onlineCatalogStatusLabel.setColour(juce::Label::textColourId, UiColors::muted);
    onlineCatalogStatusLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(onlineCatalogStatusLabel);

    // Catalogue list: one row per playable song with the player's best score.
    // Rows are painted by SongListModel in the shared navy/gold style; the card
    // behind the list already provides the rounded surface.
    songListBox.setRowHeight(56);
    songListBox.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    songListBox.setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    songListBox.setOutlineThickness(0);
    songListBox.setModel(&songListModel);
    addAndMakeVisible(songListBox);

    leaderboardTitleLabel.setText("GLOBAL RATING", juce::dontSendNotification);
    leaderboardTitleLabel.setFont(GameUi::heading(19.0f));
    leaderboardTitleLabel.setColour(juce::Label::textColourId, UiColors::text);
    leaderboardTitleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(leaderboardTitleLabel);

    leaderboardLabel.setText({}, juce::dontSendNotification);
    leaderboardLabel.setFont(GameUi::font(13.0f));
    leaderboardLabel.setColour(juce::Label::textColourId, UiColors::muted);
    leaderboardLabel.setJustificationType(juce::Justification::topLeft);
    leaderboardLabel.setMinimumHorizontalScale(0.8f);
    addAndMakeVisible(leaderboardLabel);

    refreshLeaderboardButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    refreshLeaderboardButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
    refreshLeaderboardButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    refreshLeaderboardButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    refreshLeaderboardButton.onClick = [this] { refreshGlobalLeaderboard(); };
    refreshLeaderboardButton.setButtonText("REFRESH");
    addAndMakeVisible(refreshLeaderboardButton);

    accountStatusLabel.setText("Sign in or create an account. Email confirmation is required.",
                               juce::dontSendNotification);
    accountStatusLabel.setFont(GameUi::font(12.0f));
    accountStatusLabel.setColour(juce::Label::textColourId, UiColors::muted);
    accountStatusLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(accountStatusLabel);

    loginScreenTitleLabel.setText("WELCOME BACK", juce::dontSendNotification);
    loginScreenTitleLabel.setFont(GameUi::heading(36.0f));
    loginScreenTitleLabel.setColour(juce::Label::textColourId, UiColors::text);
    loginScreenTitleLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(loginScreenTitleLabel);

    currentUserLabel.setFont(GameUi::font(13.0f, true));
    currentUserLabel.setColour(juce::Label::textColourId, UiColors::text);
    currentUserLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(currentUserLabel);

    logoutButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    logoutButton.setColour(juce::TextButton::buttonOnColourId, UiColors::surface2);
    logoutButton.setColour(juce::TextButton::textColourOffId, UiColors::muted);
    logoutButton.setColour(juce::TextButton::textColourOnId, UiColors::text);
    logoutButton.onClick = [this]
    {
        // Invalidate a sign-in chain that may still be waiting for the server.
        ++authRequestId;
        // Server-reported best scores belong to the signed-in account; the
        // locally saved values stay available after signing out.
        ++bestScoresRequestId;
        remoteBestScores.clear();
        updateCatalogRows();
        onlineAccessToken.clear();
        onlineGameSessionUuid.clear();
        currentUsername.clear();
        currentUserLabel.setText("", juce::dontSendNotification);
        accountStatusLabel.setText("Please sign in to continue.", juce::dontSendNotification);
        showAuthScreen = true;
        gameState = GameState::SongSelect;
        resized();
        repaint();
    };
    addAndMakeVisible(logoutButton);

    accountLoginButton.setColour(juce::TextButton::buttonColourId, UiColors::accent);
    accountLoginButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accentStrong);
    accountLoginButton.setColour(juce::TextButton::textColourOffId, UiColors::onAccent);
    accountLoginButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    accountLoginButton.setButtonText("SIGN IN");
    accountRegisterButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    accountRegisterButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
    accountRegisterButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    accountRegisterButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    accountRegisterButton.setButtonText("CREATE ACCOUNT");

    for (auto* button : { &verifyEmailButton, &forgotPasswordButton })
    {
        button->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        button->setColour(juce::TextButton::buttonOnColourId, UiColors::surface2);
        button->setColour(juce::TextButton::textColourOffId, UiColors::muted);
        button->setColour(juce::TextButton::textColourOnId, UiColors::text);
    }

    for (auto* button : { &accountLoginButton, &accountRegisterButton,
                          &verifyEmailButton, &forgotPasswordButton })
        addAndMakeVisible(*button);

    accountLoginButton.onClick = [this] { authenticateOnlineAccount(false); };
    accountRegisterButton.onClick = [this] { authenticateOnlineAccount(true); };
    verifyEmailButton.onClick = [this]
    {
        showEmailVerificationDialog({}, {}, false);
    };
    forgotPasswordButton.onClick = [this] { recoverOnlineAccountPassword(); };

    addAndMakeVisible(songMistButton);
    addAndMakeVisible(songChordsButton);
    addAndMakeVisible(aiSoloButton);
    addAndMakeVisible(oldGeneratorArchiveButton);
    // For now the generator uses one carefully controlled style. Keep the ID at 2
    // so it maps to MidiMelodyGenerator::Style::PowerMetal internally.
    melodyStyleCombo.addItem("Power Rock", 2);
    melodyStyleCombo.setSelectedId(2, juce::dontSendNotification);
    melodyStyleCombo.onChange = [this]
    {
        // Rebuild today's deterministic challenge with the selected style.
        // NEW GENERATION still creates a fresh random seed on demand.
        generateDailyChallengeMidi(false);
    };
    melodyStyleCombo.setColour(juce::ComboBox::backgroundColourId, UiColors::card);
    melodyStyleCombo.setColour(juce::ComboBox::textColourId, UiColors::text);
    melodyStyleCombo.setColour(juce::ComboBox::outlineColourId, UiColors::blueDark);
    addAndMakeVisible(melodyStyleCombo);
    addAndMakeVisible(generateMelodyButton);
    addAndMakeVisible(newGenerationButton);
    for (auto* button : { &songMistButton, &songChordsButton, &aiSoloButton, &oldGeneratorArchiveButton, &generateMelodyButton, &newGenerationButton })
    {
        button->setColour(juce::TextButton::buttonColourId, UiColors::card);
        button->setColour(juce::TextButton::textColourOffId, UiColors::text);
        button->setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
        button->setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    }
    addAndMakeVisible(songPridumayBestLabel);
    addAndMakeVisible(songMistBestLabel);
    addAndMakeVisible(songChordsBestLabel);
    addAndMakeVisible(dailyChallengeBestLabel);

    auto configureBestLabel = [](juce::Label& label)
    {
        label.setFont(GameUi::eyebrow(10.0f));
        label.setColour(juce::Label::textColourId, UiColors::muted);
        label.setJustificationType(juce::Justification::centredLeft);
        label.setInterceptsMouseClicks(false, false);
    };
    configureBestLabel(songPridumayBestLabel);
    configureBestLabel(songMistBestLabel);
    configureBestLabel(songChordsBestLabel);
    configureBestLabel(dailyChallengeBestLabel);
    songPridumayBestLabel.setColour(juce::Label::textColourId, UiColors::accent);
    updateSongBestScoreLabels();

    songPridumayButton.onClick = [this]
    {
        // Loads the highlighted catalogue row; online assets are downloaded on
        // demand by downloadOnlineSong() on an ApiClient worker thread.
        startSelectedSong();
    };

    onlineCatalogButton.onClick = [this]
    {
        // Re-reads the published catalogue (and the personal best scores when
        // signed in). The request runs on an ApiClient worker thread; this
        // handler never blocks the message thread.
        refreshOnlineCatalog();
    };

    // Populate the server catalogue as soon as the client starts. There is no
    // local fallback: if the request fails the list stays empty until REFRESH.
    refreshOnlineCatalog();

    songMistButton.onClick = [] {};
    songChordsButton.onClick = [] {};
    aiSoloButton.onClick = [] {};

    oldGeneratorArchiveButton.onClick = [this]
    {
        oldGeneratorArchiveOpen = !oldGeneratorArchiveOpen;
        oldGeneratorArchiveButton.setButtonText(oldGeneratorArchiveOpen
            ? "CLOSE OLD GENERATOR" : "OLD GENERATOR (ARCHIVE)");
        resized();
        repaint();
    };

    generateMelodyButton.onClick = [this]
    {
        if (loadGeneratedMelody())
            beginTuningForSelectedSong();
    };

    // Test-only control: create a fresh random seed, rebuild guitar and backing
    // MIDI together, then open the new challenge so its tablature can be tested.
    newGenerationButton.onClick = [this]
    {
        if (generateDailyChallengeMidi(true)
            && loadGeneratedMelody())
        {
            beginTuningForSelectedSong();
        }
    };

    //==========================================================================
    // Score display (gameSessionLabel is the visible score label)

    gameSessionLabel.setText(
        "0",
        juce::dontSendNotification);

    gameSessionLabel.setFont(
        GameUi::heading(46.0f));

    gameSessionLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    gameSessionLabel.setJustificationType(
        juce::Justification::centredRight);

    addAndMakeVisible(
        gameSessionLabel);

    //==========================================================================
    // Hit status

    hitLabel.setText(
        "READY",
        juce::dontSendNotification);

    hitLabel.setFont(
        GameUi::heading(27.0f));

    hitLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    hitLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        hitLabel);

    //==========================================================================
    // Target

    targetNoteLabel.setText(
        gameSession.getTargetNote(),
        juce::dontSendNotification);

    targetNoteLabel.setFont(
        GameUi::heading(34.0f));

    targetNoteLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    targetNoteLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        targetNoteLabel);

    //==========================================================================
    // Audio settings

    audioSettingsButton.setButtonText(
        "AUDIO");

    audioSettingsButton.setColour(
        juce::TextButton::buttonColourId,
        UiColors::surface2);

    audioSettingsButton.setColour(
        juce::TextButton::buttonOnColourId,
        UiColors::accent);

    audioSettingsButton.setColour(
        juce::TextButton::textColourOffId,
        UiColors::text);

    audioSettingsButton.setColour(
        juce::TextButton::textColourOnId,
        UiColors::onAccent);

    addAndMakeVisible(
        audioSettingsButton);

    audioSettingsButton.onClick =
        [this]
        {
            juce::DialogWindow::LaunchOptions options;

            options.content.setOwned(
                new AudioSettingsPanel(
                    deviceManager,
                    preferredInputChannel.load(std::memory_order_relaxed),
                    [this](int channel)
                    {
                        setPreferredInputChannel(channel);
                    }));

            options.dialogTitle =
                "Audio Settings";

            options.dialogBackgroundColour =
                UiColors::background;

            options.escapeKeyTriggersCloseButton =
                true;

            options.useNativeTitleBar =
                true;

            options.resizable =
                true;

            options.launchAsync();
        };

    //==========================================================================
    // Difficulty

    easyButton.setClickingTogglesState(true);
    normalButton.setClickingTogglesState(true);
    hardButton.setClickingTogglesState(true);

    easyButton.setRadioGroupId(1001);
    normalButton.setRadioGroupId(1001);
    hardButton.setRadioGroupId(1001);

    easyButton.setToggleState(
        false,
        juce::dontSendNotification);

    normalButton.setToggleState(
        true,
        juce::dontSendNotification);

    hardButton.setToggleState(
        false,
        juce::dontSendNotification);

    easyButton.onClick =
        [this]
        {
            difficulty =
                Difficulty::Easy;
        };

    normalButton.onClick =
        [this]
        {
            difficulty =
                Difficulty::Normal;
        };

    hardButton.onClick =
        [this]
        {
            difficulty =
                Difficulty::Hard;
        };

    //==========================================================================
    // Restore audio device settings and the server-hosted sound pack

    auto savedState =
        appProperties.getUserSettings()
        ->getXmlValue(
            "AudioDeviceState");

    // The sound pack is downloaded from the server, never read from a local
    // D:\GuitarBattle\... path.  A validated cache is decoded before the audio
    // device starts; otherwise the built-in fallbacks are used until the
    // background refresh finishes.
    const auto cachedManifest = ClientAssets::readCachedManifest();
    clientAssetsReady = ClientAssets::cacheMatches(cachedManifest);
    clientAssetsRoot = clientAssetsReady ? ClientAssets::currentRoot()
                                         : juce::File();

    loadGuitarSampleBank(clientAssetsRoot);
    loadBackingSampleBanks(clientAssetsRoot);
    loadMetronomeSamples(clientAssetsRoot);

    // Song stems are loaded when a catalogue entry is selected.

    startAudioDevice(savedState.get());
    startClientAssetRefresh();

    //==========================================================================
    // Start pitch thread

    pitchThread->startThread();

    // Added last so pause/count-in cards paint above the HUD and mixer. The
    // component is transparent and does not intercept mouse input.
    addAndMakeVisible(modalOverlayComponent);
    modalOverlayComponent.toFront(false);

    resized();
    repaint();
}

//==============================================================================
// Timer
//==============================================================================

void MainComponent::timerCallback()
{
    // A downloaded sound pack is applied only while the audio device is idle.
    if (pendingClientAssetsReload)
        applyPendingClientAssetsReload();

    updateHitFeedbackAnimation();

    if (gameState != GameState::Playing)
        return;

    // COUNT IN is rendered by MainOverlayComponent across the full window.
    // Do not call TablatureComponent::setCountInDisplay here: the tab view
    // does not own the centered count-in card.
    const bool countInNowActive = countInActive.load(std::memory_order_acquire);

    if (countInNowActive)
    {
        const auto currentSample =
            audioSampleCounter.load(std::memory_order_relaxed);
        const auto startSample =
            gameplayStartSample.load(std::memory_order_relaxed);

        if (currentSample >= startSample)
        {
            countInActive.store(false, std::memory_order_release);
            tablatureComponent.setPlayheadRunning(true);
            hitLabel.setText("GO!", juce::dontSendNotification);
            repaint(); // Remove the large countdown overlay as gameplay begins.
        }
        else
        {
            const int remaining =
                countInRemaining.load(std::memory_order_relaxed);
            hitLabel.setText(
                "COUNT-IN " + juce::String(juce::jmax(1, remaining)),
                juce::dontSendNotification);
            repaint(); // Refresh the custom-painted number on every timer tick.
            return; // Don't resolve misses or advance the playhead during count-in.
        }
    }

    const double beat =
        currentPlayheadBeat.load(
            std::memory_order_relaxed);

    const int currentTarget =
        gameSession.getTargetNoteIndex();

    //==========================================================================
    // Automatic MISS after +150 ms

    if (currentTarget >= 0
        && currentTarget <
        static_cast<int>(
            gameSession.getMidiNotes().size()))
    {
        auto* device =
            deviceManager.getCurrentAudioDevice();

        const double sampleRate =
            device != nullptr
            ? device->getCurrentSampleRate()
            : 0.0;

        if (sampleRate > 0.0)
        {
            const auto currentSample =
                audioSampleCounter.load(
                    std::memory_order_relaxed);

            const auto targetSample =
                getTargetSample(
                    currentTarget,
                    sampleRate);

            const auto missTimeoutSamples =
                static_cast<uint64_t>(
                    sampleRate * 0.300);

            if (currentSample >= targetSample + missTimeoutSamples)
            {
                DBG("AUTO MISS"
                    << " target=" << currentTarget
                    << " currentSample=" << currentSample
                    << " targetSample=" << targetSample
                    << " deltaMs="
                    << ((currentSample - targetSample) * 1000.0 / sampleRate));

                if (gameSession.resolveTarget(currentTarget))
                {
                    gameplayStats.recordMiss();
                    gameSession.setTargetHit(true);

                    juce::MessageManager::callAsync(
                        [this]
                        {
                            showHitFeedback("MISS", 4);
                        });

                    advanceTarget();
                }
            }
        }
    }

    //==========================================================================
    // Playhead

    tablatureComponent.setPlayheadBeat(
        beat);

    beatLabel.setText(
        juce::String(
            static_cast<int>(
                std::floor(beat))
            + 1),
        juce::dontSendNotification);

    const int currentBeatInBar =
        static_cast<int>(
            std::floor(beat))
        % 4;

    barBeatLabel.setText(
        juce::String(
            currentBeatInBar + 1),
        juce::dontSendNotification);

    comboLabel.setText(
        "x" + juce::String(gameplayStats.getCombo()),
        juce::dontSendNotification);
}

//==============================================================================
// Animated hit feedback
//==============================================================================

void MainComponent::showHitFeedback(const juce::String& text, int type)
{
    // Called on the message thread (all pitch-thread callers use callAsync).
    feedbackType = type;
    feedbackStartMs = juce::Time::getMillisecondCounter();
    feedbackBaseFontSize = 27.0f;
    hitLabel.setText(text, juce::dontSendNotification);
    updateHitFeedbackAnimation();
    repaint();
}

void MainComponent::updateHitFeedbackAnimation()
{
    if (feedbackType == 0)
        return;

    const uint32_t elapsed = juce::Time::getMillisecondCounter() - feedbackStartMs;
    constexpr uint32_t animationDurationMs = 620;
    if (elapsed >= animationDurationMs)
    {
        feedbackType = 0;
        hitLabel.setFont(juce::FontOptions().withHeight(feedbackBaseFontSize).withStyle("Bold"));
        hitLabel.setColour(juce::Label::textColourId, UiColors::blue);
        repaint();
        return;
    }

    const float progress = static_cast<float>(elapsed) / static_cast<float>(animationDurationMs);
    const float pulse = std::sin(progress * juce::MathConstants<float>::pi);
    const float fontSize = feedbackBaseFontSize + 10.0f * pulse;

    juce::Colour feedbackColour = UiColors::blue;
    switch (feedbackType)
    {
    case 1: feedbackColour = UiColors::orange; break; // PERFECT: gold
    case 2: feedbackColour = juce::Colour(0xFF36C98F); break; // GREAT: green
    case 3: feedbackColour = juce::Colour(0xFF55A7FF); break; // GOOD: blue
    case 4: feedbackColour = juce::Colour(0xFFFF5A67); break; // MISS: red
    case 5: feedbackColour = UiColors::orange; break; // combo milestone
    default: break;
    }

    const float alpha = 1.0f - 0.35f * progress;
    hitLabel.setFont(juce::FontOptions().withHeight(fontSize).withStyle("Bold"));
    hitLabel.setColour(juce::Label::textColourId, feedbackColour.withAlpha(alpha));
    repaint();
}

//==============================================================================
// Destructor
//==============================================================================

//==============================================================================
// Audio input channel selection
//==============================================================================

void MainComponent::setPreferredInputChannel(int channel)
{
    preferredInputChannel.store(channel, std::memory_order_relaxed);

    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue("InputChannel", channel);
        settings->saveIfNeeded();
    }
}

MainComponent::~MainComponent()
{
    if (pitchThread != nullptr)
        pitchThread->stopThread(
        1000);

    if (auto* settings =
        appProperties.getUserSettings())
    {
        settings->setValue(
            "AudioDeviceState",
            deviceManager.createStateXml().get());
    }

    shutdownAudio();

    setLookAndFeel(nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
}

//==============================================================================
// Server-hosted sound pack
//==============================================================================

void MainComponent::startAudioDevice(const juce::XmlElement* savedState)
{
    setAudioChannels(32, 2, savedState);
    audioDeviceStarted = true;

    // Older builds saved a two-input device state. Expand it once so a guitar
    // connected to any input of a multi-channel interface is captured without
    // asking the user to reconfigure the device first.
    if (auto* settings = appProperties.getUserSettings())
    {
        if (! settings->getBoolValue("AudioInputsExpanded", false))
        {
            if (auto* device = deviceManager.getCurrentAudioDevice())
            {
                const int availableInputs = device->getInputChannelNames().size();
                const int activeInputs =
                    device->getActiveInputChannels().countNumberOfSetBits();

                if (availableInputs > activeInputs && availableInputs > 0)
                {
                    auto setup = deviceManager.getAudioDeviceSetup();
                    setup.inputChannels.clear();
                    setup.inputChannels.setRange(
                        0,
                        juce::jmin(availableInputs, 32),
                        true);
                    setup.useDefaultInputChannels = false;

                    // If the driver refuses the wider channel set, restore the
                    // previously saved device setup instead of leaving the app
                    // without an audio device.
                    if (deviceManager.setAudioDeviceSetup(setup, true).isNotEmpty())
                        deviceManager.initialise(32, 2, savedState, true);
                }
            }

            settings->setValue("AudioInputsExpanded", true);
            settings->saveIfNeeded();
        }
    }
}

void MainComponent::startClientAssetRefresh()
{
    if (clientAssetsRefreshInFlight)
        return;

    clientAssetsRefreshInFlight = true;

    juce::Component::SafePointer<MainComponent> safeThis(this);

    ClientAssets::refresh(apiClient,
        [safeThis](ClientAssets::RefreshResult result)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->clientAssetsRefreshInFlight = false;

            if (! result.ok)
            {
                if (! self->clientAssetsReady)
                    self->onlineCatalogStatusLabel.setText(
                        "Sound pack unavailable: " + result.message
                            + " Built-in sounds will be used.",
                        juce::dontSendNotification);

                return;
            }

            const bool changed = result.updated
                || ! self->clientAssetsReady
                || self->clientAssetsRoot != ClientAssets::currentRoot();

            self->clientAssetsRoot = ClientAssets::currentRoot();
            self->clientAssetsReady = true;

            if (changed)
            {
                self->pendingClientAssetsReload = true;
                self->applyPendingClientAssetsReload();
            }
        });
}

void MainComponent::applyPendingClientAssetsReload()
{
    if (! pendingClientAssetsReload)
        return;

    // Never rebuild the sample banks under a running gameplay callback.  The
    // timer applies the pack as soon as the player is back in a menu screen.
    if (gameState == GameState::Playing || gameState == GameState::Paused)
        return;

    pendingClientAssetsReload = false;
    reloadClientAssets();
}

void MainComponent::reloadClientAssets()
{
    if (! clientAssetsRoot.isDirectory())
        return;

    std::unique_ptr<juce::XmlElement> deviceState;

    if (audioDeviceStarted)
    {
        deviceState = deviceManager.createStateXml();
        shutdownAudio();
        audioDeviceStarted = false;
    }

    loadGuitarSampleBank(clientAssetsRoot);
    loadBackingSampleBanks(clientAssetsRoot);
    loadMetronomeSamples(clientAssetsRoot);

    startAudioDevice(deviceState.get());

    resized();
    repaint();
}

//==============================================================================
// Pause
//==============================================================================

void MainComponent::pauseGame()
{
    if (gameState != GameState::Playing)
        return;

    stateBeforePause =
        gameState;

    const auto currentSample =
        audioSampleCounter.load(
            std::memory_order_relaxed);

    pauseStartSample.store(
        currentSample,
        std::memory_order_relaxed);

    metronome.pauseScheduler();

    metronomeSamplesRemaining = 0;
    metronomePlaybackPosition = 0;

    gameState =
        GameState::Paused;

    pauseButton.setButtonText(
        "RESUME");

    hitLabel.setText(
        "PAUSED",
        juce::dontSendNotification);

    repaint();
}

//==============================================================================
// Resume
//==============================================================================

void MainComponent::resumeGame()
{
    if (gameState != GameState::Paused)
        return;

    const auto currentSample =
        audioSampleCounter.load(
            std::memory_order_relaxed);

    const auto pauseStart =
        pauseStartSample.load(
            std::memory_order_relaxed);

    //==========================================================================

    uint64_t pauseDuration = 0;

    if (currentSample > pauseStart)
    {
        pauseDuration =
            currentSample - pauseStart;

        pausedSamples.fetch_add(
            pauseDuration,
            std::memory_order_relaxed);
    }

    //==========================================================================
    // IMPORTANT:
    //
    // Move gameplayStartSample forward by exactly the amount of time
    // spent paused.
    //
    // This means:
    //
    // MIDI beat 4 remains beat 4.
    // Target timing does NOT continue running during pause.
    //
    // Example:
    //
    // START        = sample 1000
    // target beat  = +50000 samples
    // pause        = 20000 samples
    //
    // after resume:
    //
    // gameplayStartSample = 21000
    // target sample       = 21000 + 50000
    //
    // So the target is delayed by the pause duration.

    if (pauseDuration > 0)
    {
        gameplayStartSample.fetch_add(
            pauseDuration,
            std::memory_order_relaxed);
    }

    //==========================================================================

    pauseStartSample.store(
        0,
        std::memory_order_relaxed);



    // Resume on the shifted song grid so the metronome stays aligned with
    // the MIDI backing and the already-running external audio timeline.
    const auto resumeSongStart =
        gameplayStartSample.load(
            std::memory_order_relaxed);

    metronome.resumeScheduler(
        currentSample,
        resumeSongStart);

    metronomeSamplesRemaining = 0;

    gameState =
        stateBeforePause;

    pauseButton.setButtonText(
        "PAUSE");

    hitLabel.setText(
        "READY",
        juce::dontSendNotification);

    repaint();
}

//==============================================================================
// Return to song select
//==============================================================================

void MainComponent::returnToSongSelect()
{
    countInActive.store(false, std::memory_order_release);
    countInRemaining.store(0, std::memory_order_relaxed);
    tablatureComponent.setPlayheadRunning(false);

    tunerActive.store(
        false,
        std::memory_order_relaxed);

    metronome.pauseScheduler();

    metronomeSamplesRemaining = 0;
    metronomePlaybackPosition = 0;

    gameState =
        GameState::SongSelect;

    gameStartButton.setButtonText("START");
    menuButton.setButtonText("MAIN MENU");

    gameSession.setTargetHit(false);
    gameSession.resetResolvedTarget();
    gameSession.setTargetNoteIndex(0);
    gameSession.setTargetNote("---");

    tuner.resetTunedStrings();

    pausedSamples.store(
        0,
        std::memory_order_relaxed);

    pauseStartSample.store(
        0,
        std::memory_order_relaxed);

    currentPlayheadBeat.store(
        0.0,
        std::memory_order_relaxed);

    tunerStartButton.setEnabled(
        false);

    pauseButton.setButtonText(
        "PAUSE");

    resized();
    repaint();
}

//==============================================================================
// Target helpers
//==============================================================================

void MainComponent::authenticateOnlineAccount(bool createAccount)
{
    // JUCE 9 no longer supports AlertWindow::runModalLoop(). The dialog stays
    // open while credentials are checked; only CANCEL (or a successful sign-in)
    // dismisses it.
    auto* prompt = new juce::AlertWindow(
        createAccount ? "Create FretFight account" : "Sign in to FretFight",
        createAccount
            ? "Username, email and password are required. We will send a 6-digit confirmation code to your email."
            : "Enter your username/email and password.",
        juce::AlertWindow::NoIcon);

    if (createAccount)
    {
        prompt->addTextEditor("username", "", "Username");
        prompt->addTextEditor("email", "", "Email");
        prompt->addTextEditor("display_name", "", "Display name (optional)");
        prompt->addTextEditor("password", "", "Password (10+ characters)", true);
        prompt->addButton("CREATE", 1);
    }
    else
    {
        prompt->addTextEditor("identifier", "", "Username or email");
        prompt->addTextEditor("password", "", "Password", true);
        prompt->addButton("SIGN IN", 1);
    }
    prompt->addButton("CANCEL", 0);

    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::Component::SafePointer<juce::AlertWindow> safePrompt(prompt);
    const bool createNewAccount = createAccount;

    // Invalidate an attempt left over from a previous credentials dialog.
    ++authRequestId;

    auto setSubmitEnabled = [safePrompt](bool shouldBeEnabled)
    {
        if (auto* dialog = safePrompt.getComponent())
            if (auto* submit = dialog->getButton(0))
                submit->setEnabled(shouldBeEnabled);
    };

    auto showError = [safeThis, safePrompt](const juce::String& title,
                                            const juce::String& message)
    {
        if (safePrompt.getComponent() == nullptr)
            return;

        // Defer the popup so it is added on top of the modal credentials window.
        juce::MessageManager::callAsync([safeThis, safePrompt, title, message]()
        {
            auto* self = safeThis.getComponent();
            auto* dialog = safePrompt.getComponent();
            if (self == nullptr || dialog == nullptr)
                return;

            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                title,
                message,
                "OK",
                dialog);
        });
    };

    auto signIn = [safeThis, safePrompt, createNewAccount, showError, setSubmitEnabled]
                  (int attemptId, juce::String usernameOrEmail, juce::String password)
    {
        auto* self = safeThis.getComponent();
        if (self == nullptr || attemptId != self->authRequestId)
            return;

        juce::DynamicObject::Ptr login = new juce::DynamicObject();
        login->setProperty("identifier", usernameOrEmail);
        login->setProperty("password", password);

        self->apiClient.postJson("/auth/login", juce::var(login.get()), {}, 5000,
            [safeThis, safePrompt, attemptId, createNewAccount, usernameOrEmail, showError, setSubmitEnabled]
            (ApiClient::Response response)
            {
                auto* component = safeThis.getComponent();
                if (component == nullptr || attemptId != component->authRequestId)
                    return;

                const auto accessToken =
                    response.json.getProperty("access_token", juce::var()).toString();
                if (accessToken.isEmpty())
                {
                    setSubmitEnabled(true);

                    if (response.statusCode == 403)
                    {
                        component->accountStatusLabel.setText(
                            "Email is not verified. Enter the code or resend it.",
                            juce::dontSendNotification);
                        showError("Email not verified",
                                  "Confirm your email before signing in. "
                                  "Use VERIFY EMAIL on the welcome screen to enter or resend the code.");
                    }
                    else if (response.statusCode == 401 || response.statusCode == 422)
                    {
                        component->accountStatusLabel.setText(
                            "Incorrect email/username or password",
                            juce::dontSendNotification);
                        showError("Sign-in failed", "Incorrect email/username or password");
                    }
                    else
                    {
                        component->accountStatusLabel.setText(
                            "Sign-in failed. Check server connection.",
                            juce::dontSendNotification);
                        showError("Sign-in failed",
                                  "Could not sign in. Check the server connection and try again.");
                    }
                    return;
                }

                component->onlineAccessToken = accessToken;

                // The dialog closes only after a successful sign-in.
                if (auto* dialog = safePrompt.getComponent())
                    dialog->exitModalState(1);

                component->finishOnlineSignIn(createNewAccount, usernameOrEmail,
                                              accessToken, attemptId);
            });
    };

    if (auto* submit = prompt->getButton(0))
    {
        submit->onClick = [safeThis, safePrompt, createNewAccount, signIn, showError, setSubmitEnabled]()
        {
            auto* self = safeThis.getComponent();
            auto* dialog = safePrompt.getComponent();
            if (self == nullptr || dialog == nullptr)
                return;

            juce::String usernameOrEmail;
            juce::String email;
            juce::String password;
            juce::String displayName;

            if (createNewAccount)
            {
                usernameOrEmail = dialog->getTextEditorContents("username").trim();
                email = dialog->getTextEditorContents("email").trim();
                displayName = dialog->getTextEditorContents("display_name").trim();
                password = dialog->getTextEditorContents("password");

                if (usernameOrEmail.isEmpty() || email.isEmpty() || password.length() < 10)
                {
                    showError("Registration failed",
                              "Fill in username, email and a password of at least 10 characters.");
                    return;
                }

                if (! email.containsChar('@') || ! email.containsChar('.'))
                {
                    showError("Registration failed", "Enter a valid email address.");
                    return;
                }
            }
            else
            {
                usernameOrEmail = dialog->getTextEditorContents("identifier").trim();
                password = dialog->getTextEditorContents("password");

                if (usernameOrEmail.isEmpty() || password.isEmpty())
                {
                    self->accountStatusLabel.setText(
                        "Incorrect email/username or password",
                        juce::dontSendNotification);
                    showError("Sign-in failed", "Incorrect email/username or password");
                    return;
                }
            }

            const int attemptId = ++self->authRequestId;
            setSubmitEnabled(false);

            if (! createNewAccount)
            {
                signIn(attemptId, usernameOrEmail, password);
                return;
            }

            juce::DynamicObject::Ptr registration = new juce::DynamicObject();
            registration->setProperty("username", usernameOrEmail);
            registration->setProperty("email", email);
            registration->setProperty("password", password);
            if (displayName.isNotEmpty())
                registration->setProperty("display_name", displayName);

            self->apiClient.postJson("/auth/register", juce::var(registration.get()), {}, 5000,
                [safeThis, safePrompt, attemptId, usernameOrEmail, email,
                 showError, setSubmitEnabled](ApiClient::Response response)
                {
                    auto* component = safeThis.getComponent();
                    if (component == nullptr || attemptId != component->authRequestId)
                        return;

                    if (response.statusCode >= 200 && response.statusCode < 300)
                    {
                        component->accountStatusLabel.setText(
                            "Account created. Enter the code sent to " + email + ".",
                            juce::dontSendNotification);

                        if (auto* dialog = safePrompt.getComponent())
                            dialog->exitModalState(1);

                        // The modal window must be fully dismissed before the
                        // verification dialog is opened on top of it.
                        juce::MessageManager::callAsync([safeThis, email, usernameOrEmail]()
                        {
                            if (auto* self = safeThis.getComponent())
                                self->showEmailVerificationDialog(email, usernameOrEmail, true);
                        });
                        return;
                    }

                    setSubmitEnabled(true);

                    if (response.statusCode == 409)
                    {
                        component->accountStatusLabel.setText(
                            "Username or email is already registered.",
                            juce::dontSendNotification);
                        showError("Registration failed",
                                  "Username or email is already registered. "
                                  "If you created this account earlier, use VERIFY EMAIL to confirm it.");
                    }
                    else if (response.statusCode == 503)
                    {
                        component->accountStatusLabel.setText(
                            "Could not send the verification email. Try again later.",
                            juce::dontSendNotification);
                        showError("Registration failed",
                                  "The account was not created because the verification email "
                                  "could not be sent. Try again later.");
                    }
                    else if (response.statusCode == 422)
                    {
                        component->accountStatusLabel.setText(
                            "Registration details were rejected.",
                            juce::dontSendNotification);
                        showError("Registration failed",
                                  "Check the email address, username, and password "
                                  "(at least 10 characters).");
                    }
                    else
                    {
                        component->accountStatusLabel.setText(
                            "Registration failed. Check server and account details.",
                            juce::dontSendNotification);
                        showError("Registration failed",
                                  "Registration failed. Check the server connection and account details.");
                    }
                });
        };
    }

    prompt->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis](int result)
        {
            if (result != 1)
                if (auto* self = safeThis.getComponent())
                    ++self->authRequestId;
        }), true);
}

void MainComponent::showEmailVerificationDialog(const juce::String& email,
                                                const juce::String& fallbackName,
                                                bool accountCreated)
{
    const auto normalizedEmail = email.trim().toLowerCase();
    auto* prompt = new juce::AlertWindow(
        "Verify your email",
        normalizedEmail.isNotEmpty()
            ? "Enter the 6-digit code sent to " + normalizedEmail + "."
            : "Enter your email and the 6-digit code. Use RESEND CODE if you need a new one.",
        juce::AlertWindow::NoIcon);

    prompt->addTextEditor("email", normalizedEmail, "Email");
    prompt->addTextEditor("code", "", "6-digit confirmation code");
    prompt->addButton("VERIFY", 1);
    prompt->addButton("RESEND CODE", 2);
    prompt->addButton("CANCEL", 0);

    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::Component::SafePointer<juce::AlertWindow> safePrompt(prompt);
    const int attemptId = ++authRequestId;

    auto setSubmitEnabled = [safePrompt](bool shouldBeEnabled)
    {
        if (auto* dialog = safePrompt.getComponent())
            if (auto* submit = dialog->getButton(0))
                submit->setEnabled(shouldBeEnabled);
    };

    auto showError = [safeThis, safePrompt](const juce::String& title,
                                            const juce::String& message)
    {
        if (safePrompt.getComponent() == nullptr)
            return;

        juce::MessageManager::callAsync([safeThis, safePrompt, title, message]()
        {
            auto* self = safeThis.getComponent();
            auto* dialog = safePrompt.getComponent();
            if (self == nullptr || dialog == nullptr)
                return;

            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                title,
                message,
                "OK",
                dialog);
        });
    };

    auto verify = [safeThis, safePrompt, attemptId, fallbackName, accountCreated,
                   setSubmitEnabled, showError]()
    {
        auto* self = safeThis.getComponent();
        auto* dialog = safePrompt.getComponent();
        if (self == nullptr || dialog == nullptr || attemptId != self->authRequestId)
            return;

        const auto emailAddress = dialog->getTextEditorContents("email").trim().toLowerCase();
        const auto code = dialog->getTextEditorContents("code").trim();

        if (emailAddress.isEmpty() || ! emailAddress.containsChar('@'))
        {
            showError("Verification failed", "Enter the email address used for registration.");
            return;
        }

        if (code.length() != 6 || ! code.containsOnly("0123456789"))
        {
            showError("Verification failed", "Enter the 6-digit code from the email.");
            return;
        }

        juce::DynamicObject::Ptr payload = new juce::DynamicObject();
        payload->setProperty("email", emailAddress);
        payload->setProperty("code", code);

        setSubmitEnabled(false);

        self->apiClient.postJson("/auth/verify-email", juce::var(payload.get()), {}, 5000,
            [safeThis, safePrompt, attemptId, fallbackName, accountCreated,
             emailAddress, setSubmitEnabled, showError](ApiClient::Response response)
            {
                auto* component = safeThis.getComponent();
                if (component == nullptr || attemptId != component->authRequestId)
                    return;

                const auto accessToken =
                    response.json.getProperty("access_token", juce::var()).toString();
                if (accessToken.isNotEmpty())
                {
                    component->onlineAccessToken = accessToken;
                    if (auto* dialog = safePrompt.getComponent())
                        dialog->exitModalState(1);

                    component->finishOnlineSignIn(
                        accountCreated,
                        fallbackName.isNotEmpty() ? fallbackName : emailAddress,
                        accessToken,
                        attemptId);
                    return;
                }

                setSubmitEnabled(true);
                component->accountStatusLabel.setText(
                    "Verification failed. Check the code and try again.",
                    juce::dontSendNotification);

                const auto detail =
                    response.json.getProperty("detail", juce::var()).toString();
                if (response.statusCode == 400 || response.statusCode == 422)
                {
                    showError("Verification failed",
                              detail.isNotEmpty()
                                  ? detail
                                  : "The code is invalid or expired. Request a new code and try again.");
                }
                else
                {
                    showError("Verification failed",
                              "Could not verify the email. Check the server connection and try again.");
                }
            });
    };

    auto resend = [safeThis, safePrompt, attemptId, showError]()
    {
        auto* self = safeThis.getComponent();
        auto* dialog = safePrompt.getComponent();
        if (self == nullptr || dialog == nullptr || attemptId != self->authRequestId)
            return;

        const auto emailAddress = dialog->getTextEditorContents("email").trim().toLowerCase();
        if (emailAddress.isEmpty() || ! emailAddress.containsChar('@'))
        {
            showError("Resend failed", "Enter the email address used for registration.");
            return;
        }

        juce::DynamicObject::Ptr payload = new juce::DynamicObject();
        payload->setProperty("email", emailAddress);

        self->apiClient.postJson("/auth/resend-verification", juce::var(payload.get()), {}, 5000,
            [safeThis, safePrompt, attemptId, emailAddress, showError](ApiClient::Response response)
            {
                auto* component = safeThis.getComponent();
                if (component == nullptr || attemptId != component->authRequestId)
                    return;

                if (response.statusCode >= 200 && response.statusCode < 300)
                {
                    component->accountStatusLabel.setText(
                        "A new code was sent to " + emailAddress + ".",
                        juce::dontSendNotification);
                    showError("Code sent",
                              "If this account needs verification, a new code is on its way to "
                                  + emailAddress + ".");
                }
                else
                {
                    showError("Resend failed",
                              "Could not request a new code. Check the server connection and try again.");
                }
            });
    };

    if (auto* verifyButton = prompt->getButton(0))
        verifyButton->onClick = verify;
    if (auto* resendButton = prompt->getButton(1))
        resendButton->onClick = resend;

    prompt->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis](int result)
        {
            if (result != 1)
                if (auto* self = safeThis.getComponent())
                    ++self->authRequestId;
        }), true);
}

void MainComponent::recoverOnlineAccountPassword()
{
    auto* prompt = new juce::AlertWindow(
        "Reset your password",
        "Enter the email address linked to your account. "
        "We will send a 6-digit password reset code.",
        juce::AlertWindow::NoIcon);

    prompt->addTextEditor("email", "", "Email");
    prompt->addButton("SEND CODE", 1);
    prompt->addButton("CANCEL", 0);

    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::Component::SafePointer<juce::AlertWindow> safePrompt(prompt);
    const int attemptId = ++authRequestId;

    auto setSubmitEnabled = [safePrompt](bool shouldBeEnabled)
    {
        if (auto* dialog = safePrompt.getComponent())
            if (auto* submit = dialog->getButton(0))
                submit->setEnabled(shouldBeEnabled);
    };

    auto showError = [safeThis, safePrompt](const juce::String& title,
                                            const juce::String& message)
    {
        if (safePrompt.getComponent() == nullptr)
            return;

        juce::MessageManager::callAsync([safeThis, safePrompt, title, message]()
        {
            auto* self = safeThis.getComponent();
            auto* dialog = safePrompt.getComponent();
            if (self == nullptr || dialog == nullptr)
                return;

            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                title,
                message,
                "OK",
                dialog);
        });
    };

    if (auto* submit = prompt->getButton(0))
    {
        submit->onClick = [safeThis, safePrompt, attemptId, setSubmitEnabled, showError]()
        {
            auto* self = safeThis.getComponent();
            auto* dialog = safePrompt.getComponent();
            if (self == nullptr || dialog == nullptr || attemptId != self->authRequestId)
                return;

            const auto emailAddress = dialog->getTextEditorContents("email").trim().toLowerCase();
            if (emailAddress.isEmpty() || ! emailAddress.containsChar('@'))
            {
                showError("Reset failed", "Enter the email address linked to your account.");
                return;
            }

            juce::DynamicObject::Ptr payload = new juce::DynamicObject();
            payload->setProperty("email", emailAddress);

            setSubmitEnabled(false);

            self->apiClient.postJson("/auth/forgot-password", juce::var(payload.get()), {}, 5000,
                [safeThis, safePrompt, attemptId, emailAddress,
                 setSubmitEnabled, showError](ApiClient::Response response)
                {
                    auto* component = safeThis.getComponent();
                    if (component == nullptr || attemptId != component->authRequestId)
                        return;

                    if (response.statusCode >= 200 && response.statusCode < 300)
                    {
                        component->accountStatusLabel.setText(
                            "If the account exists, a reset code was sent to " + emailAddress + ".",
                            juce::dontSendNotification);

                        if (auto* dialog = safePrompt.getComponent())
                            dialog->exitModalState(1);

                        juce::MessageManager::callAsync([safeThis, emailAddress]()
                        {
                            if (auto* self = safeThis.getComponent())
                                self->showPasswordResetDialog(emailAddress);
                        });
                    }
                    else
                    {
                        setSubmitEnabled(true);
                        showError("Reset failed",
                                  "Could not request a reset code. Check the server connection and try again.");
                    }
                });
        };
    }

    prompt->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis](int result)
        {
            if (result != 1)
                if (auto* self = safeThis.getComponent())
                    ++self->authRequestId;
        }), true);
}

void MainComponent::showPasswordResetDialog(const juce::String& email)
{
    const auto normalizedEmail = email.trim().toLowerCase();
    auto* prompt = new juce::AlertWindow(
        "Choose a new password",
        "Enter the 6-digit code sent to " + normalizedEmail
            + " and choose a new password of at least 10 characters.",
        juce::AlertWindow::NoIcon);

    prompt->addTextEditor("code", "", "6-digit reset code");
    prompt->addTextEditor("new_password", "", "New password (10+ characters)", true);
    prompt->addTextEditor("confirm_password", "", "Repeat new password", true);
    prompt->addButton("RESET PASSWORD", 1);
    prompt->addButton("RESEND CODE", 2);
    prompt->addButton("CANCEL", 0);

    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::Component::SafePointer<juce::AlertWindow> safePrompt(prompt);
    const int attemptId = ++authRequestId;

    auto setSubmitEnabled = [safePrompt](bool shouldBeEnabled)
    {
        if (auto* dialog = safePrompt.getComponent())
            if (auto* submit = dialog->getButton(0))
                submit->setEnabled(shouldBeEnabled);
    };

    auto showError = [safeThis, safePrompt](const juce::String& title,
                                            const juce::String& message)
    {
        if (safePrompt.getComponent() == nullptr)
            return;

        juce::MessageManager::callAsync([safeThis, safePrompt, title, message]()
        {
            auto* self = safeThis.getComponent();
            auto* dialog = safePrompt.getComponent();
            if (self == nullptr || dialog == nullptr)
                return;

            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                title,
                message,
                "OK",
                dialog);
        });
    };

    auto reset = [safeThis, safePrompt, attemptId, normalizedEmail,
                  setSubmitEnabled, showError]()
    {
        auto* self = safeThis.getComponent();
        auto* dialog = safePrompt.getComponent();
        if (self == nullptr || dialog == nullptr || attemptId != self->authRequestId)
            return;

        const auto code = dialog->getTextEditorContents("code").trim();
        const auto newPassword = dialog->getTextEditorContents("new_password");
        const auto confirmPassword = dialog->getTextEditorContents("confirm_password");

        if (code.length() != 6 || ! code.containsOnly("0123456789"))
        {
            showError("Reset failed", "Enter the 6-digit code from the email.");
            return;
        }

        if (newPassword.length() < 10)
        {
            showError("Reset failed", "The new password must be at least 10 characters.");
            return;
        }

        if (newPassword != confirmPassword)
        {
            showError("Reset failed", "The new passwords do not match.");
            return;
        }

        juce::DynamicObject::Ptr payload = new juce::DynamicObject();
        payload->setProperty("email", normalizedEmail);
        payload->setProperty("code", code);
        payload->setProperty("new_password", newPassword);

        setSubmitEnabled(false);

        self->apiClient.postJson("/auth/reset-password", juce::var(payload.get()), {}, 5000,
            [safeThis, safePrompt, attemptId, normalizedEmail,
             setSubmitEnabled, showError](ApiClient::Response response)
            {
                auto* component = safeThis.getComponent();
                if (component == nullptr || attemptId != component->authRequestId)
                    return;

                const auto accessToken =
                    response.json.getProperty("access_token", juce::var()).toString();
                if (accessToken.isNotEmpty())
                {
                    component->onlineAccessToken = accessToken;
                    if (auto* dialog = safePrompt.getComponent())
                        dialog->exitModalState(1);

                    component->finishOnlineSignIn(false, normalizedEmail, accessToken, attemptId);
                    return;
                }

                setSubmitEnabled(true);
                component->accountStatusLabel.setText(
                    "Password reset failed. Check the code and try again.",
                    juce::dontSendNotification);

                const auto detail =
                    response.json.getProperty("detail", juce::var()).toString();
                if (response.statusCode == 400 || response.statusCode == 422)
                {
                    showError("Reset failed",
                              detail.isNotEmpty()
                                  ? detail
                                  : "The code is invalid or expired. Request a new code and try again.");
                }
                else
                {
                    showError("Reset failed",
                              "Could not reset the password. Check the server connection and try again.");
                }
            });
    };

    auto resend = [safeThis, safePrompt, attemptId, normalizedEmail, showError]()
    {
        auto* self = safeThis.getComponent();
        auto* dialog = safePrompt.getComponent();
        if (self == nullptr || dialog == nullptr || attemptId != self->authRequestId)
            return;

        juce::DynamicObject::Ptr payload = new juce::DynamicObject();
        payload->setProperty("email", normalizedEmail);

        self->apiClient.postJson("/auth/forgot-password", juce::var(payload.get()), {}, 5000,
            [safeThis, safePrompt, attemptId, normalizedEmail, showError](ApiClient::Response response)
            {
                auto* component = safeThis.getComponent();
                if (component == nullptr || attemptId != component->authRequestId)
                    return;

                if (response.statusCode >= 200 && response.statusCode < 300)
                {
                    component->accountStatusLabel.setText(
                        "A new reset code was sent to " + normalizedEmail + ".",
                        juce::dontSendNotification);
                    showError("Code sent",
                              "If the account exists, a new reset code is on its way to "
                                  + normalizedEmail + ".");
                }
                else
                {
                    showError("Resend failed",
                              "Could not request a new code. Check the server connection and try again.");
                }
            });
    };

    if (auto* resetButton = prompt->getButton(0))
        resetButton->onClick = reset;
    if (auto* resendButton = prompt->getButton(1))
        resendButton->onClick = resend;

    prompt->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis](int result)
        {
            if (result != 1)
                if (auto* self = safeThis.getComponent())
                    ++self->authRequestId;
        }), true);
}

void MainComponent::finishOnlineSignIn(bool createAccount,
                                       const juce::String& fallbackName,
                                       const juce::String& accessToken,
                                       int requestId)
{
    juce::Component::SafePointer<MainComponent> safeThis(this);

    apiClient.getJson("/auth/me", accessToken, 4000,
        [safeThis, requestId, createAccount, fallbackName](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || requestId != self->authRequestId)
                return;

            juce::String display = fallbackName;
            juce::String username = fallbackName;
            if (response.transportOk)
            {
                const auto profile = response.json;
                const auto serverUsername = profile.getProperty("username", juce::var()).toString();
                const auto fromServer = profile.getProperty("display_name", juce::var()).toString();
                if (serverUsername.isNotEmpty())
                    username = serverUsername;
                if (fromServer.isNotEmpty())
                    display = fromServer;
                else if (serverUsername.isNotEmpty())
                    display = serverUsername;
            }

            self->currentUsername = username;
            self->currentUserLabel.setText(self->currentUsername, juce::dontSendNotification);
            self->showAuthScreen = false;
            self->resized();
            self->repaint();
            const auto successMessage = createAccount
                ? "Account created and signed in as " + display + ". Online scores are enabled."
                : "Signed in as " + display + ". Online scores are enabled.";
            self->accountStatusLabel.setText(successMessage, juce::dontSendNotification);
            // Defer the confirmation until the credentials dialog has fully closed.
            // Opening a second async dialog inside the first dialog's modal callback
            // can leave the confirmation hidden behind the active modal window.
            const auto title = createAccount ? juce::String("Registration successful")
                                             : juce::String("Sign-in successful");
            juce::MessageManager::callAsync([safeThis, title, successMessage]()
            {
                if (auto* component = safeThis.getComponent())
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::NoIcon,
                        title,
                        successMessage,
                        "OK",
                        component);
            });
            self->refreshGlobalLeaderboard();
            // Personal best scores require the freshly obtained token.
            self->refreshOnlineBestScores();
        });
}

void MainComponent::startOnlineGameSession()
{
    onlineGameSessionUuid.clear();

    // A response from an older start request must not resurrect a session.
    const int requestId = ++onlineSessionRequestId;

    if (onlineSongUuid.isEmpty() || onlineAccessToken.isEmpty())
        return;

    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("song_id", onlineSongUuid);
    payload->setProperty("speed_percent", juce::jlimit(50, 120, juce::roundToInt(speedPercent)));

    juce::Component::SafePointer<MainComponent> safeThis(this);
    apiClient.postJson("/games/sessions", juce::var(payload.get()), onlineAccessToken, 4000,
        [safeThis, requestId](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || requestId != self->onlineSessionRequestId)
                return;

            if (!response.transportOk)
            {
                self->accountStatusLabel.setText("Could not start online session; score will stay local.",
                                                 juce::dontSendNotification);
                return;
            }

            self->onlineGameSessionUuid = response.json.getProperty("id", juce::var()).toString();
            if (self->onlineGameSessionUuid.isEmpty())
                self->accountStatusLabel.setText("Server did not create a session; score will stay local.",
                                                 juce::dontSendNotification);
        });
}

void MainComponent::submitOnlineGameResult()
{
    if (onlineGameSessionUuid.isEmpty() || onlineAccessToken.isEmpty())
        return;

    const int hits = gameplayStats.getHitCount();
    const int misses = gameplayStats.getMissCount();
    const int total = hits + misses;
    const double accuracy = total > 0 ? 100.0 * static_cast<double>(hits) / total : 0.0;
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("score_points", juce::jmax(0, gameplayStats.getScore()));
    payload->setProperty("accuracy_percent", juce::jlimit(0.0, 100.0, accuracy));
    payload->setProperty("max_combo", juce::jmax(0, gameplayStats.getMaxCombo()));
    payload->setProperty("perfect_count", juce::jmax(0, gameplayStats.getPerfectCount()));
    payload->setProperty("good_count", juce::jmax(0, gameplayStats.getGreatCount() + gameplayStats.getGoodCount()));
    payload->setProperty("miss_count", juce::jmax(0, misses));

    const auto sessionUuid = onlineGameSessionUuid;
    onlineGameSessionUuid.clear();
    // The finished game cannot adopt a session that is still being created.
    ++onlineSessionRequestId;

    juce::Component::SafePointer<MainComponent> safeThis(this);
    apiClient.postJson("/games/sessions/" + sessionUuid + "/finish",
                       juce::var(payload.get()), onlineAccessToken, 5000,
        [safeThis](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr)
                return;

            if (!response.transportOk)
            {
                self->accountStatusLabel.setText("Score saved locally; server submission failed.",
                                                 juce::dontSendNotification);
                return;
            }

            if (response.json.getProperty("id", juce::var()).toString().isNotEmpty())
            {
                self->accountStatusLabel.setText("Online score submitted. Refreshing global rating...",
                                                 juce::dontSendNotification);
                self->refreshGlobalLeaderboard();
                // Pick up the new personal best for this song right away.
                self->refreshOnlineBestScores();
            }
            else
            {
                self->accountStatusLabel.setText("Score saved locally; server rejected online result.",
                                                 juce::dontSendNotification);
            }
        });
}

void MainComponent::refreshGlobalLeaderboard()
{
    leaderboardLabel.setText("Loading global rating...", juce::dontSendNotification);

    const int requestId = ++leaderboardRequestId;
    juce::Component::SafePointer<MainComponent> safeThis(this);

    apiClient.getJson("/leaderboard/global?limit=10", {}, 4000,
        [safeThis, requestId](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || requestId != self->leaderboardRequestId)
                return;

            if (!response.transportOk)
            {
                self->leaderboardLabel.setText("Server unavailable. Start the FretFight server and try again.",
                                               juce::dontSendNotification);
                return;
            }

            const auto parsed = response.json;
            if (!parsed.isObject())
            {
                self->leaderboardLabel.setText("Could not read rating response from server.",
                                               juce::dontSendNotification);
                return;
            }

            const auto itemsValue = parsed.getProperty("items", juce::var());
            const auto* items = itemsValue.getArray();
            if (items == nullptr || items->isEmpty())
            {
                self->leaderboardLabel.setText(
                    "No player results yet. Play a song and submit a score to appear here.",
                    juce::dontSendNotification);
                return;
            }

            juce::String text;
            for (const auto& item : *items)
            {
                const auto rank = item.getProperty("rank", juce::var()).toString();
                auto name = item.getProperty("display_name", juce::var()).toString();
                if (name.trim().isEmpty())
                    name = item.getProperty("username", juce::var()).toString();
                const auto points = item.getProperty("rating_points", juce::var()).toString();
                const auto games = item.getProperty("games_played", juce::var()).toString();
                text << rank << ".  " << name << "   -   " << points << " pts"
                     << "   (" << games << " games)" << juce::newLine;
            }
            self->leaderboardLabel.setText(text, juce::dontSendNotification);
        });
}

namespace
{
    // Collected by the background thread of downloadOnlineSong() and applied to
    // the song library on the message thread.
    struct OnlineCatalogDownload
    {
        bool ok = false;
        juce::String title;
        juce::String slug;
        juce::String artist;
        juce::String songId;
        double bpm = 120.0;
        int version = 0;
        bool usedCache = false;
        juce::String assetState;
        juce::String missingRoles;
        juce::File songFolder;
        juce::File midiFile;
        juce::File drumsFile;
        juce::File bassFile;
        juce::File guitarFile;
        juce::File othersFile;
    };

    juce::File onlineSongsCacheRoot()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("GuitarBattle")
            .getChildFile("OnlineSongs");
    }
}

//==============================================================================
// Song-select catalogue
//==============================================================================

int MainComponent::SongListModel::getNumRows()
{
    return static_cast<int>(owner.catalogSongs.size());
}

void MainComponent::SongListModel::paintListBoxItem(int rowNumber,
                                                    juce::Graphics& g,
                                                    int width,
                                                    int height,
                                                    bool rowIsSelected)
{
    if (! juce::isPositiveAndBelow(rowNumber,
                                   static_cast<int>(owner.catalogSongs.size())))
        return;

    const auto& song = owner.catalogSongs[static_cast<size_t>(rowNumber)];
    const auto row = juce::Rectangle<int>(0, 0, width, height).reduced(2, 3);

    g.setColour(rowIsSelected ? UiColors::accent.withAlpha(0.16f)
                              : UiColors::surface2.withAlpha(0.55f));
    g.fillRoundedRectangle(row.toFloat(), 12.0f);
    g.setColour(rowIsSelected ? UiColors::accent.withAlpha(0.55f) : UiColors::border);
    g.drawRoundedRectangle(row.toFloat().reduced(0.5f), 12.0f, 1.0f);

    if (rowIsSelected)
    {
        const juce::Rectangle<int> selectionBar(row.getX() + 2, row.getY() + 11,
                                                4, juce::jmax(4, row.getHeight() - 22));
        g.setColour(UiColors::accent);
        g.fillRoundedRectangle(selectionBar.toFloat(), 2.0f);
    }

    // The best score sits on the right; the metadata column shrinks with the card.
    const int bestWidth = juce::jlimit(84, 152, width / 3);
    const auto bestArea = row.withTrimmedRight(14).removeFromRight(bestWidth);
    auto textArea = row.withTrimmedLeft(16).withTrimmedRight(bestWidth + 22);

    juce::StringArray meta;
    if (song.artist.isNotEmpty())
        meta.add(song.artist);
    meta.add(juce::String(juce::roundToInt(song.bpm)) + " BPM");
    meta.add(song.downloaded ? "ONLINE / CACHED" : "ONLINE");

    g.setColour(UiColors::text);
    g.setFont(GameUi::font(14.0f, true));
    g.drawFittedText(song.title,
                     textArea.removeFromTop(juce::jmax(18, height * 3 / 5)).toNearestInt(),
                     juce::Justification::centredLeft, 1, 0.8f);

    g.setColour(UiColors::muted);
    g.setFont(GameUi::font(10.5f));
    g.drawFittedText(meta.joinIntoString("   |   "),
                     textArea.toNearestInt(),
                     juce::Justification::centredLeft, 1, 0.7f);

    const int best = owner.getBestScoreForSong(song.slug, song.title);

    // Fitted text with a horizontal-scale floor: never truncate the digits.
    g.setColour(best > 0 ? UiColors::accent : UiColors::muted.withAlpha(0.55f));
    g.setFont(GameUi::font(10.0f, true, 0.55f));
    g.drawFittedText(best > 0 ? "BEST: " + juce::String(best) : juce::String("NO SCORE"),
                     bestArea, juce::Justification::centredRight, 1, 0.65f);
}

void MainComponent::SongListModel::listBoxItemClicked(int rowNumber, const juce::MouseEvent&)
{
    // Selecting a row only highlights it; START SONG performs the download.
    owner.selectCatalogSong(rowNumber);
}

void MainComponent::SongListModel::selectedRowsChanged(int lastRowSelected)
{
    // Guard against the re-entry caused by selectCatalogSong() selecting the row.
    if (lastRowSelected >= 0 && lastRowSelected != owner.selectedCatalogIndex)
        owner.selectCatalogSong(lastRowSelected);
}

void MainComponent::SongListModel::returnKeyPressed(int rowNumber)
{
    owner.selectCatalogSong(rowNumber);
    owner.startSelectedSong();
}

void MainComponent::updateCatalogRows()
{
    songListBox.updateContent();
    songListBox.repaint();
    updateSongBestScoreLabels();
}

void MainComponent::rebuildCatalogRows(const juce::var& serverItems)
{
    // Remember the highlighted row so a catalogue refresh keeps the selection.
    juce::String previousSlug;
    juce::String previousTitle;
    if (juce::isPositiveAndBelow(selectedCatalogIndex,
                                 static_cast<int>(catalogSongs.size())))
    {
        previousSlug = catalogSongs[static_cast<size_t>(selectedCatalogIndex)].slug;
        previousTitle = catalogSongs[static_cast<size_t>(selectedCatalogIndex)].title;
    }

    std::vector<CatalogSong> rows;

    if (const auto* items = serverItems.getArray())
    {
        const auto cacheRoot = onlineSongsCacheRoot();

        for (const auto& item : *items)
        {
            const auto slug = item.getProperty("slug", juce::var()).toString();
            if (slug.isEmpty())
                continue;

            CatalogSong row;
            row.slug = slug;
            row.serverId = item.getProperty("id", juce::var()).toString();
            row.title = item.getProperty("title", juce::var()).toString();
            if (row.title.isEmpty())
                row.title = slug;
            row.artist = item.getProperty("artist", juce::var()).toString();
            row.bpm = static_cast<double>(item.getProperty("bpm", 120));
            row.version = static_cast<int>(item.getProperty("version", 0));
            row.online = true;
            row.downloaded = findOnlineLibraryEntry(slug) != nullptr
                || cacheRoot.getChildFile(slug).getChildFile("version.txt").existsAsFile();
            rows.push_back(row);
        }
    }

    catalogSongs = std::move(rows);

    int newIndex = 0;
    for (int i = 0; i < static_cast<int>(catalogSongs.size()); ++i)
    {
        const auto& row = catalogSongs[static_cast<size_t>(i)];
        if ((previousSlug.isNotEmpty() && row.slug == previousSlug)
            || (previousSlug.isEmpty() && previousTitle.isNotEmpty()
                && row.title == previousTitle))
        {
            newIndex = i;
            break;
        }
    }

    selectedCatalogIndex = catalogSongs.empty()
        ? 0 : juce::jlimit(0, static_cast<int>(catalogSongs.size()) - 1, newIndex);

    updateCatalogRows();

    if (!catalogSongs.empty())
        selectCatalogSong(selectedCatalogIndex);
}

void MainComponent::refreshOnlineCatalog()
{
    // The catalogue refresh is also a natural retry point when the initial
    // sound-pack download failed (for example while the network was offline).
    if (! clientAssetsReady && ! clientAssetsRefreshInFlight)
        startClientAssetRefresh();

    onlineCatalogButton.setEnabled(false);
    onlineCatalogStatusLabel.setText("Loading song catalogue...", juce::dontSendNotification);

    // The request runs on an ApiClient worker thread; only this callback touches
    // components. A failure keeps the last server catalogue, if one was loaded.
    juce::Component::SafePointer<MainComponent> safeThis(this);
    apiClient.getJson("/songs?limit=100&offset=0", {}, 6000,
        [safeThis](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr)
                return;

            self->onlineCatalogButton.setEnabled(true);

            if (!response.transportOk || !response.json.isObject())
            {
                self->onlineCatalogStatusLabel.setText(
                    "Server unavailable. Press REFRESH CATALOG to retry.",
                    juce::dontSendNotification);
                return;
            }

            const auto itemsValue = response.json.getProperty("items", juce::var());
            const auto* items = itemsValue.getArray();
            const int onlineCount = items != nullptr ? items->size() : 0;

            self->rebuildCatalogRows(itemsValue);

            if (onlineCount <= 0)
            {
                self->onlineCatalogStatusLabel.setText(
                    "No published songs yet. Publish a song, then press REFRESH CATALOG.",
                    juce::dontSendNotification);
            }
            else
            {
                self->onlineCatalogStatusLabel.setText(
                    juce::String(onlineCount) + " online song"
                        + (onlineCount == 1 ? "" : "s")
                        + " in the catalogue. Select a track and press START SONG.",
                    juce::dontSendNotification);
            }

            // Personal bests need the token, so they are only fetched when the
            // player is signed in.
            self->refreshOnlineBestScores();

            self->resized();
            self->repaint();
        });
}

void MainComponent::refreshOnlineBestScores()
{
    if (onlineAccessToken.isEmpty())
        return;

    const int requestId = ++bestScoresRequestId;
    juce::Component::SafePointer<MainComponent> safeThis(this);

    apiClient.getJson("/games/best-scores", onlineAccessToken, 6000,
        [safeThis, requestId](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || requestId != self->bestScoresRequestId)
                return;

            if (!response.transportOk || !response.json.isObject())
                return;

            const auto itemsValue = response.json.getProperty("items", juce::var());
            const auto* items = itemsValue.getArray();
            if (items == nullptr)
                return;

            for (const auto& item : *items)
            {
                const auto slug = item.getProperty("song_slug", juce::var()).toString();
                const auto score = static_cast<int>(item.getProperty("best_score", 0));
                if (slug.isEmpty() || score <= 0)
                    continue;

                auto& stored = self->remoteBestScores[slug];
                stored = juce::jmax(stored, score);
            }

            self->updateCatalogRows();
            self->repaint();
        });
}

MainComponent::SongEntry* MainComponent::findOnlineLibraryEntry(const juce::String& slug)
{
    if (slug.isEmpty())
        return nullptr;

    const auto found = std::find_if(songLibrary.begin(), songLibrary.end(),
        [&slug](const SongEntry& entry) { return entry.slug == slug; });

    return found == songLibrary.end() ? nullptr : &(*found);
}

int MainComponent::getBestScoreForSong(const juce::String& slug, const juce::String& title)
{
    // Server songs use the slug; the title key keeps scores saved by older
    // client versions.
    int best = getBestScore(title);

    if (slug.isNotEmpty())
    {
        best = juce::jmax(best, getBestScore(slug));

        const auto remote = remoteBestScores.find(slug);
        if (remote != remoteBestScores.end())
            best = juce::jmax(best, remote->second);
    }

    return best;
}

void MainComponent::selectCatalogSong(int index)
{
    if (! juce::isPositiveAndBelow(index, static_cast<int>(catalogSongs.size())))
        return;

    selectedCatalogIndex = index;
    const auto& song = catalogSongs[static_cast<size_t>(index)];

    // Every catalogue row is a server song; the session is opened on START.
    onlineSongUuid = song.serverId;
    onlineGameSessionUuid.clear();

    // Selecting a row only highlights it: audio files and MIDI are loaded when
    // START SONG is pressed, so clicking through the list stays instant.
    updateSongBestScoreLabels();
    songListBox.selectRow(index);
    resized();
    repaint();
}

void MainComponent::startSelectedSong()
{
    if (catalogSongs.empty())
    {
        onlineCatalogStatusLabel.setText(
            "No songs loaded. Press REFRESH CATALOG to fetch the server catalogue.",
            juce::dontSendNotification);
        return;
    }

    selectedCatalogIndex = juce::jlimit(0, static_cast<int>(catalogSongs.size()) - 1,
                                        selectedCatalogIndex);
    const auto& song = catalogSongs[static_cast<size_t>(selectedCatalogIndex)];

    // Cached online songs are playable without any network traffic.
    if (auto* entry = findOnlineLibraryEntry(song.slug))
    {
        onlineSongUuid = song.serverId;
        if (loadSongMidi(entry->title))
        {
            beginTuningForSelectedSong();
            return;
        }
    }

    // Assets are downloaded on demand, never for the whole catalogue at once.
    songPridumayButton.setEnabled(false);
    downloadOnlineSong(song.slug, song.serverId, true);
}

void MainComponent::downloadOnlineSong(const juce::String& slug,
                                       const juce::String& serverId,
                                       bool startAfterDownload)
{
    juce::Component::SafePointer<MainComponent> safeThis(this);
    auto download = std::make_shared<OnlineCatalogDownload>();

    // Detail lookup and file downloads run on the ApiClient worker threads.
    // Only the message-thread callback below touches components, the song library
    // or the gameplay state.
    auto backgroundWork = [download, slug, serverId]()
    {
        if (slug.isEmpty())
            return;

        const auto detailResponse = ApiClient::get("/songs/" + slug, {}, 5000);
        if (!detailResponse.transportOk)
            return;

        const auto detail = detailResponse.json;
        if (!detail.isObject())
            return;

        // The server describes asset completeness in the same song object as the
        // metadata.  Older servers do not send it; the client then behaves as before.
        const auto readinessValue = detail.getProperty("readiness", juce::var());
        if (readinessValue.isObject())
        {
            download->assetState = readinessValue.getProperty("state", juce::var()).toString();
            if (const auto* missing = readinessValue.getProperty("missing_roles", juce::var()).getArray())
            {
                juce::StringArray roles;
                for (const auto& role : *missing)
                {
                    const auto name = role.toString();
                    if (name.isNotEmpty())
                        roles.add(name);
                }
                download->missingRoles = roles.joinIntoString(", ");
            }
        }

        const auto filesValue = detail.getProperty("files", juce::var());
        const auto* files = filesValue.getArray();
        if (files == nullptr)
            return;

        const auto cacheRoot = juce::File::getSpecialLocation(
            juce::File::userApplicationDataDirectory)
            .getChildFile("GuitarBattle")
            .getChildFile("OnlineSongs");
        const auto songFolder = cacheRoot.getChildFile(slug);
        if (!songFolder.createDirectory())
            return;

        const auto midiFile = songFolder.getChildFile(slug + ".mid");
        const auto versionFile = songFolder.getChildFile("version.txt");

        // Resolve every downloadable asset of the song detail to its cache path.
        struct PendingAsset
        {
            juce::String role;
            juce::String downloadUrl;
            juce::File targetFile;
            juce::int64 expectedSize = 0;
            bool isMidi = false;
        };

        std::vector<PendingAsset> assets;
        bool assetsComplete = true;
        bool hasMidi = false;

        for (const auto& fileInfo : *files)
        {
            const auto role = fileInfo.getProperty("role", juce::var()).toString().toLowerCase();
            const auto fileUrl = fileInfo.getProperty("download_url", juce::var()).toString();
            if (fileUrl.isEmpty())
                continue;

            PendingAsset asset;
            asset.role = role;
            asset.downloadUrl = fileUrl;
            asset.expectedSize = static_cast<juce::int64>(fileInfo.getProperty("size_bytes", 0));
            asset.isMidi = role == "midi";

            if (asset.isMidi)
                asset.targetFile = midiFile;
            else if (role == "drums")
                asset.targetFile = songFolder.getChildFile("drums.mp3");
            else if (role == "bass")
                asset.targetFile = songFolder.getChildFile("bass.mp3");
            else if (role == "guitar")
                asset.targetFile = songFolder.getChildFile("guitar.mp3");
            else if (role == "others")
                asset.targetFile = songFolder.getChildFile("others.mp3");
            else
                continue;

            // Without a trustworthy size the cached files cannot be validated.
            if (asset.expectedSize <= 0)
                assetsComplete = false;

            hasMidi = hasMidi || asset.isMidi;
            assets.push_back(std::move(asset));
        }

        if (!hasMidi || !assetsComplete)
            return;

        const auto serverVersion = static_cast<int>(detail.getProperty("version", 0));
        const auto cachedVersion = versionFile.existsAsFile()
            ? versionFile.loadFileAsString().trim().getIntValue()
            : 0;

        auto hasMidiHeader = [](const juce::File& file)
        {
            juce::FileInputStream stream(file);
            if (!stream.openedOk())
                return false;

            char header[4] {};
            return stream.read(header, 4) == 4
                && header[0] == 'M' && header[1] == 'T'
                && header[2] == 'h' && header[3] == 'd';
        };

        // The cache is reused only when the server content version matches and
        // every published file is present with the advertised size.
        auto usedCache = serverVersion > 0 && cachedVersion == serverVersion;
        if (usedCache)
            for (const auto& asset : assets)
                if (!asset.targetFile.existsAsFile()
                    || asset.targetFile.getSize() != asset.expectedSize
                    || (asset.isMidi && !hasMidiHeader(asset.targetFile)))
                {
                    usedCache = false;
                    break;
                }

        if (usedCache)
        {
            DBG("ONLINE CATALOG: using cached assets for version " + juce::String(serverVersion));
        }
        else
        {
            // A version mismatch forces every stem to be downloaded again, even
            // if the previously cached file happens to have the same size.
            auto allAssetsDownloaded = true;

            for (const auto& asset : assets)
            {
                const auto writtenBytes = ApiClient::downloadToFile(asset.downloadUrl,
                                                                    asset.targetFile,
                                                                    5000);
                const auto sizeOk = writtenBytes > 0 && writtenBytes == asset.expectedSize;
                const auto headerOk = !asset.isMidi || (sizeOk && hasMidiHeader(asset.targetFile));

                if (!sizeOk || !headerOk)
                {
                    DBG("ONLINE CATALOG: could not download " + asset.role
                        + " from " + asset.downloadUrl);
                    asset.targetFile.deleteFile();

                    if (asset.isMidi)
                        return;

                    allAssetsDownloaded = false;
                    continue;
                }

                DBG("ONLINE CATALOG: downloaded " + asset.role + " ("
                    + juce::String(writtenBytes) + " bytes)");
            }

            // Only a complete asset set may be marked as this content version.
            if (allAssetsDownloaded && serverVersion > 0
                && !versionFile.replaceWithText(juce::String(serverVersion)))
                DBG("ONLINE CATALOG: could not write " + versionFile.getFullPathName());
        }

        juce::File drumsFile, bassFile, guitarFile, othersFile;
        for (const auto& asset : assets)
        {
            if (!asset.targetFile.existsAsFile())
                continue;

            if (asset.role == "drums")         drumsFile = asset.targetFile;
            else if (asset.role == "bass")     bassFile = asset.targetFile;
            else if (asset.role == "guitar")   guitarFile = asset.targetFile;
            else if (asset.role == "others")   othersFile = asset.targetFile;
        }

        const auto title = detail.getProperty("title", juce::var()).toString();
        if (title.isEmpty())
            return;

        download->title = title;
        download->slug = slug;
        download->artist = detail.getProperty("artist", juce::var()).toString();
        download->songId = detail.getProperty("id", juce::var()).toString();
        if (download->songId.isEmpty())
            download->songId = serverId;
        download->bpm = static_cast<double>(detail.getProperty("bpm", 120));
        download->version = serverVersion;
        download->usedCache = usedCache;
        download->songFolder = songFolder;
        download->midiFile = midiFile;
        download->drumsFile = drumsFile;
        download->bassFile = bassFile;
        download->guitarFile = guitarFile;
        download->othersFile = othersFile;
        download->ok = true;
        DBG("ONLINE CATALOG: loaded " + title + " (version " + juce::String(serverVersion) + ")");
    };

    apiClient.runAsync(std::move(backgroundWork), [safeThis, download, startAfterDownload]()
    {
        auto* self = safeThis.getComponent();
        if (self == nullptr)
            return;

        self->songPridumayButton.setEnabled(true);

        bool loaded = false;

        if (download->ok)
        {
            // Reuse the cached library entry created by an earlier download.
            auto* found = self->findOnlineLibraryEntry(download->slug);

            if (found == nullptr)
            {
                SongEntry entry;
                entry.title = download->title;
                entry.slug = download->slug;
                entry.artist = download->artist;
                entry.folder = download->songFolder;
                entry.soloMidi = download->midiFile;
                entry.drums = download->drumsFile;
                entry.bass = download->bassFile;
                entry.rhythmGuitar = download->guitarFile;
                entry.others = download->othersFile;
                entry.bpm = download->bpm;
                self->songLibrary.push_back(entry);
            }
            else
            {
                found->title = download->title;
                found->artist = download->artist;
                found->folder = download->songFolder;
                found->soloMidi = download->midiFile;
                found->drums = download->drumsFile;
                found->bass = download->bassFile;
                found->rhythmGuitar = download->guitarFile;
                found->others = download->othersFile;
                found->bpm = download->bpm;
            }

            // Mark the catalogue row as cached so the list reflects the state.
            for (auto& row : self->catalogSongs)
            {
                if (! row.online || row.slug != download->slug)
                    continue;

                row.serverId = download->songId;
                row.title = download->title;
                row.bpm = download->bpm;
                row.version = download->version;
                row.downloaded = true;
            }

            self->updateCatalogRows();

            if (startAfterDownload
                && self->gameState == MainComponent::GameState::SongSelect)
            {
                // Open the online session for exactly this song.
                self->onlineSongUuid = download->songId;
                self->onlineGameSessionUuid.clear();
                loaded = self->loadSongMidi(download->title);

                // Keep the highlight on the song that was actually loaded, even
                // if the player clicked another row while the download ran.
                if (loaded)
                {
                    for (int i = 0; i < static_cast<int>(self->catalogSongs.size()); ++i)
                        if (self->catalogSongs[static_cast<size_t>(i)].slug == download->slug)
                        {
                            self->selectCatalogSong(i);
                            break;
                        }
                }
            }
        }

        juce::String statusText = download->ok
            ? "Downloaded: " + download->title
            : juce::String("Could not download the selected song. Check the connection and try again.");
        if (loaded)
        {
            statusText = "Online song loaded";
            if (download->version > 0)
                statusText << " (v" << download->version
                           << (download->usedCache ? ", cached)" : ")");
            if (download->assetState == "midi_only")
                statusText << " [MIDI only]";
            else if (download->missingRoles.isNotEmpty())
                statusText << " [missing: " << download->missingRoles << "]";
            statusText << ". Select another track or press START SONG to replay.";
        }

        self->onlineCatalogStatusLabel.setText(statusText, juce::dontSendNotification);
        self->resized();
        self->repaint();
        if (loaded)
            self->beginTuningForSelectedSong();
    });
}

bool MainComponent::loadSongAudio(const SongEntry& song)
{
    for (auto& track : externalAudioTracks)
    {
        track.audio.setSize(0, 0);
        track.loaded.store(false, std::memory_order_release);
        track.sourceSampleRate = 44100.0;
    }
    // Mixer audio index order: 0 drums, 1 bass, 2 rhythm guitar, 3 others.
    if (song.drums.existsAsFile()) loadExternalAudioTrack(0, song.drums);
    if (song.bass.existsAsFile()) loadExternalAudioTrack(1, song.bass);
    if (song.rhythmGuitar.existsAsFile()) loadExternalAudioTrack(2, song.rhythmGuitar);
    if (song.others.existsAsFile()) loadExternalAudioTrack(3, song.others);
    prepareExternalAudioForSpeed(speedPercent);
    return true;
}

void MainComponent::prepareExternalAudioForSpeed(double speedPercentToUse)
{
    const double ratio = juce::jlimit(0.5, 1.2, speedPercentToUse / 100.0);
    for (auto& track : externalAudioTracks)
    {
        if (!track.loaded.load(std::memory_order_acquire) || track.audio.getNumSamples() <= 0)
        {
            std::atomic_store(&track.playbackAudio, std::shared_ptr<juce::AudioBuffer<float>>{});
            continue;
        }
        auto stretched = std::make_shared<juce::AudioBuffer<float>>(
            makePitchPreservingTimeStretch(track.audio, ratio));
        std::atomic_store(&track.playbackAudio, stretched);
    }
    DBG("Instrumental audio pitch-preserving time stretch prepared: speed=" + juce::String(speedPercentToUse, 0)
        + "% (WSOLA; pitch preserved approximately)");
}

bool MainComponent::loadSongMidi(const juce::String& songId)
{
    auto found = std::find_if(songLibrary.begin(), songLibrary.end(),
        [&songId](const SongEntry& entry) { return entry.title == songId; });
    if (found == songLibrary.end())
    {
        DBG("Song not found in catalogue: " + songId);
        return false;
    }

    std::vector<TabNote> parsedNotes;
    if (!MidiParser::loadMidi(found->soloMidi, parsedNotes) || parsedNotes.empty())
    {
        DBG("MIDI load failed: " + found->soloMidi.getFullPathName());
        return false;
    }

    selectedSong = *found;
    if (found->slug.isEmpty())
    {
        // No server identity: an online game session cannot be opened.
        onlineSongUuid.clear();
        onlineGameSessionUuid.clear();
    }
    selectedSongId = found->title;
    selectedSongSlug = found->slug;
    dailyChallengeLoaded.store(false, std::memory_order_relaxed);
    gameSongTitleLabel.setText(found->title, juce::dontSendNotification);

    // Read the MIDI tempo map; use 120 BPM only if the MIDI contains no tempo event.
    juce::MidiFile tempoMidi;
    juce::FileInputStream tempoInput(found->soloMidi);
    if (tempoInput.openedOk() && tempoMidi.readFrom(tempoInput))
    {
        for (int t = 0; t < tempoMidi.getNumTracks(); ++t)
            if (const auto* track = tempoMidi.getTrack(t))
                for (int e = 0; e < track->getNumEvents(); ++e)
                {
                    const auto& message = track->getEventPointer(e)->message;
                    if (message.isTempoMetaEvent())
                    {
                        const double secondsPerBeat = message.getTempoSecondsPerQuarterNote();
                        if (secondsPerBeat > 0.0) { found->bpm = 60.0 / secondsPerBeat; break; }
                    }
                }
    }
    const double midiBpm = juce::jlimit(30.0, 300.0, found->bpm);

    // Keep the MIDI tempo unchanged (100% speed by default). To avoid loading
    // extremely long generated phrases, keep only the first 60 seconds of notes.
    // Instrumental stems are capped to the same 60-second window in loadExternalAudioTrack().
    loadSongAudio(*found);
    constexpr double maxSongSeconds = 60.0;
    baseBpm = midiBpm;
    const double maxBeat = maxSongSeconds * baseBpm / 60.0;
    const auto originalNoteCount = parsedNotes.size();
    parsedNotes.erase(std::remove_if(parsedNotes.begin(), parsedNotes.end(),
        [maxBeat](TabNote& note)
        {
            if (note.startBeat >= maxBeat)
                return true;
            // Trim a note that crosses the one-minute boundary.
            note.durationBeats = juce::jmax(0.0,
                juce::jmin(note.durationBeats, maxBeat - note.startBeat));
            return note.durationBeats <= 0.0;
        }), parsedNotes.end());
    DBG("MIDI 60s limit: source BPM=" + juce::String(midiBpm, 2)
        + ", max beat=" + juce::String(maxBeat, 2)
        + ", notes kept=" + juce::String(static_cast<int>(parsedNotes.size()))
        + "/" + juce::String(static_cast<int>(originalNoteCount))
        + ", audio cap=60.00s; tempo not stretched.");

    const double actualBpm = baseBpm * speedPercent / 100.0;
    bpm.store(actualBpm, std::memory_order_relaxed);
    audioPlaybackRate.store(speedPercent / 100.0, std::memory_order_relaxed);
    metronome.setBpm(actualBpm);
    gameSession.setBaseBpm(baseBpm);
    gameSession.setSpeedPercent(speedPercent);
    gameSession.setBpm(actualBpm);
    bpmLabel.setText(juce::String(static_cast<int>(std::round(actualBpm))), juce::dontSendNotification);
    gameSession.getMidiNotes() = std::move(parsedNotes);
    tablatureComponent.setNotes(gameSession.getMidiNotes());
    tablatureComponent.setBpm(actualBpm);
    gameSession.setTargetNoteIndex(0);
    gameSession.resetResolvedTarget();
    setTargetFromMidiIndex(0);
    updateSongBestScoreLabels();
    DBG("Selected song: " + found->title + ", BPM=" + juce::String(baseBpm)
        + ", notes=" + juce::String((int)gameSession.getMidiNotes().size()));
    return true;
}

bool MainComponent::generateDailyChallengeMidi(bool forceRandomSeed)
{
    MidiMelodyGenerator::Settings settings;
    settings.difficulty = 1;
    settings.durationSeconds = 20;
    settings.bpm = 110.0;
    const int selectedStyleId = melodyStyleCombo.getSelectedId();
    // Genre-specific tempo ranges are intentionally distinct. The same selected BPM
    // is passed to the lead and backing generators so their MIDI remains synchronized.
    const int tempoSeed = forceRandomSeed ? juce::Random::getSystemRandom().nextInt()
        : juce::Time::getCurrentTime().getYear() * 10000
          + static_cast<int>(juce::Time::getCurrentTime().getMonth()) * 100
          + static_cast<int>(juce::Time::getCurrentTime().getDayOfMonth());
    const int tempoRoll = static_cast<int>(static_cast<unsigned int>(tempoSeed) & 0x7fffffffU);
    auto chooseTempo = [tempoRoll](int low, int high)
    {
        return static_cast<double>(low + (tempoRoll % (high - low + 1)));
    };
    switch (selectedStyleId)
    {
        case 1:  settings.bpm = chooseTempo(95, 155); break;  // Steve Vai / expressive rock
        case 2:  settings.bpm = chooseTempo(170, 200); break; // Power Rock reference range
        case 3:  settings.bpm = chooseTempo(95, 150); break;  // Progressive Metal
        case 4:  settings.bpm = chooseTempo(80, 130); break;  // Blues Rock
        case 5:  settings.bpm = chooseTempo(95, 145); break;  // Hard Rock
        case 6:  settings.bpm = chooseTempo(110, 155); break; // Polyphia-inspired
        case 7:  settings.bpm = chooseTempo(145, 180); break; // Trivium-inspired
        case 8:  settings.bpm = chooseTempo(90, 145); break;  // Djent
        case 9:  settings.bpm = chooseTempo(145, 180); break; // Neoclassical Metal
        case 10: settings.bpm = chooseTempo(90, 140); break;  // Classic Rock
        case 11: settings.bpm = chooseTempo(115, 165); break; // Heavy Metal
        case 12: settings.bpm = chooseTempo(140, 180); break; // Symphonic Metal
        case 13: settings.bpm = chooseTempo(130, 180); break; // Technical Metal
        case 14: settings.bpm = chooseTempo(125, 175); break; // Modern Metalcore
        case 15: settings.bpm = chooseTempo(170, 200); break; // Guitar Fusion
        case 16: settings.bpm = chooseTempo(70, 115); break;  // Blues
        case 17: settings.bpm = chooseTempo(90, 125); break;  // Funk / Soul
        case 18: settings.bpm = chooseTempo(100, 155); break; // Jazz Fusion
        case 19: settings.bpm = chooseTempo(80, 130); break;  // Alternative / Indie
        case 20: settings.bpm = chooseTempo(65, 110); break;  // Cinematic / Experimental
        case 21: settings.bpm = chooseTempo(80, 125); break;  // Country / Americana
        default: settings.bpm = chooseTempo(170, 200); break;
    }
    settings.bpm = juce::jlimit(170.0, 200.0, settings.bpm);
    settings.style = MidiMelodyGenerator::Style::PowerMetal; // Single available style: Power Rock.

    // Normal startup keeps the daily challenge stable for the local calendar day.
    // The NEW GENERATION test button deliberately bypasses that seed so each click
    // can explore another melody and a matching backing arrangement.
    const auto today = juce::Time::getCurrentTime();
    if (forceRandomSeed)
    {
        settings.seed = juce::Random::getSystemRandom().nextInt();
        if (settings.seed == 0)
            settings.seed = 1;
    }
    else
    {
        settings.seed = today.getYear() * 10000 + static_cast<int>(today.getMonth()) * 100
                        + static_cast<int>(today.getDayOfMonth());
    }

    const auto outputRoot = juce::File::getSpecialLocation(
        juce::File::userDocumentsDirectory)
        .getChildFile("MusicMastersArena")
        .getChildFile("GeneratedChallenges");

    const auto dateTag = today.formatted("%Y%m%d");
    const auto outputDirectory = outputRoot.getChildFile(dateTag);
    dailyChallengeMidiFile = outputDirectory.getChildFile("DailyChallenge_" + dateTag + "_Guitar.mid");

    MidiMelodyGenerator::Result generated;
    juce::String error;
    if (!MidiMelodyGenerator::generate(settings, dailyChallengeMidiFile, generated, &error))
    {
        DBG("DAILY CHALLENGE GENERATION FAILED: " + error);
        generateMelodyButton.setEnabled(false);
        generateMelodyButton.setButtonText("Daily Challenge Monday (unavailable)");
        return false;
    }

    // Generate the backing parts from the same deterministic daily seed.
    // The guitar-only MIDI above remains the sole source for tablature/scoring.
    SongArrangement::Settings arrangementSettings;
    arrangementSettings.seed = settings.seed;
    arrangementSettings.bpm = settings.bpm;
    arrangementSettings.style = static_cast<int>(settings.style);
    // Match the backing length to the generated guitar challenge duration.
    arrangementSettings.bars = juce::jlimit(4, 32,
        static_cast<int>(std::ceil((settings.durationSeconds * settings.bpm / 60.0) / 4.0)));
    SongArrangement::Result arrangement;
    if (!SongArrangement::generateBacking(arrangementSettings, outputDirectory, arrangement, &error))
    {
        DBG("DAILY CHALLENGE BACKING GENERATION FAILED: " + error);
        generateMelodyButton.setEnabled(false);
        generateMelodyButton.setButtonText("Daily Challenge Monday (unavailable)");
        return false;
    }

    if (!loadBackingMidiEvents(outputDirectory))
        DBG("DAILY CHALLENGE: backing MIDI files were generated but could not be loaded for playback.");

    generateMelodyButton.setEnabled(true);
    generateMelodyButton.setButtonText("Daily Challenge Monday");
    DBG("DAILY CHALLENGE MIDI CREATED: " + dailyChallengeMidiFile.getFullPathName()
        + ", seed=" + juce::String(settings.seed)
        + ", notes=" + juce::String(static_cast<int>(generated.notes.size()))
        + ", backing=" + arrangement.backingMidi.getFullPathName()
        + ", chords=" + arrangement.progression);
    return true;
}

bool MainComponent::loadBackingMidiEvents(const juce::File& directory)
{
    backingNotes.clear();
    struct Source { const char* fileName; int instrument; };
    const Source sources[] = {
        { "DailyChallenge_Bass.mid", 1 },
        { "DailyChallenge_Drums.mid", 2 },
        { "DailyChallenge_Piano.mid", 3 },
        { "DailyChallenge_RhythmGuitar.mid", 4 }
    };
    int loadedFiles = 0;
    for (const auto& source : sources)
    {
        juce::FileInputStream stream(directory.getChildFile(source.fileName));
        if (!stream.openedOk())
            continue;
        juce::MidiFile midi;
        if (!midi.readFrom(stream))
            continue;
        const int tpq = midi.getTimeFormat();
        if (tpq <= 0)
            continue;
        for (int trackIndex = 0; trackIndex < midi.getNumTracks(); ++trackIndex)
        {
            auto* track = midi.getTrack(trackIndex);
            if (track == nullptr)
                continue;
            double startTicks[128]{};
            int velocities[128]{};
            bool active[128]{};
            for (int eventIndex = 0; eventIndex < track->getNumEvents(); ++eventIndex)
            {
                const auto& message = track->getEventPointer(eventIndex)->message;
                if (message.isNoteOn())
                {
                    const int note = message.getNoteNumber();
                    if (note >= 0 && note < 128)
                    {
                        startTicks[note] = message.getTimeStamp();
                        velocities[note] = message.getVelocity();
                        active[note] = true;
                    }
                }
                else if (message.isNoteOff())
                {
                    const int note = message.getNoteNumber();
                    if (note >= 0 && note < 128 && active[note])
                    {
                        const double startBeat = startTicks[note] / static_cast<double>(tpq);
                        const double endBeat = message.getTimeStamp() / static_cast<double>(tpq);
                        BackingNote item;
                        item.startBeat = juce::jmax(0.0, startBeat);
                        item.durationBeats = juce::jmax(0.04, endBeat - startBeat);
                        item.midiNote = note;
                        item.velocity = velocities[note];
                        item.instrument = source.instrument;
                        backingNotes.push_back(item);
                        active[note] = false;
                    }
                }
            }
        }
        ++loadedFiles;
    }
    std::sort(backingNotes.begin(), backingNotes.end(),
        [](const BackingNote& a, const BackingNote& b) { return a.startBeat < b.startBeat; });
    DBG("DAILY CHALLENGE BACKING EVENTS: files=" + juce::String(loadedFiles)
        + ", notes=" + juce::String(static_cast<int>(backingNotes.size())));
    return loadedFiles == 4 && !backingNotes.empty();
}

bool MainComponent::loadGeneratedMelody()
{
    // Do not use generator-returned notes here: read the generated .mid file
    // with the exact same parser used for the three built-in songs.
    if (!dailyChallengeMidiFile.existsAsFile() && !generateDailyChallengeMidi())
        return false;

    std::vector<TabNote> parsedNotes;
    if (!MidiParser::loadMidi(dailyChallengeMidiFile, parsedNotes) || parsedNotes.empty())
    {
        DBG("DAILY CHALLENGE MIDI PARSE FAILED: " + dailyChallengeMidiFile.getFullPathName());
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Daily Challenge unavailable",
            "The generated MIDI could not be read by the MIDI parser.");
        return false;
    }

    selectedSongId = "daily_challenge_monday";
    selectedSongSlug.clear();
    dailyChallengeLoaded.store(true, std::memory_order_relaxed);
    gameSongTitleLabel.setText("Daily Challenge Monday", juce::dontSendNotification);

    // The MIDI parser reads the tempo from the saved file; this MVP uses 110 BPM.
    baseBpm = 110.0;
    const double actualBpm = baseBpm * speedPercent / 100.0;
    bpm.store(actualBpm, std::memory_order_relaxed);
    audioPlaybackRate.store(speedPercent / 100.0, std::memory_order_relaxed);
    metronome.setBpm(actualBpm);
    gameSession.setBaseBpm(baseBpm);
    gameSession.setSpeedPercent(speedPercent);
    gameSession.setBpm(actualBpm);
    bpmLabel.setText(juce::String(static_cast<int>(std::round(actualBpm))),
                     juce::dontSendNotification);

    gameSession.getMidiNotes() = std::move(parsedNotes);
    tablatureComponent.setNotes(gameSession.getMidiNotes());
    tablatureComponent.setBpm(actualBpm);
    gameSession.setTargetNoteIndex(0);
    gameSession.resetResolvedTarget();
    setTargetFromMidiIndex(0);
    updateSongBestScoreLabels();

    DBG("DAILY CHALLENGE LOADED THROUGH MidiParser: notes="
        + juce::String(static_cast<int>(gameSession.getMidiNotes().size()))
        + ", MIDI=" + dailyChallengeMidiFile.getFullPathName());
    return true;
}


void MainComponent::beginTuningForSelectedSong()
{
    tuner.resetTunedStrings();
    tunerActive.store(true, std::memory_order_relaxed);
    previousRmsValue = 0.0;
    previousRmsValue2 = 0.0;
    lastOnsetSample.store(0, std::memory_order_relaxed);
    lastNoteOnSample.store(0, std::memory_order_relaxed);
    tunerStartButton.setEnabled(false);
    tunerDetectedString = -1;
    tunerFrequency = 0.0;
    tunerCents = 0.0;
    tunerNoteLabel.setText("---", juce::dontSendNotification);
    tunerFrequencyLabel.setText("--- Hz", juce::dontSendNotification);
    tunerStringLabel.setText("---", juce::dontSendNotification);
    tunerCentsLabel.setText("--- cents", juce::dontSendNotification);
    tunerStatusLabel.setText("PLAY A STRING", juce::dontSendNotification);
    tunerStatusLabel.setColour(juce::Label::textColourId, UiColors::blue);
    tunerNoteLabel.setColour(juce::Label::textColourId, UiColors::blue);
    tunerCentsLabel.setColour(juce::Label::textColourId, UiColors::text);
    gameState = GameState::Tuner;
    resized();
    repaint();
}

int MainComponent::getBestScore(const juce::String& songId)
{
    if (auto* settings = appProperties.getUserSettings())
        return settings->getIntValue(juce::String("BestScore_") + songId, 0);
    return 0;
}

void MainComponent::saveBestScoreForSelectedSong(int score)
{
    if (auto* settings = appProperties.getUserSettings())
    {
        // Server songs are keyed by slug. The title key written by older client
        // versions is still read as a fallback in getBestScoreForSong().
        const auto key = juce::String("BestScore_")
            + (selectedSongSlug.isNotEmpty() ? selectedSongSlug : selectedSongId);
        const int previous = settings->getIntValue(key, 0);
        if (score > previous)
        {
            settings->setValue(key, score);
            settings->saveIfNeeded();
        }
    }
    updateSongBestScoreLabels();
}

void MainComponent::updateSongBestScoreLabels()
{
    juce::String selectedBestText = "BEST: -";

    if (juce::isPositiveAndBelow(selectedCatalogIndex,
                                 static_cast<int>(catalogSongs.size())))
    {
        const auto& row = catalogSongs[static_cast<size_t>(selectedCatalogIndex)];
        selectedBestText = "BEST: " + juce::String(getBestScoreForSong(row.slug, row.title));
    }

    songPridumayBestLabel.setText(selectedBestText, juce::dontSendNotification);
    songMistBestLabel.setText(juce::String(), juce::dontSendNotification);
    songChordsBestLabel.setText(juce::String(), juce::dontSendNotification);
    dailyChallengeBestLabel.setText(
        "BEST: " + juce::String(getBestScore("daily_challenge_monday")),
        juce::dontSendNotification);

    // Row text is resolved at paint time; a repaint is enough to show a new best.
    songListBox.repaint();
}

void MainComponent::setTargetFromMidiIndex(
    int index)
{
    if (gameSession.getMidiNotes().empty())
    {
        gameSession.setTargetNote("---");

        targetNoteLabel.setText(
            "---",
            juce::dontSendNotification);

        return;
    }

    index =
        juce::jlimit(
            0,
            static_cast<int>(
                gameSession.getMidiNotes().size()) - 1,
            index);

    gameSession.setTargetNoteIndex(index);

    const int midi =
        gameSession.getMidiNotes()[index].midiNote;

    static const char* names[] =
    {
        "C",
        "C#",
        "D",
        "D#",
        "E",
        "F",
        "F#",
        "G",
        "G#",
        "A",
        "A#",
        "B"
    };

    const int noteIndex =
        (midi % 12 + 12) % 12;

    const int octave =
        (midi / 12) - 1;

    const auto noteName =
        juce::String(
            names[noteIndex])
        + juce::String(
            octave);

    gameSession.setTargetNote(
        noteName);

    targetNoteLabel.setText(
        noteName,
        juce::dontSendNotification);
}

//==============================================================================
// Target sample
//==============================================================================

uint64_t MainComponent::getTargetSample(
    int index,
    double sampleRate) const
{
    if (index < 0
        || index >= static_cast<int>(
            gameSession.getMidiNotes().size())
        || sampleRate <= 0.0)
    {
        return 0;
    }

    const double beat =
        gameSession.getMidiNotes()[index].startBeat;

    if (beat < 0.0)
        return 0;

    const auto gameplayStart =
        gameplayStartSample.load(
            std::memory_order_relaxed);

    // IMPORTANT:
// MIDI startBeat remains unchanged.
// If MIDI says beat 4, target remains beat 4.
//
// Target timing is calculated directly from the current
// audio sample rate and BPM to preserve exact fractional
// samples-per-beat timing.

    const double currentBpm =
        bpm.load(
            std::memory_order_relaxed);

    if (currentBpm <= 0.0)
        return 0;

    const double samplesPerBeat =
        sampleRate
        * 60.0
        / currentBpm;

    return gameplayStart
        + static_cast<uint64_t>(
            std::llround(
                beat
                * samplesPerBeat));
}

//==============================================================================
// Advance target
//==============================================================================

void MainComponent::advanceTarget()
{
    const int currentIndex =
        gameSession.getTargetNoteIndex();

    // GameSession owns target-index transitions and per-target resets.
    const int nextIndex =
        gameSession.advanceTargetIndex();

    //==========================================================================
    // Finished

    if (nextIndex >=
        static_cast<int>(
            gameSession.getMidiNotes().size()))
    {
        DBG("ADVANCE: "
            + juce::String(currentIndex)
            + " -> "
            + juce::String(nextIndex));

        gameState =
            GameState::Finished;

        juce::MessageManager::callAsync(
            [this]
            {
                saveBestScoreForSelectedSong(gameplayStats.getScore());
                submitOnlineGameResult();
                gameStartButton.setButtonText("PLAY AGAIN");
                menuButton.setButtonText("MAIN MENU");

                hitLabel.setText(
                    "FINISHED",
                    juce::dontSendNotification);

                targetNoteLabel.setText(
                    "---",
                    juce::dontSendNotification);

                resized();
                repaint();
            });

        return;
    }

    //==========================================================================
    // Next target

    juce::MessageManager::callAsync(
        [this, nextIndex]
        {
            const int midi =
                gameSession.getMidiNotes()[nextIndex].midiNote;

            static const char* names[] =
            {
                "C",
                "C#",
                "D",
                "D#",
                "E",
                "F",
                "F#",
                "G",
                "G#",
                "A",
                "A#",
                "B"
            };

            const int noteIndex =
                (midi % 12 + 12) % 12;

            const int octave =
                (midi / 12) - 1;

            const auto noteName =
                juce::String(
                    names[noteIndex])
                + juce::String(
                    octave);

            gameSession.setTargetNote(
                noteName);

            targetNoteLabel.setText(
                noteName,
                juce::dontSendNotification);

            repaint();
        });
}





//==============================================================================
// Clean electric-guitar SFZ sample bank
//==============================================================================

void MainComponent::loadGuitarSampleBank(const juce::File& assetsRoot)
{
    guitarSamplesLoaded.store(false, std::memory_order_relaxed);
    guitarSampleRegions.clear();
    guitarRegionForMidi.fill(-1);

    // The bank lives in the server-hosted sound pack.  No local Sounds folder
    // or absolute development path is consulted.
    const auto bankDirectory = assetsRoot.getChildFile("guitar");

    if (! bankDirectory.isDirectory())
    {
        DBG("GUITAR SAMPLES: server sound pack is not available; using the built-in fallback tone.");
        return;
    }

    juce::Array<juce::File> sfzFiles;
    bankDirectory.findChildFiles(sfzFiles, juce::File::findFiles, false, "*.sfz");
    if (sfzFiles.isEmpty())
    {
        DBG("GUITAR SAMPLES: no .sfz file in " + bankDirectory.getFullPathName());
        return;
    }

    // Prefer the compact FreePats bank if both full and small variants are present.
    juce::File sfzFile = sfzFiles.getFirst();
    for (const auto& file : sfzFiles)
    {
        if (file.getFileName().containsIgnoreCase("small"))
        {
            sfzFile = file;
            break;
        }
    }

    struct PendingRegion
    {
        juce::String samplePath;
        int rootKey = 60;
        int lowKey = 0;
        int highKey = 127;
        double loopStart = -1.0;
        double loopEnd = -1.0;
        bool looping = false;
    };

    std::vector<PendingRegion> pending;
    bool groupLooping = false;
    int currentRegion = -1;
    const auto lines = juce::StringArray::fromLines(sfzFile.loadFileAsString());

    for (auto line : lines)
    {
        line = line.trim();
        if (line.isEmpty() || line.startsWith("//"))
            continue;

        if (line.startsWithIgnoreCase("<group>"))
        {
            currentRegion = -1;
            groupLooping = false;
            continue;
        }
        if (line.startsWithIgnoreCase("<region>"))
        {
            PendingRegion region;
            region.looping = groupLooping;
            pending.push_back(region);
            currentRegion = static_cast<int>(pending.size()) - 1;
            continue;
        }

        // SFZ opcodes are whitespace-separated key=value pairs.
        std::istringstream stream(line.toStdString());
        std::string token;
        while (stream >> token)
        {
            const auto separator = token.find('=');
            if (separator == std::string::npos)
                continue;
            const auto key = juce::String(token.substr(0, separator)).trim();
            const auto value = juce::String(token.substr(separator + 1)).trim().unquoted();

            if (key == "loop_mode")
            {
                groupLooping = value == "loop_continuous";
                if (currentRegion >= 0)
                    pending[static_cast<size_t>(currentRegion)].looping = groupLooping;
                continue;
            }
            if (currentRegion < 0)
                continue;

            auto& region = pending[static_cast<size_t>(currentRegion)];
            if (key == "sample") region.samplePath = value;
            else if (key == "pitch_keycenter" || key == "key") region.rootKey = value.getIntValue();
            else if (key == "lokey") region.lowKey = value.getIntValue();
            else if (key == "hikey") region.highKey = value.getIntValue();
            else if (key == "loop_start") region.loopStart = static_cast<double>(value.getLargeIntValue());
            else if (key == "loop_end") region.loopEnd = static_cast<double>(value.getLargeIntValue());
        }
    }

    for (const auto& source : pending)
    {
        if (source.samplePath.isEmpty())
            continue;

        auto sampleFile = sfzFile.getParentDirectory().getChildFile(source.samplePath.replaceCharacter('\\', '/'));
        if (!sampleFile.existsAsFile())
        {
            DBG("GUITAR SAMPLES: missing sample " + sampleFile.getFullPathName());
            continue;
        }

        std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(sampleFile));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > 10000000)
            continue;

        GuitarSampleRegion loaded;
        loaded.sourceSampleRate = reader->sampleRate;
        loaded.rootKey = juce::jlimit(0, 127, source.rootKey);
        loaded.lowKey = juce::jlimit(0, 127, source.lowKey);
        loaded.highKey = juce::jlimit(loaded.lowKey, 127, source.highKey);
        loaded.loopStart = source.loopStart;
        loaded.loopEnd = source.loopEnd;
        loaded.looping = source.looping && source.loopStart >= 0.0 && source.loopEnd > source.loopStart;
        const int channelsToLoad = juce::jlimit(1, 2, static_cast<int>(reader->numChannels));
        loaded.audio.setSize(channelsToLoad, static_cast<int>(reader->lengthInSamples));
        reader->read(&loaded.audio, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
        guitarSampleRegions.push_back(std::move(loaded));
    }

    for (int midi = 0; midi < 128; ++midi)
    {
        int bestRegion = -1;
        int bestDistance = 1000;
        for (size_t i = 0; i < guitarSampleRegions.size(); ++i)
        {
            const auto& region = guitarSampleRegions[i];
            if (midi < region.lowKey || midi > region.highKey)
                continue;
            const int distance = std::abs(midi - region.rootKey);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestRegion = static_cast<int>(i);
            }
        }
        guitarRegionForMidi[static_cast<size_t>(midi)] = bestRegion;
    }

    const bool anyMappedNote = std::any_of(guitarRegionForMidi.begin(), guitarRegionForMidi.end(),
                                            [](int index) { return index >= 0; });
    guitarSamplesLoaded.store(anyMappedNote, std::memory_order_release);
    DBG("GUITAR SAMPLES: " + juce::String(static_cast<int>(guitarSampleRegions.size()))
        + " regions loaded from " + sfzFile.getFileName()
        + (anyMappedNote ? " (ready)" : " (no mapped notes)"));
}

//==============================================================================
// Sample-based backing instruments (SFZ + WAV/FLAC)
//==============================================================================

void MainComponent::loadBackingSampleBanks(const juce::File& assetsRoot)
{
    DBG("BACKING DRUM WAV BUILD: 2026-10-13-v6");
    for (auto& loaded : backingSamplesLoaded)
        loaded.store(false, std::memory_order_relaxed);
    for (auto& regions : backingSampleRegions)
        regions.clear();
    for (auto& map : backingSampleMap)
        map.fill(-1);

    if (! assetsRoot.isDirectory())
    {
        DBG("BACKING: server sound pack is not available; using procedural fallbacks.");
        return;
    }

    auto midiKey = [](juce::String value) -> int
    {
        value = value.trim().toUpperCase();
        if (value.isEmpty()) return -1;
        if (value.containsOnly("0123456789-")) return juce::jlimit(0, 127, value.getIntValue());
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        // SFZ commonly spells flats (Db, Eb, Gb, Ab, Bb) as well as sharps.
        value = value.replace("DB", "C#").replace("EB", "D#").replace("GB", "F#")
                     .replace("AB", "G#").replace("BB", "A#");
        int octaveStart = value.length() - 1;
        while (octaveStart >= 0 && juce::CharacterFunctions::isDigit(value[octaveStart])) --octaveStart;
        if (octaveStart == value.length() - 1) return -1;
        const int octave = value.substring(octaveStart + 1).getIntValue();
        const auto pitch = value.substring(0, octaveStart + 1);
        for (int n = 0; n < 12; ++n)
            if (pitch == names[n]) return juce::jlimit(0, 127, (octave + 1) * 12 + n);
        return -1;
    };

    // Load the dedicated one-shot drum kit from the server sound pack.
    // GM MIDI drum notes are mapped to these WAVs; no pitch-shifting is applied.
    {
        const auto drumsDirectory = assetsRoot.getChildFile("drums");
        struct DrumFile { const char* fileName; int rootKey; };
        const DrumFile drumFiles[] = {
            { "KICK_AC.wav", 36 },
            { "SNARE_LIVE.wav", 38 },
            { "HAT2.wav", 42 },
            { "SPLASH10.wav", 55 },
            { "CRASH18.wav", 49 }
        };
        auto& regions = backingSampleRegions[2];
        auto& keyMap = backingSampleMap[2];
        for (const auto& drum : drumFiles)
        {
            const auto sampleFile = drumsDirectory.getChildFile(drum.fileName);
            if (!sampleFile.existsAsFile())
            {
                DBG("BACKING DRUMS: missing " + sampleFile.getFullPathName());
                continue;
            }
            std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(sampleFile));
            if (reader == nullptr || reader->lengthInSamples < 2 || reader->lengthInSamples > 12000000)
            {
                DBG("BACKING DRUMS: cannot read " + sampleFile.getFileName());
                continue;
            }
            BackingSampleRegion region;
            region.sourceSampleRate = reader->sampleRate;
            region.rootKey = drum.rootKey;
            region.lowKey = drum.rootKey;
            region.highKey = drum.rootKey;
            region.looping = false;
            const int channels = juce::jlimit(1, 2, static_cast<int>(reader->numChannels));
            region.audio.setSize(channels, static_cast<int>(reader->lengthInSamples));
            reader->read(&region.audio, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
            const int regionIndex = static_cast<int>(regions.size());
            regions.push_back(std::move(region));
            keyMap[static_cast<size_t>(drum.rootKey)] = regionIndex;
            DBG("BACKING DRUMS: loaded " + sampleFile.getFileName()
                + ", frames=" + juce::String(static_cast<int>(reader->lengthInSamples)));
        }

        // Route common General MIDI percussion notes to the nearest matching
        // acoustic drum sample while keeping every sample at its original pitch.
        const auto assign = [&keyMap](int midi, int sampleKey)
        {
            const int index = keyMap[static_cast<size_t>(sampleKey)];
            if (index >= 0) keyMap[static_cast<size_t>(midi)] = index;
        };
        assign(35, 36); assign(36, 36);                 // kick
        assign(37, 38); assign(38, 38); assign(40, 38); // snare / rim-ish hits
        assign(42, 42); assign(44, 42);                 // closed/pedal hi-hat
        assign(46, 42);                                 // open hi-hat fallback
        assign(49, 49); assign(57, 49);                 // crash
        assign(52, 49);                                 // Chinese cymbal -> crash
        assign(55, 55);                                 // splash
        assign(51, 49);                                 // ride -> crash fallback

        const bool ready = std::any_of(keyMap.begin(), keyMap.end(), [](int i) { return i >= 0; });
        backingSamplesLoaded[2].store(ready, std::memory_order_release);
        DBG("BACKING DRUMS: " + juce::String(static_cast<int>(regions.size()))
            + " WAV samples " + (ready ? "READY" : "FAILED"));
    }

    DBG("BACKING SFZ BANKS: bass and piano only; drums use the server WAV kit");
    struct BankSpec { int instrument; juce::File sfz; };
    const BankSpec banks[] = {
        { 1, assetsRoot.getChildFile("bass").getChildFile("FingerBassYR 20190930.sfz") },
        { 3, assetsRoot.getChildFile("piano").getChildFile("UprightPianoKW-small-20190703.sfz") }
    };

    for (const auto& bank : banks)
    {
        const auto& sfz = bank.sfz;
        if (! sfz.existsAsFile())
        {
            DBG("BACKING SFZ: missing " + sfz.getFileName() + "; using procedural fallback for instrument " + juce::String(bank.instrument));
            continue;
        }

        struct Pending { juce::String sample; int root=60, low=0, high=127; double loopStart=-1, loopEnd=-1; bool looping=false; };
        std::vector<Pending> pending;
        Pending current;
        bool inRegion = false;
        juce::String defaultPath;
        bool groupLooping = false;
        auto flush = [&]() { if (inRegion && current.sample.isNotEmpty()) pending.push_back(current); current = Pending{}; inRegion = false; };
        auto applyOpcode = [&](const juce::String& key, juce::String value)
        {
            value = value.unquoted();
            if (key == "default_path") defaultPath = value;
            else if (key == "loop_mode")
            {
                groupLooping = (value == "loop_continuous" || value == "loop_sustain");
                if (inRegion) current.looping = groupLooping;
            }
            else if (inRegion)
            {
                if (key == "sample") current.sample = value;
                else if (key == "pitch_keycenter") { int k = midiKey(value); if (k >= 0) current.root = k; }
                else if (key == "key")
                {
                    // In SFZ, key=K means this region is valid only for K.
                    // Setting only the root key made every Steel Drum region
                    // match every MIDI note, so the first sample was reused.
                    int k = midiKey(value);
                    if (k >= 0) { current.root = k; current.low = k; current.high = k; }
                }
                else if (key == "lokey") { int k = midiKey(value); if (k >= 0) current.low = k; }
                else if (key == "hikey") { int k = midiKey(value); if (k >= 0) current.high = k; }
                else if (key == "loop_start") current.loopStart = value.getDoubleValue();
                else if (key == "loop_end") current.loopEnd = value.getDoubleValue();
            }
        };

        const auto lines = juce::StringArray::fromLines(sfz.loadFileAsString());
        for (auto line : lines)
        {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("//")) continue;
            // Tokenize tags and key=value opcodes; supports both multiline FreePats
            // SFZ and inline region declarations used by the Steel Drum bank.
            juce::StringArray tokens;
            juce::String token;
            bool quoted = false;
            for (int i=0; i<line.length(); ++i)
            {
                auto ch = line[i];
                if (ch == '"') quoted = !quoted;
                if (juce::CharacterFunctions::isWhitespace(ch) && !quoted) { if (token.isNotEmpty()) { tokens.add(token); token.clear(); } }
                else token += ch;
            }
            if (token.isNotEmpty()) tokens.add(token);
            for (auto item : tokens)
            {
                if (item.startsWithIgnoreCase("<region>")) { flush(); inRegion = true; current.looping = groupLooping; item = item.substring(8); if (item.isEmpty()) continue; }
                else if (item.startsWithIgnoreCase("<group>")) { flush(); item = item.substring(7); if (item.isEmpty()) continue; }
                else if (item.startsWithIgnoreCase("<global>") || item.startsWithIgnoreCase("<control>") || item.startsWithIgnoreCase("<master>")) { flush(); auto pos=item.indexOf(">"); item = pos >= 0 ? item.substring(pos+1) : juce::String{}; if (item.isEmpty()) continue; }
                const int eq = item.indexOfChar('=');
                if (eq > 0) applyOpcode(item.substring(0,eq), item.substring(eq+1));
            }
        }
        flush();

        auto& regions = backingSampleRegions[static_cast<size_t>(bank.instrument)];
        for (const auto& src : pending)
        {
            auto relative = src.sample.replaceCharacter('\\', '/');
            auto sampleFile = sfz.getParentDirectory().getChildFile(defaultPath + relative);
            if (!sampleFile.existsAsFile()) sampleFile = sfz.getParentDirectory().getChildFile(relative);
            if (!sampleFile.existsAsFile()) { DBG("BACKING SFZ: missing sample " + sampleFile.getFullPathName()); continue; }
            std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(sampleFile));
            if (reader == nullptr || reader->lengthInSamples < 2 || reader->lengthInSamples > 12000000) continue;
            BackingSampleRegion region;
            region.sourceSampleRate = reader->sampleRate;
            region.rootKey = juce::jlimit(0,127,src.root);
            region.lowKey = juce::jlimit(0,127,src.low);
            region.highKey = juce::jlimit(region.lowKey,127,src.high);
            region.loopStart = src.loopStart; region.loopEnd = src.loopEnd;
            region.looping = src.looping && src.loopStart >= 0 && src.loopEnd > src.loopStart;
            const int channels = juce::jlimit(1,2,static_cast<int>(reader->numChannels));
            region.audio.setSize(channels, static_cast<int>(reader->lengthInSamples));
            reader->read(&region.audio, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);
            regions.push_back(std::move(region));
        }
        for (int midi=0; midi<128; ++midi)
        {
            int best=-1, distance=1000;
            for (size_t i=0; i<regions.size(); ++i)
            {
                const auto& r=regions[i];
                if (midi < r.lowKey || midi > r.highKey) continue;
                const int d=std::abs(midi-r.rootKey);
                if (d < distance) { best=static_cast<int>(i); distance=d; }
            }
            backingSampleMap[static_cast<size_t>(bank.instrument)][static_cast<size_t>(midi)] = best;
        }
        const bool ready = !regions.empty() && std::any_of(backingSampleMap[static_cast<size_t>(bank.instrument)].begin(), backingSampleMap[static_cast<size_t>(bank.instrument)].end(), [](int i){ return i>=0; });
        backingSamplesLoaded[static_cast<size_t>(bank.instrument)].store(ready, std::memory_order_release);
        DBG("BACKING SFZ: instrument=" + juce::String(bank.instrument) + ", regions=" + juce::String(static_cast<int>(regions.size())) + ", file=" + sfz.getFileName() + (ready ? " READY" : " FAILED"));
    }
}

//==============================================================================
// Metronome clicks (server-hosted sound pack)
//==============================================================================

void MainComponent::loadMetronomeSamples(const juce::File& assetsRoot)
{
    // Called only while the audio device is stopped: no allocation or file I/O
    // happens in prepareToPlay()/getNextAudioBlock().
    metronomeAccentBuffer.setSize(0, 0);
    metronomeNormalBuffer.setSize(0, 0);
    metronomeSamplesRemaining = 0;
    metronomePlaybackPosition = 0;

    if (! assetsRoot.isDirectory())
        return;

    auto loadClick = [this](const juce::File& file, juce::AudioBuffer<float>& buffer)
    {
        if (! file.existsAsFile())
        {
            DBG("METRONOME: missing " + file.getFullPathName());
            return;
        }

        std::unique_ptr<juce::AudioFormatReader> reader(
            audioFormatManager.createReaderFor(file));

        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            DBG("METRONOME: cannot read " + file.getFileName());
            return;
        }

        const int channels = juce::jlimit(
            1, 2, static_cast<int>(reader->numChannels));
        const int samples = static_cast<int>(reader->lengthInSamples);
        buffer.setSize(channels, samples);
        reader->read(&buffer, 0, samples, 0, true, true);
    };

    const auto metronomeDirectory = assetsRoot.getChildFile("metronome");
    loadClick(metronomeDirectory.getChildFile("wooden-metronome-click.wav"),
              metronomeAccentBuffer);
    loadClick(metronomeDirectory.getChildFile("light-clicking-perc.wav"),
              metronomeNormalBuffer);
}

float MainComponent::renderBackingSample(int instrument, int midiNote, double elapsedOutputSamples, double outputSampleRate) const
{
    if (instrument < 1 || instrument > 3 || midiNote < 0 || midiNote > 127 || outputSampleRate <= 0.0
        || !backingSamplesLoaded[static_cast<size_t>(instrument)].load(std::memory_order_acquire)) return 0.0f;
    // The drum lane uses dedicated one-shot WAVs. Keep their recorded pitch.
    int playedMidi = midiNote;
    if (instrument == 2)
    {
        switch (midiNote)
        {
            case 35: case 36: playedMidi = 36; break; // kick
            case 37: case 38: case 40: playedMidi = 38; break; // snare
            case 42: case 44: case 46: playedMidi = 42; break; // hi-hat
            case 49: case 51: case 52: case 57: playedMidi = 49; break; // cymbal
            case 55: playedMidi = 55; break; // splash
            default: playedMidi = 38; break;
        }
    }
    const auto index = backingSampleMap[static_cast<size_t>(instrument)][static_cast<size_t>(playedMidi)];
    const auto& regions = backingSampleRegions[static_cast<size_t>(instrument)];
    if (index < 0 || index >= static_cast<int>(regions.size())) return 0.0f;
    const auto& region = regions[static_cast<size_t>(index)];
    const int total = region.audio.getNumSamples();
    if (total < 2) return 0.0f;
    const double ratio = std::pow(2.0, static_cast<double>(playedMidi-region.rootKey)/12.0);
    double pos = elapsedOutputSamples / outputSampleRate * region.sourceSampleRate * ratio;
    if (region.looping && region.loopEnd > region.loopStart && pos >= region.loopEnd)
        pos = region.loopStart + std::fmod(pos-region.loopStart, region.loopEnd-region.loopStart);
    else if (pos >= total-1) return 0.0f;
    int i0=juce::jlimit(0,total-1,static_cast<int>(pos));
    int i1=i0+1;
    if (region.looping && i1 >= static_cast<int>(region.loopEnd)) i1=juce::jlimit(0,total-1,static_cast<int>(region.loopStart));
    else i1=juce::jmin(i1,total-1);
    const float frac=static_cast<float>(pos-i0);
    float a=0,b=0;
    for (int ch=0; ch<region.audio.getNumChannels(); ++ch) { a+=region.audio.getSample(ch,i0); b+=region.audio.getSample(ch,i1); }
    return (a+(b-a)*frac)/static_cast<float>(juce::jmax(1,region.audio.getNumChannels()));
}

float MainComponent::renderGuitarSample(int midiNote, double elapsedOutputSamples, double outputSampleRate) const
{
    if (!guitarSamplesLoaded.load(std::memory_order_acquire)
        || midiNote < 0 || midiNote >= 128 || outputSampleRate <= 0.0)
        return 0.0f;

    const int regionIndex = guitarRegionForMidi[static_cast<size_t>(midiNote)];
    if (regionIndex < 0 || regionIndex >= static_cast<int>(guitarSampleRegions.size()))
        return 0.0f;

    const auto& region = guitarSampleRegions[static_cast<size_t>(regionIndex)];
    const int totalSamples = region.audio.getNumSamples();
    if (totalSamples < 2)
        return 0.0f;

    const double pitchRatio = std::pow(2.0, static_cast<double>(midiNote - region.rootKey) / 12.0);
    double sourcePosition = (elapsedOutputSamples / outputSampleRate) * region.sourceSampleRate * pitchRatio;

    if (region.looping && region.loopEnd > region.loopStart && sourcePosition >= region.loopEnd)
        sourcePosition = region.loopStart + std::fmod(sourcePosition - region.loopStart,
                                                      region.loopEnd - region.loopStart);
    else if (sourcePosition >= static_cast<double>(totalSamples - 1))
        return 0.0f;

    if (region.looping && sourcePosition >= region.loopEnd)
        sourcePosition = region.loopStart;

    const int index0 = juce::jlimit(0, totalSamples - 1, static_cast<int>(sourcePosition));
    int index1 = index0 + 1;
    if (region.looping && index1 >= static_cast<int>(region.loopEnd))
        index1 = juce::jlimit(0, totalSamples - 1, static_cast<int>(region.loopStart));
    else
        index1 = juce::jmin(index1, totalSamples - 1);

    const float fraction = static_cast<float>(sourcePosition - static_cast<double>(index0));
    float sample0 = 0.0f;
    float sample1 = 0.0f;
    const int channels = region.audio.getNumChannels();
    for (int channel = 0; channel < channels; ++channel)
    {
        sample0 += region.audio.getSample(channel, index0);
        sample1 += region.audio.getSample(channel, index1);
    }
    const float divisor = 1.0f / static_cast<float>(juce::jmax(1, channels));
    return (sample0 + (sample1 - sample0) * fraction) * divisor;
}

// WSOLA-style offline time stretching. Samples are copied/overlapped rather than
// resampled, so changing tempo does not systematically change the instrument pitch.
static juce::AudioBuffer<float> makePitchPreservingTimeStretch(
    const juce::AudioBuffer<float>& source, double speedRatio)
{
    const int sourceLength = source.getNumSamples();
    const int channels = source.getNumChannels();
    constexpr int frameSize = 2048;
    if (sourceLength <= 0 || channels <= 0)
        return {};
    if (sourceLength <= frameSize || std::abs(speedRatio - 1.0) < 0.001)
        return juce::AudioBuffer<float>(source);

    speedRatio = juce::jlimit(0.5, 1.2, speedRatio);
    constexpr int overlap = frameSize / 2;
    constexpr int synthesisHop = frameSize - overlap;
    constexpr int searchRadius = 256;
    const int outputLength = juce::jmax(frameSize,
        static_cast<int>(std::ceil(static_cast<double>(sourceLength) / speedRatio)));

    juce::AudioBuffer<float> output(channels, outputLength);
    output.clear();
    juce::AudioBuffer<float> weights(1, outputLength);
    weights.clear();

    int previousSourceStart = 0;
    bool firstFrame = true;
    for (int outputStart = 0; outputStart < outputLength; outputStart += synthesisHop)
    {
        int candidate = firstFrame ? 0 : static_cast<int>(std::llround(outputStart * speedRatio));
        if (!firstFrame)
        {
            const int expected = candidate;
            const int minCandidate = juce::jmax(0, expected - searchRadius);
            const int maxCandidate = juce::jmin(sourceLength - frameSize, expected + searchRadius);
            double bestCorrelation = -1.0e30;
            int bestCandidate = juce::jlimit(0, juce::jmax(0, sourceLength - frameSize), expected);
            const int compareCount = juce::jmin(overlap, juce::jmin(outputLength - outputStart, sourceLength));
            for (int test = minCandidate; test <= maxCandidate; test += 8)
            {
                double dot = 0.0, energyA = 1.0e-12, energyB = 1.0e-12;
                const int available = juce::jmin(compareCount, juce::jmin(sourceLength - test,
                    sourceLength - juce::jmin(sourceLength - 1, previousSourceStart + synthesisHop)));
                for (int i = 0; i < available; i += 4)
                {
                    const float a = source.getSample(0, test + i);
                    const int previousIndex = previousSourceStart + synthesisHop + i;
                    if (previousIndex >= sourceLength) break;
                    const float b = source.getSample(0, previousIndex);
                    dot += static_cast<double>(a) * b;
                    energyA += static_cast<double>(a) * a;
                    energyB += static_cast<double>(b) * b;
                }
                const double corr = dot / std::sqrt(energyA * energyB);
                if (corr > bestCorrelation) { bestCorrelation = corr; bestCandidate = test; }
            }
            candidate = bestCandidate;
        }

        if (candidate >= sourceLength) break;
        const int count = juce::jmin(frameSize, juce::jmin(outputLength - outputStart, sourceLength - candidate));
        if (count <= 0) break;
        for (int i = 0; i < count; ++i)
        {
            const int outIndex = outputStart + i;
            const float fadeIn = firstFrame || i >= overlap ? 1.0f
                : static_cast<float>(i) / static_cast<float>(juce::jmax(1, overlap - 1));
            const float fadeOut = (!firstFrame && i < overlap) ? 1.0f - fadeIn : 0.0f;
            for (int ch = 0; ch < channels; ++ch)
            {
                const float newSample = source.getSample(ch, candidate + i);
                if (firstFrame || i >= overlap)
                    output.addSample(ch, outIndex, newSample);
                else
                {
                    // The previous frame already occupies this overlap; crossfade it.
                    const float oldSample = output.getSample(ch, outIndex);
                    output.setSample(ch, outIndex, oldSample * fadeOut + newSample * fadeIn);
                }
            }
        }
        previousSourceStart = candidate;
        firstFrame = false;
    }

    // Clear any unused tail if the source was shorter than one frame.
    return output;
}

//==============================================================================
// External WAV/MP3 tracks
//==============================================================================

bool MainComponent::loadExternalAudioTrack(size_t index, const juce::File& file)
{
    if (index >= externalAudioTracks.size())
        return false;

    if (!file.existsAsFile())
    {
        DBG("External audio track not found: " + file.getFullPathName());
        return false;
    }

    std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels <= 0)
    {
        DBG("Could not decode external audio track: " + file.getFullPathName());
        return false;
    }

    auto& track = externalAudioTracks[index];
    const int channels = static_cast<int>(juce::jmin<uint32_t>(2, reader->numChannels));
    constexpr double maxSongSeconds = 60.0;
    const int64_t maxSamplesForOneMinute = static_cast<int64_t>(reader->sampleRate * maxSongSeconds);

    // The server's MP3 stems are raw CBR streams without a gapless
    // (Xing/LAME) header. JUCE's MP3 decoder therefore leaves the standard
    // MP3 priming in front of the audio: 1105 samples of codec/encoder delay,
    // plus a full 1152-sample silent frame when the encoder wrote one.
    // Without trimming this, every stem starts ~51 ms after the MIDI and
    // metronome timeline. Detect the silent priming frame from the decoded
    // PCM instead of relying on the file name alone.
    int sourceStartOffset = 0;

    if (file.hasFileExtension("mp3"))
    {
        constexpr int probeFrameSamples = 1152;
        const int probeSamples = static_cast<int>(
            juce::jmin<int64_t>(
                reader->lengthInSamples,
                probeFrameSamples));

        juce::AudioBuffer<float> probe(channels, probeSamples);
        probe.clear();

        if (probeSamples > 0
            && reader->read(&probe, 0, probeSamples, 0, true, true))
        {
            float probeMagnitude = 0.0f;

            for (int channel = 0; channel < channels; ++channel)
            {
                probeMagnitude = juce::jmax(
                    probeMagnitude,
                    probe.getMagnitude(channel, 0, probeSamples));
            }

            const bool firstFrameIsSilent =
                probeMagnitude < 1.0e-4f;

            constexpr double codecDelaySeconds =
                1105.0 / 44100.0;
            constexpr double primingFrameSeconds =
                1152.0 / 44100.0;

            const double delaySeconds =
                codecDelaySeconds
                + (firstFrameIsSilent ? primingFrameSeconds : 0.0);

            sourceStartOffset = juce::jmax(
                0,
                static_cast<int>(
                    std::llround(
                        reader->sampleRate * delaySeconds)));
        }
    }

    // Read enough source samples that, after removing the MP3 priming,
    // the gameplay window is still a full 60 seconds for long songs.
    const int64_t maxSamplesWithPriming =
        maxSamplesForOneMinute + static_cast<int64_t>(sourceStartOffset);

    const int samples = static_cast<int>(juce::jmin<int64_t>(reader->lengthInSamples,
        juce::jmin<int64_t>(maxSamplesWithPriming,
            static_cast<int64_t>(std::numeric_limits<int>::max()))));

    const int samplesToRead =
        juce::jmax(0, samples - sourceStartOffset);

    track.audio.setSize(channels, samplesToRead);
    track.audio.clear();

    if (samplesToRead <= 0
        || !reader->read(&track.audio, 0, samplesToRead, sourceStartOffset, true, true))
    {
        track.audio.setSize(0, 0);
        DBG("Failed reading external audio track: " + file.getFullPathName());
        return false;
    }
    track.sourceSampleRate = reader->sampleRate;
    track.loaded.store(true, std::memory_order_release);
    DBG("Loaded external audio track: " + file.getFullPathName()
        + " (" + juce::String(samplesToRead) + " samples, "
        + juce::String(reader->sampleRate) + " Hz, "
        + juce::String(static_cast<double>(samplesToRead) / reader->sampleRate, 2)
        + " seconds; capped at 60 seconds; trimmed "
        + juce::String(sourceStartOffset) + " MP3 priming samples)");
    return true;
}

//==============================================================================
// Prepare to play
//==============================================================================

void MainComponent::prepareToPlay(
    int samplesPerBlockExpected,
    double sampleRate)
{
    juce::ignoreUnused(
        samplesPerBlockExpected);

    // The audio callback receives one buffer channel per active input. Cache
    // that count and the active-index -> hardware-channel map so guitar capture
    // can select any active input without querying the device from the
    // realtime thread.
    activeInputChannels.store(0, std::memory_order_relaxed);
    activeInputHardwareChannels.fill(-1);
    currentInputChannel = 0;

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const auto active = device->getActiveInputChannels();
        int activeIndex = 0;

        for (int hardware = 0;
            hardware < static_cast<int>(activeInputHardwareChannels.size());
            ++hardware)
        {
            if (!active[hardware])
                continue;

            activeInputHardwareChannels[static_cast<size_t>(activeIndex)] = hardware;
            ++activeIndex;
        }

        activeInputChannels.store(activeIndex, std::memory_order_relaxed);
    }


    // Audio formats were registered in the constructor before the sample bank
    // was loaded.  Metronome clicks are decoded by loadMetronomeSamples() while
    // the device is stopped; prepareToPlay() must not touch the filesystem.

    //==========================================================================
    // Pitch buffers

    pitchBuffer.setSize(1, pitchAnalysisSize);

    pitchBuffer.clear();

    pitchAnalysisBuffer.setSize(1, pitchAnalysisSize);

    pitchAnalysisBuffer.clear();

    pitchAnalysisBuffer2.setSize(1, pitchAnalysisSize);

    pitchAnalysisBuffer2.clear();

    pitchBufferPosition = 0;

    pitchBufferReady.store(
        false,
        std::memory_order_relaxed);

    pitchBufferReady2.store(
        false,
        std::memory_order_relaxed);

    //==========================================================================
 // Metronome timing

    metronome.setSampleRate(
        sampleRate);

    metronome.setBpm(
        bpm.load(
            std::memory_order_relaxed));

    metronome.resetScheduler(
        0);

    //==========================================================================
    // Audio timeline

    audioSampleCounter.store(
        0,
        std::memory_order_relaxed);


}
//==============================================================================
// Audio callback
//==============================================================================

void MainComponent::getNextAudioBlock(
    const juce::AudioSourceChannelInfo& bufferToFill)
{
    if (bufferToFill.buffer == nullptr)
        return;

    //==========================================================================
    // Sample counter

    const auto currentSample =
        audioSampleCounter.load(
            std::memory_order_relaxed);

    audioSampleCounter.fetch_add(
        static_cast<uint64_t>(
            bufferToFill.numSamples),
        std::memory_order_relaxed);

    //==========================================================================
// Beat scheduler

    int metronomeTriggerOffset = -1;

    if (gameState == GameState::Playing)
    {
        metronomeTriggerOffset = metronome.processScheduler(
            currentSample,
            bufferToFill.numSamples);
    }

    //==========================================================================
    // Playhead

    if (gameState == GameState::Playing)
    {
        const auto currentGameplayStart =
            gameplayStartSample.load(
                std::memory_order_relaxed);

        const auto currentBpm =
            bpm.load(
                std::memory_order_relaxed);

        if (currentBpm > 0.0
            && currentSample >= currentGameplayStart)
        {
            double sampleRate = 0.0;

            if (auto* device =
                deviceManager.getCurrentAudioDevice())
            {
                sampleRate =
                    device->getCurrentSampleRate();
            }

            if (sampleRate > 0.0)
            {
                const double samplesPerBeat =
                    sampleRate
                    * 60.0
                    / currentBpm;

                if (samplesPerBeat > 0.0)
                {
                    const double currentBeat =
                        static_cast<double>(
                            currentSample
                            - currentGameplayStart)
                        / samplesPerBeat;

                    currentPlayheadBeat.store(
                        currentBeat,
                        std::memory_order_relaxed);
                }
            }
        }
    }

    //==========================================================================
    // Input samples
    //
    // IMPORTANT:
    // Use startSample because AudioSourceChannelInfo can point to an
    // offset inside the AudioBuffer.
    //
    // JUCE fills channels 0..outputCount-1 with the output buffers (including
    // the first inputs) and appends input-only channels when the device has
    // more inputs than outputs. Never assume the guitar is on channel 0: the
    // user may have it on any input of a multi-channel interface.

    const int inputChannelCount = juce::jlimit(
        0,
        bufferToFill.buffer->getNumChannels(),
        activeInputChannels.load(std::memory_order_relaxed));

    const int outputChannelCount = juce::jmin(
        2,
        bufferToFill.buffer->getNumChannels());

    int inputChannel = 0;

    if (inputChannelCount > 0)
    {
        const int requestedHardware =
            preferredInputChannel.load(std::memory_order_relaxed);

        int requestedIndex = -1;

        if (requestedHardware >= 0)
        {
            const int mappedCount = juce::jmin(
                inputChannelCount,
                static_cast<int>(activeInputHardwareChannels.size()));

            for (int index = 0; index < mappedCount; ++index)
            {
                if (activeInputHardwareChannels[static_cast<size_t>(index)]
                    == requestedHardware)
                {
                    requestedIndex = index;
                    break;
                }
            }
        }

        if (requestedIndex >= 0)
        {
            inputChannel = requestedIndex;
            currentInputChannel = requestedIndex;
        }
        else
        {
            // Auto mode: follow the loudest active input, but keep the current
            // channel unless another one is clearly stronger. This prevents
            // flapping between silent or noisy inputs during a performance.
            int bestChannel = juce::jlimit(
                0, inputChannelCount - 1, currentInputChannel);
            float bestRms = bufferToFill.buffer->getRMSLevel(
                bestChannel,
                bufferToFill.startSample,
                bufferToFill.numSamples);

            for (int channel = 0;
                channel < inputChannelCount;
                ++channel)
            {
                const float rms = bufferToFill.buffer->getRMSLevel(
                    channel,
                    bufferToFill.startSample,
                    bufferToFill.numSamples);

                if (rms > bestRms * 1.6f + 0.0015f)
                {
                    bestChannel = channel;
                    bestRms = rms;
                }
            }

            inputChannel = bestChannel;
            currentInputChannel = bestChannel;
        }
    }

    const auto* samples = inputChannelCount > 0
        ? bufferToFill.buffer->getReadPointer(
              inputChannel,
              bufferToFill.startSample)
        : nullptr;

    //==========================================================================
    // Mono guitar -> stereo monitoring

    if (outputChannelCount > 0)
    {
        if (samples != nullptr)
        {
            auto* left = bufferToFill.buffer->getWritePointer(
                0,
                bufferToFill.startSample);

            for (int i = 0;
                i < bufferToFill.numSamples;
                ++i)
            {
                left[i] = samples[i];
            }

            if (outputChannelCount > 1)
            {
                auto* right = bufferToFill.buffer->getWritePointer(
                    1,
                    bufferToFill.startSample);

                for (int i = 0;
                    i < bufferToFill.numSamples;
                    ++i)
                {
                    right[i] = samples[i];
                }
            }
        }
        else
        {
            for (int channel = 0;
                channel < outputChannelCount;
                ++channel)
            {
                bufferToFill.buffer->clear(
                    channel,
                    bufferToFill.startSample,
                    bufferToFill.numSamples);
            }
        }
    }

    //==========================================================================
    //==========================================================================
// Pitch buffer

    for (int i = 0;
        i < bufferToFill.numSamples;
        ++i)
    {
        pitchBuffer.setSample(
            0,
            pitchBufferPosition,
            samples != nullptr ? samples[i] : 0.0f);

        ++pitchBufferPosition;

        if (pitchBufferPosition >= pitchAnalysisSize)
        {
            //==================================================================
            // First analysis buffer
            //
            // This buffer has priority because it is the scoring buffer.

            if (!pitchBufferReady.load(
                std::memory_order_acquire))
            {
                auto* destination =
                    pitchAnalysisBuffer.getWritePointer(0);

                const auto* source =
                    pitchBuffer.getReadPointer(0);

                for (int sample = 0;
                    sample < pitchAnalysisSize;
                    ++sample)
                {
                    destination[sample] =
                        source[sample];
                }

                const auto analysisStartSample =
                    currentSample
                    + static_cast<uint64_t>(i + 1)
                    - static_cast<uint64_t>(pitchAnalysisSize);

                pitchAnalysisStartSample.store(
                    analysisStartSample,
                    std::memory_order_relaxed);

                // IMPORTANT:
                // Publish the completed buffer only after all samples
                // and metadata have been written.

                pitchBufferReady.store(
                    true,
                    std::memory_order_release);
            }

            //==================================================================
            // Second analysis buffer
            //
            // Display only. It does not participate in scoring.

            else if (!pitchBufferReady2.load(
                std::memory_order_acquire))
            {
                auto* destination =
                    pitchAnalysisBuffer2.getWritePointer(0);

                const auto* source =
                    pitchBuffer.getReadPointer(0);

                for (int sample = 0;
                    sample < pitchAnalysisSize;
                    ++sample)
                {
                    destination[sample] =
                        source[sample];
                }

                const auto analysisStartSample =
                    currentSample
                    + static_cast<uint64_t>(i + 1)
                    - static_cast<uint64_t>(pitchAnalysisSize);

                pitchAnalysisStartSample2.store(
                    analysisStartSample,
                    std::memory_order_relaxed);

                pitchBufferReady2.store(
                    true,
                    std::memory_order_release);
            }

            pitchBufferPosition = 0;
        }
    }

    //==========================================================================
    // Start metronome

    int metronomeBlockOffset = 0;

    if (gameState == GameState::Playing
        && metronome.consumeTrigger()
        && metronomeTriggerOffset >= 0)
    {
        if (countInActive.load(std::memory_order_acquire))
        {
            const int remaining =
                countInRemaining.load(std::memory_order_relaxed);
            if (remaining > 0)
                countInRemaining.store(remaining - 1, std::memory_order_relaxed);
        }

        metronomePlayingAccent =
            metronome.isAccent();

        metronomePlaybackPosition = 0;

        const auto& buffer =
            metronomePlayingAccent
            ? metronomeAccentBuffer
            : metronomeNormalBuffer;

        metronomeSamplesRemaining =
            buffer.getNumSamples();

        // Start the click on the exact beat sample inside this block instead
        // of quantising it to the block start.
        metronomeBlockOffset = metronomeTriggerOffset;
    }

    //==========================================================================
    // No metronome outside gameplay

    if (gameState != GameState::Playing)
        return;

    // Apply distortion to the monitored guitar signal only after the clean samples
    // have been copied to the pitch-analysis buffers, preserving fair scoring.
    if (distortionEnabled.load(std::memory_order_relaxed))
    {
        const float drive = distortionDrive.load(std::memory_order_relaxed);
        const float tone = distortionTone.load(std::memory_order_relaxed);
        const int channels = outputChannelCount;
        // Gain-compensated soft clipping: increasing DRIVE adds saturation without
        // the huge output jump of an uncompensated tanh(x * drive).
        const float compensation = 1.0f / juce::jmax(1.0f, std::sqrt(drive));
        for (int channel = 0; channel < channels; ++channel)
        {
            auto* output = bufferToFill.buffer->getWritePointer(channel, bufferToFill.startSample);
            float lowState = distortionFilterState[channel];
            // TONE moves between a darker low-pass sound and a brighter direct sound.
            const float lowPass = 0.015f + (1.0f - tone) * 0.30f;
            for (int i = 0; i < bufferToFill.numSamples; ++i)
            {
                const float clean = output[i];
                const float saturated = std::tanh(clean * drive) * compensation;
                lowState += lowPass * (saturated - lowState);
                output[i] = lowState * (1.0f - tone) + saturated * tone;
            }
            distortionFilterState[channel] = lowState;
        }
    }

    //==========================================================================
    // External music mixer. All four instrumental stems share gameplayStartSample and remain aligned.
    if (gameState == GameState::Playing)
    {
        const uint64_t songStart = gameplayStartSample.load(std::memory_order_relaxed);
        const double outputRate = deviceManager.getCurrentAudioDevice() != nullptr
            ? deviceManager.getCurrentAudioDevice()->getCurrentSampleRate() : 0.0;
        if (outputRate > 0.0 && currentSample + static_cast<uint64_t>(bufferToFill.numSamples) > songStart)
        {
            const std::array<float, 4> gains = {
                midiDrumsEnabled.load(std::memory_order_relaxed) ? midiDrumsVolume.load(std::memory_order_relaxed) : 0.0f,
                midiBassEnabled.load(std::memory_order_relaxed) ? midiBassVolume.load(std::memory_order_relaxed) : 0.0f,
                midiRhythmGuitarEnabled.load(std::memory_order_relaxed) ? midiRhythmGuitarVolume.load(std::memory_order_relaxed) : 0.0f,
                midiPianoEnabled.load(std::memory_order_relaxed) ? midiPianoVolume.load(std::memory_order_relaxed) : 0.0f
            };
            const int outputChannels = outputChannelCount;
            for (size_t trackIndex = 0; trackIndex < externalAudioTracks.size(); ++trackIndex)
            {
                const auto& track = externalAudioTracks[trackIndex];
                const float gain = gains[trackIndex];
                auto playbackAudio = std::atomic_load(&track.playbackAudio);
                if (!track.loaded.load(std::memory_order_acquire) || gain <= 0.0001f
                    || playbackAudio == nullptr || playbackAudio->getNumSamples() <= 0
                    || track.sourceSampleRate <= 0.0)
                    continue;

                for (int i = 0; i < bufferToFill.numSamples; ++i)
                {
                    const uint64_t absoluteSample = currentSample + static_cast<uint64_t>(i);
                    if (absoluteSample < songStart)
                        continue; // four-beat count-in; audio starts on the game timeline
                    // The stem has already been time-stretched offline for the selected
                    // BPM; playback now advances at the normal sample-rate ratio so pitch stays stable.
                    const double sourcePos = static_cast<double>(absoluteSample - songStart)
                        * track.sourceSampleRate / outputRate;
                    if (sourcePos >= static_cast<double>(playbackAudio->getNumSamples() - 1))
                        break;
                    const int index0 = static_cast<int>(sourcePos);
                    const int index1 = juce::jmin(index0 + 1, playbackAudio->getNumSamples() - 1);
                    const float fraction = static_cast<float>(sourcePos - index0);
                    for (int channel = 0; channel < outputChannels; ++channel)
                    {
                        const int sourceChannel = juce::jmin(channel, track.audio.getNumChannels() - 1);
                        const float a = playbackAudio->getSample(sourceChannel, index0);
                        const float b = playbackAudio->getSample(sourceChannel, index1);
                        bufferToFill.buffer->addSample(channel, bufferToFill.startSample + i,
                            (a + (b - a) * fraction) * gain);
                    }
                }
            }
        }
    }

    //==========================================================================
    // MIDI guitar backing track. The pitch-analysis buffers above contain only
    // the clean physical guitar input, so this generated sound cannot affect scoring.
    if (midiBackingEnabled.load(std::memory_order_relaxed))
    {
        const double currentBpm = bpm.load(std::memory_order_relaxed);
        const double sr = (deviceManager.getCurrentAudioDevice() != nullptr)
            ? deviceManager.getCurrentAudioDevice()->getCurrentSampleRate() : 0.0;
        const uint64_t songStart = gameplayStartSample.load(std::memory_order_relaxed);
        const float backingVolume = midiBackingVolume.load(std::memory_order_relaxed);
        const auto& notes = gameSession.getMidiNotes();
        if (sr > 0.0 && currentBpm > 0.0 && backingVolume > 0.0f
            && notes.size() > 0 && currentSample + static_cast<uint64_t>(bufferToFill.numSamples) > songStart)
        {
            const double samplesPerBeat = sr * 60.0 / currentBpm;
            const int channels = outputChannelCount;
            for (int i = 0; i < bufferToFill.numSamples; ++i)
            {
                const uint64_t absoluteSample = currentSample + static_cast<uint64_t>(i);
                if (absoluteSample < songStart)
                    continue; // keep the four-beat count-in free of melody notes
                const double elapsedSongSamples = static_cast<double>(absoluteSample - songStart);
                float sample = 0.0f;
                for (const auto& note : notes)
                {
                    const double noteStart = note.startBeat * samplesPerBeat;
                    const double noteLength = juce::jmax(0.08, note.durationBeats * samplesPerBeat);
                    const double noteElapsedSamples = elapsedSongSamples - noteStart;
                    if (noteElapsedSamples < 0.0 || noteElapsedSamples >= noteLength)
                        continue;
                    const double t = noteElapsedSamples / sr;
                    const double releaseStart = noteLength * 0.82;
                    const double release = noteElapsedSamples > releaseStart
                        ? juce::jmax(0.0, 1.0 - (noteElapsedSamples - releaseStart) / juce::jmax(1.0, noteLength - releaseStart)) : 1.0;
                    float noteSample = 0.0f;
                    if (guitarSamplesLoaded.load(std::memory_order_relaxed))
                    {
                        // Recorded clean electric-guitar sample, pitch-shifted from
                        // its nearest mapped key and looped for sustained MIDI notes.
                        noteSample = renderGuitarSample(note.midiNote, noteElapsedSamples, sr);
                    }
                    else
                    {
                        // Keep the known-working v28 tone as a fallback when the
                        // optional sample bank has not yet been installed.
                        const double frequency = juce::MidiMessage::getMidiNoteInHertz(note.midiNote);
                        const double attack = 1.0 - std::exp(-t * 90.0);
                        const double decay = std::exp(-t * 2.7);
                        const double fundamental = std::sin(juce::MathConstants<double>::twoPi * frequency * t);
                        const double second = std::sin(juce::MathConstants<double>::twoPi * frequency * 2.01 * t);
                        const double third = std::sin(juce::MathConstants<double>::twoPi * frequency * 3.02 * t);
                        noteSample = static_cast<float>((fundamental * 0.70 + second * 0.20 + third * 0.10)
                                                       * attack * decay);
                    }
                    sample += noteSample * static_cast<float>(release);
                }
                sample = juce::jlimit(-1.0f, 1.0f, sample * backingVolume);
                for (int channel = 0; channel < channels; ++channel)
                    bufferToFill.buffer->addSample(channel, bufferToFill.startSample + i, sample);
            }
        }
    }

    //==========================================================================
    // Play generated MIDI backing parts. These events are mixed only after clean
    // guitar capture, so they cannot influence pitch detection or scoring.
    if (!externalAudioMixerActive
        && gameState == GameState::Playing
        && dailyChallengeLoaded.load(std::memory_order_relaxed)
        && !backingNotes.empty())
    {
        const double sr = (deviceManager.getCurrentAudioDevice() != nullptr)
            ? deviceManager.getCurrentAudioDevice()->getCurrentSampleRate() : 0.0;
        const double currentBpm = bpm.load(std::memory_order_relaxed);
        const uint64_t songStart = gameplayStartSample.load(std::memory_order_relaxed);
        if (sr > 0.0 && currentBpm > 0.0 && currentSample + static_cast<uint64_t>(bufferToFill.numSamples) > songStart)
        {
            const double samplesPerBeat = sr * 60.0 / currentBpm;
            const int channels = outputChannelCount;
            const double elapsedAtBlockStart = currentSample >= songStart
                ? static_cast<double>(currentSample - songStart) : -static_cast<double>(songStart - currentSample);
            for (const auto& note : backingNotes)
            {
                const float volume = note.instrument == 1 ? midiBassVolume.load(std::memory_order_relaxed)
                    : note.instrument == 2 ? midiDrumsVolume.load(std::memory_order_relaxed)
                    : note.instrument == 3 ? midiPianoVolume.load(std::memory_order_relaxed)
                                           : midiRhythmGuitarVolume.load(std::memory_order_relaxed);
                const bool enabled = note.instrument == 1 ? midiBassEnabled.load(std::memory_order_relaxed)
                    : note.instrument == 2 ? midiDrumsEnabled.load(std::memory_order_relaxed)
                    : note.instrument == 3 ? midiPianoEnabled.load(std::memory_order_relaxed)
                                           : midiRhythmGuitarEnabled.load(std::memory_order_relaxed);
                if (!enabled || volume <= 0.0001f) continue;
                const double relativeStart = note.startBeat * samplesPerBeat - elapsedAtBlockStart;
                const double midiLength = juce::jmax(1.0, note.durationBeats * samplesPerBeat);
                // Drum WAVs are one-shots. Allow cymbals and splashes to ring out
                // naturally even when the MIDI percussion event is very short.
                const double length = note.instrument == 2 ? juce::jmax(midiLength, sr * 3.0) : midiLength;
                const int first = juce::jmax(0, static_cast<int>(std::floor(relativeStart)));
                const int last = juce::jmin(bufferToFill.numSamples, static_cast<int>(std::ceil(relativeStart + length)));
                if (last <= first) continue;
                const double frequency = juce::MidiMessage::getMidiNoteInHertz(note.midiNote);
                const float velocity = static_cast<float>(note.velocity) / 127.0f;
                for (int i = first; i < last; ++i)
                {
                    const double noteSamplePos = static_cast<double>(i) - relativeStart;
                    if (noteSamplePos < 0.0 || noteSamplePos >= length) continue;
                    const double t = noteSamplePos / sr;
                    float sample = 0.0f;
                    if (note.instrument == 4)
                    {
                        // Reuse the app's recorded clean electric-guitar sample bank for chord notes.
                        // Short MIDI durations create tight rhythm-guitar attacks without changing scoring input.
                        sample = guitarSamplesLoaded.load(std::memory_order_relaxed)
                            ? renderGuitarSample(note.midiNote, noteSamplePos, sr)
                            : static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * frequency * t)
                                * std::exp(-t * 9.0) * 0.16);
                        const double releaseStart = length * 0.72;
                        if (noteSamplePos > releaseStart)
                            sample *= static_cast<float>(juce::jlimit(0.0, 1.0,
                                (length - noteSamplePos) / juce::jmax(1.0, length - releaseStart)));
                    }
                    const float sampled = note.instrument == 4 ? 0.0f
                        : renderBackingSample(note.instrument, note.midiNote, noteSamplePos, sr);
                    if (note.instrument != 4 && backingSamplesLoaded[static_cast<size_t>(note.instrument)].load(std::memory_order_relaxed))
                    {
                        sample = sampled;
                        // Natural sample tail for one-shot drums and bass;
                        // piano uses its SFZ loop while the MIDI note is held.
                        if (note.instrument != 3 && noteSamplePos > length * 0.82)
                            sample *= static_cast<float>(juce::jlimit(0.0, 1.0, (length-noteSamplePos) / juce::jmax(1.0, length*0.18)));
                    }
                    else if (note.instrument == 1)
                    {
                        const double env = std::min(1.0, t * 80.0) * std::exp(-t * 1.8);
                        sample = static_cast<float>((std::sin(juce::MathConstants<double>::twoPi * frequency * t) * 0.82
                            + std::sin(juce::MathConstants<double>::twoPi * frequency * 2.0 * t) * 0.18) * env * 0.30);
                    }
                    else if (note.instrument == 3)
                    {
                        const double env = std::exp(-t * 3.8) * std::min(1.0, t * 100.0);
                        sample = static_cast<float>((std::sin(juce::MathConstants<double>::twoPi * frequency * t) * 0.68
                            + std::sin(juce::MathConstants<double>::twoPi * frequency * 2.76 * t) * 0.22
                            + std::sin(juce::MathConstants<double>::twoPi * frequency * 4.1 * t) * 0.10) * env * 0.20);
                    }
                    else if (note.instrument == 2)
                    {
                        // Do not replace the requested acoustic WAV kit with a synthesized drum.
                        sample = 0.0f;
                    }
                    sample *= volume * velocity;
                    for (int channel = 0; channel < channels; ++channel)
                        bufferToFill.buffer->addSample(channel, bufferToFill.startSample + i, sample);
                }
            }
        }
    }

    //==========================================================================
// Play metronome
//
// Play the loaded WAV sample continuously across the audio block.
// The playback position is advanced for every output sample.

    if (metronomeSamplesRemaining > 0)
    {
        const auto& buffer =
            metronomePlayingAccent
            ? metronomeAccentBuffer
            : metronomeNormalBuffer;

        const float mixerVolume = metronomeMixerEnabled.load(std::memory_order_relaxed)
            ? metronomeMixerVolume.load(std::memory_order_relaxed)
            : 0.0f;
        const float volume = mixerVolume * (metronomePlayingAccent ? 0.30f : 0.18f);

        const int availableSamples =
            buffer.getNumSamples()
            - metronomePlaybackPosition;

        const int outputOffset =
            juce::jlimit(
                0,
                bufferToFill.numSamples,
                metronomeBlockOffset);

        const int samplesToPlay =
            std::min(
                metronomeSamplesRemaining,
                std::min(
                    availableSamples,
                    bufferToFill.numSamples - outputOffset));

        if (samplesToPlay > 0)
        {
            for (int i = 0;
                i < samplesToPlay;
                ++i)
            {
                const float metronomeSample =
                    volume
                    * buffer.getSample(
                        0,
                        metronomePlaybackPosition + i);

                for (int channel = 0;
                    channel < outputChannelCount;
                    ++channel)
                {
                    bufferToFill.buffer->addSample(
                        channel,
                        bufferToFill.startSample + outputOffset + i,
                        metronomeSample);
                }
            }

            metronomePlaybackPosition +=
                samplesToPlay;

            metronomeSamplesRemaining -=
                samplesToPlay;
        }

        // Safety: never leave the playback position outside the WAV.
        if (metronomePlaybackPosition >=
            buffer.getNumSamples())
        {
            metronomePlaybackPosition = 0;
            metronomeSamplesRemaining = 0;
        }
    }
}







void MainComponent::releaseResources()
{
}

//==============================================================================
// Paint
//==============================================================================

void MainComponent::paint(juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.fillAll(UiColors::background);

    auto drawGlow = [&g, &area](float centreX, float centreY, float radius,
                                juce::Colour colour)
    {
        juce::ColourGradient glow(colour, centreX, centreY,
                                  colour.withAlpha(0.0f),
                                  centreX, centreY + radius, true);
        g.setGradientFill(glow);
        g.fillRect(area);
    };

    drawGlow(area.getWidth() * 0.10f, -area.getHeight() * 0.08f,
             area.getHeight() * 0.78f, UiColors::accent.withAlpha(0.115f));
    drawGlow(area.getWidth() * 0.94f, area.getHeight() * 0.03f,
             area.getHeight() * 0.62f, UiColors::blue.withAlpha(0.065f));
    drawGlow(area.getWidth() * 0.50f, area.getHeight() * 1.08f,
             area.getHeight() * 0.48f, UiColors::green.withAlpha(0.035f));

    juce::ColourGradient vignette(
        juce::Colours::transparentBlack, 0.0f, area.getHeight() * 0.44f,
        UiColors::bgSoft.withAlpha(0.82f), 0.0f, area.getHeight(), false);
    g.setGradientFill(vignette);
    g.fillRect(area);
}

void MainOverlayComponent::paint(juce::Graphics& g)
{
    owner.paintOverlay(g);
}

void MainComponent::paintOverlay(juce::Graphics& g)
{
    const float width = static_cast<float>(getWidth());
    const float height = static_cast<float>(getHeight());

    if (width <= 0.0f || height <= 0.0f)
        return;

    auto drawBrand = [&g](float x, float y, bool withBadge)
    {
        g.setColour(UiColors::accent);
        g.fillRoundedRectangle(x, y, 32.0f, 32.0f, 10.0f);
        g.setColour(UiColors::onAccent);
        g.setFont(GameUi::heading(14.0f));
        g.drawText("FF", juce::Rectangle<float>(x, y, 32.0f, 32.0f),
                   juce::Justification::centred, false);

        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(16.0f));
        g.drawText("FRETFIGHT",
                   juce::Rectangle<float>(x + 44.0f, y + 1.0f, 190.0f, 30.0f),
                   juce::Justification::centredLeft, false);

        if (withBadge)
            GameUi::drawPill(g,
                             juce::Rectangle<float>(x + 184.0f, y + 7.0f, 82.0f, 18.0f),
                             "PLAYER",
                             UiColors::accent,
                             false);
    };

    //==========================================================================
    // Authentication / landing screen
    //==========================================================================

    if (gameState == GameState::SongSelect && showAuthScreen)
    {
        drawBrand(32.0f, 26.0f, true);

        GameUi::drawPill(g,
                         juce::Rectangle<float>(width - 194.0f, 31.0f, 162.0f, 22.0f),
                         "DESKTOP CLIENT",
                         UiColors::muted,
                         false);

        const float cardWidth = juce::jmin(560.0f, width - 48.0f);
        const float cardHeight = 400.0f;
        const float cardX = (width - cardWidth) * 0.5f;
        const float cardY = juce::jmax(150.0f, (height - cardHeight) * 0.5f);
        const juce::Rectangle<float> card(cardX, cardY, cardWidth, cardHeight);

        GameUi::drawCard(g, card, 24.0f, true);
        g.setColour(UiColors::accent);
        g.fillRoundedRectangle(cardX + 24.0f, cardY, 78.0f, 3.0f, 1.5f);

        const float iconSize = 58.0f;
        const juce::Rectangle<float> icon(cardX + (cardWidth - iconSize) * 0.5f,
                                          cardY + 34.0f, iconSize, iconSize);
        g.setColour(UiColors::accent.withAlpha(0.08f));
        g.fillEllipse(icon.expanded(8.0f));
        g.setColour(UiColors::surface2);
        g.fillEllipse(icon);
        g.setColour(UiColors::accent.withAlpha(0.55f));
        g.drawEllipse(icon, 1.2f);
        g.setColour(UiColors::accent);
        g.setFont(GameUi::heading(20.0f));
        g.drawText("FF", icon, juce::Justification::centred, false);

        g.setColour(UiColors::accent);
        g.setFont(GameUi::eyebrow(10.0f));
        g.drawText("PLAYER ACCESS",
                   juce::Rectangle<float>(cardX, cardY + 101.0f, cardWidth, 17.0f),
                   juce::Justification::centred, false);

        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(13.0f));
        g.drawText("Sign in to sync your scores, unlock online songs and track your global rating.",
                   juce::Rectangle<float>(cardX + 44.0f, cardY + 168.0f,
                                          cardWidth - 88.0f, 36.0f),
                   juce::Justification::centred, true);

        g.setColour(UiColors::surface2.withAlpha(0.55f));
        g.fillRoundedRectangle(cardX + 42.0f, cardY + 202.0f,
                               cardWidth - 84.0f, 40.0f, 12.0f);
        g.setColour(UiColors::border);
        g.drawRoundedRectangle(cardX + 42.0f, cardY + 202.0f,
                               cardWidth - 84.0f, 40.0f, 12.0f, 1.0f);

        GameUi::drawDivider(g, cardX + 42.0f, cardX + cardWidth - 42.0f,
                            cardY + 258.0f);

        const float pillY = cardY + cardHeight + 20.0f;
        if (pillY + 26.0f < height - 20.0f)
        {
            const float total = 160.0f + 132.0f + 152.0f + 20.0f;
            float x = (width - total) * 0.5f;
            GameUi::drawPill(g, { x, pillY, 160.0f, 26.0f },
                             "REAL-TIME PITCH", UiColors::green, false);
            x += 170.0f;
            GameUi::drawPill(g, { x, pillY, 132.0f, 26.0f },
                             "TABLATURE", UiColors::accent, false);
            x += 142.0f;
            GameUi::drawPill(g, { x, pillY, 152.0f, 26.0f },
                             "GLOBAL RATING", UiColors::blue, false);
        }

        return;
    }

    //==========================================================================
    // Song select
    //==========================================================================

    if (gameState == GameState::SongSelect)
    {
        const juce::Rectangle<float> nav(24.0f, 20.0f, width - 48.0f, 78.0f);
        GameUi::drawCard(g, nav, 18.0f);
        drawBrand(44.0f, 40.0f, true);

        if (currentUsername.isNotEmpty())
        {
            const float avatarSize = 30.0f;
            const juce::Rectangle<float> avatar(width - 404.0f, 42.0f,
                                                avatarSize, avatarSize);
            g.setColour(UiColors::accent.withAlpha(0.16f));
            g.fillEllipse(avatar);
            g.setColour(UiColors::accent.withAlpha(0.55f));
            g.drawEllipse(avatar, 1.0f);
            g.setColour(UiColors::accent);
            g.setFont(GameUi::heading(13.0f));
            g.drawText(currentUsername.substring(0, 1).toUpperCase(),
                       avatar, juce::Justification::centred, false);

            g.setColour(UiColors::muted);
            g.setFont(GameUi::eyebrow(8.0f));
            g.drawText("SIGNED IN AS",
                       juce::Rectangle<float>(width - 362.0f, 35.0f, 160.0f, 13.0f),
                       juce::Justification::centredLeft, false);
        }

        const float contentY = 228.0f;
        const float availableH = juce::jmax(320.0f, height - 68.0f - 20.0f - contentY);
        const float contentH = juce::jmin(560.0f, availableH);
        const float footerY = contentY + contentH + 20.0f;
        const float contentW = width - 96.0f;
        const float gap = 20.0f;
        const float leftW = contentW * 0.56f;
        const float rightX = 48.0f + leftW + gap;
        const float rightW = contentW - leftW - gap;

        GameUi::drawEyebrow(g, "SONG LIBRARY",
                            juce::Rectangle<float>(48.0f, 121.0f, 360.0f, 16.0f));
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(13.0f));
        g.drawText("Pick a track, tune up and make every note count.",
                   juce::Rectangle<float>(48.0f, 188.0f, width - 96.0f, 24.0f),
                   juce::Justification::centredLeft, false);

        const juce::Rectangle<float> leftCard(48.0f, contentY, leftW, contentH);
        const juce::Rectangle<float> rightCard(rightX, contentY, rightW, contentH);
        GameUi::drawCard(g, leftCard, 20.0f, true);
        GameUi::drawCard(g, rightCard, 20.0f);

        // Album artwork placeholder, made from the same brand mark as the nav.
        const juce::Rectangle<float> art(leftCard.getX() + 24.0f,
                                         leftCard.getY() + 24.0f,
                                         118.0f, 118.0f);
        juce::ColourGradient artGradient(
            UiColors::accentStrong, art.getX(), art.getY(),
            UiColors::accent.darker(0.45f), art.getRight(), art.getBottom(),
            false);
        g.setGradientFill(artGradient);
        g.fillRoundedRectangle(art, 18.0f);
        g.setColour(UiColors::onAccent.withAlpha(0.12f));
        g.drawRoundedRectangle(art.reduced(1.0f), 18.0f, 1.0f);
        g.setColour(UiColors::onAccent);
        g.setFont(GameUi::heading(40.0f));
        g.drawText("FF", art, juce::Justification::centred, false);

        // The header shows whichever catalogue row is highlighted, so the card
        // always previews the song START SONG will load.
        const CatalogSong* headerSong = juce::isPositiveAndBelow(
            selectedCatalogIndex, static_cast<int>(catalogSongs.size()))
            ? &catalogSongs[static_cast<size_t>(selectedCatalogIndex)]
            : nullptr;

        const juce::String headerTitle = headerSong != nullptr
            ? headerSong->title
            : juce::String("NO SONG SELECTED");
        const juce::String headerKind = headerSong != nullptr
            ? juce::String("ONLINE SONG")
            : juce::String("SERVER CATALOG");
        const double headerBpm = headerSong != nullptr ? headerSong->bpm : baseBpm;

        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(8.5f, true, 0.9f));
        g.drawText(headerKind + "  /  " + juce::String(juce::roundToInt(headerBpm)) + " BPM",
                   juce::Rectangle<float>(art.getRight() + 20.0f,
                                          leftCard.getY() + 28.0f,
                                          leftW - 170.0f, 16.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(26.0f));
        g.drawText(headerTitle,
                   juce::Rectangle<float>(art.getRight() + 20.0f,
                                          leftCard.getY() + 44.0f,
                                          leftW - 170.0f, 34.0f),
                   juce::Justification::centredLeft, true);

        GameUi::drawDivider(g, leftCard.getX() + 24.0f,
                            leftCard.getRight() - 24.0f,
                            leftCard.getY() + 164.0f);

        // Catalogue heading: the ListBox below paints the rows themselves.
        GameUi::drawEyebrow(g, "SONG CATALOG",
                            juce::Rectangle<float>(leftCard.getX() + 24.0f,
                                                   leftCard.getY() + 170.0f,
                                                   leftW - 48.0f, 14.0f));
        g.setColour(UiColors::muted.withAlpha(0.8f));
        g.setFont(GameUi::font(9.0f, true));
        g.drawText(juce::String(static_cast<int>(catalogSongs.size())) + " TRACKS",
                   juce::Rectangle<float>(leftCard.getX() + 24.0f,
                                          leftCard.getY() + 170.0f,
                                          leftW - 48.0f, 14.0f),
                   juce::Justification::centredRight, false);

        g.setColour(UiColors::accent.withAlpha(0.75f));
        g.setFont(GameUi::eyebrow(9.0f));
        g.drawText("GLOBAL LEADERBOARD",
                   juce::Rectangle<float>(rightCard.getX() + 24.0f,
                                          rightCard.getY() + 24.0f,
                                          rightW - 48.0f, 16.0f),
                   juce::Justification::centredLeft, false);
        GameUi::drawDivider(g, rightCard.getX() + 24.0f,
                            rightCard.getRight() - 24.0f,
                            rightCard.getY() + 104.0f);

        if (leaderboardLabel.getText().isEmpty())
        {
            const float centreX = rightCard.getCentreX();
            const float iconY = rightCard.getCentreY() - 34.0f;
            g.setColour(UiColors::accent.withAlpha(0.08f));
            g.fillEllipse(centreX - 44.0f, iconY - 44.0f, 88.0f, 88.0f);
            g.setColour(UiColors::accent.withAlpha(0.32f));
            g.drawEllipse(centreX - 44.0f, iconY - 44.0f, 88.0f, 88.0f, 1.0f);

            g.setColour(UiColors::accent.withAlpha(0.82f));
            g.fillRoundedRectangle(centreX - 24.0f, iconY + 4.0f, 13.0f, 24.0f, 3.0f);
            g.fillRoundedRectangle(centreX - 7.0f, iconY - 8.0f, 13.0f, 36.0f, 3.0f);
            g.fillRoundedRectangle(centreX + 10.0f, iconY - 20.0f, 13.0f, 48.0f, 3.0f);

            g.setColour(UiColors::text);
            g.setFont(GameUi::font(13.0f, true));
            g.drawText("NO SCORES YET",
                       juce::Rectangle<float>(centreX - 160.0f, iconY + 62.0f,
                                              320.0f, 20.0f),
                       juce::Justification::centred, false);
            g.setColour(UiColors::muted);
            g.setFont(GameUi::font(11.0f));
            g.drawText("Press REFRESH to load the global rating.",
                       juce::Rectangle<float>(centreX - 180.0f, iconY + 84.0f,
                                              360.0f, 18.0f),
                       juce::Justification::centred, false);
        }

        // Footer status strip.
        const juce::Rectangle<float> footer(24.0f, footerY, width - 48.0f, 44.0f);
        g.setColour(UiColors::surface.withAlpha(0.62f));
        g.fillRoundedRectangle(footer, 14.0f);
        g.setColour(UiColors::border);
        g.drawRoundedRectangle(footer, 14.0f, 1.0f);
        g.setColour(currentUsername.isNotEmpty() ? UiColors::green : UiColors::accent);
        g.fillEllipse(footer.getX() + 18.0f, footer.getY() + 18.0f, 7.0f, 7.0f);

        return;
    }

    //==========================================================================
    // Tuner
    //==========================================================================

    if (gameState == GameState::Tuner)
    {
        drawBrand(42.0f, 24.0f, true);
        const int tunedMaskTop = tuner.getTunedMask();
        int tunedCountTop = 0;
        for (int i = 0; i < 6; ++i)
            tunedCountTop += (tunedMaskTop & (1 << i)) != 0 ? 1 : 0;
        const bool allTunedTop = tunedCountTop == 6;

        GameUi::drawPill(g,
                         juce::Rectangle<float>(width - 178.0f, 29.0f, 136.0f, 22.0f),
                         allTunedTop ? juce::String("READY")
                                     : "TUNING " + juce::String(tunedCountTop) + "/6",
                         allTunedTop ? UiColors::green : UiColors::accent,
                         allTunedTop);

        GameUi::drawEyebrow(g, "PRE-FLIGHT CHECK",
                            juce::Rectangle<float>(42.0f, 82.0f, 360.0f, 16.0f));
        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(34.0f));
        g.drawText("Tune your guitar",
                   juce::Rectangle<float>(42.0f, 100.0f, width - 84.0f, 44.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(13.0f));
        g.drawText("Standard tuning E A D G B E. Play each string and wait for the green light.",
                   juce::Rectangle<float>(42.0f, 148.0f, width - 84.0f, 24.0f),
                   juce::Justification::centredLeft, false);

        const float margin = 42.0f;
        const float gap = 20.0f;
        const float panelY = 190.0f;
        const float panelH = juce::jmin(560.0f,
                                        juce::jmax(380.0f, height - 190.0f - 120.0f));
        const float leftW = (width - 84.0f - gap) * 0.44f;
        const float rightX = margin + leftW + gap;
        const float rightW = width - margin - rightX;
        const juce::Rectangle<float> leftPanel(margin, panelY, leftW, panelH);
        const juce::Rectangle<float> rightPanel(rightX, panelY, rightW, panelH);
        GameUi::drawCard(g, leftPanel, 20.0f);
        GameUi::drawCard(g, rightPanel, 20.0f, true);

        g.setColour(UiColors::accent);
        g.setFont(GameUi::eyebrow(10.0f));
        g.drawText("STRINGS",
                   juce::Rectangle<float>(leftPanel.getX() + 24.0f,
                                          leftPanel.getY() + 24.0f,
                                          leftW - 48.0f, 16.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(11.0f));
        g.drawText("STANDARD TUNING  /  E  A  D  G  B  E",
                   juce::Rectangle<float>(leftPanel.getX() + 24.0f,
                                          leftPanel.getY() + 44.0f,
                                          leftW - 48.0f, 18.0f),
                   juce::Justification::centredLeft, false);

        const char* stringNames[6] = { "E", "A", "D", "G", "B", "E" };
        const char* stringNumbers[6] = { "6", "5", "4", "3", "2", "1" };
        const int tunedMask = tuner.getTunedMask();
        const float tileGap = 12.0f;
        const float tileW = (leftW - 48.0f - tileGap * 2.0f) / 3.0f;
        const float tileH = 112.0f;

        for (int i = 0; i < 6; ++i)
        {
            const int column = i % 3;
            const int row = i / 3;
            const float x = leftPanel.getX() + 24.0f
                + static_cast<float>(column) * (tileW + tileGap);
            const float y = leftPanel.getY() + 82.0f
                + static_cast<float>(row) * (tileH + tileGap);
            const bool tuned = (tunedMask & (1 << i)) != 0;
            const auto colour = tuned ? UiColors::green : UiColors::accent;

            g.setColour(tuned ? UiColors::green.withAlpha(0.10f)
                              : UiColors::surface2.withAlpha(0.72f));
            g.fillRoundedRectangle(x, y, tileW, tileH, 14.0f);
            g.setColour(colour.withAlpha(tuned ? 0.46f : 0.22f));
            g.drawRoundedRectangle(x, y, tileW, tileH, 14.0f, 1.0f);

            g.setColour(colour);
            g.setFont(GameUi::heading(34.0f));
            g.drawText(stringNames[i],
                       juce::Rectangle<float>(x, y + 10.0f, tileW, 40.0f),
                       juce::Justification::centred, false);
            g.setColour(UiColors::muted);
            g.setFont(GameUi::eyebrow(8.5f));
            g.drawText("STRING " + juce::String(stringNumbers[i]),
                       juce::Rectangle<float>(x, y + 52.0f, tileW, 16.0f),
                       juce::Justification::centred, false);
            GameUi::drawPill(g,
                             juce::Rectangle<float>(x + 10.0f, y + 77.0f,
                                                    tileW - 20.0f, 20.0f),
                             tuned ? "IN TUNE" : "WAITING",
                             colour,
                             tuned);
        }

        if (panelH > 470.0f && panelH <= 520.0f)
        {
            g.setColour(UiColors::muted.withAlpha(0.72f));
            g.setFont(GameUi::font(11.0f));
            g.drawText("Play each string clearly and let the note ring.",
                       juce::Rectangle<float>(leftPanel.getX() + 24.0f,
                                              leftPanel.getBottom() - 48.0f,
                                              leftW - 48.0f, 22.0f),
                       juce::Justification::centred, false);
        }

        if (panelH > 470.0f)
        {
            GameUi::drawDivider(g, leftPanel.getX() + 24.0f,
                                leftPanel.getRight() - 24.0f,
                                leftPanel.getY() + 352.0f);
            GameUi::drawEyebrow(g, "REFERENCE PITCH",
                                juce::Rectangle<float>(leftPanel.getX() + 24.0f,
                                                       leftPanel.getY() + 368.0f,
                                                       leftW - 48.0f, 14.0f),
                                UiColors::muted);

            const char* references[6]
            {
                "6   E2   82.4 Hz", "5   A2  110.0 Hz", "4   D3  146.8 Hz",
                "3   G3  196.0 Hz", "2   B3  246.9 Hz", "1   E4  329.6 Hz"
            };
            const float refGap = 10.0f;
            const float refW = (leftW - 48.0f - refGap * 2.0f) / 3.0f;

            for (int i = 0; i < 6; ++i)
            {
                const int column = i % 3;
                const int row = i / 3;
                const float x = leftPanel.getX() + 24.0f
                    + static_cast<float>(column) * (refW + refGap);
                const float y = leftPanel.getY() + 396.0f
                    + static_cast<float>(row) * 24.0f;
                g.setColour(UiColors::accent.withAlpha(0.72f));
                g.fillEllipse(x, y + 5.0f, 4.0f, 4.0f);
                g.setColour(UiColors::muted);
                g.setFont(GameUi::font(10.5f));
                g.drawText(references[i],
                           juce::Rectangle<float>(x + 10.0f, y, refW - 10.0f, 16.0f),
                           juce::Justification::centredLeft, false);
            }
        }

        if (panelH > 520.0f)
        {
            const bool allReady = tuner.areAllStringsTuned();
            int tunedCount = 0;
            for (int i = 0; i < 6; ++i)
                tunedCount += (tunedMask & (1 << i)) != 0 ? 1 : 0;

            const auto readyColour = allReady ? UiColors::green : UiColors::accent;
            const juce::Rectangle<float> readyCard(
                leftPanel.getX() + 24.0f, leftPanel.getY() + 474.0f,
                leftW - 48.0f, 72.0f);
            g.setColour(UiColors::surface2.withAlpha(0.56f));
            g.fillRoundedRectangle(readyCard, 14.0f);
            g.setColour(readyColour.withAlpha(0.42f));
            g.drawRoundedRectangle(readyCard, 14.0f, 1.0f);

            const juce::Rectangle<float> readyIcon(
                readyCard.getX() + 16.0f, readyCard.getCentreY() - 15.0f,
                30.0f, 30.0f);
            g.setColour(readyColour.withAlpha(0.16f));
            g.fillEllipse(readyIcon);
            g.setColour(readyColour);
            g.setFont(GameUi::font(9.5f, true));
            g.drawText(allReady ? juce::String("OK") : juce::String(tunedCount),
                       readyIcon.toNearestInt(), juce::Justification::centred, false);

            g.setColour(UiColors::text);
            g.setFont(GameUi::font(12.5f, true));
            g.drawText(allReady ? juce::String("ALL STRINGS READY")
                                : juce::String(tunedCount) + " / 6 STRINGS READY",
                       juce::Rectangle<float>(readyCard.getX() + 58.0f,
                                              readyCard.getY() + 14.0f,
                                              readyCard.getWidth() - 72.0f, 18.0f),
                       juce::Justification::centredLeft, false);
            g.setColour(UiColors::muted);
            g.setFont(GameUi::font(10.5f));
            g.drawText(allReady ? "All six strings are in tune."
                                : "Keep tuning the remaining strings.",
                       juce::Rectangle<float>(readyCard.getX() + 58.0f,
                                              readyCard.getY() + 38.0f,
                                              readyCard.getWidth() - 72.0f, 16.0f),
                       juce::Justification::centredLeft, false);
        }

        // Large input dial. The note itself is a real Label above this paint.
        const float dialRadius = juce::jmin(88.0f, rightW * 0.21f);
        const float dialX = rightPanel.getCentreX();
        const float dialY = rightPanel.getY() + 196.0f;
        g.setColour(UiColors::accent.withAlpha(0.035f));
        g.fillEllipse(dialX - dialRadius - 16.0f, dialY - dialRadius - 16.0f,
                      (dialRadius + 16.0f) * 2.0f, (dialRadius + 16.0f) * 2.0f);
        g.setColour(UiColors::surface2.withAlpha(0.78f));
        g.fillEllipse(dialX - dialRadius, dialY - dialRadius,
                      dialRadius * 2.0f, dialRadius * 2.0f);
        g.setColour(UiColors::borderStrong);
        g.drawEllipse(dialX - dialRadius, dialY - dialRadius,
                      dialRadius * 2.0f, dialRadius * 2.0f, 1.0f);
        g.setColour(UiColors::accent.withAlpha(0.22f));
        g.drawEllipse(dialX - dialRadius + 12.0f, dialY - dialRadius + 12.0f,
                      (dialRadius - 12.0f) * 2.0f, (dialRadius - 12.0f) * 2.0f, 1.0f);

        GameUi::drawDivider(g, rightPanel.getX() + 24.0f,
                            rightPanel.getRight() - 24.0f,
                            rightPanel.getY() + 352.0f);

        g.setColour(UiColors::muted);
        g.setFont(GameUi::eyebrow(8.5f));
        g.drawText("DETECTED STRING",
                   juce::Rectangle<float>(rightPanel.getX() + 24.0f,
                                          rightPanel.getY() + 286.0f,
                                          220.0f, 15.0f),
                   juce::Justification::centredLeft, false);
        g.drawText("OFFSET",
                   juce::Rectangle<float>(rightPanel.getX() + 24.0f,
                                          rightPanel.getY() + 318.0f,
                                          150.0f, 15.0f),
                   juce::Justification::centredLeft, false);

        const auto statusColour = tuner.areAllStringsTuned()
            ? UiColors::green : UiColors::accent;
        g.setColour(statusColour.withAlpha(0.10f));
        g.fillRoundedRectangle(rightPanel.getX() + 40.0f,
                               rightPanel.getY() + 368.0f,
                               rightW - 80.0f, 40.0f, 20.0f);
        g.setColour(statusColour.withAlpha(0.38f));
        g.drawRoundedRectangle(rightPanel.getX() + 40.0f,
                               rightPanel.getY() + 368.0f,
                               rightW - 80.0f, 40.0f, 20.0f, 1.0f);

        if (panelH > 520.0f)
        {
            GameUi::drawDivider(g, rightPanel.getX() + 24.0f,
                                rightPanel.getRight() - 24.0f,
                                rightPanel.getY() + 432.0f);
            GameUi::drawEyebrow(g, "QUICK TIPS",
                                juce::Rectangle<float>(rightPanel.getX() + 24.0f,
                                                       rightPanel.getY() + 448.0f,
                                                       rightW - 48.0f, 14.0f),
                                UiColors::muted);
            g.setColour(UiColors::muted.withAlpha(0.82f));
            g.setFont(GameUi::font(11.0f));
            g.drawText("Pluck one string at a time.",
                       juce::Rectangle<float>(rightPanel.getX() + 24.0f,
                                              rightPanel.getY() + 468.0f,
                                              rightW - 48.0f, 18.0f),
                       juce::Justification::centredLeft, false);
            g.drawText("Let the note ring before reading.",
                       juce::Rectangle<float>(rightPanel.getX() + 24.0f,
                                              rightPanel.getY() + 488.0f,
                                              rightW - 48.0f, 18.0f),
                       juce::Justification::centredLeft, false);
        }

        if (panelH > 500.0f)
        {
            g.setColour(UiColors::muted.withAlpha(0.65f));
            g.setFont(GameUi::font(10.5f));
            g.drawText("TIP  /  USE THE NECK PICKUP AND MUTE THE OTHER STRINGS",
                       juce::Rectangle<float>(rightPanel.getX() + 24.0f,
                                              rightPanel.getBottom() - 48.0f,
                                              rightW - 48.0f, 22.0f),
                       juce::Justification::centred, false);
        }

        return;
    }

    //==========================================================================
    // Results
    //==========================================================================

    if (gameState == GameState::Finished)
    {
        drawBrand(48.0f, 32.0f, true);
        GameUi::drawPill(g,
                         juce::Rectangle<float>(width - 154.0f, 36.0f, 106.0f, 22.0f),
                         "RESULTS",
                         UiColors::accent,
                         true);
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(10.5f));
        g.drawText("BEST ON THIS TRACK  " + juce::String(getBestScoreForSong(selectedSongSlug, selectedSongId)),
                   juce::Rectangle<float>(width - 460.0f, 36.0f, 278.0f, 22.0f),
                   juce::Justification::centredRight, false);

        const int score = gameplayStats.getScore();
        const int hits = gameplayStats.getHitCount();
        const int misses = gameplayStats.getMissCount();
        const int total = hits + misses;
        const double accuracy = total > 0
            ? 100.0 * static_cast<double>(hits) / static_cast<double>(total)
            : 0.0;

        const juce::Rectangle<float> card(
            48.0f, 82.0f, width - 96.0f,
            juce::jmax(520.0f, height - 170.0f));
        GameUi::drawCard(g, card, 24.0f, true);

        GameUi::drawEyebrow(g, "SESSION COMPLETE",
                            juce::Rectangle<float>(card.getX() + 32.0f,
                                                   card.getY() + 26.0f,
                                                   340.0f, 16.0f));
        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(38.0f));
        g.drawText("SONG COMPLETE",
                   juce::Rectangle<float>(card.getX() + 32.0f,
                                          card.getY() + 46.0f,
                                          card.getWidth() - 64.0f, 48.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(13.0f));
        g.drawText("Here is how your run landed. Replay the track to push the score higher.",
                   juce::Rectangle<float>(card.getX() + 32.0f,
                                          card.getY() + 92.0f,
                                          card.getWidth() - 64.0f, 22.0f),
                   juce::Justification::centredLeft, false);

        const juce::Rectangle<float> scorePanel(
            card.getX() + 32.0f, card.getY() + 132.0f,
            card.getWidth() - 64.0f, 176.0f);
        juce::ColourGradient scoreGradient(
            UiColors::accentStrong, scorePanel.getX(), scorePanel.getY(),
            UiColors::accent.darker(0.46f), scorePanel.getRight(), scorePanel.getBottom(),
            false);
        g.setGradientFill(scoreGradient);
        g.fillRoundedRectangle(scorePanel, 20.0f);
        g.setColour(juce::Colours::white.withAlpha(0.16f));
        g.drawRoundedRectangle(scorePanel, 20.0f, 1.0f);

        g.setColour(UiColors::onAccent.withAlpha(0.72f));
        g.setFont(GameUi::eyebrow(9.5f));
        g.drawText("TOTAL SCORE",
                   juce::Rectangle<float>(scorePanel.getX() + 24.0f,
                                          scorePanel.getY() + 20.0f,
                                          300.0f, 16.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::onAccent);
        g.setFont(GameUi::heading(72.0f));
        g.drawText(juce::String(score),
                   juce::Rectangle<float>(scorePanel.getX() + 24.0f,
                                          scorePanel.getY() + 38.0f,
                                          scorePanel.getWidth() - 48.0f, 92.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::onAccent.withAlpha(0.70f));
        g.setFont(GameUi::font(12.0f));
        g.drawText("POINTS",
                   juce::Rectangle<float>(scorePanel.getX() + 26.0f,
                                          scorePanel.getY() + 134.0f,
                                          180.0f, 20.0f),
                   juce::Justification::centredLeft, false);

        const int columnWidth = juce::jmax(120, (juce::roundToInt(card.getWidth()) - 88) / 3);
        const float gridX = card.getX() + 32.0f;
        const float gridY = card.getY() + 334.0f;
        const std::array<juce::String, 6> values
        {
            juce::String(juce::roundToInt(accuracy)) + "%",
            juce::String(hits),
            juce::String(misses),
            juce::String(gameplayStats.getPerfectCount()),
            juce::String(gameplayStats.getGreatCount()),
            juce::String(gameplayStats.getGoodCount())
        };
        const std::array<juce::String, 6> captions
        {
            "ACCURACY", "HITS", "MISSES", "PERFECT", "GREAT", "GOOD"
        };

        for (int i = 0; i < 6; ++i)
        {
            const int column = i % 3;
            const int row = i / 3;
            const float x = gridX + static_cast<float>(column) * static_cast<float>(columnWidth);
            const float y = gridY + static_cast<float>(row) * 94.0f;
            const juce::Rectangle<float> tile(x, y, static_cast<float>(columnWidth) - 12.0f, 82.0f);
            g.setColour(UiColors::surface2.withAlpha(0.72f));
            g.fillRoundedRectangle(tile, 14.0f);
            g.setColour(UiColors::border);
            g.drawRoundedRectangle(tile, 14.0f, 1.0f);

            auto valueColour = UiColors::text;
            if (i == 2) valueColour = UiColors::red;
            else if (i == 3) valueColour = UiColors::accent;
            else if (i == 4) valueColour = UiColors::green;
            else if (i == 5) valueColour = UiColors::info;

            g.setColour(valueColour);
            g.setFont(GameUi::heading(28.0f));
            g.drawText(values[static_cast<size_t>(i)],
                       juce::Rectangle<float>(tile.getX() + 16.0f,
                                              tile.getY() + 8.0f,
                                              tile.getWidth() - 32.0f, 38.0f),
                       juce::Justification::centredLeft, false);
            g.setColour(UiColors::muted);
            g.setFont(GameUi::eyebrow(8.5f));
            g.drawText(captions[static_cast<size_t>(i)],
                       juce::Rectangle<float>(tile.getX() + 16.0f,
                                              tile.getY() + 50.0f,
                                              tile.getWidth() - 32.0f, 16.0f),
                       juce::Justification::centredLeft, false);
        }

        GameUi::drawPill(g,
                         juce::Rectangle<float>(card.getX() + 32.0f,
                                                card.getY() + 548.0f,
                                                180.0f, 30.0f),
                         "MAX COMBO  x" + juce::String(gameplayStats.getMaxCombo()),
                         UiColors::accent,
                         true);
        GameUi::drawPill(g,
                         juce::Rectangle<float>(card.getX() + 224.0f,
                                                card.getY() + 548.0f,
                                                210.0f, 30.0f),
                         "COMBO BONUS  +" + juce::String(gameplayStats.getComboBonusPoints()),
                         UiColors::green,
                         false);

        if (card.getHeight() > 720.0f)
        {
            const int perfect = gameplayStats.getPerfectCount();
            const int great = gameplayStats.getGreatCount();
            const int good = gameplayStats.getGoodCount();
            const int judged = juce::jmax(1, perfect + great + good + misses);

            GameUi::drawEyebrow(g, "JUDGEMENT BREAKDOWN",
                                juce::Rectangle<float>(card.getX() + 32.0f,
                                                       card.getY() + 596.0f,
                                                       420.0f, 14.0f),
                                UiColors::muted);

            const juce::Rectangle<float> bar(card.getX() + 32.0f,
                                             card.getY() + 620.0f,
                                             card.getWidth() - 64.0f,
                                             14.0f);
            g.setColour(UiColors::surface2);
            g.fillRoundedRectangle(bar, 7.0f);

            {
                juce::Graphics::ScopedSaveState saved(g);
                juce::Path clip;
                clip.addRoundedRectangle(bar, 7.0f);
                g.reduceClipRegion(clip);

                const std::array<int, 4> counts { perfect, great, good, misses };
                const std::array<juce::Colour, 4> colours
                {
                    UiColors::accent, UiColors::green, UiColors::info, UiColors::red
                };
                float segmentX = bar.getX();

                for (size_t i = 0; i < counts.size(); ++i)
                {
                    const float segmentW = bar.getWidth()
                        * static_cast<float>(counts[i]) / static_cast<float>(judged);
                    if (segmentW > 1.5f)
                    {
                        g.setColour(colours[i]);
                        g.fillRect(segmentX, bar.getY(),
                                   juce::jmax(0.0f, segmentW - 2.0f), bar.getHeight());
                    }
                    segmentX += segmentW;
                }
            }

            const std::array<juce::String, 4> legend
            {
                "PERFECT  " + juce::String(perfect),
                "GREAT  " + juce::String(great),
                "GOOD  " + juce::String(good),
                "MISS  " + juce::String(misses)
            };
            const std::array<juce::Colour, 4> legendColours
            {
                UiColors::accent, UiColors::green, UiColors::info, UiColors::red
            };
            const float legendWidth = (card.getWidth() - 64.0f) / 4.0f;

            for (int i = 0; i < 4; ++i)
            {
                const float x = card.getX() + 32.0f
                    + static_cast<float>(i) * legendWidth;
                const float y = card.getY() + 649.0f;
                g.setColour(legendColours[static_cast<size_t>(i)]);
                g.fillEllipse(x, y + 4.0f, 7.0f, 7.0f);
                g.setColour(UiColors::muted);
                g.setFont(GameUi::font(10.5f));
                g.drawText(legend[static_cast<size_t>(i)],
                           juce::Rectangle<float>(x + 13.0f, y,
                                                   legendWidth - 20.0f, 16.0f),
                           juce::Justification::centredLeft, false);
            }
        }

        return;
    }

    //==========================================================================
    // Gameplay
    //==========================================================================

    const float topY = 20.0f;
    const float topH = 88.0f;
    const float hudY = 120.0f;
    const float hudH = 96.0f;
    const float bottomH = juce::jlimit(124.0f, 160.0f, height / 6.0f);
    const float bottomY = height - 24.0f - bottomH;
    const float studioH = juce::jlimit(176.0f, 218.0f, height / 4.0f);
    const float studioY = bottomY - 14.0f - studioH;
    const float tabY = hudY + hudH + 12.0f;
    const float tabH = juce::jmax(150.0f, studioY - 12.0f - tabY);

    const juce::Rectangle<float> topBar(24.0f, topY, width - 48.0f, topH);
    GameUi::drawCard(g, topBar, 18.0f);
    drawBrand(44.0f, 32.0f, false);

    g.setColour(UiColors::muted);
    g.setFont(GameUi::eyebrow(8.0f));
    g.drawText("NOW PLAYING",
               juce::Rectangle<float>(210.0f, 31.0f, 260.0f, 12.0f),
               juce::Justification::centredLeft, false);

    // Tempo pills: BEAT / BAR / BPM.
    auto drawTempoPill = [&g](juce::Rectangle<float> area,
                              const juce::String& caption)
    {
        g.setColour(UiColors::surface2.withAlpha(0.72f));
        g.fillRoundedRectangle(area, 9.0f);
        g.setColour(UiColors::border);
        g.drawRoundedRectangle(area, 9.0f, 1.0f);
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(8.0f, true));
        g.drawText(caption,
                   juce::Rectangle<float>(area.getX() + 9.0f, area.getY() + 3.0f,
                                          36.0f, 14.0f),
                   juce::Justification::centredLeft, false);
    };

    drawTempoPill({ 44.0f, 82.0f, 80.0f, 24.0f }, "BEAT");
    drawTempoPill({ 132.0f, 82.0f, 72.0f, 24.0f }, "BAR");
    drawTempoPill({ 212.0f, 82.0f, 88.0f, 24.0f }, "BPM");

    constexpr float hudGap = 12.0f;
    const float comboW = 124.0f;
    const float pitchW = 214.0f;
    const float targetW = 190.0f;

    const juce::Rectangle<float> comboCard(24.0f, hudY, comboW, hudH);
    const juce::Rectangle<float> pitchCard(comboCard.getRight() + hudGap, hudY,
                                           pitchW, hudH);
    const juce::Rectangle<float> targetCard(width - 24.0f - targetW, hudY,
                                            targetW, hudH);
    const float statsX = pitchCard.getRight() + hudGap;
    const float statsW = juce::jmax(180.0f, targetCard.getX() - hudGap - statsX);
    const juce::Rectangle<float> statsCard(statsX, hudY, statsW, hudH);

    GameUi::drawCard(g, comboCard, 16.0f);
    GameUi::drawCard(g, pitchCard, 16.0f);
    GameUi::drawCard(g, statsCard, 16.0f);
    GameUi::drawCard(g, targetCard, 16.0f);

    GameUi::drawEyebrow(g, "COMBO",
                        juce::Rectangle<float>(comboCard.getX() + 14.0f,
                                               comboCard.getY() + 14.0f,
                                               comboW - 28.0f, 15.0f));

    GameUi::drawEyebrow(g, "PITCH",
                        juce::Rectangle<float>(pitchCard.getX() + 16.0f,
                                               pitchCard.getY() + 14.0f,
                                               pitchW - 32.0f, 15.0f),
                        UiColors::green);

    g.setColour(UiColors::accent);
    g.setFont(GameUi::eyebrow(9.0f));
    g.drawText("NEXT NOTE",
               juce::Rectangle<float>(targetCard.getX() + 18.0f,
                                      targetCard.getY() + 14.0f,
                                      targetW - 30.0f, 15.0f),
               juce::Justification::centredLeft, false);

    GameUi::drawEyebrow(g, "LIVE PERFORMANCE",
                        juce::Rectangle<float>(statsCard.getX() + 18.0f,
                                               statsCard.getY() + 9.0f,
                                               statsCard.getWidth() - 36.0f, 14.0f),
                        UiColors::muted);

    {
        const float cellWidth = statsCard.getWidth() / 4.0f;
        const std::array<juce::String, 4> values
        {
            juce::String(gameplayStats.getMissCount()),
            juce::String(gameplayStats.getPerfectCount()),
            juce::String(gameplayStats.getGreatCount()),
            juce::String(gameplayStats.getGoodCount())
        };
        const std::array<juce::String, 4> captions { "MISS", "PERFECT", "GREAT", "GOOD" };
        const std::array<juce::Colour, 4> colours
        {
            UiColors::red, UiColors::accent, UiColors::green, UiColors::info
        };

        for (int i = 0; i < 4; ++i)
        {
            if (i > 0)
            {
                g.setColour(UiColors::border);
                g.fillRect(statsCard.getX() + cellWidth * static_cast<float>(i),
                           statsCard.getY() + 28.0f, 1.0f, 52.0f);
            }
            g.setColour(colours[static_cast<size_t>(i)]);
            g.setFont(GameUi::heading(24.0f));
            g.drawText(values[static_cast<size_t>(i)],
                       juce::Rectangle<float>(statsCard.getX() + cellWidth * static_cast<float>(i),
                                              statsCard.getY() + 28.0f,
                                              cellWidth, 34.0f),
                       juce::Justification::centred, false);
            g.setColour(UiColors::muted);
            g.setFont(GameUi::eyebrow(8.0f));
            g.drawText(captions[static_cast<size_t>(i)],
                       juce::Rectangle<float>(statsCard.getX() + cellWidth * static_cast<float>(i),
                                              statsCard.getY() + 65.0f,
                                              cellWidth, 14.0f),
                       juce::Justification::centred, false);
        }
    }

    // Tablature sits on its own opaque surface; the shadow below it is painted here.
    g.setColour(juce::Colours::black.withAlpha(0.25f));
    g.fillRoundedRectangle(24.0f, tabY + 3.0f, width - 48.0f, tabH, 16.0f);

    // Guitar-effect pedals are intentionally not shown yet; the backing-track
    // mixer uses the full width until the effects UI is implemented.
    const juce::Rectangle<float> mixer(24.0f, studioY,
                                       width - 48.0f, studioH);

    GameUi::drawCard(g, mixer, 18.0f);
    GameUi::drawEyebrow(g, "MIXER",
                        juce::Rectangle<float>(mixer.getX() + 22.0f,
                                               mixer.getY() + 16.0f,
                                               180.0f, 14.0f));
    g.setColour(UiColors::text);
    g.setFont(GameUi::heading(17.0f));
    g.drawText("BACKING TRACK",
               juce::Rectangle<float>(mixer.getX() + 22.0f,
                                      mixer.getY() + 31.0f,
                                      mixer.getWidth() - 190.0f, 22.0f),
               juce::Justification::centredLeft, false);
    GameUi::drawPill(g,
                     juce::Rectangle<float>(mixer.getRight() - 122.0f,
                                            mixer.getY() + 18.0f,
                                            100.0f, 20.0f),
                     "6 CHANNELS",
                     UiColors::green,
                     false);
    GameUi::drawDivider(g, mixer.getX() + 22.0f,
                        mixer.getRight() - 22.0f,
                        mixer.getY() + 58.0f);

    {
        const float cellWidth = (mixer.getWidth() - 28.0f - 24.0f) / 3.0f;
        for (int i = 1; i < 3; ++i)
        {
            const float dividerX = mixer.getX() + 14.0f
                + static_cast<float>(i) * (cellWidth + 12.0f) - 6.0f;
            g.setColour(UiColors::border);
            g.fillRect(dividerX, mixer.getY() + 68.0f, 1.0f,
                       mixer.getHeight() - 78.0f);
        }
    }

    const juce::Rectangle<float> bottomCard(24.0f, bottomY, width - 48.0f, bottomH);
    GameUi::drawCard(g, bottomCard, 18.0f, true);

    const float judgementW = bottomCard.getWidth() * 0.40f;
    const float accuracyX = bottomCard.getX() + judgementW;
    const float scoreX = bottomCard.getX() + bottomCard.getWidth() * 0.66f;
    GameUi::drawEyebrow(g, "JUDGEMENT",
                        juce::Rectangle<float>(bottomCard.getX() + 24.0f,
                                               bottomCard.getY() + 15.0f,
                                               judgementW - 48.0f, 14.0f));
    GameUi::drawEyebrow(g, "ACCURACY",
                        juce::Rectangle<float>(accuracyX + 22.0f,
                                               bottomCard.getY() + 15.0f,
                                               160.0f, 14.0f),
                        UiColors::muted);
    GameUi::drawEyebrow(g, "SCORE",
                        juce::Rectangle<float>(scoreX + 22.0f,
                                               bottomCard.getY() + 15.0f,
                                               120.0f, 14.0f));

    const int hits = gameplayStats.getHitCount();
    const int misses = gameplayStats.getMissCount();
    const int total = hits + misses;
    const double accuracy = total > 0
        ? 100.0 * static_cast<double>(hits) / static_cast<double>(total)
        : 0.0;
    g.setColour(UiColors::text);
    g.setFont(GameUi::heading(30.0f));
    g.drawText(juce::String(juce::roundToInt(accuracy)) + "%",
               juce::Rectangle<float>(accuracyX + 22.0f,
                                      bottomCard.getY() + 34.0f,
                                      160.0f, 38.0f),
               juce::Justification::centredLeft, false);
    g.setColour(UiColors::muted);
    g.setFont(GameUi::font(9.5f));
    g.drawText(juce::String(hits) + " / " + juce::String(total) + " NOTES",
               juce::Rectangle<float>(accuracyX + 22.0f,
                                      bottomCard.getY() + bottomH - 34.0f,
                                      180.0f, 16.0f),
               juce::Justification::centredLeft, false);

    g.setColour(UiColors::border);
    g.fillRect(accuracyX, bottomCard.getY() + 20.0f, 1.0f, bottomH - 40.0f);
    g.fillRect(scoreX, bottomCard.getY() + 20.0f, 1.0f, bottomH - 40.0f);

    g.setColour(UiColors::muted.withAlpha(0.72f));
    g.setFont(GameUi::font(9.0f, true));
    g.drawText("POINTS",
               juce::Rectangle<float>(scoreX + 22.0f,
                                      bottomCard.getY() + bottomH - 34.0f,
                                      120.0f, 16.0f),
               juce::Justification::centredRight, false);

}

//==============================================================================
// Modal overlay (pause and count-in above every control)
//==============================================================================

void MainModalOverlayComponent::paint(juce::Graphics& g)
{
    owner.paintModalOverlay(g);
}

void MainComponent::paintModalOverlay(juce::Graphics& g)
{
    const float width = static_cast<float>(getWidth());
    const float height = static_cast<float>(getHeight());

    if (width <= 0.0f || height <= 0.0f)
        return;

    // Count-in card.
    if (gameState == GameState::Playing
        && countInActive.load(std::memory_order_acquire))
    {
        const float cardWidth = 280.0f;
        const float cardHeight = 220.0f;
        const juce::Rectangle<float> countCard(
            (width - cardWidth) * 0.5f,
            (height - cardHeight) * 0.5f,
            cardWidth, cardHeight);

        g.setColour(UiColors::background.withAlpha(0.32f));
        g.fillAll();

        GameUi::drawCard(g, countCard, 24.0f, true);
        g.setColour(UiColors::accent);
        g.fillRoundedRectangle(countCard.getX() + 24.0f, countCard.getY(),
                               78.0f, 3.0f, 1.5f);
        g.setColour(UiColors::accent);
        g.setFont(GameUi::eyebrow(10.0f));
        g.drawText("GET READY",
                   juce::Rectangle<float>(countCard.getX(), countCard.getY() + 28.0f,
                                          cardWidth, 18.0f),
                   juce::Justification::centred, false);

        const int count = juce::jmax(1, countInRemaining.load(std::memory_order_relaxed));
        g.setColour(count <= 1 ? UiColors::accentStrong : UiColors::accent);
        g.setFont(GameUi::heading(92.0f));
        g.drawText(juce::String(count),
                   juce::Rectangle<float>(countCard.getX(),
                                          countCard.getY() + 48.0f,
                                          cardWidth, 110.0f),
                   juce::Justification::centred, false);

        for (int i = 0; i < 4; ++i)
        {
            const float dotX = countCard.getCentreX() - 34.0f + static_cast<float>(i) * 22.0f;
            g.setColour(i < count ? UiColors::accent : UiColors::borderStrong);
            g.fillEllipse(dotX, countCard.getBottom() - 42.0f, 6.0f, 6.0f);
        }
    }

    // Pause overlay.
    if (gameState == GameState::Paused)
    {
        g.setColour(UiColors::background.withAlpha(0.76f));
        g.fillAll();

        const juce::Rectangle<float> pauseCard(
            width * 0.5f - 250.0f,
            height * 0.5f - 170.0f,
            500.0f, 340.0f);
        GameUi::drawCard(g, pauseCard, 24.0f, true);
        g.setColour(UiColors::accent);
        g.fillRoundedRectangle(pauseCard.getX() + 28.0f, pauseCard.getY(),
                               84.0f, 3.0f, 1.5f);
        GameUi::drawEyebrow(g, "SESSION PAUSED",
                            juce::Rectangle<float>(pauseCard.getX() + 28.0f,
                                                   pauseCard.getY() + 32.0f,
                                                   300.0f, 16.0f));
        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(32.0f));
        g.drawText("Take a breath.",
                   juce::Rectangle<float>(pauseCard.getX() + 28.0f,
                                          pauseCard.getY() + 52.0f,
                                          pauseCard.getWidth() - 56.0f, 44.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(13.0f));
        g.drawText("Your timing and score are frozen exactly where you left them.",
                   juce::Rectangle<float>(pauseCard.getX() + 28.0f,
                                          pauseCard.getY() + 102.0f,
                                          pauseCard.getWidth() - 56.0f, 24.0f),
                   juce::Justification::centredLeft, false);

        GameUi::drawDivider(g, pauseCard.getX() + 28.0f,
                            pauseCard.getRight() - 28.0f,
                            pauseCard.getY() + 142.0f);

        GameUi::drawEyebrow(g, "SCORE",
                            juce::Rectangle<float>(pauseCard.getX() + 28.0f,
                                                   pauseCard.getY() + 164.0f,
                                                   160.0f, 14.0f),
                            UiColors::muted);
        g.setColour(UiColors::accent);
        g.setFont(GameUi::heading(38.0f));
        g.drawText(juce::String(gameplayStats.getScore()),
                   juce::Rectangle<float>(pauseCard.getX() + 28.0f,
                                          pauseCard.getY() + 182.0f,
                                          240.0f, 46.0f),
                   juce::Justification::centredLeft, false);
        GameUi::drawEyebrow(g, "COMBO",
                            juce::Rectangle<float>(pauseCard.getX() + 280.0f,
                                                   pauseCard.getY() + 164.0f,
                                                   160.0f, 14.0f),
                            UiColors::muted);
        g.setColour(UiColors::green);
        g.setFont(GameUi::heading(38.0f));
        g.drawText("x" + juce::String(gameplayStats.getCombo()),
                   juce::Rectangle<float>(pauseCard.getX() + 280.0f,
                                          pauseCard.getY() + 182.0f,
                                          180.0f, 46.0f),
                   juce::Justification::centredLeft, false);

        GameUi::drawPill(g,
                         juce::Rectangle<float>(pauseCard.getX() + 28.0f,
                                                pauseCard.getBottom() - 56.0f,
                                                pauseCard.getWidth() - 56.0f, 30.0f),
                         "PRESS RESUME IN THE TOP BAR TO CONTINUE",
                         UiColors::accent,
                         true);
    }
}

//==============================================================================
// Layout
//==============================================================================

void MainComponent::resized()
{
    overlayComponent.setBounds(getLocalBounds());
    modalOverlayComponent.setBounds(getLocalBounds());

    const int width = getWidth();
    const int height = getHeight();

    const bool authScreen = showAuthScreen;
    const bool songSelect = gameState == GameState::SongSelect && !authScreen;
    const bool tunerScreen = gameState == GameState::Tuner;
    const bool finishedScreen = gameState == GameState::Finished;
    const bool showGame = gameState == GameState::Playing
        || gameState == GameState::Paused;

    //==========================================================================
    // Visibility
    //==========================================================================

    songTitleLabel.setVisible(songSelect);
    loginScreenTitleLabel.setVisible(authScreen);
    currentUserLabel.setVisible(songSelect && currentUsername.isNotEmpty());
    logoutButton.setVisible(songSelect && currentUsername.isNotEmpty());
    gameSongTitleLabel.setVisible(showGame);

    songPridumayButton.setVisible(songSelect);
    onlineCatalogButton.setVisible(songSelect);
    onlineCatalogStatusLabel.setVisible(songSelect);
    leaderboardTitleLabel.setVisible(songSelect);
    leaderboardLabel.setVisible(songSelect);
    refreshLeaderboardButton.setVisible(songSelect);
    accountStatusLabel.setVisible(songSelect || authScreen);
    accountLoginButton.setVisible(authScreen);
    accountRegisterButton.setVisible(authScreen);
    verifyEmailButton.setVisible(authScreen);
    forgotPasswordButton.setVisible(authScreen);
    songMistButton.setVisible(false);
    songChordsButton.setVisible(false);
    aiSoloButton.setVisible(false);
    oldGeneratorArchiveButton.setVisible(false);
    melodyStyleCombo.setVisible(false);
    generateMelodyButton.setVisible(false);
    newGenerationButton.setVisible(false);
    songPridumayBestLabel.setVisible(songSelect);
    songListBox.setVisible(songSelect);
    songMistBestLabel.setVisible(false);
    songChordsBestLabel.setVisible(false);
    dailyChallengeBestLabel.setVisible(false);

    tunerTitleLabel.setVisible(tunerScreen);
    tunerNoteLabel.setVisible(tunerScreen);
    tunerFrequencyLabel.setVisible(tunerScreen);
    tunerStringLabel.setVisible(tunerScreen);
    tunerCentsLabel.setVisible(tunerScreen);
    tunerStatusLabel.setVisible(tunerScreen);
    tunerStartButton.setVisible(tunerScreen);
    tunerSkipButton.setVisible(tunerScreen);

    pitchLabel.setVisible(showGame);
    comboLabel.setVisible(showGame);
    beatLabel.setVisible(showGame);
    barBeatLabel.setVisible(showGame);
    bpmLabel.setVisible(showGame);
    gameSessionLabel.setVisible(showGame);
    hitLabel.setVisible(showGame);
    targetNoteLabel.setVisible(showGame);

    gameStartButton.setVisible(showGame || finishedScreen);
    pauseButton.setVisible(showGame);
    menuButton.setVisible(showGame || finishedScreen);
    distortionButton.setVisible(false);
    midiBackingButton.setVisible(showGame);
    midiBackingVolumeSlider.setVisible(showGame);
    midiBackingVolumeLabel.setVisible(showGame);
    midiRhythmGuitarButton.setVisible(showGame);
    midiRhythmGuitarVolumeSlider.setVisible(showGame);
    midiRhythmGuitarVolumeLabel.setVisible(showGame);
    midiBassButton.setVisible(showGame);
    midiBassVolumeSlider.setVisible(showGame);
    midiBassVolumeLabel.setVisible(showGame);
    midiDrumsButton.setVisible(showGame);
    midiDrumsVolumeSlider.setVisible(showGame);
    midiDrumsVolumeLabel.setVisible(showGame);
    midiPianoButton.setVisible(showGame);
    midiPianoVolumeSlider.setVisible(showGame);
    midiPianoVolumeLabel.setVisible(showGame);
    metronomeMixerButton.setVisible(showGame);
    metronomeMixerVolumeSlider.setVisible(showGame);
    metronomeMixerVolumeLabel.setVisible(showGame);
    distortionDriveSlider.setVisible(false);
    distortionToneSlider.setVisible(false);
    distortionDriveLabel.setVisible(false);
    distortionToneLabel.setVisible(false);
    speed50Button.setVisible(showGame);
    speed100Button.setVisible(showGame);
    speed120Button.setVisible(showGame);
    audioSettingsButton.setVisible(showGame);
    easyButton.setVisible(false);
    normalButton.setVisible(false);
    hardButton.setVisible(false);
    tablatureComponent.setVisible(showGame);

    //==========================================================================
    // Authentication
    //==========================================================================

    if (authScreen)
    {
        const int cardWidth = juce::jmin(560, width - 48);
        const int cardHeight = 400;
        const int cardX = (width - cardWidth) / 2;
        const int cardY = juce::jmax(150, (height - cardHeight) / 2);

        loginScreenTitleLabel.setBounds(cardX + 40, cardY + 114, cardWidth - 80, 52);
        accountStatusLabel.setJustificationType(juce::Justification::centred);
        accountStatusLabel.setBounds(cardX + 50, cardY + 198, cardWidth - 100, 38);

        const int buttonWidth = (cardWidth - 92) / 2;
        accountLoginButton.setBounds(cardX + 40, cardY + 276, buttonWidth, 44);
        accountRegisterButton.setBounds(cardX + 52 + buttonWidth, cardY + 276,
                                        buttonWidth, 44);

        verifyEmailButton.setBounds(cardX + 40, cardY + 338, buttonWidth, 28);
        forgotPasswordButton.setBounds(cardX + 52 + buttonWidth, cardY + 338,
                                       buttonWidth, 28);
    }

    //==========================================================================
    // Song select
    //==========================================================================

    if (songSelect)
    {
        accountStatusLabel.setJustificationType(juce::Justification::centredLeft);
        currentUserLabel.setBounds(width - 362, 48, 220, 22);
        currentUserLabel.setJustificationType(juce::Justification::centredLeft);
        logoutButton.setBounds(width - 122, 42, 94, 32);
        songTitleLabel.setBounds(48, 140, width - 96, 44);

        const int contentY = 228;
        const int availableH = juce::jmax(320, height - 68 - 20 - contentY);
        const int contentH = juce::jmin(560, availableH);
        const int footerY = contentY + contentH + 20;
        const int contentW = width - 96;
        const int gap = 20;
        const int leftW = contentW * 56 / 100;
        const int rightX = 48 + leftW + gap;
        const int rightW = contentW - leftW - gap;

        songPridumayBestLabel.setBounds(48 + 24 + 138, contentY + 78,
                                        leftW - 24 - 138 - 24, 20);

        const int buttonY = contentY + contentH - 64;
        const int available = leftW - 48;
        const int startW = juce::jmin(180, (available - 12) / 2);
        const int onlineW = juce::jmin(240, available - startW - 12);
        songPridumayButton.setBounds(48 + 24, buttonY, startW, 42);
        onlineCatalogButton.setBounds(48 + 24 + startW + 12, buttonY, onlineW, 42);
        onlineCatalogStatusLabel.setBounds(48 + 24, buttonY - 30, leftW - 48, 22);

        // Catalogue list between the header block and the action buttons.
        const int listTop = contentY + 186;
        const int listBottom = buttonY - 38;
        songListBox.setBounds(48 + 18, listTop, leftW - 36,
                              juce::jmax(56, listBottom - listTop));

        leaderboardTitleLabel.setBounds(rightX + 24, contentY + 38, rightW - 48, 30);
        refreshLeaderboardButton.setBounds(rightX + rightW - 142, contentY + 68,
                                           118, 32);
        leaderboardLabel.setBounds(rightX + 24, contentY + 118,
                                   rightW - 48, contentH - 142);

        accountStatusLabel.setBounds(24 + 34, footerY + 7, width - 48 - 58, 30);
    }

    //==========================================================================
    // Tuner
    //==========================================================================

    if (tunerScreen)
    {
        const int margin = 42;
        const int gap = 20;
        const int panelY = 190;
        const int panelH = juce::jmin(560, juce::jmax(380, height - 190 - 120));
        const int leftW = (width - 84 - gap) * 44 / 100;
        const int rightX = margin + leftW + gap;
        const int rightW = width - margin - rightX;

        tunerTitleLabel.setBounds(rightX + 24, panelY + 24, rightW - 48, 20);
        tunerNoteLabel.setBounds(rightX + 24, panelY + 142, rightW - 48, 96);
        tunerFrequencyLabel.setBounds(rightX + 24, panelY + 242, rightW - 48, 26);
        tunerStringLabel.setBounds(rightX + rightW - 180, panelY + 278, 160, 30);
        tunerCentsLabel.setBounds(rightX + rightW - 180, panelY + 310, 160, 30);
        tunerStringLabel.setJustificationType(juce::Justification::centredRight);
        tunerCentsLabel.setJustificationType(juce::Justification::centredRight);
        tunerStatusLabel.setBounds(rightX + 40, panelY + 368, rightW - 80, 40);
        tunerStatusLabel.setJustificationType(juce::Justification::centred);

        const int buttonWidth = juce::jmin(230, (width - 100) / 2);
        const int buttonGap = 16;
        const int buttonsX = (width - (buttonWidth * 2 + buttonGap)) / 2;
        const int buttonY = panelY + panelH + 24;
        tunerSkipButton.setBounds(buttonsX, buttonY, buttonWidth, 46);
        tunerStartButton.setBounds(buttonsX + buttonWidth + buttonGap,
                                   buttonY, buttonWidth, 46);
    }

    //==========================================================================
    // Results
    //==========================================================================

    if (finishedScreen)
    {
        const int buttonWidth = juce::jmin(180, (width - 100) / 2);
        const int gap = 20;
        const int totalWidth = buttonWidth * 2 + gap;
        const int left = (width - totalWidth) / 2;
        const int cardBottom = 82 + juce::jmax(520, height - 170);
        const int y = height >= 820 ? cardBottom - 56 : height - 76;
        gameStartButton.setBounds(left, y, buttonWidth, 44);
        menuButton.setBounds(left + buttonWidth + gap, y, buttonWidth, 44);
        gameStartButton.setButtonText("PLAY AGAIN");
        menuButton.setButtonText("MAIN MENU");
    }

    //==========================================================================
    // Gameplay
    //==========================================================================

    if (showGame)
    {
        menuButton.setButtonText(gameState == GameState::Paused ? "BACK" : "MENU");
        const int hudY = 120;
        const int hudH = 96;
        const int bottomH = juce::jlimit(124, 160, height / 6);
        const int bottomY = height - 24 - bottomH;
        const int studioH = juce::jlimit(176, 218, height / 4);
        const int studioY = bottomY - 14 - studioH;
        const int tabY = hudY + hudH + 12;
        const int tabH = juce::jmax(150, studioY - 12 - tabY);

        const int audioW = 112;
        const int menuW = 100;
        const int pauseW = 82;
        const int startW = 76;
        const int actionGap = 8;
        const int actionsW = audioW + menuW + pauseW + startW + actionGap * 3;
        const int actionY = 46;
        const int actionH = 36;
        int actionX = width - 24 - actionsW;
        gameStartButton.setBounds(actionX, actionY, startW, actionH);
        actionX += startW + actionGap;
        pauseButton.setBounds(actionX, actionY, pauseW, actionH);
        actionX += pauseW + actionGap;
        menuButton.setBounds(actionX, actionY, menuW, actionH);
        actionX += menuW + actionGap;
        audioSettingsButton.setBounds(actionX, actionY, audioW, actionH);

        gameSongTitleLabel.setBounds(210, 45, juce::jmax(200, width - 640), 28);

        beatLabel.setBounds(44 + 46, 83, 30, 22);
        barBeatLabel.setBounds(132 + 40, 83, 28, 22);
        bpmLabel.setBounds(212 + 42, 83, 44, 22);

        speed50Button.setBounds(310, 82, 44, 24);
        speed100Button.setBounds(360, 82, 48, 24);
        speed120Button.setBounds(414, 82, 48, 24);

        const int hudGap = 12;
        const int comboW = 124;
        const int pitchW = 214;
        const int targetW = 190;
        const int comboX = 24;
        const int pitchX = comboX + comboW + hudGap;
        const int targetX = width - 24 - targetW;
        const int statsX = pitchX + pitchW + hudGap;
        const int statsW = juce::jmax(180, targetX - hudGap - statsX);
        juce::ignoreUnused(statsW);

        comboLabel.setBounds(comboX + 8, hudY + 32, comboW - 16, 46);
        pitchLabel.setBounds(pitchX + 10, hudY + 32, pitchW - 20, 46);
        targetNoteLabel.setBounds(targetX + 10, hudY + 32, targetW - 20, 46);

        tablatureComponent.setBounds(24, tabY, width - 48, tabH);

        const int mixerX = 24;
        const int mixerW = width - 48;
        const int channelX = mixerX + 14;
        const int channelTop = studioY + 66;
        const int cellGap = 12;
        const int cellW = (mixerW - 28 - cellGap * 2) / 3;
        const int rowH = (studioH - 66 - 10) / 2;

        auto layoutMixerChannel = [&](juce::TextButton& button, juce::Label& label,
                                      juce::Slider& slider, int column, int row)
        {
            const int x = channelX + column * (cellW + cellGap);
            const int y = channelTop + row * rowH;
            button.setBounds(x, y, cellW, 22);
            slider.setBounds(x, y + 27, cellW, 18);
            label.setBounds(x, y + 48, cellW, 12);
        };

        layoutMixerChannel(midiBackingButton, midiBackingVolumeLabel,
                           midiBackingVolumeSlider, 0, 0);
        layoutMixerChannel(midiDrumsButton, midiDrumsVolumeLabel,
                           midiDrumsVolumeSlider, 1, 0);
        layoutMixerChannel(midiBassButton, midiBassVolumeLabel,
                           midiBassVolumeSlider, 2, 0);
        layoutMixerChannel(midiRhythmGuitarButton, midiRhythmGuitarVolumeLabel,
                           midiRhythmGuitarVolumeSlider, 0, 1);
        layoutMixerChannel(midiPianoButton, midiPianoVolumeLabel,
                           midiPianoVolumeSlider, 1, 1);
        layoutMixerChannel(metronomeMixerButton, metronomeMixerVolumeLabel,
                           metronomeMixerVolumeSlider, 2, 1);

        const int bottomW = width - 48;
        const int judgementW = bottomW * 40 / 100;
        const int scoreX = 24 + bottomW * 66 / 100;
        hitLabel.setBounds(24 + 24, bottomY + 34, judgementW - 48, bottomH - 44);
        gameSessionLabel.setBounds(scoreX + 16, bottomY + 24,
                                   width - 24 - scoreX - 32, bottomH - 30);
    }
}
