#include "MainComponent.h"

#include <vector>

//==============================================================================
// UI palette
//
// Основная идея:
// - почти белый холодный фон;
// - белые карточки;
// - тёмно-синий текст;
// - синий как основной accent;
// - зелёный только для HIT;
// - красный зарезервирован под MISS.
//
// Цвета собраны здесь, чтобы их было легко менять позже.
//==============================================================================

namespace UiColors
{
    // Background of the whole application.
    const juce::Colour background =
        juce::Colour::fromString("FFF3F5F7");

    // Main white cards.
    const juce::Colour card =
        juce::Colour::fromString("FFFFFFFF");

    // Very light border around cards.
    const juce::Colour border =
        juce::Colour::fromString("FFE2E8F0");

    // Main dark text.
    const juce::Colour text =
        juce::Colour::fromString("FF1E293B");

    // Secondary text.
    const juce::Colour secondaryText =
        juce::Colour::fromString("FF64748B");

    // Main application blue.
    const juce::Colour blue =
        juce::Colour::fromString("FF2563EB");

    // Darker blue for active UI elements.
    const juce::Colour blueDark =
        juce::Colour::fromString("FF1D4ED8");

    // Light blue used for selected / active areas.
    const juce::Colour blueLight =
        juce::Colour::fromString("FFEFF6FF");

    // Successful hit.
    const juce::Colour green =
        juce::Colour::fromString("FF16A34A");

    // Miss state reserved for later gameplay logic.
    const juce::Colour red =
        juce::Colour::fromString("FFDC2626");
}

//==============================================================================
// Constructor
//==============================================================================

MainComponent::MainComponent()
{
    //--------------------------------------------------------------------------
    // Main window size

    setSize(900, 650);

    //--------------------------------------------------------------------------
    // Pitch
    //
    // Большой центральный элемент интерфейса.
    // Сначала показываем "---", пока нота не определена.

    pitchLabel.setText(
        "Pitch: ---",
        juce::dontSendNotification);

    pitchLabel.setFont(
        juce::Font(
            42.0f,
            juce::Font::bold));

    pitchLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    pitchLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(pitchLabel);

    //--------------------------------------------------------------------------
    // Beat
    //
    // Только маленькая цифра.
    // Никакого "Beat:" и никакого визуального акцента
    // на первую долю.

    beatLabel.setText(
        "0",
        juce::dontSendNotification);

    beatLabel.setFont(
        juce::Font(
            16.0f,
            juce::Font::bold));

    beatLabel.setColour(
        juce::Label::textColourId,
        UiColors::secondaryText);

    beatLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(beatLabel);

    //--------------------------------------------------------------------------
    // Bar beat
    //
    // Также только цифра.

    barBeatLabel.setText(
        "0",
        juce::dontSendNotification);

    barBeatLabel.setFont(
        juce::Font(
            16.0f,
            juce::Font::bold));

    barBeatLabel.setColour(
        juce::Label::textColourId,
        UiColors::secondaryText);

    barBeatLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(barBeatLabel);

    //--------------------------------------------------------------------------
    // BPM
    //
    // Вместо обычного горизонтального slider используем
    // компактный числовой control.
    //
    // Он поддерживает:
    // - ручной ввод;
    // - mouse wheel;
    // - increment/decrement buttons.

    bpmLabel.setText(
        "BPM",
        juce::dontSendNotification);

    bpmLabel.setFont(
        juce::Font(
            13.0f,
            juce::Font::bold));

    bpmLabel.setColour(
        juce::Label::textColourId,
        UiColors::secondaryText);

    bpmLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(bpmLabel);

    bpmSlider.setRange(
        40.0,
        240.0,
        1.0);

    bpmSlider.setValue(
        bpm.load());

    // Number field with small increment/decrement buttons.
    // Mouse wheel is also supported by JUCE Slider.
    bpmSlider.setSliderStyle(
        juce::Slider::IncDecButtons);

    // Editable text box.
    bpmSlider.setTextBoxStyle(
        juce::Slider::TextBoxLeft,
        false,
        70,
        38);

    bpmSlider.setColour(
        juce::Slider::textBoxTextColourId,
        UiColors::text);

    bpmSlider.setColour(
        juce::Slider::textBoxBackgroundColourId,
        UiColors::card);

    bpmSlider.setColour(
        juce::Slider::textBoxOutlineColourId,
        UiColors::border);

    bpmSlider.setColour(
        juce::Slider::thumbColourId,
        UiColors::blue);

    addAndMakeVisible(bpmSlider);

    bpmSlider.onValueChange = [this]
        {
            const auto newBpm =
                bpmSlider.getValue();

            bpm.store(
                newBpm,
                std::memory_order_relaxed);

            // BPM changes beat length in audio samples.
            auto* device =
                deviceManager.getCurrentAudioDevice();

            if (device != nullptr)
            {
                const double sampleRate =
                    device->getCurrentSampleRate();

                beatLengthSamples.store(
                    static_cast<uint64_t>(
                        sampleRate * 60.0 / newBpm),
                    std::memory_order_relaxed);
            }
        };

    //--------------------------------------------------------------------------
    // Score
    //
    // Score находится отдельно от status block.

    scoreLabel.setText(
        "0",
        juce::dontSendNotification);

    scoreLabel.setFont(
        juce::Font(
            30.0f,
            juce::Font::bold));

    scoreLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    scoreLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(scoreLabel);

    //--------------------------------------------------------------------------
    // Hit / Ready / Miss status

    hitLabel.setText(
        "READY",
        juce::dontSendNotification);

    hitLabel.setFont(
        juce::Font(
            24.0f,
            juce::Font::bold));

    hitLabel.setColour(
        juce::Label::textColourId,
        UiColors::blue);

    hitLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(hitLabel);

    //--------------------------------------------------------------------------
    // Target note

    targetNoteLabel.setText(
        "E2",
        juce::dontSendNotification);

    targetNoteLabel.setFont(
        juce::Font(
            34.0f,
            juce::Font::bold));

    targetNoteLabel.setColour(
        juce::Label::textColourId,
        UiColors::blueDark);

    targetNoteLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(targetNoteLabel);

    //--------------------------------------------------------------------------
    // Application properties

    juce::PropertiesFile::Options options;

    options.applicationName =
        "GuitarBattle";

    options.filenameSuffix =
        ".settings";

    options.osxLibrarySubFolder =
        "Application Support";

    options.folderName =
        "GuitarBattle";

    options.commonToAllUsers =
        false;

    appProperties.setStorageParameters(
        options);

    //--------------------------------------------------------------------------
    // Audio settings button

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

    audioSettingsButton.onClick = [this]
        {
            juce::DialogWindow::LaunchOptions options;

            options.content.setOwned(
                new juce::AudioDeviceSelectorComponent(
                    deviceManager,
                    0,
                    2,
                    0,
                    2,
                    true,
                    true,
                    true,
                    false));

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

    //--------------------------------------------------------------------------
    // Restore saved audio device settings

    auto savedState =
        appProperties.getUserSettings()
        ->getXmlValue(
            "AudioDeviceState");

    setAudioChannels(
        2,
        2,
        savedState.get());

    //--------------------------------------------------------------------------
    // Start background pitch detection

    pitchThread.startThread();
}

//==============================================================================
// Destructor
//==============================================================================

MainComponent::~MainComponent()
{
    pitchThread.stopThread(1000);

    // Save audio device state.
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
// Audio initialization
//==============================================================================

void MainComponent::prepareToPlay(
    int samplesPerBlockExpected,
    double sampleRate)
{
    juce::ignoreUnused(
        samplesPerBlockExpected);

    //--------------------------------------------------------------------------
    // WAV support

    audioFormatManager.registerBasicFormats();

    //--------------------------------------------------------------------------
    // Accent metronome

    metronomeAccentReader.reset(
        audioFormatManager.createReaderFor(
            juce::File(
                "D:\\GuitarBattle\\GuitarBattle\\Source\\Sounds\\wooden-metronome-click.wav")));

    //--------------------------------------------------------------------------
    // Normal metronome

    metronomeNormalReader.reset(
        audioFormatManager.createReaderFor(
            juce::File(
                "D:\\GuitarBattle\\GuitarBattle\\Source\\Sounds\\light-clicking-perc.wav")));

    //--------------------------------------------------------------------------
    // Copy accent WAV into memory

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

    //--------------------------------------------------------------------------
    // Copy normal WAV into memory

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

    //--------------------------------------------------------------------------
    // Pitch buffers

    pitchBuffer.setSize(
        1,
        2048);

    pitchBuffer.clear();

    pitchAnalysisBuffer.setSize(
        1,
        2048);

    pitchAnalysisBuffer.clear();

    pitchAnalysisBuffer2.setSize(
        1,
        2048);

    pitchAnalysisBuffer2.clear();

    pitchBufferPosition = 0;

    pitchBufferReady.store(false);
    pitchBufferReady2.store(false);

    //--------------------------------------------------------------------------
    // Beat duration

    beatLengthSamples.store(
        static_cast<uint64_t>(
            sampleRate
            * 60.0
            / bpm.load(
                std::memory_order_relaxed)),
        std::memory_order_relaxed);

    //--------------------------------------------------------------------------
    // Reset beat counters

    beatCounter.store(
        0,
        std::memory_order_relaxed);

    beatInBar.store(
        0,
        std::memory_order_relaxed);

    //--------------------------------------------------------------------------
    // First scheduled beat

    nextBeatSample.store(
        static_cast<uint64_t>(
            sampleRate
            * 60.0
            / bpm.load(
                std::memory_order_relaxed)),
        std::memory_order_relaxed);
}

//==============================================================================
// Audio callback
//
// ВАЖНО:
// Здесь не выполняется pitch detection.
//
// Callback только:
// - считает samples;
// - запускает beats;
// - копирует guitar input;
// - проигрывает уже загруженный metronome WAV.
//
// Pitch detection выполняется в PitchThread.
//==============================================================================

void MainComponent::getNextAudioBlock(
    const juce::AudioSourceChannelInfo& bufferToFill)
{
    if (bufferToFill.buffer == nullptr)
        return;

    //--------------------------------------------------------------------------
    // Update global sample counter

    audioSampleCounter.fetch_add(
        static_cast<uint64_t>(
            bufferToFill.numSamples),
        std::memory_order_relaxed);

    //--------------------------------------------------------------------------
    // Initialize beat scheduler

    if (nextBeatSample.load(
        std::memory_order_relaxed) == 0)
    {
        nextBeatSample.store(
            audioSampleCounter.load(
                std::memory_order_relaxed)
            + beatLengthSamples.load(
                std::memory_order_relaxed),
            std::memory_order_relaxed);
    }

    const auto currentSample =
        audioSampleCounter.load(
            std::memory_order_relaxed);

    const auto beatSample =
        nextBeatSample.load(
            std::memory_order_relaxed);

    //--------------------------------------------------------------------------
    // Beat scheduler

    if (currentSample >= beatSample)
    {
        beatCounter.fetch_add(
            1,
            std::memory_order_relaxed);

        const int currentBeatInBar =
            beatInBar.fetch_add(
                1,
                std::memory_order_relaxed)
            + 1;

        if (currentBeatInBar >= 4)
        {
            beatInBar.store(
                0,
                std::memory_order_relaxed);
        }

        const auto beatNumber =
            beatCounter.load(
                std::memory_order_relaxed);

        const int barBeatNumber =
            currentBeatInBar == 0
            ? 4
            : currentBeatInBar;

        //--------------------------------------------------------------------------
        // Accent is still used for the audio.
        //
        // Визуального акцента первой доли больше нет.

        metronomeAccent.store(
            barBeatNumber == 1,
            std::memory_order_relaxed);

        metronomeTrigger.store(
            true,
            std::memory_order_relaxed);

        //--------------------------------------------------------------------------
        // Update small beat counters in GUI.

        juce::MessageManager::callAsync(
            [this, beatNumber, barBeatNumber]
            {
                beatLabel.setText(
                    juce::String(beatNumber),
                    juce::dontSendNotification);

                barBeatLabel.setText(
                    juce::String(barBeatNumber),
                    juce::dontSendNotification);
            });

        //--------------------------------------------------------------------------
        // Schedule next beat

        nextBeatSample.store(
            beatSample
            + beatLengthSamples.load(
                std::memory_order_relaxed),
            std::memory_order_relaxed);
    }

    //--------------------------------------------------------------------------
    // Input samples
    //
    // Guitar input is read from channel 0.
    //
    // ВАЖНО:
    // Сначала сохраняем оригинальный guitar signal,
    // и только потом работаем с output.
    //
    // Это сохраняет защиту от попадания метронома
    // в pitch detection.

    auto* samples =
        bufferToFill.buffer->getReadPointer(0);

    //--------------------------------------------------------------------------
    // Mono guitar -> stereo output
    //
    // Один guitar signal отправляется одновременно
    // в Left и Right.
    //
    // Pitch detection по-прежнему использует только
    // оригинальный channel 0.

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
            right[i] = left[i];
        }
    }

    //--------------------------------------------------------------------------
    // Copy guitar input into pitch analysis buffer

    for (int i = 0;
        i < bufferToFill.numSamples;
        ++i)
    {
        pitchBuffer.setSample(
            0,
            pitchBufferPosition,
            samples[i]);

        ++pitchBufferPosition;

        if (pitchBufferPosition >= 2048)
        {
            //----------------------------------------------------------------------
            // First analysis buffer

            if (!pitchBufferReady.load())
            {
                auto* destination =
                    pitchAnalysisBuffer.getWritePointer(0);

                const auto* source =
                    pitchBuffer.getReadPointer(0);

                for (int sample = 0;
                    sample < 2048;
                    ++sample)
                {
                    destination[sample] =
                        source[sample];
                }

                pitchAnalysisStartSample.store(
                    audioSampleCounter.load(
                        std::memory_order_relaxed)
                    - 2048,
                    std::memory_order_relaxed);

                pitchBufferReady.store(true);
            }

            //----------------------------------------------------------------------
            // Second analysis buffer

            else if (!pitchBufferReady2.load())
            {
                auto* destination =
                    pitchAnalysisBuffer2.getWritePointer(0);

                const auto* source =
                    pitchBuffer.getReadPointer(0);

                for (int sample = 0;
                    sample < 2048;
                    ++sample)
                {
                    destination[sample] =
                        source[sample];
                }

                pitchAnalysisStartSample2.store(
                    audioSampleCounter.load(
                        std::memory_order_relaxed)
                    - 2048,
                    std::memory_order_relaxed);

                pitchBufferReady2.store(true);
            }

            pitchBufferPosition = 0;
        }
    }

    //--------------------------------------------------------------------------
    // Start metronome playback when a beat occurs

    if (metronomeTrigger.exchange(
        false,
        std::memory_order_relaxed))
    {
        metronomePlayingAccent =
            metronomeAccent.load(
                std::memory_order_relaxed);

        metronomePlaybackPosition = 0;

        const auto& buffer =
            metronomePlayingAccent
            ? metronomeAccentBuffer
            : metronomeNormalBuffer;

        metronomeSamplesRemaining =
            buffer.getNumSamples();
    }

    //--------------------------------------------------------------------------
    // Play metronome WAV
    //
    // Mono metronome is duplicated to all output channels.
    //
    // Playback position is advanced only once per sample,
    // not once per channel.

    if (metronomeSamplesRemaining > 0)
    {
        const auto& buffer =
            metronomePlayingAccent
            ? metronomeAccentBuffer
            : metronomeNormalBuffer;

        const int samplesToCopy =
            std::min(
                metronomeSamplesRemaining,
                bufferToFill.numSamples);

        const float volume =
            metronomePlayingAccent
            ? 0.30f
            : 0.18f;

        for (int i = 0;
            i < samplesToCopy;
            ++i)
        {
            const float metronomeSample =
                volume
                * buffer.getSample(
                    0,
                    metronomePlaybackPosition);

            for (int channel = 0;
                channel < bufferToFill.buffer->getNumChannels();
                ++channel)
            {
                auto* output =
                    bufferToFill.buffer->getWritePointer(
                        channel,
                        bufferToFill.startSample);

                output[i] += metronomeSample;
            }

            ++metronomePlaybackPosition;
            --metronomeSamplesRemaining;
        }
    }
}

//==============================================================================
// RMS detection
//==============================================================================

double MainComponent::detectRms(
    const float* samples,
    int numSamples)
{
    if (samples == nullptr
        || numSamples <= 0)
    {
        return 0.0;
    }

    double sum = 0.0;

    for (int i = 0;
        i < numSamples;
        ++i)
    {
        const double x =
            static_cast<double>(samples[i]);

        sum += x * x;
    }

    return std::sqrt(
        sum / static_cast<double>(numSamples));
}

//==============================================================================
// Pitch detection
//==============================================================================

double MainComponent::detectPitch(
    const float* samples,
    int numSamples,
    double sampleRate)
{
    if (samples == nullptr
        || numSamples < 1024
        || sampleRate <= 0.0)
    {
        return 0.0;
    }

    //--------------------------------------------------------------------------
    // Mean / DC offset

    double mean = 0.0;

    for (int i = 0;
        i < numSamples;
        ++i)
    {
        mean += samples[i];
    }

    mean /=
        static_cast<double>(numSamples);

    //--------------------------------------------------------------------------
    // RMS

    double rms = 0.0;

    for (int i = 0;
        i < numSamples;
        ++i)
    {
        const double x =
            static_cast<double>(samples[i])
            - mean;

        rms += x * x;
    }

    rms =
        std::sqrt(
            rms / static_cast<double>(numSamples));

    if (rms < 0.005)
        return 0.0;

    //--------------------------------------------------------------------------
    // Search range: 60 Hz - 500 Hz

    const int minLag =
        static_cast<int>(
            sampleRate / 500.0);

    const int maxLag =
        static_cast<int>(
            sampleRate / 60.0);

    if (minLag < 1
        || maxLag >= numSamples)
    {
        return 0.0;
    }

    //--------------------------------------------------------------------------
    // Difference function

    std::vector<double> difference(
        maxLag + 1,
        0.0);

    std::vector<double> cmndf(
        maxLag + 1,
        1.0);

    for (int lag = 1;
        lag <= maxLag;
        ++lag)
    {
        double sum = 0.0;

        for (int i = 0;
            i < numSamples - lag;
            ++i)
        {
            const double a =
                static_cast<double>(samples[i])
                - mean;

            const double b =
                static_cast<double>(samples[i + lag])
                - mean;

            const double delta =
                a - b;

            sum += delta * delta;
        }

        difference[lag] = sum;
    }

    //--------------------------------------------------------------------------
    // CMNDF

    double runningSum = 0.0;

    for (int lag = 1;
        lag <= maxLag;
        ++lag)
    {
        runningSum += difference[lag];

        if (runningSum > 0.0)
        {
            cmndf[lag] =
                difference[lag]
                * static_cast<double>(lag)
                / runningSum;
        }
    }

    //--------------------------------------------------------------------------
    // Find first strong minimum

    int bestLag = 0;

    for (int lag = minLag;
        lag <= maxLag;
        ++lag)
    {
        if (cmndf[lag] < 0.10)
        {
            bestLag = lag;

            while (bestLag + 1 <= maxLag
                && cmndf[bestLag + 1]
                < cmndf[bestLag])
            {
                ++bestLag;
            }

            break;
        }
    }

    //--------------------------------------------------------------------------
    // Fallback: absolute minimum

    if (bestLag == 0)
    {
        double bestValue = 1.0;

        for (int lag = minLag;
            lag <= maxLag;
            ++lag)
        {
            if (cmndf[lag] < bestValue)
            {
                bestValue = cmndf[lag];
                bestLag = lag;
            }
        }
    }

    if (bestLag == 0)
        return 0.0;

    //--------------------------------------------------------------------------
    // Period -> frequency

    return sampleRate
        / static_cast<double>(bestLag);
}

//==============================================================================
// Convert frequency to musical note
//==============================================================================

juce::String MainComponent::pitchToNote(
    double frequency)
{
    if (frequency <= 0.0)
        return "--";

    const double midi =
        69.0
        + 12.0
        * std::log2(
            frequency / 440.0);

    const int midiNote =
        static_cast<int>(
            std::round(midi));

    static const char* noteNames[] =
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
        (midiNote % 12 + 12) % 12;

    const int octave =
        (midiNote / 12) - 1;

    return juce::String(
        noteNames[noteIndex])
        + juce::String(octave);
}

//==============================================================================
// Release audio resources
//==============================================================================

void MainComponent::releaseResources()
{
}

//==============================================================================
// Main UI painting
//
// Здесь рисуются:
// - фон;
// - карточка верхней панели;
// - карточка Pitch;
// - карточка Target;
// - карточка Status;
// - карточка Score.
//
// Сам текст рисуют Label-компоненты.
//==============================================================================

void MainComponent::paint(
    juce::Graphics& g)
{
    //--------------------------------------------------------------------------
    // Background

    g.fillAll(
        UiColors::background);

    //--------------------------------------------------------------------------
    // Helper for drawing modern rounded cards.

    auto drawCard =
        [&g](juce::Rectangle<float> area)
        {
            // Very soft shadow.
            g.setColour(
                juce::Colour(0x12000000));

            g.fillRoundedRectangle(
                area.translated(0.0f, 3.0f),
                16.0f);

            // White card.
            g.setColour(
                UiColors::card);

            g.fillRoundedRectangle(
                area,
                16.0f);

            // Thin border.
            g.setColour(
                UiColors::border);

            g.drawRoundedRectangle(
                area,
                16.0f,
                1.0f);
        };

    //--------------------------------------------------------------------------
    // Top bar card

    drawCard(
        juce::Rectangle<float>(
            24.0f,
            24.0f,
            static_cast<float>(getWidth() - 48),
            72.0f));

    //--------------------------------------------------------------------------
    // Pitch card

    drawCard(
        juce::Rectangle<float>(
            24.0f,
            120.0f,
            static_cast<float>(getWidth() - 48),
            150.0f));

    //--------------------------------------------------------------------------
    // Target card

    drawCard(
        juce::Rectangle<float>(
            24.0f,
            294.0f,
            static_cast<float>(getWidth() - 48),
            100.0f));

    //--------------------------------------------------------------------------
    // Status card

    drawCard(
        juce::Rectangle<float>(
            24.0f,
            418.0f,
            static_cast<float>(getWidth() - 48),
            90.0f));

    //--------------------------------------------------------------------------
    // Score card

    drawCard(
        juce::Rectangle<float>(
            24.0f,
            532.0f,
            static_cast<float>(getWidth() - 48),
            90.0f));
}

//==============================================================================
// UI layout
//==============================================================================

void MainComponent::resized()
{
    const int width =
        getWidth();

    //--------------------------------------------------------------------------
    // Top information bar
    //
    // Beat / Bar beat are deliberately small.
    // Audio Settings is placed on the right.

    beatLabel.setBounds(
        34,
        38,
        55,
        32);

    barBeatLabel.setBounds(
        92,
        38,
        55,
        32);

    bpmLabel.setBounds(
        170,
        31,
        55,
        24);

    bpmSlider.setBounds(
        225,
        32,
        120,
        42);

    audioSettingsButton.setBounds(
        width - 154,
        40,
        120,
        32);

    //--------------------------------------------------------------------------
    // Pitch
    //
    // Large central value.

    pitchLabel.setBounds(
        40,
        155,
        width - 80,
        80);

    //--------------------------------------------------------------------------
    // Target

    targetNoteLabel.setBounds(
        40,
        314,
        width - 80,
        60);

    //--------------------------------------------------------------------------
    // Status

    hitLabel.setBounds(
        40,
        432,
        width - 80,
        62);

    //--------------------------------------------------------------------------
    // Score

    scoreLabel.setBounds(
        40,
        546,
        width - 80,
        60);
}