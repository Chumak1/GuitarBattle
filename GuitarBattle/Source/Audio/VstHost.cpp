#include "VstHost.h"

#include "Vst3ScanWorker.h"

#include <algorithm>

namespace
{
    //==========================================================================
    // Writes the plugin's opaque state blob next to the application settings.
    // Called on the message thread only, after the audio thread has stopped
    // referencing the instance.

    void writePluginState (const juce::File& file,
                           juce::AudioPluginInstance& plugin)
    {
        if (file.getFullPathName().isEmpty())
            return;

        juce::MemoryBlock state;
        plugin.getStateInformation (state);

        if (state.getSize() == 0)
            return;

        file.getParentDirectory().createDirectory();

        juce::FileOutputStream stream (file);

        if (! stream.openedOk())
            return;

        stream.write (state.getData(), state.getSize());
        stream.flush();
    }
}

//==============================================================================
// VST3 scanner that inspects every plugin file in a separate helper process.
//
// JUCE's VST3 implementation asserts that plugin initialisation runs on the
// message thread, and the VST3 spec agrees: many plugins call message-thread
// only functions while their factory is enumerated. Scanning in a background
// thread therefore trips JUCE_ASSERT_MESSAGE_THREAD (and can deadlock a real
// plugin). The helper process gives us a message thread that is free to block
// for minutes, and it isolates crashes: a broken shell only kills the helper.
//
// The helper is the same executable started with Vst3ScanWorker arguments. The
// scan thread blocks until the helper exits, but it can kill it at any moment,
// so CANCEL stays instant even when a plugin hangs.

namespace
{
    class OutOfProcessVst3Scanner final : public juce::KnownPluginList::CustomScanner
    {
    public:
        OutOfProcessVst3Scanner (juce::File executableToUse,
                                 juce::File tempDirectoryToUse,
                                 std::function<bool()> shouldStopToUse)
            : executable (std::move (executableToUse)),
              tempDirectory (std::move (tempDirectoryToUse)),
              shouldStop (std::move (shouldStopToUse))
        {
            tempDirectory.createDirectory();
        }

        bool findPluginTypesFor (juce::AudioPluginFormat& format,
                                 juce::OwnedArray<juce::PluginDescription>& result,
                                 const juce::String& fileOrIdentifier) override
        {
            juce::ignoreUnused (format);

            // The user cancelled: report success with no types so the file is
            // not blacklisted. The scan loop stops on its own afterwards.
            if (shouldStop())
                return true;

            const auto outputFile = tempDirectory.getChildFile (
                "scan_" + juce::String (juce::Time::currentTimeMillis())
                    + "_" + juce::String (++fileCounter) + ".xml");

            outputFile.deleteFile();

            juce::StringArray arguments;
            arguments.add (executable.getFullPathName());
            arguments.add (juce::String (Vst3ScanWorker::fileArgument)
                               + fileOrIdentifier);
            arguments.add (juce::String (Vst3ScanWorker::outputArgument)
                               + outputFile.getFullPathName());

            juce::ChildProcess helper;

            if (! helper.start (arguments, 0))
            {
                // The helper could not be launched at all. That says nothing
                // about the plugin, so do not blacklist it.
                return true;
            }

            while (helper.isRunning())
            {
                if (shouldStop())
                {
                    // TerminateProcess: a plugin that hangs inside the helper
                    // must not keep the scan alive after CANCEL.
                    helper.kill();
                    helper.waitForProcessToFinish (2000);
                    outputFile.deleteFile();
                    return true;
                }

                juce::Thread::sleep (20);
            }

            // A plugin that crashed the helper leaves no result file behind.
            // Returning false makes KnownPluginList blacklist it, so the rest
            // of the scan can continue.
            if (! outputFile.existsAsFile())
                return false;

            if (auto xml = juce::parseXML (outputFile))
            {
                for (int i = 0; i < xml->getNumChildElements(); ++i)
                {
                    if (const auto* element = xml->getChildElement (i))
                    {
                        juce::PluginDescription description;

                        if (description.loadFromXml (*element))
                            result.add (new juce::PluginDescription (description));
                    }
                }
            }

            outputFile.deleteFile();
            return true;
        }

        void scanFinished() override
        {
            // The directory belongs to this scan only, so it is safe to remove
            // even when a newer scan is already running in its own directory.
            tempDirectory.deleteRecursively();
        }

    private:
        const juce::File executable;
        const juce::File tempDirectory;
        const std::function<bool()> shouldStop;
        int fileCounter = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OutOfProcessVst3Scanner)
    };
}

//==============================================================================
// Background VST3 scanner. Scanning can take minutes and must never run on the
// message thread. The worker owns its progress counter and only reports back
// through a weak lifetime token, so it never dereferences the host directly.

class VstHost::ScanThread final : public juce::Thread
{
public:
    ScanThread (juce::File deadMansPedalFileToUse,
                std::weak_ptr<int> lifetimeTokenToUse,
                VstHost* hostToUse,
                int generationToUse)
        : juce::Thread ("GuitarBattle VST3 scan"),
          deadMansPedalFile (std::move (deadMansPedalFileToUse)),
          lifetimeToken (std::move (lifetimeTokenToUse)),
          host (hostToUse),
          generation (generationToUse)
    {
    }

    float getProgress() const noexcept
    {
        return progress.load (std::memory_order_relaxed);
    }

    void run() override
    {
        std::unique_ptr<juce::XmlElement> scanned;
        int failedCount = 0;

       #if JUCE_PLUGINHOST_VST3
        {
            juce::KnownPluginList found;
            juce::VST3PluginFormat format;

            // Every plugin file is inspected by a helper process. The helper
            // has a free message thread (the VST3 spec requires one) and its
            // crashes cannot reach this process.
            //
            // Each scan owns a private subdirectory: a cancelled scan can be
            // reaped while the next one is already running, and its cleanup
            // must never delete the new scan's in-flight result files.
            found.setCustomScanner (std::make_unique<OutOfProcessVst3Scanner> (
                juce::File::getSpecialLocation (juce::File::currentExecutableFile),
                juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("GuitarBattleVst3Scan")
                    .getChildFile ("scan_" + juce::String (generation)),
                [this] { return threadShouldExit(); }));

            juce::PluginDirectoryScanner scanner (
                found,
                format,
                format.getDefaultLocationsToSearch(),
                true,
                deadMansPedalFile,
                false);

            juce::String currentName;

            while (! threadShouldExit()
                   && scanner.scanNextFile (true, currentName))
            {
                progress.store (scanner.getProgress(),
                                std::memory_order_relaxed);
            }

            if (threadShouldExit())
                return;

            progress.store (1.0f, std::memory_order_relaxed);
            failedCount = scanner.getFailedFiles().size();
            scanned = found.createXml();
        }
       #endif

        juce::MessageManager::callAsync (
            [hostToUse = host, token = lifetimeToken,
             generationToUse = generation,
             failedCountToUse = failedCount,
             scanned = std::move (scanned)]() mutable
            {
                // The host may have been destroyed while the scan was running,
                // or the user may have cancelled and started another scan.
                if (token.expired() || hostToUse == nullptr)
                    return;

                hostToUse->scanFinished (generationToUse,
                                         scanned.get(),
                                         failedCountToUse);
            });
    }

private:
    const juce::File deadMansPedalFile;
    const std::weak_ptr<int> lifetimeToken;
    VstHost* const host;
    const int generation;
    std::atomic<float> progress{ 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScanThread)
};

//==============================================================================
// A desktop window that owns the plugin editor. Closing the window hides it;
// the editor stays alive until the plugin is replaced or removed, so reopening
// the window is instant and the plugin keeps its UI state.

namespace
{
    class VstEditorWindow final : public juce::DocumentWindow
    {
    public:
        VstEditorWindow (const juce::String& name,
                         juce::AudioProcessorEditor* editorToOwn)
            : juce::DocumentWindow (name,
                                    juce::Colours::black,
                                    juce::DocumentWindow::closeButton)
        {
            setUsingNativeTitleBar (true);
            setAlwaysOnTop (true);
            setContentOwned (editorToOwn, true);
            setResizable (editorToOwn->isResizable(), false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            setVisible (false);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VstEditorWindow)
    };
}

//==============================================================================
VstHost::VstHost()
    : lifetimeToken (std::make_shared<int> (0))
{
}

VstHost::~VstHost()
{
    stopTimer();

    // Invalidate pending asynchronous plugin-creation callbacks.
    lifetimeToken.reset();

    // The scanner cannot be interrupted inside scanNextFile(). Ask it to stop
    // and give it a moment; a worker that is still inside the VST3 SDK is
    // leaked on purpose, because force-killing it would corrupt the SDK.
    ++scanGeneration;

    auto abandonRunningThread = [] (std::unique_ptr<ScanThread>& thread)
    {
        if (thread != nullptr)
        {
            thread->signalThreadShouldExit();

            if (! thread->waitForThreadToExit (250))
                (void) thread.release();
        }
    };

    abandonRunningThread (scanThread);

    for (auto& thread : abandonedScanThreads)
        abandonRunningThread (thread);

    abandonedScanThreads.clear();

    editorWindow.reset();

    // MainComponent calls shutdownAudio() before destroying its members, so
    // the audio callback is already stopped. The handshake still runs to be
    // defensive when the host is destroyed from another order.
    suspendAudio (2000);

    if (activeSlot != nullptr)
    {
        if (activeSlot->plugin != nullptr)
        {
            writePluginState (stateFileFor (activeSlot->identifier),
                              *activeSlot->plugin);
            activeSlot->plugin->releaseResources();
        }

        activeSlot.reset();
    }

    publishedSlot.store (nullptr, std::memory_order_release);
    resumeAudio();

    releaseRetiredSlots (true);

    // Slots that never acknowledged the audio handshake are kept alive for the
    // whole process lifetime; releasing them here is safe because the device is
    // stopped and the audio callback cannot run any more.
    for (auto& slot : stuckSlots)
    {
        if (slot != nullptr && slot->plugin != nullptr)
            slot->plugin->releaseResources();
    }

    stuckSlots.clear();
}

//==============================================================================
void VstHost::initialise (juce::PropertiesFile* settingsToUse)
{
    settings = settingsToUse;

    juce::addDefaultFormatsToManager (formatManager);

    if (settings != nullptr)
    {
        const auto directory = settings->getFile().getParentDirectory();
        knownPluginsFile = directory.getChildFile ("VST3Plugins.xml");
        scanCrashedFile = directory.getChildFile ("VST3ScanCrashed.txt");

        bypassed.store (settings->getBoolValue ("VstBypassed", false),
                        std::memory_order_relaxed);

        if (knownPluginsFile.existsAsFile())
        {
            if (auto xml = juce::parseXML (knownPluginsFile))
                knownPlugins.recreateFromXml (*xml);
        }
    }

    // Scanning is manual: the standard VST3 folders can contain hundreds of
    // plugins (and some vendor shells take minutes to enumerate), so the app
    // never scans behind the player's back. RESCAN in the VST panel starts it.
    notifyStateChanged();
}

void VstHost::restoreSavedPlugin()
{
    if (settings == nullptr)
        return;

    bypassed.store (settings->getBoolValue ("VstBypassed", false),
                    std::memory_order_relaxed);

    const auto savedXml = settings->getValue ("VstPluginDescription");

    if (savedXml.isEmpty())
    {
        statusMessage = "No plugin loaded.";
        notifyStateChanged();
        return;
    }

    auto xml = juce::parseXML (savedXml);

    if (xml == nullptr)
    {
        statusMessage = "Saved VST settings are unreadable.";
        notifyStateChanged();
        return;
    }

    juce::PluginDescription description;

    if (! description.loadFromXml (*xml))
    {
        statusMessage = "Saved VST plugin description is invalid.";
        notifyStateChanged();
        return;
    }

    loadPlugin (description);
}

void VstHost::saveSettings()
{
    if (settings == nullptr)
        return;

    settings->setValue ("VstBypassed",
                        bypassed.load (std::memory_order_relaxed));

    if (activeSlot != nullptr && activeSlot->identifier.isNotEmpty())
    {
        if (auto xml = activeSlot->description.createXml())
        {
            settings->setValue ("VstPluginDescription",
                                xml->toString (juce::XmlElement::TextFormat()
                                                   .singleLine()));
        }
    }
    else
    {
        settings->removeValue ("VstPluginDescription");
    }

    settings->saveIfNeeded();
}

//==============================================================================
juce::Array<juce::PluginDescription> VstHost::getScannedPlugins() const
{
    juce::Array<juce::PluginDescription> result;

    for (const auto& description : knownPlugins.getTypes())
    {
        // Only effects can process the monitored guitar. Instruments would
        // need MIDI input and are not useful in this signal path.
        if (! description.isInstrument)
            result.add (description);
    }

    std::sort (result.begin(), result.end(),
               [] (const juce::PluginDescription& a,
                   const juce::PluginDescription& b)
               {
                   return a.name.compareIgnoreCase (b.name) < 0;
               });

    return result;
}

bool VstHost::isScanning() const noexcept
{
    return scanning;
}

float VstHost::getScanProgress() const noexcept
{
    return scanThread != nullptr ? scanThread->getProgress() : 0.0f;
}

void VstHost::scanPlugins()
{
    if (scanning)
        return;

    if (settings == nullptr)
    {
        statusMessage = "VST host is not initialised.";
        notifyStateChanged();
        return;
    }

    // A cancelled scan may still be inside the VST3 SDK. Starting a second
    // scanner while the first one is alive would load the same plugin shells
    // twice, so wait until the previous worker has actually exited.
    reapAbandonedScans();

    if (! abandonedScanThreads.empty())
    {
        statusMessage = "The previous scan is still stopping. Try again in a moment.";
        notifyStateChanged();
        return;
    }

    // A file that crashed the helper during an earlier manual scan is retried
    // now: scanning is out-of-process, so the retry cannot hurt the app. The
    // dead-man's pedal is only used to skip it for the rest of this scan.
    scanCrashedFile.deleteFile();

    ++scanGeneration;
    scanning = true;

    scanThread = std::make_unique<ScanThread> (scanCrashedFile,
                                               lifetimeToken,
                                               this,
                                               scanGeneration);

    if (! scanThread->startThread (juce::Thread::Priority::low))
    {
        scanThread.reset();
        scanning = false;
        statusMessage = "Could not start the VST3 scan.";
    }
    else
    {
        statusMessage = "Scanning VST3 plugins...";
    }

    notifyStateChanged();
}

void VstHost::cancelScan()
{
    if (! scanning && scanThread == nullptr)
        return;

    // Invalidate any result that the worker may already be posting.
    ++scanGeneration;
    scanning = false;

    if (scanThread != nullptr)
    {
        scanThread->signalThreadShouldExit();

        // Never call stopThread(): its timeout path force-terminates the
        // worker, which would leave the VST3 SDK in an undefined state. Keep
        // the object alive and let the timer reap it once run() returns.
        if (scanThread->waitForThreadToExit (200))
            scanThread.reset();
        else
            abandonedScanThreads.push_back (std::move (scanThread));
    }

    // The worker was alive when it wrote the dead-man's-pedal entry, so the
    // entry was not caused by a crash; drop it so the next scan does not skip
    // a healthy plugin.
    scanCrashedFile.deleteFile();

    statusMessage = "Scan cancelled.";
    startTimer (250);
    notifyStateChanged();
}

void VstHost::reapAbandonedScans()
{
    for (auto it = abandonedScanThreads.begin();
         it != abandonedScanThreads.end();)
    {
        // isThreadRunning() becomes false only after closeThreadHandle(), i.e.
        // after run() has returned; deleting the object is safe at that point.
        if (*it == nullptr || ! (*it)->isThreadRunning())
            it = abandonedScanThreads.erase (it);
        else
            ++it;
    }
}

void VstHost::scanFinished (int generation,
                            const juce::XmlElement* scannedTypes,
                            int failedCount)
{
    // A cancelled or superseded scan must never overwrite the plugin list.
    if (generation != scanGeneration || ! scanning)
        return;

    // run() has posted this message and is about to return. The timer reaps
    // the thread object, so the message thread never waits for the scanner.
    if (scanThread != nullptr)
        abandonedScanThreads.push_back (std::move (scanThread));

    if (scannedTypes != nullptr)
    {
        knownPlugins.recreateFromXml (*scannedTypes);

        // The out-of-process scanner survives plugin crashes, so a failed file
        // is retried on the next manual RESCAN instead of being hidden
        // permanently. The scan itself still skips it via the blacklist.
        knownPlugins.clearBlacklistedFiles();
        scanCrashedFile.deleteFile();

        saveKnownPluginCache();
    }

    scanning = false;

    statusMessage = "Scan finished: "
                    + juce::String (getScannedPlugins().size())
                    + " effect plugin(s)";

    if (failedCount > 0)
        statusMessage += ", " + juce::String (failedCount) + " could not be read";

    statusMessage += ".";

    startTimer (250);
    notifyStateChanged();
}

void VstHost::saveKnownPluginCache()
{
    if (knownPluginsFile.getFullPathName().isEmpty())
        return;

    if (auto xml = knownPlugins.createXml())
    {
        knownPluginsFile.getParentDirectory().createDirectory();
        xml->writeTo (knownPluginsFile);
    }
}

//==============================================================================
void VstHost::loadPlugin (const juce::PluginDescription& description)
{
    if (scanning)
    {
        statusMessage = "Wait for the VST3 scan to finish.";
        notifyStateChanged();
        return;
    }

    const int requestId = ++loadRequestId;

    statusMessage = "Loading " + description.name + "...";
    notifyStateChanged();

    std::weak_ptr<int> token = lifetimeToken;

    formatManager.createPluginInstanceAsync (
        description,
        deviceSampleRate,
        deviceBlockSize,
        [this, token, requestId, description] (
            std::unique_ptr<juce::AudioPluginInstance> instance,
            const juce::String& error)
        {
            if (token.expired() || requestId != loadRequestId)
                return;

            if (instance == nullptr)
            {
                statusMessage = error.isNotEmpty()
                    ? error
                    : juce::String ("Could not load the plugin.");
                notifyStateChanged();
                return;
            }

            if (instance->getTotalNumInputChannels() <= 0)
            {
                statusMessage = "This plugin has no audio input. "
                                "Instrument plugins are not supported here.";
                notifyStateChanged();
                return;
            }

            auto slot = std::make_unique<Slot>();
            slot->plugin = std::move (instance);
            slot->description = description;
            slot->identifier = description.createIdentifierString();
            slot->sampleRate = deviceSampleRate;
            slot->maxBlockSize = juce::jmax (1, deviceBlockSize);
            slot->numChannels = juce::jmax (1,
                slot->plugin->getTotalNumInputChannels(),
                slot->plugin->getTotalNumOutputChannels());
            slot->numOutputChannels = juce::jmax (1,
                slot->plugin->getTotalNumOutputChannels());

            slot->plugin->setRateAndBufferSizeDetails (slot->sampleRate,
                                                       slot->maxBlockSize);
            slot->plugin->prepareToPlay (slot->sampleRate,
                                         slot->maxBlockSize);
            slot->buffer.setSize (slot->numChannels,
                                  slot->maxBlockSize,
                                  false, true, true);

            restorePluginState (*slot->plugin, slot->identifier);

            const auto loadedName = description.name;
            publishSlot (std::move (slot));

            statusMessage = "Loaded " + loadedName;
            saveSettings();
            notifyStateChanged();
        });
}

void VstHost::unloadPlugin()
{
    // Any in-flight load must not publish itself after the user removed the
    // plugin.
    ++loadRequestId;

    const bool hadPlugin = activeSlot != nullptr;

    detachSlot();

    statusMessage = hadPlugin ? "Plugin removed." : "No plugin loaded.";

    saveSettings();
    notifyStateChanged();
}

bool VstHost::isPluginLoaded() const
{
    return activeSlot != nullptr;
}

juce::String VstHost::getLoadedPluginName() const
{
    return activeSlot != nullptr && activeSlot->plugin != nullptr
        ? activeSlot->plugin->getName()
        : juce::String();
}

bool VstHost::isBypassed() const noexcept
{
    return bypassed.load (std::memory_order_relaxed);
}

void VstHost::setBypassed (bool shouldBypass)
{
    bypassed.store (shouldBypass, std::memory_order_relaxed);
    saveSettings();
    notifyStateChanged();
}

//==============================================================================
void VstHost::showEditor()
{
    if (activeSlot == nullptr || activeSlot->plugin == nullptr)
    {
        statusMessage = "Load a plugin first.";
        notifyStateChanged();
        return;
    }

    if (editorWindow != nullptr)
    {
        editorWindow->setVisible (true);
        editorWindow->toFront (true);
        return;
    }

    juce::AudioProcessorEditor* editor = nullptr;

    if (activeSlot->plugin->hasEditor())
        editor = activeSlot->plugin->createEditorAndMakeActive();

    if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor (*activeSlot->plugin);

    if (editor == nullptr)
    {
        statusMessage = "This plugin has no editor.";
        notifyStateChanged();
        return;
    }

    editorWindow = std::make_unique<VstEditorWindow> (
        activeSlot->plugin->getName(), editor);
}

void VstHost::closeEditor()
{
    editorWindow.reset();
}

//==============================================================================
juce::String VstHost::getLastStatusMessage() const
{
    return statusMessage;
}

int VstHost::getStateVersion() const noexcept
{
    return stateVersion;
}

//==============================================================================
void VstHost::prepareForDevice (double sampleRate, int blockSize)
{
    if (sampleRate <= 0.0 || blockSize <= 0)
        return;

    const bool changed = (sampleRate != deviceSampleRate
                          || blockSize != deviceBlockSize);

    deviceSampleRate = sampleRate;
    deviceBlockSize = blockSize;

    processingEnabled.store (true, std::memory_order_release);

    if (! changed || activeSlot == nullptr)
        return;

    // The plugin was prepared for the previous sample rate / block size. A
    // JUCE processor cannot be re-prepared while the audio thread may still be
    // inside processBlock(), so detach the slot and create a fresh instance
    // with the new configuration.
    const auto description = activeSlot->description;

    detachSlot();
    loadPlugin (description);
}

//==============================================================================
void VstHost::processBlock (juce::AudioBuffer<float>& buffer,
                            int startSample,
                            int numSamples,
                            int outputChannels) noexcept
{
    if (numSamples <= 0 || outputChannels <= 0)
        return;

    if (suspendRequested.load (std::memory_order_acquire))
        return;

    if (! processingEnabled.load (std::memory_order_relaxed)
        || bypassed.load (std::memory_order_relaxed))
        return;

    // Announce that the callback is about to touch the plugin. The message
    // thread sets suspendRequested before it waits for this flag, so the two
    // sides cannot miss each other.
    audioInCallback.store (true, std::memory_order_seq_cst);

    if (suspendRequested.load (std::memory_order_seq_cst))
    {
        audioInCallback.store (false, std::memory_order_seq_cst);
        return;
    }

    Slot* slot = publishedSlot.load (std::memory_order_acquire);

    if (slot != nullptr
        && slot->plugin != nullptr
        && slot->numChannels > 0
        && numSamples <= slot->maxBlockSize)
    {
        // Copy the dry monitored guitar into the plugin buffer. Mono input is
        // duplicated to every plugin input; the pitch-analysis buffers were
        // filled before this point, so scoring stays dry.
        for (int channel = 0; channel < slot->numChannels; ++channel)
        {
            slot->buffer.clear (channel, 0, numSamples);

            const int source = juce::jmin (channel, outputChannels - 1);
            slot->buffer.copyFrom (channel, 0, buffer, source, startSample,
                                   numSamples);
        }

        juce::AudioBuffer<float> view (slot->buffer.getArrayOfWritePointers(),
                                       slot->numChannels,
                                       numSamples);

        slot->midi.clear();
        slot->plugin->processBlock (view, slot->midi);

        const int usableOutputs = juce::jmin (slot->numOutputChannels,
                                              slot->numChannels);

        if (usableOutputs > 0)
        {
            for (int channel = 0; channel < outputChannels; ++channel)
            {
                const int source = juce::jmin (channel, usableOutputs - 1);
                buffer.copyFrom (channel, startSample, slot->buffer, source, 0,
                                 numSamples);
            }
        }
    }

    audioInCallback.store (false, std::memory_order_seq_cst);
}

void VstHost::suspendProcessing() noexcept
{
    processingEnabled.store (false, std::memory_order_release);
}

//==============================================================================
bool VstHost::suspendAudio (int timeoutMs)
{
    suspendRequested.store (true, std::memory_order_seq_cst);

    const auto start = juce::Time::getMillisecondCounter();

    while (audioInCallback.load (std::memory_order_seq_cst))
    {
        if (juce::Time::getMillisecondCounter() - start
            >= static_cast<juce::uint32> (timeoutMs))
            return false;

        juce::Thread::sleep (1);
    }

    return true;
}

void VstHost::resumeAudio()
{
    suspendRequested.store (false, std::memory_order_seq_cst);
}

//==============================================================================
void VstHost::publishSlot (std::unique_ptr<Slot> slot)
{
    const bool audioStopped = suspendAudio (500);

    // The editor owns plugin UI objects and must die before the instance.
    editorWindow.reset();

    if (activeSlot != nullptr)
    {
        if (audioStopped)
        {
            // Saves the outgoing state before the new instance restores it.
            retireSlot (std::move (activeSlot));
        }
        else
        {
            // The callback never acknowledged. Keep the instance alive for the
            // rest of the process rather than risk a use-after-free.
            stuckSlots.push_back (std::move (activeSlot));
        }
    }

    activeSlot = std::move (slot);
    publishedSlot.store (activeSlot.get(), std::memory_order_release);

    resumeAudio();
}

void VstHost::detachSlot()
{
    const bool audioStopped = suspendAudio (500);

    editorWindow.reset();

    if (activeSlot != nullptr)
    {
        if (audioStopped)
            retireSlot (std::move (activeSlot));
        else
            stuckSlots.push_back (std::move (activeSlot));
    }

    publishedSlot.store (nullptr, std::memory_order_release);

    resumeAudio();
}

void VstHost::retireSlot (std::unique_ptr<Slot> slot)
{
    if (slot == nullptr)
        return;

    // Persist the state now: a reload triggered right after this call must see
    // the latest parameters, not a stale file from the previous session.
    if (slot->plugin != nullptr)
        writePluginState (stateFileFor (slot->identifier), *slot->plugin);

    retiredSlots.push_back (std::move (slot));
    lastRetireMs = juce::Time::getMillisecondCounter();

    if (! isTimerRunning())
        startTimer (250);
}

void VstHost::releaseRetiredSlots (bool force)
{
    if (retiredSlots.empty())
        return;

    if (! force)
    {
        const auto now = juce::Time::getMillisecondCounter();

        // The audio thread is already outside the instance (publishSlot and
        // detachSlot wait for it), so the delay only keeps a plugin swap from
        // stalling the UI thread with releaseResources().
        if (now - lastRetireMs < 600)
            return;
    }

    for (auto& slot : retiredSlots)
    {
        if (slot != nullptr && slot->plugin != nullptr)
            slot->plugin->releaseResources();
    }

    retiredSlots.clear();
}

//==============================================================================
void VstHost::timerCallback()
{
    releaseRetiredSlots (false);
    reapAbandonedScans();

    // One timer serves both the deferred plugin release and the scan-thread
    // reaper; stop it only when neither has work left.
    if (retiredSlots.empty() && abandonedScanThreads.empty())
        stopTimer();
}

void VstHost::notifyStateChanged()
{
    ++stateVersion;

    if (onStateChanged)
        onStateChanged();
}

//==============================================================================
void VstHost::restorePluginState (juce::AudioPluginInstance& plugin,
                                  const juce::String& identifier)
{
    const auto file = stateFileFor (identifier);

    if (file.getFullPathName().isEmpty() || ! file.existsAsFile())
        return;

    juce::MemoryBlock state;

    if (! file.loadFileAsData (state) || state.getSize() == 0)
        return;

    plugin.setStateInformation (state.getData(),
                                static_cast<int> (state.getSize()));
}

juce::File VstHost::stateFileFor (const juce::String& identifier) const
{
    if (settings == nullptr || identifier.isEmpty())
        return {};

    const auto directory = settings->getFile().getParentDirectory();
    const auto hash = identifier.hashCode64();

    return directory.getChildFile (
        "VstState_" + juce::String::toHexString (hash) + ".dat");
}
