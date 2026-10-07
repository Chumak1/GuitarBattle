#include "MainComponent.h"

#include <vector>

//==============================================================================
// Constructor
//
// Создаём интерфейс, настраиваем BPM, аудиоустройство и запускаем
// background thread для определения pitch.
//==============================================================================
MainComponent::MainComponent()
{
    setSize(700, 500);

    //==========================================================================
    // Pitch display

    pitchLabel.setText(
        "Pitch: -- Hz",
        juce::dontSendNotification);

    pitchLabel.setFont(32.0f);
    pitchLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(pitchLabel);

    //==========================================================================
    // Beat display

    beatLabel.setText(
        "Beat: 0",
        juce::dontSendNotification);

    beatLabel.setFont(24.0f);
    beatLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(beatLabel);

    //==========================================================================
    // Beat inside bar

    barBeatLabel.setText(
        "Bar beat: 0",
        juce::dontSendNotification);

    barBeatLabel.setFont(24.0f);
    barBeatLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(barBeatLabel);

    //==========================================================================
    // BPM display

    bpmLabel.setText(
        "BPM: " + juce::String(bpm.load()),
        juce::dontSendNotification);

    bpmLabel.setFont(24.0f);
    bpmLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(bpmLabel);

    //==========================================================================
    // Score

    scoreLabel.setText(
        "Score: 0",
        juce::dontSendNotification);

    scoreLabel.setFont(24.0f);
    scoreLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(scoreLabel);

    //==========================================================================
    // Hit feedback

    hitLabel.setText(
        "Ready",
        juce::dontSendNotification);

    hitLabel.setFont(28.0f);
    hitLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(hitLabel);

    //==========================================================================
    // Target note

    targetNoteLabel.setText(
        "Target: E2",
        juce::dontSendNotification);

    targetNoteLabel.setFont(28.0f);
    targetNoteLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(targetNoteLabel);

    //==========================================================================
    // BPM slider

    bpmSlider.setRange(
        40.0,
        240.0,
        1.0);

    bpmSlider.setValue(
        bpm.load());

    bpmSlider.setSliderStyle(
        juce::Slider::LinearHorizontal);

    bpmSlider.setTextBoxStyle(
        juce::Slider::TextBoxRight,
        false,
        80,
        30);

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

            bpmLabel.setText(
                "BPM: "
                + juce::String(
                    static_cast<int>(newBpm)),
                juce::dontSendNotification);
        };

    //==========================================================================
    // Application properties

    juce::PropertiesFile::Options options;

    options.applicationName = "GuitarBattle";
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    options.folderName = "GuitarBattle";
    options.commonToAllUsers = false;

    appProperties.setStorageParameters(options);

    //==========================================================================
    // Audio settings button

    addAndMakeVisible(audioSettingsButton);

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
                juce::Colours::darkgrey;

            options.escapeKeyTriggersCloseButton =
                true;

            options.useNativeTitleBar =
                true;

            options.resizable =
                true;

            options.launchAsync();
        };

    //==========================================================================
    // Restore previously saved audio device settings

    auto savedState =
        appProperties.getUserSettings()
        ->getXmlValue("AudioDeviceState");

    setAudioChannels(
        2,
        2,
        savedState.get());

    //==========================================================================
    // Start background pitch detection

    pitchThread.startThread();
}

//==============================================================================
// Destructor
//
// Останавливаем pitch thread, сохраняем настройки аудиоустройства
// и отключаем audio device.
//==============================================================================
MainComponent::~MainComponent()
{
    pitchThread.stopThread(1000);

    //==========================================================================
    // Save audio device state

    if (auto* settings =
        appProperties.getUserSettings())
    {
        settings->setValue(
            "AudioDeviceState",
            deviceManager.createStateXml().get());
    }

    //==========================================================================
    // Shutdown audio

    shutdownAudio();
}

//==============================================================================
// Audio initialization
//
// Здесь загружаются WAV-файлы метронома и создаются analysis buffers.
//==============================================================================
void MainComponent::prepareToPlay(
    int samplesPerBlockExpected,
    double sampleRate)
{
    juce::ignoreUnused(samplesPerBlockExpected);

    //==========================================================================
    // WAV format support

    audioFormatManager.registerBasicFormats();

    //==========================================================================
    // Load accent metronome sound
    //
    // Сейчас используется прямой путь к WAV-файлу.

    metronomeAccentReader.reset(
        audioFormatManager.createReaderFor(
            juce::File(
                "D:\\GuitarBattle\\GuitarBattle\\Source\\Sounds\\wooden-metronome-click.wav")));

    //==========================================================================
    // Load normal metronome sound

    metronomeNormalReader.reset(
        audioFormatManager.createReaderFor(
            juce::File(
                "D:\\GuitarBattle\\GuitarBattle\\Source\\Sounds\\light-clicking-perc.wav")));

    //==========================================================================
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

    //==========================================================================
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

    //==========================================================================
    // Pitch capture buffer
    //
    // 2048 samples are collected before sending them
    // to the background analysis thread.

    pitchBuffer.setSize(
        1,
        2048);

    pitchBuffer.clear();

    //==========================================================================
    // First analysis buffer

    pitchAnalysisBuffer.setSize(
        1,
        2048);

    pitchAnalysisBuffer.clear();

    //==========================================================================
    // Second analysis buffer

    pitchAnalysisBuffer2.setSize(
        1,
        2048);

    pitchAnalysisBuffer2.clear();

    //==========================================================================
    // Reset pitch buffer state

    pitchBufferPosition = 0;

    pitchBufferReady.store(false);
    pitchBufferReady2.store(false);

    //==========================================================================
    // Calculate beat duration from BPM

    beatLengthSamples.store(
        static_cast<uint64_t>(
            sampleRate
            * 60.0
            / bpm.load(
                std::memory_order_relaxed)),
        std::memory_order_relaxed);

    //==========================================================================
    // Reset beat counters

    beatCounter.store(
        0,
        std::memory_order_relaxed);

    beatInBar.store(
        0,
        std::memory_order_relaxed);

    //==========================================================================
    // Schedule first beat

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
// Здесь нельзя выполнять тяжёлые операции вроде pitch detection.
// Callback только:
// - считает audio samples;
// - запускает beats;
// - проигрывает уже загруженный WAV;
// - копирует входную гитару в analysis buffer.
//==============================================================================
void MainComponent::getNextAudioBlock(
    const juce::AudioSourceChannelInfo& bufferToFill)
{
    if (bufferToFill.buffer == nullptr)
        return;

    //==========================================================================
    // Update global audio sample counter

    audioSampleCounter.fetch_add(
        static_cast<uint64_t>(
            bufferToFill.numSamples),
        std::memory_order_relaxed);

    //==========================================================================
    // Initialize beat scheduler if necessary

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

    //==========================================================================
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

        // First beat of every bar gets the accent sound.
        metronomeAccent.store(
            barBeatNumber == 1,
            std::memory_order_relaxed);

        // Every beat triggers the metronome.
        metronomeTrigger.store(
            true,
            std::memory_order_relaxed);

        // Update beat information in the GUI thread.
        juce::MessageManager::callAsync(
            [this, beatNumber, barBeatNumber]
            {
                beatLabel.setText(
                    "Beat: "
                    + juce::String(beatNumber),
                    juce::dontSendNotification);

                barBeatLabel.setText(
                    "Bar beat: "
                    + juce::String(barBeatNumber),
                    juce::dontSendNotification);

                barBeatLabel.setFont(
                    barBeatNumber == 1
                    ? 32.0f
                    : 24.0f);
            });

        // Schedule next beat.
        nextBeatSample.store(
            beatSample
            + beatLengthSamples.load(
                std::memory_order_relaxed),
            std::memory_order_relaxed);
    }

    //==========================================================================
    // Input samples
    //
    // Guitar is connected to input channel 1,
    // which corresponds to index 0.

    auto* samples =
        bufferToFill.buffer->getReadPointer(0);


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

        for (int i = 0; i < bufferToFill.numSamples; ++i)
            right[i] = left[i];
    }

    juce::AudioBuffer<float> inputBuffer(
        bufferToFill.buffer->getArrayOfWritePointers(),
        1,
        bufferToFill.startSample,
        bufferToFill.numSamples);



    //==========================================================================
// Copy guitar input into pitch analysis buffer
//
// Когда набирается 2048 samples, блок передаётся одному из
// двух analysis buffers.

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
            //==============================================================
            // First analysis buffer available

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

            //==============================================================
            // Second analysis buffer available

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

            // Start collecting the next block.
            pitchBufferPosition = 0;
        }
    }

    // Никакой pitch detection здесь не выполняется.



    //==========================================================================
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

    //==========================================================================
    // Play current metronome WAV
    //
    // Accent beat is louder than normal beats.

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

        for (int i = 0; i < samplesToCopy; ++i)
        {
            const float metronomeSample =
                volume * buffer.getSample(
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
//
// Возвращает среднеквадратичную амплитуду блока.
// Используется для определения наличия сигнала и onset.
//==============================================================================
double MainComponent::detectRms(
    const float* samples,
    int numSamples)
{
    if (samples == nullptr || numSamples <= 0)
        return 0.0;

    double sum = 0.0;

    for (int i = 0; i < numSamples; ++i)
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
//
// Используется алгоритм YIN-подобного типа:
// - удаление DC offset;
// - RMS threshold;
// - difference function;
// - CMNDF;
// - поиск наиболее подходящего периода;
// - перевод периода в частоту.
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

    //==========================================================================
    // Calculate mean / DC offset

    double mean = 0.0;

    for (int i = 0; i < numSamples; ++i)
        mean += samples[i];

    mean /=
        static_cast<double>(numSamples);

    //==========================================================================
    // Calculate RMS after removing DC offset

    double rms = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const double x =
            static_cast<double>(samples[i])
            - mean;

        rms += x * x;
    }

    rms = std::sqrt(
        rms / static_cast<double>(numSamples));

    // Ignore very quiet blocks.
    if (rms < 0.005)
        return 0.0;

    //==========================================================================
    // Pitch search range: 60 Hz - 500 Hz

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

    //==========================================================================
    // Difference function and CMNDF

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

    //==========================================================================
    // Cumulative mean normalized difference function

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

    //==========================================================================
    // Find first strong CMNDF minimum

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

    //==========================================================================
    // If no strong minimum was found,
    // use the absolute minimum.

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

    // Period -> frequency.
    return sampleRate
        / static_cast<double>(bestLag);
}

//==============================================================================
// Convert frequency to musical note
//
// Uses MIDI note numbering and converts it into note name + octave.
//==============================================================================
juce::String MainComponent::pitchToNote(
    double frequency)
{
    if (frequency <= 0.0)
        return "--";

    const double midi =
        69.0
        + 12.0
        * std::log2(frequency / 440.0);

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
// Main background
//==============================================================================
void MainComponent::paint(
    juce::Graphics& g)
{
    g.fillAll(
        getLookAndFeel().findColour(
            juce::ResizableWindow::backgroundColourId));

    g.setColour(
        juce::Colours::white);

    g.setFont(24.0f);

    g.drawText(
        "Guitar Battle - Audio Setup",
        20,
        80,
        getWidth() - 40,
        40,
        juce::Justification::centred);
}

//==============================================================================
// UI layout
//==============================================================================
void MainComponent::resized()
{
    audioSettingsButton.setBounds(
        20,
        20,
        150,
        40);

    pitchLabel.setBounds(
        20,
        100,
        getWidth() - 40,
        60);

    beatLabel.setBounds(
        20,
        170,
        getWidth() - 40,
        40);

    barBeatLabel.setBounds(
        20,
        220,
        getWidth() - 40,
        40);

    bpmLabel.setBounds(
        20,
        270,
        getWidth() - 40,
        40);

    bpmSlider.setBounds(
        100,
        320,
        getWidth() - 200,
        40);

    scoreLabel.setBounds(
        20,
        370,
        getWidth() - 40,
        40);

    targetNoteLabel.setBounds(
        20,
        420,
        getWidth() - 40,
        50);

    hitLabel.setBounds(
        20,
        470,
        getWidth() - 40,
        40);
}