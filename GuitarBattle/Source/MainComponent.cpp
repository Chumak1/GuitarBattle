#include "MainComponent.h"
#include <vector>

//==============================================================================
MainComponent::MainComponent()
{
        
    setSize(700, 500);


    pitchLabel.setText("Pitch: -- Hz", juce::dontSendNotification);
    pitchLabel.setFont(32.0f);
    pitchLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(pitchLabel);

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
    // UI

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

            options.dialogTitle = "Audio Settings";
            options.dialogBackgroundColour = juce::Colours::darkgrey;
            options.escapeKeyTriggersCloseButton = true;
            options.useNativeTitleBar = true;
            options.resizable = true;

            options.launchAsync();
        };
    
    



    //==========================================================================
    // Audio

    auto savedState =
        appProperties.getUserSettings()
        ->getXmlValue("AudioDeviceState");

    setAudioChannels(
        2,
        2,
        savedState.get());

    //==========================================================================
    // Debug information

    auto* currentDevice = deviceManager.getCurrentAudioDevice();

    if (currentDevice != nullptr)
    {
        DBG("Current audio device: "
            + currentDevice->getName());

        DBG("Sample rate: "
            + juce::String(
                currentDevice->getCurrentSampleRate()));

        DBG("Buffer size: "
            + juce::String(
                currentDevice->getCurrentBufferSizeSamples()));

        DBG("Input channels: "
            + juce::String(
                currentDevice->getActiveInputChannels()
                .countNumberOfSetBits()));

        DBG("Output channels: "
            + juce::String(
                currentDevice->getActiveOutputChannels()
                .countNumberOfSetBits()));
    }
    pitchThread.startThread();
}



//==============================================================================
MainComponent::~MainComponent()
{

    pitchThread.stopThread(1000);
    //==========================================================================
    // Save audio device state

    if (auto* settings = appProperties.getUserSettings())
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
void MainComponent::prepareToPlay(
    int samplesPerBlockExpected,
    double sampleRate)
{
    DBG("=== PREPARE TO PLAY ===");

    DBG("Sample rate: "
        + juce::String(sampleRate));

    DBG("Block size: "
        + juce::String(samplesPerBlockExpected));

    pitchBuffer.setSize(1, 4096);
    pitchBuffer.clear();

    pitchAnalysisBuffer.setSize(1, 4096);
    pitchAnalysisBuffer.clear();

    pitchAnalysisBuffer2.setSize(1, 4096);
    pitchAnalysisBuffer2.clear();

    pitchBufferPosition = 0;
    pitchBufferReady.store(false);
    pitchBufferReady2.store(false);
}

//==============================================================================
void MainComponent::getNextAudioBlock(
    const juce::AudioSourceChannelInfo& bufferToFill)
{
    if (bufferToFill.buffer == nullptr)
        return;

    auto* samples = bufferToFill.buffer->getReadPointer(0);

   

    for (int i = 0; i < bufferToFill.numSamples; ++i)
    {
        pitchBuffer.setSample(
            0,
            pitchBufferPosition,
            samples[i]);

        ++pitchBufferPosition;

        if (pitchBufferPosition >= 4096)

        {


            if (!pitchBufferReady.load())
            {
                auto* destination =
                    pitchAnalysisBuffer.getWritePointer(0);

                const auto* source =
                    pitchBuffer.getReadPointer(0);

                for (int sample = 0; sample < 4096; ++sample)
                    destination[sample] = source[sample];

                pitchBufferReady.store(true);
            }
            else if (!pitchBufferReady2.load())
            {
                auto* destination =
                    pitchAnalysisBuffer2.getWritePointer(0);

                const auto* source =
                    pitchBuffer.getReadPointer(0);

                for (int sample = 0; sample < 4096; ++sample)
                    destination[sample] = source[sample];

                pitchBufferReady2.store(true);
            }

            pitchBufferPosition = 0;
        }
    }


    // Пока ничего не отправляем на выход.
    // Это важно: здесь мы только проверяем входной сигнал.
}


double MainComponent::detectPitch(const float* samples,
    int numSamples,
    double sampleRate)
{
    if (samples == nullptr || numSamples < 1024 || sampleRate <= 0.0)
        return 0.0;

    double mean = 0.0;

    for (int i = 0; i < numSamples; ++i)
        mean += samples[i];

    mean /= static_cast<double>(numSamples);

    double rms = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const double x =
            static_cast<double>(samples[i]) - mean;

        rms += x * x;
    }

    rms = std::sqrt(rms / static_cast<double>(numSamples));

    if (rms < 0.005)
        return 0.0;

    

    const int minLag =
        static_cast<int>(sampleRate / 500.0);

    const int maxLag =
        static_cast<int>(sampleRate / 60.0);

    if (minLag < 1 || maxLag >= numSamples)
        return 0.0;

    std::vector<double> difference(maxLag + 1, 0.0);
    std::vector<double> cmndf(maxLag + 1, 1.0);

    for (int lag = 1; lag <= maxLag; ++lag)
    {
        double sum = 0.0;

        for (int i = 0; i < numSamples - lag; ++i)
        {
            const double a =
                static_cast<double>(samples[i]) - mean;

            const double b =
                static_cast<double>(samples[i + lag]) - mean;

            const double delta = a - b;

            sum += delta * delta;
        }

        difference[lag] = sum;
    }

    double runningSum = 0.0;

    for (int lag = 1; lag <= maxLag; ++lag)
    {
        runningSum += difference[lag];

        if (runningSum > 0.0)
        {
            cmndf[lag] =
                difference[lag] *
                static_cast<double>(lag) /
                runningSum;
        }
    }

    int bestLag = 0;

    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        if (cmndf[lag] < 0.10)
        {
            bestLag = lag;

            while (bestLag + 1 <= maxLag &&
                cmndf[bestLag + 1] < cmndf[bestLag])
            {
                ++bestLag;
            }

            break;
        }
    }

    if (bestLag == 0)
    {
        double bestValue = 1.0;

        for (int lag = minLag; lag <= maxLag; ++lag)
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

    return sampleRate /
        static_cast<double>(bestLag);
}


juce::String MainComponent::pitchToNote(double frequency)
{
    if (frequency <= 0.0)
        return "--";

    const double midi =
        69.0 + 12.0 * std::log2(frequency / 440.0);

    const int midiNote =
        static_cast<int>(std::round(midi));

    static const char* noteNames[] =
    {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"
    };

    const int noteIndex =
        (midiNote % 12 + 12) % 12;

    const int octave =
        (midiNote / 12) - 1;

    return juce::String(noteNames[noteIndex])
        + juce::String(octave);
}

//==============================================================================
void MainComponent::releaseResources()
{
}

//==============================================================================
void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(
        getLookAndFeel().findColour(
            juce::ResizableWindow::backgroundColourId));

    g.setColour(juce::Colours::white);
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
}

