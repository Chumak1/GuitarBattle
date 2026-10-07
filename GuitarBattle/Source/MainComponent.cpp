#include "MainComponent.h"

#include <vector>

//==============================================================================
// UI palette
//
// Все основные цвета приложения находятся здесь.
// Это позволяет менять визуальный стиль в одном месте.
//
// Основная идея:
//
// - почти белый холодный фон;
// - белые карточки;
// - тёмный текст;
// - синий как основной accent;
// - зелёный используется для успешного попадания;
// - красный зарезервирован под MISS.
//==============================================================================

namespace UiColors
{
    // Фон всего приложения.
    const juce::Colour background =
        juce::Colour::fromString("FFF3F5F7");

    // Белый цвет карточек.
    const juce::Colour card =
        juce::Colour::fromString("FFFFFFFF");

    // Тонкая граница карточек.
    const juce::Colour border =
        juce::Colour::fromString("FFE2E8F0");

    // Основной тёмный текст.
    const juce::Colour text =
        juce::Colour::fromString("FF1E293B");

    // Вторичный текст.
    const juce::Colour secondaryText =
        juce::Colour::fromString("FF64748B");

    // Основной синий цвет приложения.
    const juce::Colour blue =
        juce::Colour::fromString("FF2563EB");

    // Более тёмный синий для активных элементов.
    const juce::Colour blueDark =
        juce::Colour::fromString("FF1D4ED8");

    // Светло-синий фон выбранных элементов.
    const juce::Colour blueLight =
        juce::Colour::fromString("FFEFF6FF");

    // Цвет успешного попадания.
    const juce::Colour green =
        juce::Colour::fromString("FF16A34A");

    // Цвет MISS.
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
    // Pitch label
    //
    // Центральный и самый крупный элемент интерфейса.
    //
    // Пока pitch не определён, показываем:
    //
    // Pitch: ---
    //
    // При обнаружении ноты сюда будет выводиться:
    //
    // E2 | 82.31 Hz

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

    addAndMakeVisible(pitchLabel);

    //--------------------------------------------------------------------------
    // Beat label
    //
    // Показываем только номер текущего beat.
    //
    // Например:
    //
    // 17

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

    addAndMakeVisible(beatLabel);

    //--------------------------------------------------------------------------
    // Bar beat label
    //
    // Показывает положение внутри такта:
    //
    // 1
    // 2
    // 3
    // 4
    //
    // Визуального акцента на первой доле нет.
    // Акцент остаётся только в звуке метронома.

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

    addAndMakeVisible(barBeatLabel);

    //--------------------------------------------------------------------------
    // BPM label

    bpmLabel.setText(
        "120",
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

    addAndMakeVisible(bpmLabel);

    //--------------------------------------------------------------------------
   


    //--------------------------------------------------------------------------
    //==============================================================================
//==============================================================================
// Speed buttons

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

    auto setSpeed = [this](double percent)
        {
            speedPercent = percent;

            const double actualBpm =
                baseBpm * speedPercent / 100.0;

            bpm.store(
                actualBpm,
                std::memory_order_relaxed);

            auto* device =
                deviceManager.getCurrentAudioDevice();

            if (device != nullptr)
            {
                const double sampleRate =
                    device->getCurrentSampleRate();

                if (sampleRate > 0.0
                    && actualBpm > 0.0)
                {
                    const auto beatLength =
                        static_cast<uint64_t>(
                            sampleRate
                            * 60.0
                            / actualBpm);

                    beatLengthSamples.store(
                        beatLength,
                        std::memory_order_relaxed);
                }
            }

            bpmLabel.setText(
                juce::String(
                    static_cast<int>(
                        std::round(actualBpm))),
                juce::dontSendNotification);
        };

    speed50Button.onClick = [this, setSpeed]
        {
            setSpeed(50.0);
        };

    speed100Button.onClick = [this, setSpeed]
        {
            setSpeed(100.0);
        };

    speed120Button.onClick = [this, setSpeed]
        {
            setSpeed(120.0);
        };

    for (auto* button :
        { &speed50Button, &speed100Button, &speed120Button })
    {
        button->setColour(
            juce::TextButton::buttonColourId,
            UiColors::blueLight);

        button->setColour(
            juce::TextButton::buttonOnColourId,
            juce::Colour::fromString("FFDBEAFE"));

        button->setColour(
            juce::TextButton::textColourOffId,
            UiColors::blueDark);

        button->setColour(
            juce::TextButton::textColourOnId,
            UiColors::blueDark);
    }

    addAndMakeVisible(speed50Button);
    addAndMakeVisible(speed100Button);
    addAndMakeVisible(speed120Button);
    //--------------------------------------------------------------------------
    // Score label
    //
    // Пока score равен нулю.

    scoreLabel.setText(
        "0",
        juce::dontSendNotification);

    scoreLabel.setFont(
        juce::FontOptions()
        .withHeight(30.0f)
        .withStyle("Bold"));

    scoreLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    scoreLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(scoreLabel);

    //--------------------------------------------------------------------------
    // Hit / Ready / Miss status
    //
    // Изначально:
    //
    // READY
    //
    // После попадания:
    //
    // PERFECT
    // -3 ms EARLY
    //
    // или:
    //
    // GREAT
    // +18 ms LATE

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

    addAndMakeVisible(hitLabel);

    //--------------------------------------------------------------------------
    // Target note
    //
    // Первая цель:
    //
    // E2

    targetNoteLabel.setText(
        "E2",
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

    addAndMakeVisible(targetNoteLabel);

    //--------------------------------------------------------------------------
    // Application properties
    //
    // Используются для сохранения настроек аудиоустройства.

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

    //--------------------------------------------------------------------------
    // Audio settings dialog
    //
    // Здесь пользователь выбирает:
    //
    // - input device;
    // - output device;
    // - количество каналов;
    // - другие параметры аудиоустройства.

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

    //==========================================================================
    // Difficulty buttons
    //
    // Difficulty НЕ определяет BPM.
    //
    // Он влияет на:
    //
    // - multiplier score;
    // - требования к точности в дальнейшем.
    //
    // Сейчас multiplier:
    //
    // EASY   = 0.80
    // NORMAL = 1.00
    // HARD   = 1.25

    easyButton.setClickingTogglesState(
        true);

    normalButton.setClickingTogglesState(
        true);

    hardButton.setClickingTogglesState(
        true);

    // Все три кнопки работают как radio group.
    easyButton.setRadioGroupId(1001);
    normalButton.setRadioGroupId(1001);
    hardButton.setRadioGroupId(1001);

    // По умолчанию NORMAL.
    easyButton.setToggleState(
        false,
        juce::dontSendNotification);

    normalButton.setToggleState(
        true,
        juce::dontSendNotification);

    hardButton.setToggleState(
        false,
        juce::dontSendNotification);

    //--------------------------------------------------------------------------
    // Difficulty selection

    easyButton.onClick = [this]
        {
            difficulty =
                Difficulty::Easy;
        };

    normalButton.onClick = [this]
        {
            difficulty =
                Difficulty::Normal;
        };

    hardButton.onClick = [this]
        {
            difficulty =
                Difficulty::Hard;
        };

    //--------------------------------------------------------------------------
    // Difficulty colours
    //
    // TextButton в JUCE 9 не требует setFont().
    // Используем только цвета.

    for (auto* button :
        { &easyButton, &normalButton, &hardButton })
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

    addAndMakeVisible(easyButton);
    addAndMakeVisible(normalButton);
    addAndMakeVisible(hardButton);





    //--------------------------------------------------------------------------
    // Restore saved audio device settings
    //
    // Если настройки были сохранены раньше,
    // JUCE восстановит выбранное устройство.

    auto savedState =
        appProperties.getUserSettings()
        ->getXmlValue(
            "AudioDeviceState");

    setAudioChannels(
        2,
        2,
        savedState.get());

    //--------------------------------------------------------------------------
    // Start pitch detection thread
    //
    // Важно:
    //
    // Pitch detection НЕ выполняется в audio callback.
    // Callback только складывает samples в buffer.
    //
    // PitchThread выполняет тяжёлый анализ отдельно.

    pitchThread.startThread();
}

//==============================================================================
// Destructor
//==============================================================================

MainComponent::~MainComponent()
{
    //--------------------------------------------------------------------------
    // Останавливаем pitch detection thread.

    pitchThread.stopThread(1000);

    //--------------------------------------------------------------------------
    // Сохраняем состояние аудиоустройства.

    if (auto* settings =
        appProperties.getUserSettings())
    {
        settings->setValue(
            "AudioDeviceState",
            deviceManager.createStateXml().get());
    }

    //--------------------------------------------------------------------------
    // Освобождаем аудиоресурсы.

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
    // Load accent metronome sound.
    //
    // Первый beat такта получает более сильный звук.

    metronomeAccentReader.reset(
        audioFormatManager.createReaderFor(
            juce::File(
                "D:\\GuitarBattle\\GuitarBattle\\Source\\Sounds\\wooden-metronome-click.wav")));

    //--------------------------------------------------------------------------
    // Load normal metronome sound.
    //
    // Beats 2, 3, 4 используют этот звук.

    metronomeNormalReader.reset(
        audioFormatManager.createReaderFor(
            juce::File(
                "D:\\GuitarBattle\\GuitarBattle\\Source\\Sounds\\light-clicking-perc.wav")));

    //--------------------------------------------------------------------------
    // Copy accent WAV into RAM.
    //
    // Audio callback не должен читать файл с диска.

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
    // Copy normal WAV into RAM.

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
    //
    // Основной input buffer:
    //
    // 1 channel
    // 2048 samples
    //
    // Далее используются два analysis buffer,
    // чтобы audio callback и PitchThread не блокировали друг друга.

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
    // Calculate actual BPM.
    //
    // Actual BPM =
    //
    // base BPM × SPEED / 100
    //
    // Например:
    //
    // 120 BPM × 50% = 60 BPM
    // 120 BPM × 100% = 120 BPM
    // 120 BPM × 120% = 144 BPM

    const double actualBpm =
        bpm.load(
            std::memory_order_relaxed);

    //--------------------------------------------------------------------------
    // Beat length in samples.

    beatLengthSamples.store(
        static_cast<uint64_t>(
            sampleRate
            * 60.0
            / actualBpm),
        std::memory_order_relaxed);

    //--------------------------------------------------------------------------
    // Reset beat counters.

    beatCounter.store(
        0,
        std::memory_order_relaxed);

    beatInBar.store(
        0,
        std::memory_order_relaxed);

    //--------------------------------------------------------------------------
    // Schedule first beat.

    nextBeatSample.store(
        static_cast<uint64_t>(
            sampleRate
            * 60.0
            / actualBpm),
        std::memory_order_relaxed);
}

//==============================================================================
// Audio callback
//
// ВАЖНО:
//
// Здесь НЕ выполняется pitch detection.
//
// Callback занимается только:
//
// 1. sample counter;
// 2. beat scheduler;
// 3. копированием guitar input;
// 4. записью samples в pitch buffer;
// 5. воспроизведением уже загруженного WAV.
//
// Pitch detection выполняется в PitchThread.
//==============================================================================

void MainComponent::getNextAudioBlock(
    const juce::AudioSourceChannelInfo& bufferToFill)
{
    if (bufferToFill.buffer == nullptr)
        return;

    //--------------------------------------------------------------------------
    // Update global sample counter.

    audioSampleCounter.fetch_add(
        static_cast<uint64_t>(
            bufferToFill.numSamples),
        std::memory_order_relaxed);

    //--------------------------------------------------------------------------
    // Initialize beat scheduler if necessary.

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
    //
    // Когда audio callback доходит до следующего beat,
    // увеличиваем счётчики и запускаем метроном.

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

        // После 4-го beat начинаем новый такт.

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
        // Audio accent
        //
        // Первый beat такта получает отдельный звук.
        //
        // В UI никакого визуального акцента нет.

        metronomeAccent.store(
            barBeatNumber == 1,
            std::memory_order_relaxed);

        metronomeTrigger.store(
            true,
            std::memory_order_relaxed);

        //--------------------------------------------------------------------------
        // Update beat numbers in UI.
        //
        // Это MessageManager callback, поэтому UI обновляется
        // безопасно не из audio thread.

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
        // Schedule next beat.

        nextBeatSample.store(
            beatSample
            + beatLengthSamples.load(
                std::memory_order_relaxed),
            std::memory_order_relaxed);
    }

    //--------------------------------------------------------------------------
    // Guitar input
    //
    // Берём оригинальный сигнал с input channel 0.
    //
    // Важно:
    //
    // этот сигнал должен быть сохранён до добавления
    // метронома в output.
    //
    // Поэтому pitch detector не видит звук метронома.

    const auto* samples =
        bufferToFill.buffer->getReadPointer(0);

    //--------------------------------------------------------------------------
    // Mono guitar -> stereo output
    //
    // Guitar input channel 0 отправляется одновременно
    // в Left и Right.
    //
    // Pitch detection продолжает использовать оригинальный
    // input channel 0.

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
    // Copy guitar input into pitch buffer.
    //
    // Когда накопится 2048 samples,
    // передаём блок PitchThread.

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
            //==================================================================
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

            //==================================================================
            // Second analysis buffer
            //
            // Если первый buffer ещё занят PitchThread,
            // используем второй.

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
    // Start metronome playback when a beat occurs.

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
    // Один mono sample метронома отправляется
    // одновременно во все output channels.
    //
    // Playback position увеличивается только один раз
    // за sample, а не за каждый channel.

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

        // Первый beat громче остальных.

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
//
// Определяет энергию входного сигнала.
//
// Используется для:
// - определения наличия звука;
// - onset detection;
// - отбрасывания тишины.
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
            static_cast<double>(
                samples[i]);

        sum += x * x;
    }

    return std::sqrt(
        sum
        / static_cast<double>(
            numSamples));
}

//==============================================================================
// Pitch detection
//
// Используется YIN-подобный алгоритм:
//
// 1. удаляем DC offset;
// 2. считаем RMS;
// 3. считаем difference function;
// 4. считаем CMNDF;
// 5. ищем наиболее вероятный период;
// 6. period -> frequency.
//
// Эта функция НЕ вызывается из audio callback.
// Она выполняется в PitchThread.
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
        static_cast<double>(
            numSamples);

    //--------------------------------------------------------------------------
    // RMS

    double rms = 0.0;

    for (int i = 0;
        i < numSamples;
        ++i)
    {
        const double x =
            static_cast<double>(
                samples[i])
            - mean;

        rms += x * x;
    }

    rms =
        std::sqrt(
            rms
            / static_cast<double>(
                numSamples));

    // Слишком тихий сигнал считаем тишиной.

    if (rms < 0.005)
        return 0.0;

    //--------------------------------------------------------------------------
    // Search range
    //
    // Guitar range:
    //
    // 60 Hz ... 500 Hz
    //
    // Диапазон немного шире реальных открытых струн,
    // чтобы detection был устойчивее.

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
                static_cast<double>(
                    samples[i])
                - mean;

            const double b =
                static_cast<double>(
                    samples[i + lag])
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
        runningSum +=
            difference[lag];

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

            // Идём дальше по минимуму,
            // пока значение продолжает улучшаться.

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
    // Fallback:
    //
    // Если сильного минимума нет,
    // используем абсолютный минимум CMNDF.

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
        / static_cast<double>(
            bestLag);
}

//==============================================================================
// Convert frequency to musical note
//
// Например:
//
// 82.41 Hz  -> E2
// 110.00 Hz -> A2
// 146.83 Hz -> D3
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
// Здесь рисуются только графические карточки.
//
// Сам текст находится в Label-компонентах.
//
// Карточки:
//
// 1. Top bar
// 2. Pitch
// 3. Target
// 4. Status
// 5. Score
//==============================================================================

void MainComponent::paint(
    juce::Graphics& g)
{
    //--------------------------------------------------------------------------
    // Background

    g.fillAll(
        UiColors::background);

    //--------------------------------------------------------------------------
    // Helper для современных rounded cards.

    auto drawCard =
        [&g](juce::Rectangle<float> area)
        {
            // Очень мягкая тень.

            g.setColour(
                juce::Colour(
                    0x12000000));

            g.fillRoundedRectangle(
                area.translated(
                    0.0f,
                    3.0f),
                16.0f);

            // Основная белая карточка.

            g.setColour(
                UiColors::card);

            g.fillRoundedRectangle(
                area,
                16.0f);

            // Тонкая граница.

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
            static_cast<float>(
                getWidth() - 48),
            72.0f));

    //--------------------------------------------------------------------------
    // Pitch card

    drawCard(
        juce::Rectangle<float>(
            24.0f,
            120.0f,
            static_cast<float>(
                getWidth() - 48),
            150.0f));

    //--------------------------------------------------------------------------
    // Target card

    drawCard(
        juce::Rectangle<float>(
            24.0f,
            294.0f,
            static_cast<float>(
                getWidth() - 48),
            100.0f));

    //--------------------------------------------------------------------------
    // Status card

    drawCard(
        juce::Rectangle<float>(
            24.0f,
            418.0f,
            static_cast<float>(
                getWidth() - 48),
            90.0f));

    //--------------------------------------------------------------------------
    // Score card

    drawCard(
        juce::Rectangle<float>(
            24.0f,
            532.0f,
            static_cast<float>(
                getWidth() - 48),
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
    // Слева:
    //
    // Beat | Bar Beat | BPM | Speed | Difficulty
    //
    // Справа:
    //
    // Audio Settings

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

    //--------------------------------------------------------------------------
    // Current BPM
    //
    // Показываем только число.
    //
    // Например:
    //
    // 60
    // 120
    // 144

    bpmLabel.setBounds(
        128,
        38,
        55,
        32);

    //--------------------------------------------------------------------------
    // Speed buttons

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

    //--------------------------------------------------------------------------
    // Difficulty buttons

    easyButton.setBounds(
        375,
        40,
        70,
        32);

    normalButton.setBounds(
        451,
        40,
        85,
        32);

    hardButton.setBounds(
        542,
        40,
        70,
        32);

    //--------------------------------------------------------------------------
    // Audio Settings

    audioSettingsButton.setBounds(
        width - 154,
        40,
        120,
        32);

    //--------------------------------------------------------------------------
    // Pitch

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