#pragma once

#include <JuceHeader.h>

//==============================================================================
// Main application component.
//
// Отвечает за:
// - пользовательский интерфейс;
// - работу с аудиоустройством;
// - захват входного сигнала гитары;
// - определение pitch и onset на отдельном потоке;
// - метроном;
// - BPM и beat scheduler;
// - игровую цель и score.
//
// ВАЖНО:
// Аудиологика отделена от визуального оформления.
// Изменения UI не должны влиять на audio callback и pitch detection.
//==============================================================================
class MainComponent : public juce::AudioAppComponent
{
public:
    MainComponent();
    ~MainComponent() override;

    //==========================================================================
    // JUCE audio callbacks

    void prepareToPlay(
        int samplesPerBlockExpected,
        double sampleRate) override;

    void getNextAudioBlock(
        const juce::AudioSourceChannelInfo& bufferToFill) override;

    void releaseResources() override;

    //==========================================================================
    // JUCE component callbacks

    void paint(juce::Graphics& g) override;
    void resized() override;

private:

    //==========================================================================
    // UI
    //
    // Все элементы интерфейса находятся здесь.
    // Карточки сам компонент рисует в paint().
    //
    // Label используются только для текста.
    // Фон и скруглённые блоки рисуются отдельно.

    juce::Label pitchLabel;
    juce::Label beatLabel;
    juce::Label barBeatLabel;
    juce::Label bpmLabel;
    juce::Label scoreLabel;
    juce::Label hitLabel;
    juce::Label targetNoteLabel;

    // BPM теперь не обычный горизонтальный slider.
    //
    // Пользователь может:
    // - кликнуть по числу и ввести значение;
    // - использовать колесо мыши;
    // - использовать маленькие кнопки +/-.
    juce::Slider bpmSlider;

    // Маленькая кнопка в правом верхнем углу.
    juce::TextButton audioSettingsButton{ "Audio Settings" };

    //==========================================================================
    // Pitch input buffer
    //
    // Audio callback быстро складывает входной сигнал сюда.
    // Когда набирается 2048 samples, блок передаётся одному из двух
    // analysis buffers для обработки на отдельном потоке.

    juce::AudioBuffer<float> pitchBuffer;
    int pitchBufferPosition = 0;

    std::atomic<uint64_t> audioSampleCounter{ 0 };

    //==========================================================================
    // Double buffer для анализа pitch

    juce::AudioBuffer<float> pitchAnalysisBuffer;
    std::atomic<bool> pitchBufferReady{ false };

    juce::AudioBuffer<float> pitchAnalysisBuffer2;
    std::atomic<bool> pitchBufferReady2{ false };

    std::atomic<uint64_t> pitchAnalysisStartSample{ 0 };
    std::atomic<uint64_t> pitchAnalysisStartSample2{ 0 };

    //==========================================================================
    // Onset / pitch state

    std::atomic<bool> noteOn{ false };
    std::atomic<uint64_t> lastNoteOnSample{ 0 };
    std::atomic<uint64_t> lastOnsetSample{ 0 };

    double previousRmsValue = 0.0;
    double lastPitch = 0.0;

    //==========================================================================
    // BPM

    std::atomic<double> bpm{ 120.0 };

    //==========================================================================
    // Game state

    std::atomic<int> score{ 0 };
    std::atomic<bool> targetHit{ false };
    std::atomic<uint64_t> lastHitSample{ 0 };

    juce::String targetNote = "E2";
    int targetNoteIndex = 0;

    const juce::String targetNotes[6] =
    {
        "E2",
        "A2",
        "D3",
        "G3",
        "B3",
        "E4"
    };

    //==========================================================================
    // Beat scheduler

    std::atomic<uint64_t> beatLengthSamples{ 0 };
    std::atomic<uint64_t> nextBeatSample{ 0 };
    std::atomic<uint64_t> beatCounter{ 0 };
    std::atomic<int> beatInBar{ 0 };

    //==========================================================================
    // Metronome

    std::atomic<bool> metronomeTrigger{ false };
    std::atomic<bool> metronomeAccent{ false };

    int metronomeSamplesRemaining = 0;
    int metronomePlaybackPosition = 0;
    bool metronomePlayingAccent = false;

    juce::AudioFormatManager audioFormatManager;

    std::unique_ptr<juce::AudioFormatReader> metronomeAccentReader;
    std::unique_ptr<juce::AudioFormatReader> metronomeNormalReader;

    juce::AudioBuffer<float> metronomeAccentBuffer;
    juce::AudioBuffer<float> metronomeNormalBuffer;

    //==========================================================================
    // Pitch detection functions

    double detectPitch(
        const float* samples,
        int numSamples,
        double sampleRate);

    double detectRms(
        const float* samples,
        int numSamples);

    juce::String pitchToNote(double frequency);

    //==========================================================================
    // Background pitch detection thread

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
                //==================================================================
                // Первый analysis buffer

                if (owner.pitchBufferReady.exchange(false))
                {
                    auto* device =
                        owner.deviceManager.getCurrentAudioDevice();

                    if (device == nullptr)
                        continue;

                    const double sampleRate =
                        device->getCurrentSampleRate();

                    const double pitch =
                        owner.detectPitch(
                            owner.pitchAnalysisBuffer.getReadPointer(0),
                            2048,
                            sampleRate);

                    const double rms =
                        owner.detectRms(
                            owner.pitchAnalysisBuffer.getReadPointer(0),
                            2048);

                    const auto analysisSample =
                        owner.pitchAnalysisStartSample.load(
                            std::memory_order_relaxed);

                    //==============================================================
                    // Определяем фактическое начало звука внутри блока.

                    const auto* blockSamples =
                        owner.pitchAnalysisBuffer.getReadPointer(0);

                    int onsetOffset = 0;
                    double maxSample = 0.0;

                    for (int i = 0; i < 2048; ++i)
                    {
                        maxSample = std::max(
                            maxSample,
                            std::abs(
                                static_cast<double>(blockSamples[i])));
                    }

                    const double threshold = maxSample * 0.2;

                    for (int i = 0; i < 2048; ++i)
                    {
                        if (std::abs(
                            static_cast<double>(blockSamples[i]))
                            >= threshold)
                        {
                            onsetOffset = i;
                            break;
                        }
                    }

                    const auto detectedOnsetSample =
                        analysisSample
                        + static_cast<uint64_t>(onsetOffset);

                    //==============================================================
                    // RMS / onset detection

                    const double previousRms =
                        owner.previousRmsValue;

                    owner.previousRmsValue = rms;

                    if (rms < 0.005)
                    {
                        juce::MessageManager::callAsync(
                            [this]
                            {
                                owner.pitchLabel.setText(
                                    "Pitch: ---",
                                    juce::dontSendNotification);
                            });
                    }

                    const bool onset =
                        rms > 0.005
                        && rms > previousRms * 1.02;

                    //==============================================================
                    // Запоминаем onset

                    if (onset)
                    {
                        owner.lastOnsetSample.store(
                            detectedOnsetSample,
                            std::memory_order_relaxed);

                        const auto lastOnset =
                            owner.lastNoteOnSample.load(
                                std::memory_order_relaxed);

                        const auto onsetGap =
                            analysisSample > lastOnset
                            ? analysisSample - lastOnset
                            : 0;

                        // Защита от повторного определения одной ноты.
                        if (lastOnset != 0 && onsetGap < 11025)
                            continue;
                    }

                    if (onset)
                    {
                        owner.lastNoteOnSample.store(
                            analysisSample,
                            std::memory_order_relaxed);

                        owner.targetHit.store(
                            false,
                            std::memory_order_relaxed);
                    }

                    //==============================================================
                    // Valid pitch + onset

                    if (pitch > 20.0
                        && pitch < 1000.0
                        && rms >= 0.005
                        && onset)
                    {
                        owner.lastPitch = pitch;

                        const auto text =
                            owner.pitchToNote(pitch)
                            + "  |  "
                            + juce::String(pitch, 2)
                            + " Hz";

                        const auto detectedNote =
                            owner.pitchToNote(pitch);

                        //==========================================================
                        // Проверяем попадание в target.

                        if (detectedNote == owner.targetNote
                            && !owner.targetHit.exchange(true))
                        {
                            owner.lastHitSample.store(
                                analysisSample,
                                std::memory_order_relaxed);

                            //======================================================
                            // Timing calculation.
                            //
                            // Пока используется только для диагностики.
                            // Логика GOOD/MISS здесь намеренно не добавляется.

                            const auto hitSample =
                                owner.lastOnsetSample.load(
                                    std::memory_order_relaxed);

                            const auto nextBeatSample =
                                owner.nextBeatSample.load(
                                    std::memory_order_relaxed);

                            const auto previousBeatSample =
                                nextBeatSample
                                - owner.beatLengthSamples.load(
                                    std::memory_order_relaxed);

                            const auto errorToNext =
                                std::llabs(
                                    static_cast<long long>(hitSample)
                                    - static_cast<long long>(nextBeatSample));

                            const auto errorToPrevious =
                                std::llabs(
                                    static_cast<long long>(hitSample)
                                    - static_cast<long long>(previousBeatSample));

                            const auto timingErrorSamples =
                                std::min(
                                    errorToNext,
                                    errorToPrevious);

                            const bool timingGood =
                                timingErrorSamples
                                <= static_cast<long long>(
                                    sampleRate * 0.100);

                            juce::ignoreUnused(timingGood);

                            //======================================================
                            // Score

                            const auto newScore =
                                owner.score.fetch_add(
                                    10,
                                    std::memory_order_relaxed)
                                + 10;

                            juce::MessageManager::callAsync(
                                [this]
                                {
                                    owner.hitLabel.setText(
                                        "HIT",
                                        juce::dontSendNotification);

                                    juce::Timer::callAfterDelay(
                                        300,
                                        [this]
                                        {
                                            owner.hitLabel.setText(
                                                "READY",
                                                juce::dontSendNotification);
                                        });
                                });

                            //======================================================
                            // Следующая target note

                            owner.targetNoteIndex =
                                (owner.targetNoteIndex + 1) % 6;

                            owner.targetNote =
                                owner.targetNotes[
                                    owner.targetNoteIndex];

                            //======================================================
                            // Update score

                            juce::MessageManager::callAsync(
                                [this, newScore]
                                {
                                    owner.scoreLabel.setText(
                                        juce::String(newScore),
                                        juce::dontSendNotification);
                                });
                        }

                        //==========================================================
                        // Update target

                        juce::MessageManager::callAsync(
                            [this]
                            {
                                owner.targetNoteLabel.setText(
                                    owner.targetNote,
                                    juce::dontSendNotification);
                            });

                        //==========================================================
                        // Update pitch

                        juce::MessageManager::callAsync(
                            [this, text]
                            {
                                owner.pitchLabel.setText(
                                    text,
                                    juce::dontSendNotification);
                            });
                    }
                }

                //==================================================================
                // Второй analysis buffer

                if (owner.pitchBufferReady2.exchange(false))
                {
                    auto* device =
                        owner.deviceManager.getCurrentAudioDevice();

                    if (device == nullptr)
                        continue;

                    const double sampleRate =
                        device->getCurrentSampleRate();

                    const double pitch =
                        owner.detectPitch(
                            owner.pitchAnalysisBuffer2.getReadPointer(0),
                            2048,
                            sampleRate);

                    const double rms =
                        owner.detectRms(
                            owner.pitchAnalysisBuffer2.getReadPointer(0),
                            2048);

                    const auto analysisSample =
                        owner.pitchAnalysisStartSample2.load(
                            std::memory_order_relaxed);

                    const double previousRms =
                        owner.previousRmsValue;

                    owner.previousRmsValue = rms;

                    if (rms < 0.005)
                    {
                        juce::MessageManager::callAsync(
                            [this]
                            {
                                owner.pitchLabel.setText(
                                    "Pitch: ---",
                                    juce::dontSendNotification);
                            });
                    }

                    const bool onset =
                        rms > 0.005
                        && rms > previousRms * 1.02;

                    if (onset)
                    {
                        owner.lastNoteOnSample.store(
                            analysisSample,
                            std::memory_order_relaxed);

                        owner.targetHit.store(
                            false,
                            std::memory_order_relaxed);
                    }

                    if (pitch > 20.0
                        && pitch < 1000.0
                        && rms >= 0.005
                        && onset)
                    {
                        owner.lastPitch = pitch;

                        const auto text =
                            owner.pitchToNote(pitch)
                            + "  |  "
                            + juce::String(pitch, 2)
                            + " Hz";

                        juce::MessageManager::callAsync(
                            [this]
                            {
                                owner.targetNoteLabel.setText(
                                    owner.targetNote,
                                    juce::dontSendNotification);
                            });

                        juce::MessageManager::callAsync(
                            [this, text]
                            {
                                owner.pitchLabel.setText(
                                    text,
                                    juce::dontSendNotification);
                            });
                    }
                }

                // Небольшая пауза для корректной работы thread.
                wait(1);
            }
        }

    private:

        MainComponent& owner;
    };

    PitchThread pitchThread{ *this };

    //==========================================================================
    // Application settings

    juce::ApplicationProperties appProperties;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};