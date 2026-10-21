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
// while placing it inside the app's light blue/orange visual system.
class AudioSettingsPanel final : public juce::Component
{
public:
    explicit AudioSettingsPanel(juce::AudioDeviceManager& manager)
        : selector(manager, 0, 2, 0, 2, true, true, true, false)
    {
        setSize(760, 560);
        addAndMakeVisible(selector);

        // JUCE's native selector can inherit white label text from the app theme.
        // Explicitly use dark text on its light controls.
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
                combo->setColour(juce::ComboBox::arrowColourId, UiColors::blueDark);
            }

            if (auto* button = dynamic_cast<juce::Button*>(&component))
                button->setColour(juce::TextButton::textColourOffId, UiColors::text);

            for (int i = 0; i < component.getNumChildComponents(); ++i)
                applyReadableAudioColours(*component.getChildComponent(i));
        };
        applyReadableAudioColours(selector);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UiColors::background);

        auto bounds = getLocalBounds().toFloat();
        g.setColour(UiColors::card);
        g.fillRoundedRectangle(bounds.reduced(12.0f), 18.0f);
        g.setColour(UiColors::border);
        g.drawRoundedRectangle(bounds.reduced(12.0f), 18.0f, 1.2f);

        g.setColour(UiColors::blue);
        g.fillRoundedRectangle(juce::Rectangle<float>(30.0f, 29.0f, 5.0f, 34.0f), 2.5f);

        g.setColour(UiColors::text);
        g.setFont(juce::FontOptions().withHeight(25.0f).withStyle("Bold"));
        g.drawText("AUDIO SETTINGS", 48, 26, getWidth() - 80, 34,
                   juce::Justification::centredLeft);

        g.setColour(UiColors::secondaryText);
        g.setFont(juce::FontOptions().withHeight(13.0f));
        g.drawText("Choose your guitar input and configure audio performance.",
                   48, 59, getWidth() - 80, 24, juce::Justification::centredLeft);

        g.setColour(UiColors::orange);
        g.fillRoundedRectangle(juce::Rectangle<float>(getWidth() - 57.0f, 35.0f, 10.0f, 10.0f), 5.0f);

        g.setColour(UiColors::border);
        g.drawLine(30.0f, 94.0f, static_cast<float>(getWidth() - 30), 94.0f, 1.0f);
    }

    void resized() override
    {
        selector.setBounds(30, 108, getWidth() - 60, getHeight() - 132);
    }

private:
    juce::AudioDeviceSelectorComponent selector;
};

//==============================================================================
// Constructor
//==============================================================================

MainComponent::MainComponent()
    : overlayComponent(*this)
{
    pitchThread = std::make_unique<PitchDetectionThread>(*this);
    // Configure persistent settings before loading songs or querying best scores.
    juce::PropertiesFile::Options options;
    options.applicationName = "GuitarBattle";
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    options.folderName = "GuitarBattle";
    options.commonToAllUsers = false;
    appProperties.setStorageParameters(options);

    setSize(
        1000,
        950);

    startTimerHz(30);

    // Use a single still frame as the background; no background animation.
    // The image is optional, so the light theme remains usable if it is absent.
    const juce::File framesDirectory("D:\\GuitarBattle\\GuitarBattle\\Style\\VideoFrames");
    backgroundFrame = juce::ImageFileFormat::loadFrom(
        framesDirectory.getChildFile("frame_0001.jpg"));

    overlayComponent.setBounds(getLocalBounds());
    addAndMakeVisible(overlayComponent);

    addAndMakeVisible(
        tablatureComponent);

    //==========================================================================
    // Tuner UI

    tunerTitleLabel.setText(
        "TUNER",
        juce::dontSendNotification);

    tunerTitleLabel.setFont(
        juce::FontOptions()
        .withHeight(28.0f)
        .withStyle("Bold"));

    tunerTitleLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    tunerTitleLabel.setJustificationType(
        juce::Justification::centred);

    tunerNoteLabel.setText(
        "---",
        juce::dontSendNotification);

    tunerNoteLabel.setFont(
        juce::FontOptions()
        .withHeight(64.0f)
        .withStyle("Bold"));

    tunerNoteLabel.setColour(
        juce::Label::textColourId,
        UiColors::blue);

    tunerNoteLabel.setJustificationType(
        juce::Justification::centred);

    tunerFrequencyLabel.setText(
        "--- Hz",
        juce::dontSendNotification);

    tunerFrequencyLabel.setFont(
        juce::FontOptions()
        .withHeight(20.0f));

    tunerFrequencyLabel.setColour(
        juce::Label::textColourId,
        UiColors::secondaryText);

    tunerFrequencyLabel.setJustificationType(
        juce::Justification::centred);

    tunerStringLabel.setText(
        "---",
        juce::dontSendNotification);

    tunerStringLabel.setFont(
        juce::FontOptions()
        .withHeight(22.0f)
        .withStyle("Bold"));

    tunerStringLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    tunerStringLabel.setJustificationType(
        juce::Justification::centred);

    tunerCentsLabel.setText(
        "--- cents",
        juce::dontSendNotification);

    tunerCentsLabel.setFont(
        juce::FontOptions()
        .withHeight(22.0f)
        .withStyle("Bold"));

    tunerCentsLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    tunerCentsLabel.setJustificationType(
        juce::Justification::centred);

    tunerStatusLabel.setText(
        "PLAY A STRING",
        juce::dontSendNotification);

    tunerStatusLabel.setFont(
        juce::FontOptions()
        .withHeight(24.0f)
        .withStyle("Bold"));

    tunerStatusLabel.setColour(
        juce::Label::textColourId,
        UiColors::blue);

    tunerStatusLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(tunerTitleLabel);
    addAndMakeVisible(tunerNoteLabel);
    addAndMakeVisible(tunerFrequencyLabel);
    addAndMakeVisible(tunerStringLabel);
    addAndMakeVisible(tunerCentsLabel);
    addAndMakeVisible(tunerStatusLabel);

    tunerStartButton.setEnabled(false);

    addAndMakeVisible(tunerStartButton);

    tunerSkipButton.setColour(juce::TextButton::buttonColourId, UiColors::orange);
    tunerSkipButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
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

            const auto beatLengthSamples =
                metronome.getBeatLengthSamples();

            // Give the player a full bar of four audible metronome clicks.
            // The song timeline starts on the beat immediately after click 4.
            countInRemaining.store(4, std::memory_order_relaxed);
            countInActive.store(true, std::memory_order_release);
            tablatureComponent.setPlayheadRunning(false);

            gameplayStartSample.store(
                currentSample + beatLengthSamples * 4,
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
        UiColors::blue);
    gameStartButton.setColour(juce::TextButton::buttonColourId, UiColors::blue);
    gameStartButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    gameStartButton.setLookAndFeel(&getLookAndFeel());

    gameStartButton.setColour(
        juce::TextButton::textColourOffId,
        juce::Colours::white);

    gameStartButton.setColour(
        juce::TextButton::buttonOnColourId,
        UiColors::blueDark);

    gameStartButton.setColour(
        juce::TextButton::textColourOnId,
        juce::Colours::white);

    pauseButton.setColour(
        juce::TextButton::buttonColourId,
        UiColors::blueLight);

    pauseButton.setColour(
        juce::TextButton::textColourOffId,
        UiColors::blueDark);

    pauseButton.setColour(
        juce::TextButton::buttonOnColourId,
        juce::Colour::fromString("FFDBEAFE"));

    pauseButton.setColour(
        juce::TextButton::textColourOnId,
        UiColors::blueDark);

    menuButton.setColour(
        juce::TextButton::buttonColourId,
        UiColors::card);

    menuButton.setColour(
        juce::TextButton::textColourOffId,
        UiColors::secondaryText);

    menuButton.setColour(
        juce::TextButton::buttonOnColourId,
        UiColors::blueLight);

    menuButton.setColour(
        juce::TextButton::textColourOnId,
        UiColors::blueDark);

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
    distortionButton.setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(190, 32, 42));
    distortionButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    distortionButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour::fromRGB(225, 45, 52));
    distortionButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    distortionButton.setTooltip("Switch the distortion pedal on or off.");
    addAndMakeVisible(distortionButton);
    midiBackingButton.setColour(juce::TextButton::buttonColourId, UiColors::blue);
    midiBackingButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    midiBackingButton.setTooltip("Toggle the MIDI solo-guitar part generated by GuitarBattle.");
    addAndMakeVisible(midiBackingButton);
    midiBackingButton.onClick = [this]
    {
        const bool enabled = !midiBackingEnabled.load(std::memory_order_relaxed);
        midiBackingEnabled.store(enabled, std::memory_order_relaxed);
        midiBackingButton.setButtonText(enabled ? "SOLO GUITAR MIDI ON" : "SOLO GUITAR MIDI OFF");
        midiBackingButton.setColour(juce::TextButton::buttonColourId,
                                    enabled ? UiColors::blue : UiColors::secondaryText);
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

    midiRhythmGuitarButton.setColour(juce::TextButton::buttonColourId, UiColors::blue);
    midiRhythmGuitarButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    midiRhythmGuitarButton.setTooltip("Toggle the rhythm-guitar WAV stem.");
    addAndMakeVisible(midiRhythmGuitarButton);
    midiRhythmGuitarButton.onClick = [this]
    {
        const bool enabled = !midiRhythmGuitarEnabled.load(std::memory_order_relaxed);
        midiRhythmGuitarEnabled.store(enabled, std::memory_order_relaxed);
        midiRhythmGuitarButton.setButtonText(enabled ? "RHYTHM GUITAR ON" : "RHYTHM GUITAR OFF");
        midiRhythmGuitarButton.setColour(juce::TextButton::buttonColourId,
            enabled ? UiColors::blue : UiColors::secondaryText);
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
        button.setColour(juce::TextButton::buttonColourId, UiColors::blue);
        button.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        button.setTooltip("Toggle the " + name + " audio channel.");
        addAndMakeVisible(button);
        auto* buttonPtr = &button;
        auto* enabledPtr = &enabled;
        button.onClick = [buttonPtr, enabledPtr, name]()
        {
            const bool next = !enabledPtr->load(std::memory_order_relaxed);
            enabledPtr->store(next, std::memory_order_relaxed);
            buttonPtr->setButtonText(name + (next ? " ON" : " OFF"));
            buttonPtr->setColour(juce::TextButton::buttonColourId,
                                 next ? UiColors::blue : UiColors::secondaryText);
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
        distortionButton.setColour(juce::TextButton::buttonColourId,
                                   enabled ? juce::Colour::fromRGB(225, 45, 52) : juce::Colour::fromRGB(190, 32, 42));
        distortionButton.setColour(juce::TextButton::textColourOffId,
                                   juce::Colours::white);
    };

    auto setupPedalKnob = [this](juce::Slider& knob, juce::Label& label,
                                  const juce::String& name, double minimum,
                                  double maximum, double initial)
    {
        knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 48, 15);
        knob.setRange(minimum, maximum, 0.01);
        knob.setValue(initial, juce::dontSendNotification);
        knob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour::fromRGB(245, 65, 65));
        knob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour::fromRGB(90, 18, 24));
        knob.setColour(juce::Slider::thumbColourId, juce::Colour::fromRGB(255, 210, 210));
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
    // Register WAV/MP3 decoders before the local catalogue loads its stems.
    audioFormatManager.registerBasicFormats();

    // Build the local song catalogue before selecting the initial song.
    songsRoot = juce::File("D:\\GuitarBattle\\GuitarBattle\\Songs");
    if (!songsRoot.isDirectory())
        songsRoot = juce::File::getCurrentWorkingDirectory().getChildFile("Songs");
    scanSongLibrary();
    if (!songLibrary.empty())
    {
        songPridumayButton.setButtonText(songLibrary.front().title);
        loadSongMidi(songLibrary.front().title);
    }
    else
        DBG("SONG LIBRARY EMPTY: expected song folders under " + songsRoot.getFullPathName());

    //==========================================================================
    // Pitch label

    pitchLabel.setText(
        "Pitch: ---",
        juce::dontSendNotification);

    pitchLabel.setFont(
        juce::FontOptions()
        .withHeight(42.0f)
        .withStyle("Bold"));

    pitchLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    pitchLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        pitchLabel);

    //==========================================================================
    // Beat label

    beatLabel.setText(
        "0",
        juce::dontSendNotification);

    beatLabel.setFont(
        juce::FontOptions()
        .withHeight(16.0f)
        .withStyle("Bold"));

    beatLabel.setColour(
        juce::Label::textColourId,
        UiColors::secondaryText);

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
        juce::FontOptions()
        .withHeight(16.0f)
        .withStyle("Bold"));

    barBeatLabel.setColour(
        juce::Label::textColourId,
        UiColors::secondaryText);

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
        juce::FontOptions()
        .withHeight(13.0f)
        .withStyle("Bold"));

    bpmLabel.setColour(
        juce::Label::textColourId,
        UiColors::secondaryText);

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
            UiColors::blueLight);

        button->setColour(
            juce::TextButton::buttonOnColourId,
            juce::Colour::fromString(
                "FFDBEAFE"));

        button->setColour(
            juce::TextButton::textColourOffId,
            UiColors::blueDark);

        button->setColour(
            juce::TextButton::textColourOnId,
            UiColors::blueDark);
    }

    //==========================================================================
    // Song select

    songTitleLabel.setText("", juce::dontSendNotification);
    songTitleLabel.setFont(juce::FontOptions().withHeight(27.0f).withStyle("Bold"));
    songTitleLabel.setColour(juce::Label::textColourId, UiColors::text);
    songTitleLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(songTitleLabel);

    // Current song title shown in the centre of the gameplay header.
    gameSongTitleLabel.setText(juce::String::fromUTF8("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB4\xD1\x83\xD0\xBC\xD0\xB0\xD0\xB9 \xD1\x81\xD0\xB2\xD0\xB5\xD1\x82\xD0\xBB\xD1\x8B\xD0\xB9 \xD0\xBC\xD0\xB8\xD1\x80"), juce::dontSendNotification);
    gameSongTitleLabel.setFont(juce::FontOptions().withHeight(22.0f).withStyle("Bold"));
    gameSongTitleLabel.setColour(juce::Label::textColourId, UiColors::text);
    gameSongTitleLabel.setJustificationType(juce::Justification::centred);
    gameSongTitleLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(gameSongTitleLabel);

    addAndMakeVisible(songPridumayButton);
    onlineCatalogButton.setColour(juce::TextButton::buttonColourId, UiColors::blue);
    onlineCatalogButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    addAndMakeVisible(onlineCatalogButton);
    onlineCatalogStatusLabel.setFont(juce::FontOptions().withHeight(15.0f));
    onlineCatalogStatusLabel.setColour(juce::Label::textColourId, UiColors::secondaryText);
    onlineCatalogStatusLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(onlineCatalogStatusLabel);

    leaderboardTitleLabel.setText("GLOBAL PLAYER RATING", juce::dontSendNotification);
    leaderboardTitleLabel.setFont(juce::FontOptions().withHeight(22.0f).withStyle("Bold"));
    leaderboardTitleLabel.setColour(juce::Label::textColourId, UiColors::text);
    leaderboardTitleLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(leaderboardTitleLabel);

    leaderboardLabel.setText("Press REFRESH RATING to load players from the server.", juce::dontSendNotification);
    leaderboardLabel.setFont(juce::FontOptions().withHeight(16.0f));
    leaderboardLabel.setColour(juce::Label::textColourId, UiColors::secondaryText);
    leaderboardLabel.setJustificationType(juce::Justification::topLeft);
    leaderboardLabel.setMinimumHorizontalScale(0.8f);
    addAndMakeVisible(leaderboardLabel);

    refreshLeaderboardButton.setColour(juce::TextButton::buttonColourId, UiColors::blue);
    refreshLeaderboardButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    refreshLeaderboardButton.onClick = [this] { refreshGlobalLeaderboard(); };
    addAndMakeVisible(refreshLeaderboardButton);

    accountStatusLabel.setText("Not signed in. Sign in to submit scores.", juce::dontSendNotification);
    accountStatusLabel.setFont(juce::FontOptions().withHeight(13.0f));
    accountStatusLabel.setColour(juce::Label::textColourId, UiColors::secondaryText);
    accountStatusLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(accountStatusLabel);

    loginScreenTitleLabel.setText("GUITARBATTLE", juce::dontSendNotification);
    loginScreenTitleLabel.setFont(juce::FontOptions().withHeight(34.0f).withStyle("Bold"));
    loginScreenTitleLabel.setColour(juce::Label::textColourId, UiColors::text);
    loginScreenTitleLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(loginScreenTitleLabel);

    currentUserLabel.setFont(juce::FontOptions().withHeight(14.0f).withStyle("Bold"));
    currentUserLabel.setColour(juce::Label::textColourId, UiColors::text);
    currentUserLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(currentUserLabel);

    logoutButton.setColour(juce::TextButton::buttonColourId, UiColors::blue);
    logoutButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    logoutButton.onClick = [this]
    {
        // Invalidate a sign-in chain that may still be waiting for the server.
        ++authRequestId;
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

    for (auto* button : { &accountLoginButton, &accountRegisterButton })
    {
        button->setColour(juce::TextButton::buttonColourId, UiColors::blue);
        button->setColour(juce::TextButton::textColourOffId, UiColors::text);
        addAndMakeVisible(*button);
    }
    accountLoginButton.onClick = [this] { authenticateOnlineAccount(false); };
    accountRegisterButton.onClick = [this] { authenticateOnlineAccount(true); };

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
    for (auto* button : { &songPridumayButton, &songMistButton, &songChordsButton, &aiSoloButton, &oldGeneratorArchiveButton, &generateMelodyButton, &newGenerationButton })
    {
        button->setColour(juce::TextButton::buttonColourId, UiColors::blueLight);
        button->setColour(juce::TextButton::buttonColourId, UiColors::card);
        button->setColour(juce::TextButton::textColourOffId, UiColors::text);
        button->setColour(juce::TextButton::buttonOnColourId, UiColors::blue);
        button->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        button->setColour(juce::TextButton::textColourOffId, UiColors::blueDark);
        button->setColour(juce::TextButton::buttonOnColourId, UiColors::orange);
        button->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    }
    addAndMakeVisible(songPridumayBestLabel);
    addAndMakeVisible(songMistBestLabel);
    addAndMakeVisible(songChordsBestLabel);
    addAndMakeVisible(dailyChallengeBestLabel);

    auto configureBestLabel = [](juce::Label& label)
    {
        label.setFont(juce::FontOptions().withHeight(16.0f));
        label.setColour(juce::Label::textColourId, UiColors::secondaryText);
        label.setJustificationType(juce::Justification::centred);
        label.setInterceptsMouseClicks(false, false);
    };
    configureBestLabel(songPridumayBestLabel);
    configureBestLabel(songMistBestLabel);
    configureBestLabel(songChordsBestLabel);
    configureBestLabel(dailyChallengeBestLabel);
    updateSongBestScoreLabels();

    songPridumayButton.onClick = [this]
    {
        if (!songLibrary.empty() && loadSongMidi(songLibrary.front().title))
            beginTuningForSelectedSong();
    };

    onlineCatalogButton.onClick = [this]
    {
        onlineCatalogButton.setEnabled(false);
        onlineCatalogStatusLabel.setText("Connecting to GuitarBattle server...", juce::dontSendNotification);
        // Catalogue lookup and the file downloads run on the ApiClient worker
        // threads; the button is re-enabled and the status text updated when the
        // result arrives on the message thread. Local gameplay/audio callbacks do
        // not perform network I/O.
        loadFirstOnlineCatalogSong();
    };

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
        juce::FontOptions()
        .withHeight(30.0f)
        .withStyle("Bold"));

    gameSessionLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    gameSessionLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        gameSessionLabel);

    //==========================================================================
    // Hit status

    hitLabel.setText(
        "READY",
        juce::dontSendNotification);

    hitLabel.setFont(
        juce::FontOptions()
        .withHeight(24.0f)
        .withStyle("Bold"));

    hitLabel.setColour(
        juce::Label::textColourId,
        UiColors::blue);

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
        juce::FontOptions()
        .withHeight(34.0f)
        .withStyle("Bold"));

    targetNoteLabel.setColour(
        juce::Label::textColourId,
        UiColors::blueDark);

    targetNoteLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        targetNoteLabel);

    //==========================================================================
    // Audio settings

    audioSettingsButton.setButtonText(
        "Audio Settings");

    audioSettingsButton.setColour(
        juce::TextButton::buttonColourId,
        UiColors::card);

    audioSettingsButton.setColour(
        juce::TextButton::textColourOffId,
        UiColors::secondaryText);

    audioSettingsButton.setColour(
        juce::TextButton::buttonOnColourId,
        UiColors::blueLight);

    audioSettingsButton.setColour(
        juce::TextButton::textColourOnId,
        UiColors::blueDark);

    addAndMakeVisible(
        audioSettingsButton);

    audioSettingsButton.onClick =
        [this]
        {
            juce::DialogWindow::LaunchOptions options;

            options.content.setOwned(
                new AudioSettingsPanel(deviceManager));

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
    // Restore audio device settings

    auto savedState =
        appProperties.getUserSettings()
        ->getXmlValue(
            "AudioDeviceState");

    // Register decoders and load the optional clean electric-guitar SFZ bank
    // before the audio device starts. The existing synth tone remains a fallback.
    loadGuitarSampleBank();
    loadBackingSampleBanks();

    // Song stems are loaded when a catalogue entry is selected.

    setAudioChannels(
        2,
        2,
        savedState.get());

    //==========================================================================
    // Start pitch thread

    pitchThread->startThread();

    resized();
    repaint();
}

//==============================================================================
// Timer
//==============================================================================

void MainComponent::timerCallback()
{
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
}

//==============================================================================
// Animated hit feedback
//==============================================================================

void MainComponent::showHitFeedback(const juce::String& text, int type)
{
    // Called on the message thread (all pitch-thread callers use callAsync).
    feedbackType = type;
    feedbackStartMs = juce::Time::getMillisecondCounter();
    feedbackBaseFontSize = 20.0f;
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



    // First click after resume comes one beat later.
    metronome.resumeScheduler(
        currentSample);

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

juce::File MainComponent::findSongAsset(const juce::File& folder,
                                                const juce::StringArray& stems,
                                                const juce::StringArray& extensions) const
{
    for (const auto& stem : stems)
        for (const auto& extension : extensions)
        {
            auto candidate = folder.getChildFile(stem + extension);
            if (candidate.existsAsFile()) return candidate;
        }
    // Accept files whose names contain the role, e.g. "Power metal - bass.wav".
    juce::Array<juce::File> files;
    folder.findChildFiles(files, juce::File::findFiles, false);
    for (const auto& file : files)
    {
        const auto lowerName = file.getFileNameWithoutExtension().toLowerCase();
        const auto lowerExt = file.getFileExtension().toLowerCase();
        if (!extensions.contains(lowerExt)) continue;
        for (const auto& stem : stems)
            if (lowerName == stem.toLowerCase() || lowerName.contains(stem.toLowerCase()))
                return file;
    }
    return {};
}


void MainComponent::authenticateOnlineAccount(bool createAccount)
{
    // JUCE 9 no longer supports AlertWindow::runModalLoop(). The dialog stays
    // open while credentials are checked; only CANCEL (or a successful sign-in)
    // dismisses it.
    auto* prompt = new juce::AlertWindow(
        createAccount ? "Create GuitarBattle account" : "Sign in to GuitarBattle",
        createAccount
            ? "Username and email are required. Password must be at least 10 characters."
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

                    if (response.statusCode == 401 || response.statusCode == 422)
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
                [safeThis, safePrompt, attemptId, usernameOrEmail, password,
                 signIn, showError, setSubmitEnabled](ApiClient::Response response)
                {
                    auto* component = safeThis.getComponent();
                    if (component == nullptr || attemptId != component->authRequestId)
                        return;

                    if (response.statusCode >= 200 && response.statusCode < 300)
                    {
                        signIn(attemptId, usernameOrEmail, password);
                        return;
                    }

                    setSubmitEnabled(true);

                    if (response.statusCode == 409)
                    {
                        component->accountStatusLabel.setText(
                            "Username or email is already registered.",
                            juce::dontSendNotification);
                        showError("Registration failed",
                                  "Username or email is already registered.");
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
            self->currentUserLabel.setText("Signed in: " + self->currentUsername, juce::dontSendNotification);
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
                self->leaderboardLabel.setText("Server unavailable. Start GuitarBattleServer and try again.",
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
                text << rank << ".  " << name << "   —   " << points << " pts"
                     << "   (" << games << " games)" << juce::newLine;
            }
            self->leaderboardLabel.setText(text, juce::dontSendNotification);
        });
}

namespace
{
    // Collected by the background thread of loadFirstOnlineCatalogSong() and
    // applied to the song library on the message thread.
    struct OnlineCatalogDownload
    {
        bool ok = false;
        juce::String title;
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
}

void MainComponent::loadFirstOnlineCatalogSong()
{
    juce::Component::SafePointer<MainComponent> safeThis(this);
    auto download = std::make_shared<OnlineCatalogDownload>();

    // Catalogue lookup and file downloads run on the ApiClient worker threads.
    // Only the message-thread callback below touches components, the song library
    // or the gameplay state.
    auto backgroundWork = [download]()
    {
        const auto listResponse = ApiClient::get("/songs?limit=20&offset=0", {}, 5000);
        if (!listResponse.transportOk)
        {
            DBG("ONLINE CATALOG: could not connect to " + juce::String(ApiClient::baseUrl));
            return;
        }

        const auto listJson = listResponse.json;
        if (!listJson.isObject())
            return;

        const auto itemsValue = listJson.getProperty("items", juce::var());
        const auto* items = itemsValue.getArray();
        if (items == nullptr || items->isEmpty())
        {
            DBG("ONLINE CATALOG: server listed no published songs");
            return;
        }

        // Prefer the known test song if it is in the public catalogue; otherwise
        // load the first item returned by the server.
        juce::var selected = items->getReference(0);
        for (const auto& item : *items)
            if (item.getProperty("slug", juce::var()).toString() == "pridumay-svetly-mir")
            {
                selected = item;
                break;
            }

        const auto slug = selected.getProperty("slug", juce::var()).toString();
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
        download->songId = detail.getProperty("id", juce::var()).toString();
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

    apiClient.runAsync(std::move(backgroundWork), [safeThis, download]()
    {
        auto* self = safeThis.getComponent();
        if (self == nullptr)
            return;

        bool loaded = false;

        if (download->ok)
        {
            self->onlineSongUuid = download->songId;
            self->onlineGameSessionUuid.clear();

            // Reuse cached entries on subsequent button presses.
            auto found = std::find_if(self->songLibrary.begin(), self->songLibrary.end(),
                [&download](const SongEntry& entry)
                {
                    return entry.title == download->title
                        && entry.folder.getParentDirectory().getFileName() == "OnlineSongs";
                });

            if (found == self->songLibrary.end())
            {
                SongEntry entry;
                entry.title = download->title;
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
                found->folder = download->songFolder;
                found->soloMidi = download->midiFile;
                found->drums = download->drumsFile;
                found->bass = download->bassFile;
                found->rhythmGuitar = download->guitarFile;
                found->others = download->othersFile;
                found->bpm = download->bpm;
            }

            self->onlineCatalogStatusLabel.setText("Downloaded: " + download->title,
                                                   juce::dontSendNotification);

            if (self->gameState != MainComponent::GameState::SongSelect)
            {
                // A game started while the download was running: keep the song
                // cached and leave the active session untouched.
                self->onlineCatalogButton.setEnabled(true);
                self->resized();
                self->repaint();
                return;
            }

            loaded = self->loadSongMidi(download->title);
        }

        self->onlineCatalogButton.setEnabled(true);

        juce::String statusText = "Could not load online song. Check that GuitarBattleServer is running.";
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
            statusText << ". Press the local song button to switch back.";
        }

        self->onlineCatalogStatusLabel.setText(statusText, juce::dontSendNotification);
        self->resized();
        self->repaint();
        if (loaded)
            self->beginTuningForSelectedSong();
    });
}

void MainComponent::scanSongLibrary()
{
    songLibrary.clear();
    if (!songsRoot.isDirectory()) return;

    juce::Array<juce::File> folders;
    songsRoot.findChildFiles(folders, juce::File::findDirectories, false);
    std::sort(folders.begin(), folders.end(), [](const juce::File& a, const juce::File& b)
    { return a.getFileName().compareIgnoreCase(b.getFileName()) < 0; });
    for (const auto& folder : folders)
    {
        SongEntry entry;
        entry.title = folder.getFileName();
        entry.folder = folder;
        entry.soloMidi = findSongAsset(folder, { "solo", "midi", "guitar_solo", "solo_guitar" }, { ".mid", ".midi" });
        if (!entry.soloMidi.existsAsFile())
            entry.soloMidi = findSongAsset(folder, { "" }, { ".mid", ".midi" });
        entry.drums = findSongAsset(folder, { "drums", "drum" }, { ".wav", ".mp3", ".aiff", ".flac" });
        entry.bass = findSongAsset(folder, { "bass" }, { ".wav", ".mp3", ".aiff", ".flac" });
        entry.rhythmGuitar = findSongAsset(folder, { "rhythm_guitar", "rhythm guitar", "guitar", "rhythm" }, { ".wav", ".mp3", ".aiff", ".flac" });
        entry.others = findSongAsset(folder, { "others", "other", "keys", "instruments" }, { ".wav", ".mp3", ".aiff", ".flac" });
        if (entry.soloMidi.existsAsFile())
        {
            songLibrary.push_back(entry);
            DBG("SONG CATALOG: " + entry.title + " MIDI=" + entry.soloMidi.getFullPathName());
        }
        else
            DBG("SONG SKIPPED (no solo MIDI): " + folder.getFullPathName());
    }
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
    if (found->folder.getParentDirectory().getFileName() != "OnlineSongs")
    {
        onlineSongUuid.clear();
        onlineGameSessionUuid.clear();
    }
    selectedSongId = found->title;
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
        const auto key = juce::String("BestScore_") + selectedSongId;
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
    songPridumayBestLabel.setText("BEST: " + juce::String(getBestScore(selectedSongId)), juce::dontSendNotification);
    songMistBestLabel.setText(juce::String(), juce::dontSendNotification);
    songChordsBestLabel.setText(juce::String(), juce::dontSendNotification);
    dailyChallengeBestLabel.setText(
        "BEST: " + juce::String(getBestScore("daily_challenge_monday")),
        juce::dontSendNotification);
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
                gameStartButton.setButtonText("RETRY");
                menuButton.setButtonText("BACK");

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

void MainComponent::loadGuitarSampleBank()
{
    guitarSamplesLoaded.store(false, std::memory_order_relaxed);
    guitarSampleRegions.clear();
    guitarRegionForMidi.fill(-1);

    juce::File bankDirectory;
    auto tryRoot = [&bankDirectory](const juce::File& root)
    {
        // Preferred location: <project root>\Sounds\GuitarSamples
        // Also support an SFZ bank stored directly in <project root>\Sounds.
        const auto soundsDirectory = root.getChildFile("Sounds");
        const auto candidates = {
            soundsDirectory.getChildFile("GuitarSamples"),
            soundsDirectory
        };

        for (const auto& candidate : candidates)
        {
            if (!candidate.isDirectory())
                continue;

            juce::Array<juce::File> sfzFiles;
            candidate.findChildFiles(sfzFiles, juce::File::findFiles, false, "*.sfz");
            if (!sfzFiles.isEmpty())
            {
                bankDirectory = candidate;
                return true;
            }
        }
        return false;
    };

    if (!tryRoot(juce::File::getCurrentWorkingDirectory()))
    {
        auto location = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
        for (int depth = 0; depth < 7 && !location.isRoot(); ++depth)
        {
            if (tryRoot(location))
                break;
            location = location.getParentDirectory();
        }
    }

    if (!bankDirectory.isDirectory())
    {
        DBG("GUITAR SAMPLES: folder not found; using the built-in fallback tone.");
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

void MainComponent::loadBackingSampleBanks()
{
    DBG("BACKING DRUM WAV BUILD: 2026-10-13-v6");
    for (auto& loaded : backingSamplesLoaded)
        loaded.store(false, std::memory_order_relaxed);
    for (auto& regions : backingSampleRegions)
        regions.clear();
    for (auto& map : backingSampleMap)
        map.fill(-1);

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

    juce::File soundsRoot;
    auto findSounds = [&soundsRoot](juce::File start)
    {
        juce::File firstSoundsFolder;
        for (int depth = 0; depth < 12 && start != start.getParentDirectory(); ++depth)
        {
            const auto candidate = start.getChildFile("Sounds");
            if (candidate.isDirectory())
            {
                if (!firstSoundsFolder.isDirectory()) firstSoundsFolder = candidate;
                const auto drums = candidate.getChildFile("Drums");
                if (drums.getChildFile("KICK_AC.wav").existsAsFile()
                    || drums.getChildFile("SNARE_LIVE.wav").existsAsFile())
                {
                    soundsRoot = candidate;
                    DBG("BACKING: selected Sounds folder with drum WAVs: " + soundsRoot.getFullPathName());
                    return true;
                }
            }
            start = start.getParentDirectory();
        }
        if (firstSoundsFolder.isDirectory()) soundsRoot = firstSoundsFolder;
        return soundsRoot.isDirectory();
    };
    findSounds(juce::File::getCurrentWorkingDirectory());
    if (!soundsRoot.isDirectory())
        findSounds(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory());
    if (!soundsRoot.isDirectory())
        DBG("BACKING: Sounds folder not found. Expected Sounds/Drums/*.wav.");

    // Load the user's dedicated one-shot drum kit from Sounds/Drums.
    // GM MIDI drum notes are mapped to these WAVs; no pitch-shifting is applied.
    {
        const auto drumsDirectory = soundsRoot.getChildFile("Drums");
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

    DBG("BACKING SFZ BANKS: bass and piano only; drums must use Sounds/Drums WAVs");
    struct BankSpec { int instrument; juce::String sfzName; };
    const BankSpec banks[] = {
        { 1, "FingerBassYR 20190930.sfz" },
        { 3, "UprightPianoKW-small-20190703.sfz" }
    };

    if (soundsRoot.isDirectory()) for (const auto& bank : banks)
    {
        juce::Array<juce::File> found;
        soundsRoot.findChildFiles(found, juce::File::findFiles, true, "*.sfz");
        juce::File sfz;
        for (const auto& candidate : found)
            if (candidate.getFileName().equalsIgnoreCase(bank.sfzName)) { sfz = candidate; break; }
        if (!sfz.existsAsFile())
        {
            DBG("BACKING SFZ: missing " + bank.sfzName + "; using procedural fallback for instrument " + juce::String(bank.instrument));
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
    const int samples = static_cast<int>(juce::jmin<int64_t>(reader->lengthInSamples,
        juce::jmin<int64_t>(maxSamplesForOneMinute,
            static_cast<int64_t>(std::numeric_limits<int>::max()))));
    track.audio.setSize(channels, samples);
    track.audio.clear();
    if (!reader->read(&track.audio, 0, samples, 0, true, true))
    {
        track.audio.setSize(0, 0);
        DBG("Failed reading external audio track: " + file.getFullPathName());
        return false;
    }
    track.sourceSampleRate = reader->sampleRate;
    track.loaded.store(true, std::memory_order_release);
    DBG("Loaded external audio track: " + file.getFullPathName()
        + " (" + juce::String(samples) + " samples, "
        + juce::String(reader->sampleRate) + " Hz, "
        + juce::String(static_cast<double>(samples) / reader->sampleRate, 2)
        + " seconds; capped at 60 seconds)");
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


    // Audio formats were registered in the constructor before the sample bank was loaded.

    //==========================================================================
    // Metronome sounds

    metronomeAccentReader.reset(
        audioFormatManager.createReaderFor(
            juce::File(
                "D:\\GuitarBattle\\GuitarBattle\\Sounds\\wooden-metronome-click.wav")));

    metronomeNormalReader.reset(
        audioFormatManager.createReaderFor(
            juce::File(
                "D:\\GuitarBattle\\GuitarBattle\\Sounds\\light-clicking-perc.wav")));

    //==========================================================================
    // Accent

    if (metronomeAccentReader != nullptr)
    {
        metronomeAccentBuffer.setSize(
            static_cast<int>(
                metronomeAccentReader->numChannels),
            static_cast<int>(
                metronomeAccentReader->lengthInSamples));

        metronomeAccentReader->read(
            &metronomeAccentBuffer,
            0,
            static_cast<int>(
                metronomeAccentReader->lengthInSamples),
            0,
            true,
            true);
    }

    //==========================================================================
    // Normal

    if (metronomeNormalReader != nullptr)
    {
        metronomeNormalBuffer.setSize(
            static_cast<int>(
                metronomeNormalReader->numChannels),
            static_cast<int>(
                metronomeNormalReader->lengthInSamples));

        metronomeNormalReader->read(
            &metronomeNormalBuffer,
            0,
            static_cast<int>(
                metronomeNormalReader->lengthInSamples),
            0,
            true,
            true);
    }

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

    if (gameState == GameState::Playing)
    {
        metronome.processScheduler(
            currentSample);
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

    const auto* samples =
        bufferToFill.buffer->getReadPointer(
            0,
            bufferToFill.startSample);

    //==========================================================================
    // Mono guitar -> stereo

    if (bufferToFill.buffer->getNumChannels() >= 2)
    {
        auto* left =
            bufferToFill.buffer->getWritePointer(
                0,
                bufferToFill.startSample);

        auto* right =
            bufferToFill.buffer->getWritePointer(
                1,
                bufferToFill.startSample);

        for (int i = 0;
            i < bufferToFill.numSamples;
            ++i)
        {
            right[i] =
                left[i];
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
            samples[i]);

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

    if (gameState == GameState::Playing
        && metronome.consumeTrigger())
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
        const int channels = juce::jmin(2, bufferToFill.buffer->getNumChannels());
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
            const int outputChannels = bufferToFill.buffer->getNumChannels();
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
            const int channels = bufferToFill.buffer->getNumChannels();
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
            const int channels = bufferToFill.buffer->getNumChannels();
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

        const int samplesToPlay =
            std::min(
                metronomeSamplesRemaining,
                std::min(
                    availableSamples,
                    bufferToFill.numSamples));

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
                    channel < bufferToFill.buffer->getNumChannels();
                    ++channel)
                {
                    bufferToFill.buffer->addSample(
                        channel,
                        bufferToFill.startSample + i,
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
    g.fillAll(UiColors::background);

    if (!backgroundFrame.isValid())
        return;

    // Fill the entire window while preserving aspect ratio: crop the sides or
    // top/bottom as needed instead of letterboxing or stretching the video.
    const auto dest = getLocalBounds().toFloat();
    const float imageW = static_cast<float>(backgroundFrame.getWidth());
    const float imageH = static_cast<float>(backgroundFrame.getHeight());
    const float destW = dest.getWidth();
    const float destH = dest.getHeight();
    if (imageW <= 0.0f || imageH <= 0.0f || destW <= 0.0f || destH <= 0.0f)
        return;

    const float imageAspect = imageW / imageH;
    const float destAspect = destW / destH;
    float sourceX = 0.0f, sourceY = 0.0f, sourceW = imageW, sourceH = imageH;
    if (imageAspect > destAspect)
    {
        sourceW = imageH * destAspect;
        sourceX = (imageW - sourceW) * 0.5f;
    }
    else if (imageAspect < destAspect)
    {
        sourceH = imageW / destAspect;
        sourceY = (imageH - sourceH) * 0.5f;
    }

    g.drawImage(backgroundFrame,
        0, 0, getWidth(), getHeight(),
        juce::roundToInt(sourceX), juce::roundToInt(sourceY),
        juce::roundToInt(sourceW), juce::roundToInt(sourceH));

    // Keep the still background subtle under the light blue/orange theme.
    g.setColour(juce::Colour::fromString("DDF8FAFF"));
    g.fillAll();

    if (gameState == GameState::Playing)
    {
        const int pedalWidth = 252;
        const int pedalX = (getWidth() - pedalWidth) / 2;
        const int pedalTop = 204 + 200 + 28;
        auto pedal = juce::Rectangle<float>((float) pedalX, (float) pedalTop,
                                            (float) pedalWidth, 142.0f);
        g.setColour(juce::Colour::fromRGB(170, 22, 32));
        g.fillRoundedRectangle(pedal, 12.0f);
        g.setColour(juce::Colour::fromRGB(245, 95, 95));
        g.drawRoundedRectangle(pedal, 12.0f, 2.0f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        g.drawText("DISTORTION", pedal.toNearestInt().withTrimmedBottom(112),
                   juce::Justification::centred, false);

        // A separate mixer panel sits directly below the distortion pedal.
        const int mixerWidth = juce::jmax(320, getWidth() - 48);
        const int mixerTop = pedalTop + 142 + 14;
        auto mixer = juce::Rectangle<float>(
            static_cast<float>((getWidth() - mixerWidth) / 2),
            static_cast<float>(mixerTop),
            static_cast<float>(mixerWidth), 190.0f);
        g.setColour(UiColors::card);
        g.fillRoundedRectangle(mixer, 12.0f);
        g.setColour(UiColors::border);
        g.drawRoundedRectangle(mixer, 12.0f, 1.5f);
        g.setColour(UiColors::blue);
        g.fillRoundedRectangle(juce::Rectangle<float>(
            mixer.getX() + 14.0f, mixer.getY() + 13.0f, 4.0f, 20.0f), 2.0f);
        g.setColour(UiColors::text);
        g.setFont(juce::FontOptions().withHeight(15.0f).withStyle("Bold"));
        g.drawText(juce::String::fromUTF8("\xD0\x9C\xD0\x98\xD0\x9A\xD0\xA8\xD0\x95\xD0\xA0"), mixer.toNearestInt().withTrimmedBottom(158).withTrimmedLeft(26),
                   juce::Justification::centredLeft, false);
    }

}

void MainOverlayComponent::paint(juce::Graphics& g)
{
    owner.paintOverlay(g);
}

void MainComponent::paintOverlay(juce::Graphics& g)
{
    // Transparent overlay: the looping video is the base layer. UI cards and
    // labels are drawn here so they remain visible above the video.

    //==========================================================================
    // Song select

    if (gameState == GameState::SongSelect)
    {
        g.setColour(
            UiColors::text);

        g.setFont(
            juce::FontOptions()
            .withHeight(28.0f)
            .withStyle("Bold"));

        g.drawText(
            "CHOOSE YOUR NEXT CHALLENGE",
            100,
            88,
            getWidth() - 200,
            42,
            juce::Justification::centred);

        // Song cards: restrained white surfaces and thin blue outline.
        const int cardWidth = juce::jmin(660, getWidth() - 96);
        const int cardX = (getWidth() - cardWidth) / 2;
        for (const int cardY : { 196 })
        {
            const juce::Rectangle<float> card(static_cast<float>(cardX),
                static_cast<float>(cardY), static_cast<float>(cardWidth), 92.0f);
            g.setColour(UiColors::card);
            g.fillRoundedRectangle(card, 16.0f);
            g.setColour(UiColors::border);
            g.drawRoundedRectangle(card, 16.0f, 1.2f);
        }

        return;
    }

    //==========================================================================
    // Tuner

    if (gameState == GameState::Tuner)
    {
        const float w = static_cast<float>(getWidth());
        const float h = static_cast<float>(getHeight());
        auto drawGlass = [&g](juce::Rectangle<float> r)
        {
            g.setColour(juce::Colour::fromString("080F172A"));
            g.fillRoundedRectangle(r.translated(0.0f, 4.0f), 20.0f);
            g.setColour(juce::Colour::fromString("F7FFFFFF"));
            g.fillRoundedRectangle(r, 20.0f);
            g.setColour(juce::Colour::fromString("553B82F6"));
            g.drawRoundedRectangle(r, 20.0f, 1.2f);
            g.setColour(juce::Colour::fromString("20FFFFFF"));
            g.drawLine(r.getX() + 18.0f, r.getY() + 2.0f, r.getRight() - 18.0f, r.getY() + 2.0f, 1.0f);
        };
        const float margin = 42.0f, gap = 22.0f, panelY = 122.0f;
        const float panelH = juce::jmax(300.0f, h - 255.0f);
        const float leftW = (w - margin * 2.0f - gap) * 0.43f;
        const float rightX = margin + leftW + gap;
        const float rightW = w - margin - rightX;
        drawGlass({ margin, panelY, leftW, panelH });
        drawGlass({ rightX, panelY, rightW, panelH });
        // Simple string labels: each label turns green once that string is tuned.
        g.setColour(UiColors::orange.withAlpha(0.85f));
        g.setFont(juce::FontOptions().withHeight(13.0f).withStyle("Bold"));
        g.drawText("STRINGS", juce::roundToInt(margin + 18.0f), juce::roundToInt(panelY + 22.0f),
                   juce::roundToInt(leftW - 36.0f), 22, juce::Justification::centred);

        g.setColour(UiColors::secondaryText);
        g.setFont(juce::FontOptions().withHeight(11.0f));
        g.drawText("Standard tuning: E A D G B E",
                   juce::roundToInt(margin + 18.0f), juce::roundToInt(panelY + 43.0f),
                   juce::roundToInt(leftW - 36.0f), 18, juce::Justification::centredLeft);

        const char* stringNames[6] = { "E", "A", "D", "G", "B", "E" };
        const char* stringNumbers[6] = { "6", "5", "4", "3", "2", "1" };
        const int tunedMask = tuner.getTunedMask();
        const float labelGap = 8.0f;
        const float labelMargin = 18.0f;
        const float labelWidth = (leftW - labelMargin * 2.0f - labelGap * 5.0f) / 6.0f;
        const float labelY = panelY + panelH * 0.43f;

        for (int i = 0; i < 6; ++i)
        {
            const float x = margin + labelMargin + static_cast<float>(i) * (labelWidth + labelGap);
            const bool tuned = (tunedMask & (1 << i)) != 0;
            const auto labelColour = tuned
                ? UiColors::green
                : UiColors::blue;

            g.setColour(labelColour.withAlpha(tuned ? 0.18f : 0.08f));
            g.fillRoundedRectangle(x, labelY, labelWidth, 66.0f, 10.0f);
            g.setColour(labelColour.withAlpha(0.75f));
            g.drawRoundedRectangle(x, labelY, labelWidth, 66.0f, 10.0f, 1.2f);
            g.setColour(labelColour);
            g.setFont(juce::FontOptions().withHeight(25.0f).withStyle("Bold"));
            g.drawText(stringNames[i], static_cast<int>(x), static_cast<int>(labelY + 5.0f),
                       static_cast<int>(labelWidth), 32, juce::Justification::centred);
            g.setColour(tuned ? UiColors::green : UiColors::secondaryText);
            g.setFont(juce::FontOptions().withHeight(11.0f));
            g.drawText(stringNumbers[i], static_cast<int>(x), static_cast<int>(labelY + 38.0f),
                       static_cast<int>(labelWidth), 18, juce::Justification::centred);
        }

        g.setColour(UiColors::secondaryText);
        g.setFont(juce::FontOptions().withHeight(12.0f));
        g.drawText("Play each string to check tuning", juce::roundToInt(margin + 12.0f),
                   juce::roundToInt(panelY + panelH - 48.0f), juce::roundToInt(leftW - 24.0f),
                   24, juce::Justification::centred);
        // The unreliable tuner gauge is intentionally hidden for now.
        g.setColour(
            UiColors::text);

        g.setFont(
            juce::FontOptions()
            .withHeight(28.0f)
            .withStyle("Bold"));

        g.drawText(
            "Tune your guitar",
            100,
            50,
            getWidth() - 200,
            40,
            juce::Justification::centred);

        return;
    }

    //==========================================================================
    // Results screen

    if (gameState == GameState::Finished)
    {
        const int score = gameplayStats.getScore();
        const int hits = gameplayStats.getHitCount();
        const int misses = gameplayStats.getMissCount();
        const int total = hits + misses;
        const double accuracy = total > 0
            ? 100.0 * static_cast<double>(hits) / static_cast<double>(total)
            : 0.0;

        auto card = juce::Rectangle<float>(
            48.0f, 48.0f,
            static_cast<float>(juce::jmax(320, getWidth() - 96)),
            static_cast<float>(juce::jmax(360, getHeight() - 150)));

        g.setColour(juce::Colour(0x18000000));
        g.fillRoundedRectangle(card.translated(0.0f, 4.0f), 22.0f);
        g.setColour(UiColors::card);
        g.fillRoundedRectangle(card, 22.0f);
        g.setColour(UiColors::border);
        g.drawRoundedRectangle(card, 22.0f, 1.2f);
        // Subtle score highlight keeps the main result immediately readable.
        auto scorePanel = juce::Rectangle<float>(
            80.0f, 128.0f, static_cast<float>(juce::jmax(240, getWidth() - 160)), 126.0f);
        g.setColour(UiColors::blueLight.withAlpha(0.55f));
        g.fillRoundedRectangle(scorePanel, 18.0f);
        g.setColour(UiColors::border);
        g.drawRoundedRectangle(scorePanel, 18.0f, 1.0f);

        g.setColour(UiColors::text);
        g.setFont(juce::FontOptions().withHeight(34.0f).withStyle("Bold"));
        g.drawText("SONG COMPLETE", 40, 76, getWidth() - 80, 48,
                   juce::Justification::centred);

        g.setColour(UiColors::blue);
        g.setFont(juce::FontOptions().withHeight(54.0f).withStyle("Bold"));
        g.drawText(juce::String(score), 60, 142, getWidth() - 120, 68,
                   juce::Justification::centred);

        g.setColour(UiColors::secondaryText);
        g.setFont(juce::FontOptions().withHeight(18.0f));
        g.drawText("TOTAL SCORE", 60, 207, getWidth() - 120, 28,
                   juce::Justification::centred);

        const int columnWidth = juce::jmax(100, (getWidth() - 100) / 3);
        const int startX = (getWidth() - columnWidth * 3) / 2;
        const int rowY = 270;
        auto drawStat = [&g](const juce::String& value, const juce::String& caption,
                             int x, int y, int w)
        {
            g.setColour(UiColors::text);
            g.setFont(juce::FontOptions().withHeight(30.0f).withStyle("Bold"));
            g.drawText(value, x, y, w, 38, juce::Justification::centred);
            g.setColour(UiColors::secondaryText);
            g.setFont(juce::FontOptions().withHeight(15.0f));
            g.drawText(caption, x, y + 38, w, 25, juce::Justification::centred);
        };

        drawStat(juce::String(juce::roundToInt(accuracy)) + "%", "ACCURACY", startX, rowY, columnWidth);
        drawStat(juce::String(hits), "HITS", startX + columnWidth, rowY, columnWidth);
        drawStat(juce::String(misses), "MISSES", startX + columnWidth * 2, rowY, columnWidth);

        g.setColour(UiColors::border);
        g.drawLine(80.0f, 365.0f, static_cast<float>(getWidth() - 80), 365.0f, 1.0f);

        const int qualityY = 395;
        drawStat(juce::String(gameplayStats.getPerfectCount()), "PERFECT", startX, qualityY, columnWidth);
        drawStat(juce::String(gameplayStats.getGreatCount()), "GREAT", startX + columnWidth, qualityY, columnWidth);
        drawStat(juce::String(gameplayStats.getGoodCount()), "GOOD", startX + columnWidth * 2, qualityY, columnWidth);

        g.setColour(UiColors::blue);
        g.setFont(juce::FontOptions().withHeight(25.0f).withStyle("Bold"));
        g.drawText("MAX COMBO  x" + juce::String(gameplayStats.getMaxCombo()),
                   60, 475, getWidth() - 120, 36, juce::Justification::centred);

        g.setColour(UiColors::secondaryText);
        g.setFont(juce::FontOptions().withHeight(14.0f));
        g.drawText("COMBO BONUS  +" + juce::String(gameplayStats.getComboBonusPoints()) + " pts",
                   60, 510, getWidth() - 120, 24, juce::Justification::centred);
        return;
    }

    //==========================================================================
    // Game

    auto drawCard =
        [&g](juce::Rectangle<float> area)
        {
            g.setColour(juce::Colour::fromString("183B82F6"));

            g.fillRoundedRectangle(
                area.translated(
                    0.0f,
                    3.0f),
                16.0f);

            // Translucent glass fill with a subtle cool highlight.
            g.setColour(UiColors::card);

            g.fillRoundedRectangle(
                area,
                16.0f);

            g.setColour(
                UiColors::border);

            g.drawRoundedRectangle(area, 16.0f, 1.25f);

            auto highlight = area.reduced(1.5f);
            g.setColour(juce::Colour::fromString("AAFFFFFF"));
            g.drawLine(highlight.getX() + 18.0f, highlight.getY() + 1.0f,
                       highlight.getRight() - 18.0f, highlight.getY() + 1.0f, 1.0f);
        };

    // Gameplay header and compact side panels.
    drawCard(juce::Rectangle<float>(24.0f, 24.0f, static_cast<float>(getWidth() - 48), 72.0f));

    // Pitch and target are deliberately compact, leaving the centre for song
    // information and performance feedback.
    drawCard(juce::Rectangle<float>(24.0f, 108.0f, 150.0f, 88.0f));
    drawCard(juce::Rectangle<float>(static_cast<float>(getWidth() - 174), 108.0f, 150.0f, 88.0f));
    drawCard(juce::Rectangle<float>(190.0f, 108.0f, static_cast<float>(juce::jmax(180, getWidth() - 380)), 88.0f));

    // Score and current hit feedback share a compact panel below the tablature.
    drawCard(juce::Rectangle<float>(24.0f, static_cast<float>(getHeight() - 142),
                                   static_cast<float>(getWidth() - 48), 112.0f));

    // Live accuracy counters under the song title.
    {
        const float statsX = 190.0f;
        const float statsWidth = static_cast<float>(juce::jmax(180, getWidth() - 380));
        const float cellWidth = statsWidth / 4.0f;
        const std::array<juce::String, 4> values
        {
            juce::String(gameplayStats.getMissCount()),
            juce::String(gameplayStats.getPerfectCount()),
            juce::String(gameplayStats.getGreatCount()),
            juce::String(gameplayStats.getGoodCount())
        };
        const std::array<juce::String, 4> captions { "MISS", "PERFECT", "GREAT", "GOOD" };
        for (int i = 0; i < 4; ++i)
        {
            g.setColour(UiColors::text);
            g.setFont(juce::FontOptions().withHeight(22.0f).withStyle("Bold"));
            g.drawText(values[static_cast<size_t>(i)],
                       static_cast<int>(statsX + cellWidth * i), 119,
                       static_cast<int>(cellWidth), 28, juce::Justification::centred);
            g.setColour(UiColors::secondaryText);
            g.setFont(juce::FontOptions().withHeight(12.0f).withStyle("Bold"));
            g.drawText(captions[static_cast<size_t>(i)],
                       static_cast<int>(statsX + cellWidth * i), 151,
                       static_cast<int>(cellWidth), 22, juce::Justification::centred);
        }
    }

    // Small headings for the pitch/target side cards.
    g.setColour(UiColors::secondaryText);
    g.setFont(juce::FontOptions().withHeight(12.0f).withStyle("Bold"));
    g.drawText("PITCH", 32, 112, 134, 18, juce::Justification::centred);
    g.drawText("TARGET", getWidth() - 166, 112, 134, 18, juce::Justification::centred);

    // Draw the whole COUNT IN card at the center of the entire game window.
    // This is painted by the full-size overlay, not clipped to the tab area.
    if (gameState == GameState::Playing
        && countInActive.load(std::memory_order_acquire))
    {
        const float cardWidth = 250.0f;
        const float cardHeight = 190.0f;
        const juce::Rectangle<float> countCard(
            (getWidth() - cardWidth) * 0.5f,
            (getHeight() - cardHeight) * 0.5f,
            cardWidth, cardHeight);

        g.setColour(juce::Colour::fromString("EFFFFFFF"));
        g.fillRoundedRectangle(countCard, 22.0f);
        g.setColour(juce::Colour::fromString("FF2563EB"));
        g.drawRoundedRectangle(countCard, 22.0f, 2.5f);
        g.setColour(UiColors::blue);
        g.setFont(juce::FontOptions().withHeight(19.0f).withStyle("Bold"));
        g.drawText("COUNT IN", countCard.toNearestInt().withY((int)countCard.getY() + 20).withHeight(30),
                   juce::Justification::centred, false);
        const int count = juce::jmax(1, countInRemaining.load(std::memory_order_relaxed));
        g.setColour(count <= 1 ? juce::Colour::fromString("FFF97316")
                               : juce::Colour::fromString("FF2563EB"));
        g.setFont(juce::FontOptions().withHeight(92.0f).withStyle("Bold"));
        g.drawText(juce::String(count),
                   countCard.toNearestInt().withY((int)countCard.getY() + 52).withHeight(112),
                   juce::Justification::centred, false);
    }

    // Pause screen: a clear, centered light card keeps the underlying game
    // visible but removes visual noise while the music is stopped.
    if (gameState == GameState::Paused)
    {
        g.setColour(juce::Colour::fromString("990C2145"));
        g.fillAll();

        const juce::Rectangle<float> pauseCard(
            getWidth() * 0.5f - 230.0f,
            getHeight() * 0.5f - 150.0f,
            460.0f, 300.0f);
        g.setColour(UiColors::card);
        g.fillRoundedRectangle(pauseCard, 24.0f);
        g.setColour(UiColors::border);
        g.drawRoundedRectangle(pauseCard, 24.0f, 1.5f);

        g.setColour(UiColors::blue);
        g.setFont(juce::FontOptions().withHeight(30.0f).withStyle("Bold"));
        g.drawText("GAME PAUSED", pauseCard.toNearestInt().withHeight(48).withY((int)pauseCard.getY() + 28), juce::Justification::centred);

        g.setColour(UiColors::secondaryText);
        g.setFont(juce::FontOptions().withHeight(16.0f));
        g.drawText("Take a breath. Your progress is saved for this session.",
                   (int)pauseCard.getX() + 24, (int)pauseCard.getY() + 90,
                   (int)pauseCard.getWidth() - 48, 28, juce::Justification::centred);

        g.setColour(UiColors::orange);
        g.fillRoundedRectangle(juce::Rectangle<float>(pauseCard.getX() + 74.0f,
            pauseCard.getY() + 150.0f, pauseCard.getWidth() - 148.0f, 5.0f), 2.5f);

        g.setColour(UiColors::text);
        g.setFont(juce::FontOptions().withHeight(14.0f).withStyle("Bold"));
        g.drawText("Press RESUME to continue", (int)pauseCard.getX() + 24,
                   (int)pauseCard.getY() + 182, (int)pauseCard.getWidth() - 48,
                   24, juce::Justification::centred);
    }


}

//==============================================================================
// Layout
//==============================================================================

void MainComponent::resized()
{
    overlayComponent.setBounds(getLocalBounds());

    const int width =
        getWidth();

    const bool authScreen = showAuthScreen;
    const bool songSelect =
        gameState == GameState::SongSelect && !authScreen;

    const bool tunerScreen =
        gameState == GameState::Tuner;

    const bool finishedScreen =
        gameState == GameState::Finished;

    const bool showGame =
        gameState == GameState::Playing
        || gameState == GameState::Paused;

    //==========================================================================
    // Song select

    songTitleLabel.setVisible(songSelect);
    loginScreenTitleLabel.setVisible(authScreen);
    currentUserLabel.setVisible(!authScreen && currentUsername.isNotEmpty());
    logoutButton.setVisible(!authScreen && currentUsername.isNotEmpty());
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
    songMistButton.setVisible(false);
    songChordsButton.setVisible(false);
    aiSoloButton.setVisible(false);
    oldGeneratorArchiveButton.setVisible(false);
    melodyStyleCombo.setVisible(false);
    generateMelodyButton.setVisible(false);
    newGenerationButton.setVisible(false);
    songPridumayBestLabel.setVisible(songSelect);
    songMistBestLabel.setVisible(false);
    songChordsBestLabel.setVisible(false);
    dailyChallengeBestLabel.setVisible(false);

    //==========================================================================
    // Tuner

    tunerTitleLabel.setVisible(
        tunerScreen);

    tunerNoteLabel.setVisible(
        tunerScreen);

    tunerFrequencyLabel.setVisible(
        tunerScreen);

    tunerStringLabel.setVisible(
        tunerScreen);

    tunerCentsLabel.setVisible(
        tunerScreen);

    tunerStatusLabel.setVisible(
        tunerScreen);

    tunerStartButton.setVisible(tunerScreen);
    tunerSkipButton.setVisible(tunerScreen);

    //==========================================================================
    // Game

    pitchLabel.setVisible(
        showGame);

    beatLabel.setVisible(
        showGame);

    barBeatLabel.setVisible(
        showGame);

    bpmLabel.setVisible(
        showGame);

    gameSessionLabel.setVisible(
        showGame);

    hitLabel.setVisible(
        showGame);

    targetNoteLabel.setVisible(
        showGame);

    gameStartButton.setVisible(
        showGame || finishedScreen);

    pauseButton.setVisible(
        showGame);

    menuButton.setVisible(
        showGame || finishedScreen);

    distortionButton.setVisible(showGame);
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
    distortionDriveSlider.setVisible(showGame);
    distortionToneSlider.setVisible(showGame);
    distortionDriveLabel.setVisible(showGame);
    distortionToneLabel.setVisible(showGame);

    speed50Button.setVisible(
        showGame);

    speed100Button.setVisible(
        showGame);

    speed120Button.setVisible(
        showGame);

    audioSettingsButton.setVisible(
        showGame);

    // Difficulty UI deliberately hidden.

    easyButton.setVisible(false);
    normalButton.setVisible(false);
    hardButton.setVisible(false);

    tablatureComponent.setVisible(
        showGame);

    //==========================================================================
    // Top bar

    beatLabel.setBounds(
        34,
        38,
        40,
        32);

    barBeatLabel.setBounds(
        78,
        38,
        40,
        32);

    bpmLabel.setBounds(
        128,
        38,
        55,
        32);

    speed50Button.setBounds(
        193,
        40,
        48,
        32);

    speed100Button.setBounds(
        247,
        40,
        52,
        32);

    speed120Button.setBounds(
        305,
        40,
        52,
        32);

    audioSettingsButton.setBounds(
        width - 154,
        40,
        120,
        32);

    gameStartButton.setBounds(
        375,
        40,
        78,
        32);

    pauseButton.setBounds(
        461,
        40,
        90,
        32);

    menuButton.setBounds(
        559,
        40,
        105,
        32);

    //==========================================================================
    // Gameplay header and compact HUD

    gameSongTitleLabel.setBounds(680, 34, juce::jmax(180, width - 870), 48);

    pitchLabel.setFont(juce::FontOptions().withHeight(20.0f).withStyle("Bold"));
    pitchLabel.setBounds(32, 128, 134, 34);

    targetNoteLabel.setFont(juce::FontOptions().withHeight(20.0f).withStyle("Bold"));
    targetNoteLabel.setBounds(width - 166, 128, 134, 34);

    // Status and score occupy separate rows with a clear visual gap.
    hitLabel.setBounds(32, getHeight() - 126, width - 64, 38);
    gameSessionLabel.setBounds(32, getHeight() - 72, width - 64, 46);

    // Give the tablature a little more top breathing room and only enough
    // height for the six strings and title. The lower area belongs to the pedal.
    constexpr int tabTop = 204;
    constexpr int tabHeight = 200;
    tablatureComponent.setBounds(24, tabTop, width - 48, tabHeight);

    // Small red stompbox: title, two knobs with values, then the footswitch.
    const int pedalWidth = 252;
    const int pedalHeight = 142;
    const int pedalX = (width - pedalWidth) / 2;
    const int pedalTop = tabTop + tabHeight + 28;
    distortionButton.setBounds(pedalX + 16, pedalTop + 108, pedalWidth - 32, 25);
    distortionDriveLabel.setBounds(pedalX + 14, pedalTop + 29, 104, 18);
    distortionToneLabel.setBounds(pedalX + 134, pedalTop + 29, 104, 18);
    distortionDriveSlider.setBounds(pedalX + 12, pedalTop + 43, 108, 64);
    distortionToneSlider.setBounds(pedalX + 132, pedalTop + 43, 108, 64);
    // Full-width mixer: three columns, two rows, with readable dark value boxes.
    const int mixerWidth = juce::jmax(320, width - 48);
    const int mixerX = (width - mixerWidth) / 2;
    const int mixerTop = pedalTop + pedalHeight + 14;
    const int colGap = 12;
    const int cellWidth = (mixerWidth - 28 - 2 * colGap) / 3;
    auto layoutMixerChannel = [&](juce::TextButton& button, juce::Label& label,
                                  juce::Slider& slider, int column, int row)
    {
        const int x = mixerX + 14 + column * (cellWidth + colGap);
        const int y = mixerTop + 34 + row * 78;
        button.setBounds(x, y, cellWidth, 23);
        label.setBounds(x, y + 25, 74, 16);
        slider.setBounds(x + 72, y + 23, juce::jmax(64, cellWidth - 74), 22);
    };
    layoutMixerChannel(midiBackingButton, midiBackingVolumeLabel, midiBackingVolumeSlider, 0, 0);
    layoutMixerChannel(midiDrumsButton, midiDrumsVolumeLabel, midiDrumsVolumeSlider, 1, 0);
    layoutMixerChannel(midiBassButton, midiBassVolumeLabel, midiBassVolumeSlider, 2, 0);
    layoutMixerChannel(midiRhythmGuitarButton, midiRhythmGuitarVolumeLabel, midiRhythmGuitarVolumeSlider, 0, 1);
    layoutMixerChannel(midiPianoButton, midiPianoVolumeLabel, midiPianoVolumeSlider, 1, 1);
    layoutMixerChannel(metronomeMixerButton, metronomeMixerVolumeLabel, metronomeMixerVolumeSlider, 2, 1);

    //==========================================================================
    // Song select

    songTitleLabel.setBounds(100, 120, width - 200, 60);
    loginScreenTitleLabel.setBounds((width - 420) / 2, 230, 420, 58);
    accountStatusLabel.setBounds((width - 420) / 2, 292, 420, 34);
    accountLoginButton.setBounds((width - 420) / 2 + 20, 344, 180, 42);
    accountRegisterButton.setBounds((width - 420) / 2 + 220, 344, 180, 42);
    currentUserLabel.setBounds(18, 12, juce::jmax(200, width - 150), 28);
    logoutButton.setBounds(width - 122, 10, 104, 30);

    const int songCardWidth = juce::jmin(620, width - 120);
    const int songCardX = (width - songCardWidth) / 2;
    songPridumayButton.setBounds(songCardX + 14, 210, songCardWidth - 28, 42);
    songPridumayBestLabel.setBounds(songCardX + 14, 252, songCardWidth - 28, 24);
    onlineCatalogButton.setBounds(songCardX + 14, 292, songCardWidth - 28, 42);
    onlineCatalogStatusLabel.setBounds(songCardX + 14, 338, songCardWidth - 28, 28);
    accountStatusLabel.setBounds(songCardX + 14, 368, songCardWidth - 230, 30);
    accountLoginButton.setBounds(songCardX + songCardWidth - 208, 368, 92, 30);
    accountRegisterButton.setBounds(songCardX + songCardWidth - 108, 368, 94, 30);
    leaderboardTitleLabel.setBounds(songCardX + 14, 405, songCardWidth - 28, 30);
    refreshLeaderboardButton.setBounds(songCardX + songCardWidth - 174, 440, 160, 34);
    leaderboardLabel.setBounds(songCardX + 24, 440, songCardWidth - 220, 375);


    //==========================================================================
    // Tuner

    if (tunerScreen)
    {
        const int tunerLeft = 42;
        const int tunerGap = 22;
        const int tunerLeftWidth = (width - 84 - tunerGap) * 43 / 100;
        const int tunerRightX = tunerLeft + tunerLeftWidth + tunerGap;
        const int tunerRightWidth = width - 42 - tunerRightX;
        tunerTitleLabel.setBounds(tunerRightX + 12, 126, tunerRightWidth - 24, 28);
        tunerNoteLabel.setBounds(tunerRightX + 12, 210, tunerRightWidth - 24, 76);
        tunerFrequencyLabel.setBounds(tunerRightX + 12, 282, tunerRightWidth - 24, 28);
        tunerStringLabel.setBounds(tunerRightX + 12, 318, tunerRightWidth - 24, 30);
        tunerCentsLabel.setBounds(tunerRightX + 12, 350, tunerRightWidth - 24, 30);
        tunerStatusLabel.setBounds(tunerRightX + 12, 388, tunerRightWidth - 24, 40);
        const int tunerButtonWidth = juce::jmin(250, (width - 100) / 2);
        const int tunerButtonGap = 18;
        const int tunerButtonsX = (width - (tunerButtonWidth * 2 + tunerButtonGap)) / 2;
        tunerSkipButton.setBounds(tunerButtonsX, getHeight() - 76, tunerButtonWidth, 46);
        tunerStartButton.setBounds(tunerButtonsX + tunerButtonWidth + tunerButtonGap, getHeight() - 76, tunerButtonWidth, 46);
    }

    if (authScreen)
    {
        const int cardWidth = juce::jmin(460, width - 48);
        const int cardX = (width - cardWidth) / 2;
        loginScreenTitleLabel.setBounds(cardX, 230, cardWidth, 58);
        accountStatusLabel.setBounds(cardX + 20, 292, cardWidth - 40, 34);
        accountLoginButton.setBounds(cardX + 20, 344, (cardWidth - 50) / 2, 42);
        accountRegisterButton.setBounds(cardX + 30 + (cardWidth - 50) / 2, 344,
                                        (cardWidth - 50) / 2, 42);
    }

    if (finishedScreen)
    {
        const int buttonWidth = juce::jmin(180, (width - 100) / 2);
        const int gap = 20;
        const int totalWidth = buttonWidth * 2 + gap;
        const int left = (width - totalWidth) / 2;
        const int y = getHeight() - 82;
        gameStartButton.setBounds(left, y, buttonWidth, 44);
        menuButton.setBounds(left + buttonWidth + gap, y, buttonWidth, 44);
    }
}
