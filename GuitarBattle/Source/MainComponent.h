#pragma once

#include <JuceHeader.h>

class MainComponent : public juce::AudioAppComponent
{
public:
    MainComponent();
    ~MainComponent() override;

    void prepareToPlay(
        int samplesPerBlockExpected,
        double sampleRate) override;

    void getNextAudioBlock(
        const juce::AudioSourceChannelInfo& bufferToFill) override;

    void releaseResources() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:

    juce::Label pitchLabel;
    juce::AudioBuffer<float> pitchBuffer;
    int pitchBufferPosition = 0;

    juce::AudioBuffer<float> pitchAnalysisBuffer;
    std::atomic<bool> pitchBufferReady{ false };

    juce::AudioBuffer<float> pitchAnalysisBuffer2;
    std::atomic<bool> pitchBufferReady2{ false };
    
    double detectPitch(const float* samples, int numSamples, double sampleRate);
    juce::String pitchToNote(double frequency);
    double lastPitch = 0.0;

    juce::TextButton audioSettingsButton{ "Audio Settings" };

    class PitchThread : public juce::Thread
    {
    public:
        PitchThread(MainComponent& owner)
            : juce::Thread("Pitch Detection Thread"),
            owner(owner)
        {
        }

        void run() override
        {
            while (!threadShouldExit())
            {
                if (owner.pitchBufferReady.exchange(false))
                {
                    const double sampleRate =
                        owner.deviceManager
                        .getCurrentAudioDevice()
                        ->getCurrentSampleRate();

                    const double pitch =
                        owner.detectPitch(
                            owner.pitchAnalysisBuffer.getReadPointer(0),
                            4096,
                            sampleRate);

                    if (pitch > 20.0 && pitch < 1000.0)
                    {
                        owner.lastPitch = pitch;

                        const auto text =
                            owner.pitchToNote(pitch) + "  | " + juce::String(pitch, 2) + " Hz";
                        DBG(text);

                        juce::MessageManager::callAsync(
                            [this, text]
                            {
                                owner.pitchLabel.setText(
                                    text,
                                    juce::dontSendNotification);
                            });
                    }
                }
                if (owner.pitchBufferReady2.exchange(false))
                {
                    const double sampleRate =
                        owner.deviceManager
                        .getCurrentAudioDevice()
                        ->getCurrentSampleRate();

                    const double pitch =
                        owner.detectPitch(
                            owner.pitchAnalysisBuffer2.getReadPointer(0),
                            4096,
                            sampleRate);

                    if (pitch > 20.0 && pitch < 1000.0)
                    {
                        owner.lastPitch = pitch;

                        const auto text =
                            owner.pitchToNote(pitch) + "  |  " + juce::String(pitch, 2) + " Hz";

                        DBG(text);

                        juce::MessageManager::callAsync(
                            [this, text]
                            {
                                owner.pitchLabel.setText(
                                    text,
                                    juce::dontSendNotification);
                            });
                    }
                }
                

                wait(1);
            }
        }

    private:
        MainComponent& owner;
    };
    juce::dsp::FFT pitchFFT{ 12 };
    PitchThread pitchThread{ *this };

    juce::ApplicationProperties appProperties;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};