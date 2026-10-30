#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

//==============================================================================
// VST3 monitor host.
//
// The plugin processes only the monitored guitar path: getNextAudioBlock()
// copies the dry input into the output channels first, the pitch-analysis
// buffers are filled from that same dry input, and only then the published
// plugin slot is asked to process the output copy. Note detection and scoring
// therefore never see the processed signal.
//
// Threading contract:
//   * every method except processBlock()/suspendProcessing() runs on the
//     message thread;
//   * processBlock() only reads two atomics and a pre-prepared slot: no
//     allocation, no locks, no filesystem and no shared_ptr refcount traffic;
//   * replacing a plugin is bracketed by suspendAudio()/resumeAudio(). The
//     audio callback announces itself through `audioInCallback`, so the
//     message thread can wait until the plugin is provably unused before it
//     touches or destroys the instance.
class VstHost final : private juce::Timer
{
public:
    VstHost();
    ~VstHost() override;

    //==========================================================================
    // Message-thread lifecycle

    void initialise (juce::PropertiesFile* settingsToUse);
    void restoreSavedPlugin();
    void saveSettings();

    //==========================================================================
    // Scanning

    juce::Array<juce::PluginDescription> getScannedPlugins() const;
    bool isScanning() const noexcept;
    float getScanProgress() const noexcept;
    void scanPlugins();
    void cancelScan();

    //==========================================================================
    // Loading / bypass / editor

    void loadPlugin (const juce::PluginDescription& description);
    void unloadPlugin();

    bool isPluginLoaded() const;
    juce::String getLoadedPluginName() const;
    bool isBypassed() const noexcept;
    void setBypassed (bool shouldBypass);

    void showEditor();
    void closeEditor();

    // Human-readable result of the last load/unload/scan action.
    juce::String getLastStatusMessage() const;
    int getStateVersion() const noexcept;

    //==========================================================================
    // Audio device

    // Called on the message thread when the device starts or changes. When the
    // sample rate or block size changed, the current plugin is detached and
    // reloaded with the new configuration.
    void prepareForDevice (double sampleRate, int blockSize);

    //==========================================================================
    // Audio thread

    void processBlock (juce::AudioBuffer<float>& buffer,
                       int startSample,
                       int numSamples,
                       int outputChannels) noexcept;

    // Called from releaseResources(): stop touching the plugin while the
    // device is shut down. No allocation, no locking.
    void suspendProcessing() noexcept;

    // Called on the message thread after a state change (load, unload, bypass,
    // scan finished). Used by the main screen to refresh the VST button.
    std::function<void()> onStateChanged;

private:
    //==========================================================================
    // Immutable per-plugin data published to the audio thread. Everything is
    // prepared on the message thread before the pointer becomes visible.

    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::AudioBuffer<float> buffer;
        juce::MidiBuffer midi;
        juce::PluginDescription description;
        juce::String identifier;
        int numChannels = 0;
        int numOutputChannels = 0;
        int maxBlockSize = 0;
        double sampleRate = 0.0;
    };

    class ScanThread;

    //==========================================================================
    // Message-thread helpers

    void timerCallback() override;
    void publishSlot (std::unique_ptr<Slot> slot);
    void detachSlot();
    void retireSlot (std::unique_ptr<Slot> slot);
    void releaseRetiredSlots (bool force);
    void notifyStateChanged();
    void saveKnownPluginCache();
    void restorePluginState (juce::AudioPluginInstance& plugin,
                             const juce::String& identifier);
    juce::File stateFileFor (const juce::String& identifier) const;
    void scanFinished (int generation,
                       const juce::XmlElement* scannedTypes,
                       int failedCount);
    void reapAbandonedScans();

    // Blocks the message thread until the audio callback is outside
    // processBlock(). The wait is bounded; if the callback does not report
    // back, the caller keeps the old slot alive instead of risking a
    // use-after-free.
    bool suspendAudio (int timeoutMs);
    void resumeAudio();

    //==========================================================================
    // State

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::PropertiesFile* settings = nullptr;
    juce::File knownPluginsFile;
    juce::File scanCrashedFile;

    // Message-thread ownership of the published slot.
    std::unique_ptr<Slot> activeSlot;

    // Read by the audio thread. Written only while the audio callback is
    // suspended (or while the device is stopped).
    std::atomic<Slot*> publishedSlot{ nullptr };

    // Audio-thread handshake. `suspendRequested` is set by the message thread
    // before it waits for `audioInCallback` to drop.
    std::atomic<bool> suspendRequested{ false };
    std::atomic<bool> audioInCallback{ false };

    std::atomic<bool> processingEnabled{ true };
    std::atomic<bool> bypassed{ false };

    double deviceSampleRate = 44100.0;
    int deviceBlockSize = 512;

    std::unique_ptr<juce::DocumentWindow> editorWindow;

    // Plugins that are no longer published. Their instances are released on
    // the message thread after a short grace period so a plugin swap does not
    // stall the UI thread.
    std::vector<std::unique_ptr<Slot>> retiredSlots;
    juce::uint32 lastRetireMs = 0;

    // Slots the audio callback never released (handshake timeout). They are
    // intentionally leaked until the host is destroyed: the callback might
    // still be inside one of them.
    std::vector<std::unique_ptr<Slot>> stuckSlots;

    juce::String statusMessage{ "No plugin loaded." };
    int stateVersion = 0;
    int loadRequestId = 0;

    bool scanning = false;

    // Bumped whenever a scan starts or is cancelled. A worker that posts a
    // result with a stale generation is ignored, so a cancelled scan can never
    // overwrite the plugin list after the user moved on.
    int scanGeneration = 0;
    std::unique_ptr<ScanThread> scanThread;

    // Cancelled scans whose worker has not exited yet. The VST3 scanner cannot
    // be interrupted inside scanNextFile(), so the thread object is kept alive
    // and reaped by the timer once the thread reports that it has finished.
    std::vector<std::unique_ptr<ScanThread>> abandonedScanThreads;

    // Invalidated in the destructor; asynchronous plugin creation callbacks
    // check it before touching `this`.
    std::shared_ptr<int> lifetimeToken;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VstHost)
};
