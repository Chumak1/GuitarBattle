#pragma once

#include <JuceHeader.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

class ApiClient;

/*
    Central UI sound system.

    One registry of fixed event keys, one manifest downloaded from the server,
    one realtime-safe mixer. Screens never reference audio files: they call
    play("nav.section") and the manager decides whether the event exists, is
    enabled, which category it belongs to and how loud it is.

    The mixer only reads pre-decoded buffers that live for the whole session
    (replaced buffers are kept alive), so the audio callback does no allocation,
    locking or file I/O. Triggers travel through a lock-free AbstractFifo.
*/
class UiSounds final
{
public:
    enum class Category { Navigation = 0, Action = 1, System = 2 };

    // Fixed registry: these keys are also the server-side identifiers.
    static juce::StringArray allKeys();
    static juce::String keyName (const juce::String& key);
    static Category keyCategory (const juce::String& key);

    UiSounds();
    ~UiSounds();

    void setApiClient (ApiClient* client) { api = client; }

    // Downloads the manifest and any changed files. Call on the message thread.
    void refreshManifest (const juce::String& token);

    // User preferences (persisted by MainComponent in the app properties).
    void setPreferences (bool soundsEnabled, float volume,
                         bool navigation, bool actions, bool system);
    bool isEnabled() const { return enabled.load (std::memory_order_relaxed); }
    float getVolume() const { return masterGain.load (std::memory_order_relaxed); }

    // Fire an event by key. Safe to call from the message thread only.
    void play (const juce::String& key);

    void prepare (double newSampleRate);
    // Adds the active UI voices to the output. Realtime safe.
    void render (juce::AudioBuffer<float>& output, int startSample, int numSamples);

    // Diagnostics for the settings screen: how many events have a file.
    int loadedEventCount() const;

private:
    enum { maxEntries = 32, maxVoices = 10, fifoSize = 64 };

    struct Entry
    {
        juce::String key;
        juce::String url;
        Category category = Category::Action;
        float gain = 1.0f;
        std::atomic<bool> active { true };
        std::atomic<int> version { 0 };
        std::atomic<int> loadedVersion { -1 };
        std::atomic<int> inflightVersion { -1 };
        std::atomic<const juce::AudioBuffer<float>*> buffer { nullptr };
        std::atomic<double> sourceRate { 44100.0 };
    };

    struct Voice
    {
        const juce::AudioBuffer<float>* buffer = nullptr;
        double position = 0.0;
        double step = 1.0;
        float gain = 0.0f;
        bool active = false;
    };

    int indexForKey (const juce::String& key) const;
    Entry* ensureEntry (const juce::String& key);
    void requestFile (int index, int version);
    void startVoice (int index);
    double nowMs() const;

    ApiClient* api = nullptr;

    std::array<std::unique_ptr<Entry>, maxEntries> entries;
    std::array<Voice, maxVoices> voices;
    std::array<double, maxEntries> lastPlayMs {};
    std::vector<std::unique_ptr<juce::AudioBuffer<float>>> ownedBuffers;

    juce::AbstractFifo fifo { fifoSize };
    std::array<int, fifoSize> fifoData {};

    juce::AudioFormatManager formats;

    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<bool> enabled { true };
    std::atomic<float> masterGain { 0.8f };
    std::array<std::atomic<bool>, 3> categoryEnabled { { {true}, {true}, {true} } };

    // Guards callbacks that may arrive after the manager is gone.
    std::shared_ptr<std::atomic<bool>> alive;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UiSounds)
};
