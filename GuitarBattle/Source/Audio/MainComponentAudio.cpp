#include "../MainComponent.h"

#include "PitchDetectionThread.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <future>
#include <limits>
#include <vector>

// Audio-only implementation for MainComponent. Everything here runs on the
// message thread except prepareToPlay()/getNextAudioBlock()/releaseResources(),
// which must remain realtime-safe.

// Forward declaration: the implementation is located later in this file.
static juce::AudioBuffer<float> makePitchPreservingTimeStretch(
    const juce::AudioBuffer<float>& source, double speedRatio);

void MainComponent::setPreferredInputChannel(int channel)
{
    if (channel >= 0 && ! ensureInputChannelEnabled(channel))
        channel = -1;

    preferredInputChannel.store(channel, std::memory_order_relaxed);

    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue("InputChannel", channel);
        settings->saveIfNeeded();
    }

    // Do not rebuild the combo from inside its own onChange callback. Defer
    // the refresh so the "(off)" marker disappears once the device reopens.
    juce::Component::SafePointer<MainComponent> safeThis(this);

    juce::MessageManager::callAsync(
        [safeThis]
        {
            if (auto* self = safeThis.getComponent())
                self->refreshGuitarInputCombo();
        });
}

void MainComponent::setInputSensitivity(float normalised)
{
    normalised = juce::jlimit(0.0f, 1.0f, normalised);
    inputSensitivity.store(normalised, std::memory_order_relaxed);

    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue("InputSensitivity", static_cast<double>(normalised));
        settings->saveIfNeeded();
    }

    if (inputMonitor != nullptr)
        inputMonitor->setSensitivityNormalised(normalised);
}

void MainComponent::updateInputMonitor()
{
    if (inputMonitor == nullptr)
        return;

    const float rms = inputMeterRms.load(std::memory_order_relaxed);
    const float peak = inputMeterPeak.load(std::memory_order_relaxed);
    const int clips = inputClipEvents.load(std::memory_order_relaxed);

    const bool noteActive = noteOn.load(std::memory_order_relaxed)
        && lastPitch > 20.0 && lastPitch < 1000.0;

    juce::String noteName;

    if (noteActive)
        noteName = pitchDetector.frequencyToNote(lastPitch);

    inputMonitor->updateMeter(rms, peak, clips, noteActive, noteName,
                              noteActive ? lastPitch : 0.0);
    inputMonitor->tick();
}

bool MainComponent::ensureInputChannelEnabled(int hardwareChannel)
{
    if (hardwareChannel < 0
        || hardwareChannel >= static_cast<int>(activeInputHardwareChannels.size()))
        return false;

    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return false;

    const auto names = device->getInputChannelNames();

    if (hardwareChannel >= names.size())
        return false;

    const auto active = device->getActiveInputChannels();
    auto setup = deviceManager.getAudioDeviceSetup();
    const auto previousSetup = setup;

    auto mask = setup.useDefaultInputChannels
        ? active
        : setup.inputChannels;

    if (mask[hardwareChannel])
        return true;

    mask.setBit(hardwareChannel, true);

    setup.inputChannels = mask;
    setup.useDefaultInputChannels = false;

    if (deviceManager.setAudioDeviceSetup(setup, true).isNotEmpty())
    {
        // The driver refused the wider channel set. Restore the previous
        // setup so the app keeps working with the channels it had.
        deviceManager.setAudioDeviceSetup(previousSetup, true);
        return false;
    }

    return true;
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &deviceManager)
        refreshGuitarInputCombo();
}

void MainComponent::startAudioDevice(const juce::XmlElement* savedState)
{
    setAudioChannels(32, 2, savedState);
    audioDeviceStarted = true;

    // Always make every available hardware input active. The saved device
    // state may contain a narrower mask from an older build or from the
    // device selector; the guitar may be plugged into any input, so the
    // client must be able to see all of them.
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const int availableInputs = device->getInputChannelNames().size();
        const int activeInputs =
            device->getActiveInputChannels().countNumberOfSetBits();

        if (availableInputs > activeInputs && availableInputs > 0)
        {
            auto setup = deviceManager.getAudioDeviceSetup();
            setup.inputChannels.clear();
            setup.inputChannels.setRange(
                0,
                juce::jmin(availableInputs, 32),
                true);
            setup.useDefaultInputChannels = false;

            // If the driver refuses the wider channel set, restore the
            // previously saved device setup instead of leaving the app
            // without an audio device.
            if (deviceManager.setAudioDeviceSetup(setup, true).isNotEmpty())
                deviceManager.initialise(32, 2, savedState, true);
        }
    }

    refreshGuitarInputCombo();
}

void MainComponent::prepareExternalAudioForSpeed(double speedPercentToUse)
{
    const double ratio = juce::jlimit(0.5, 1.2, speedPercentToUse / 100.0);

    // Render all loaded stems concurrently: the four time-stretches are
    // independent and this cuts the tempo-change wait roughly fourfold.
    std::vector<std::future<std::shared_ptr<juce::AudioBuffer<float>>>> jobs;
    jobs.reserve(externalAudioTracks.size());

    for (auto& track : externalAudioTracks)
    {
        if (!track.loaded.load(std::memory_order_acquire)
            || track.audio.getNumSamples() <= 0)
        {
            std::atomic_store(&track.playbackAudio,
                              std::shared_ptr<juce::AudioBuffer<float>>{});
            continue;
        }

        auto& source = track.audio;
        jobs.push_back(std::async(std::launch::async,
            [&source, ratio]()
            {
                return std::make_shared<juce::AudioBuffer<float>>(
                    makePitchPreservingTimeStretch(source, ratio));
            }));
    }

    // Swap the finished buffers in only after every stem is ready, so the
    // audio thread never sees a half-updated set of tracks.
    size_t jobIndex = 0;

    for (auto& track : externalAudioTracks)
    {
        if (!track.loaded.load(std::memory_order_acquire)
            || track.audio.getNumSamples() <= 0)
            continue;

        std::atomic_store(&track.playbackAudio, jobs[jobIndex].get());
        ++jobIndex;
    }

    DBG("Instrumental audio pitch-preserving time stretch prepared: speed=" + juce::String(speedPercentToUse, 0)
        + "% (WSOLA; pitch preserved approximately)");
}

void MainComponent::appendAudioDebugLog(const juce::String& line) const
{
    const auto directory = juce::File::getSpecialLocation(
        juce::File::userApplicationDataDirectory).getChildFile("GuitarBattle");

    directory.createDirectory();

    directory.getChildFile("audio-debug.log")
        .appendText(juce::Time::getCurrentTime().toString(true, true)
                    + "  " + line + "\n");
}

void MainComponent::loadGuitarSampleBank(const juce::File& assetsRoot)
{
    guitarSamplesLoaded.store(false, std::memory_order_relaxed);
    guitarSampleRegions.clear();
    guitarRegionForMidi.fill(-1);

    // The bank lives in the server-hosted sound pack.  No local Sounds folder
    // or absolute development path is consulted.
    const auto bankDirectory = assetsRoot.getChildFile("guitar");

    if (! bankDirectory.isDirectory())
    {
        DBG("GUITAR SAMPLES: server sound pack is not available; using the built-in fallback tone.");
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

void MainComponent::loadBackingSampleBanks(const juce::File& assetsRoot)
{
    DBG("BACKING DRUM WAV BUILD: 2026-10-13-v6");
    for (auto& loaded : backingSamplesLoaded)
        loaded.store(false, std::memory_order_relaxed);
    for (auto& regions : backingSampleRegions)
        regions.clear();
    for (auto& map : backingSampleMap)
        map.fill(-1);

    if (! assetsRoot.isDirectory())
    {
        DBG("BACKING: server sound pack is not available; using procedural fallbacks.");
        return;
    }

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

    // Load the dedicated one-shot drum kit from the server sound pack.
    // GM MIDI drum notes are mapped to these WAVs; no pitch-shifting is applied.
    {
        const auto drumsDirectory = assetsRoot.getChildFile("drums");
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

    DBG("BACKING SFZ BANKS: bass and piano only; drums use the server WAV kit");
    struct BankSpec { int instrument; juce::File sfz; };
    const BankSpec banks[] = {
        { 1, assetsRoot.getChildFile("bass").getChildFile("FingerBassYR 20190930.sfz") },
        { 3, assetsRoot.getChildFile("piano").getChildFile("UprightPianoKW-small-20190703.sfz") }
    };

    for (const auto& bank : banks)
    {
        const auto& sfz = bank.sfz;
        if (! sfz.existsAsFile())
        {
            DBG("BACKING SFZ: missing " + sfz.getFileName() + "; using procedural fallback for instrument " + juce::String(bank.instrument));
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

void MainComponent::loadMetronomeSamples(const juce::File& assetsRoot)
{
    // Called only while the audio device is stopped: no allocation or file I/O
    // happens in prepareToPlay()/getNextAudioBlock().
    metronomeAccentBuffer.setSize(0, 0);
    metronomeNormalBuffer.setSize(0, 0);
    metronomeSamplesRemaining = 0;
    metronomePlaybackPosition = 0;

    if (! assetsRoot.isDirectory())
        return;

    auto loadClick = [this](const juce::File& file, juce::AudioBuffer<float>& buffer)
    {
        if (! file.existsAsFile())
        {
            DBG("METRONOME: missing " + file.getFullPathName());
            return;
        }

        std::unique_ptr<juce::AudioFormatReader> reader(
            audioFormatManager.createReaderFor(file));

        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            DBG("METRONOME: cannot read " + file.getFileName());
            return;
        }

        const int channels = juce::jlimit(
            1, 2, static_cast<int>(reader->numChannels));
        const int samples = static_cast<int>(reader->lengthInSamples);
        buffer.setSize(channels, samples);
        reader->read(&buffer, 0, samples, 0, true, true);
    };

    const auto metronomeDirectory = assetsRoot.getChildFile("metronome");
    loadClick(metronomeDirectory.getChildFile("wooden-metronome-click.wav"),
              metronomeAccentBuffer);
    loadClick(metronomeDirectory.getChildFile("light-clicking-perc.wav"),
              metronomeNormalBuffer);
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

bool MainComponent::loadExternalAudioTrack(size_t index, const juce::File& file)
{
    if (index >= externalAudioTracks.size())
        return false;

    if (!file.existsAsFile())
    {
        appendAudioDebugLog("STEM[" + juce::String(index)
                            + "] MISSING " + file.getFullPathName());
        DBG("External audio track not found: " + file.getFullPathName());
        return false;
    }

    std::unique_ptr<juce::AudioFormatReader> reader(audioFormatManager.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels <= 0)
    {
        appendAudioDebugLog("STEM[" + juce::String(index)
                            + "] DECODE FAILED " + file.getFileName()
                            + " bytes=" + juce::String(file.getSize()));
        DBG("Could not decode external audio track: " + file.getFullPathName());
        return false;
    }

    auto& track = externalAudioTracks[index];
    const int channels = static_cast<int>(juce::jmin<uint32_t>(2, reader->numChannels));

    appendAudioDebugLog("STEM[" + juce::String(index) + "] "
                        + file.getFileName()
                        + " bytes=" + juce::String(file.getSize())
                        + " len=" + juce::String(reader->lengthInSamples)
                        + " rate=" + juce::String(reader->sampleRate)
                        + " ch=" + juce::String(reader->numChannels));

    // The server's MP3 stems are raw CBR streams without a gapless
    // (Xing/LAME) header. JUCE's MP3 decoder therefore leaves the standard
    // MP3 priming in front of the audio: 1105 samples of codec/encoder delay,
    // plus a full 1152-sample silent frame when the encoder wrote one.
    // Without trimming this, every stem starts ~51 ms after the MIDI and
    // metronome timeline. Detect the silent priming frame from the decoded
    // PCM instead of relying on the file name alone.
    int sourceStartOffset = 0;

    if (file.hasFileExtension("mp3"))
    {
        constexpr int probeFrameSamples = 1152;
        const int probeSamples = static_cast<int>(
            juce::jmin<int64_t>(
                reader->lengthInSamples,
                probeFrameSamples));

        juce::AudioBuffer<float> probe(channels, probeSamples);
        probe.clear();

        if (probeSamples > 0
            && reader->read(&probe, 0, probeSamples, 0, true, true))
        {
            float probeMagnitude = 0.0f;

            for (int channel = 0; channel < channels; ++channel)
            {
                probeMagnitude = juce::jmax(
                    probeMagnitude,
                    probe.getMagnitude(channel, 0, probeSamples));
            }

            const bool firstFrameIsSilent =
                probeMagnitude < 1.0e-4f;

            constexpr double codecDelaySeconds =
                1105.0 / 44100.0;
            constexpr double primingFrameSeconds =
                1152.0 / 44100.0;

            const double delaySeconds =
                codecDelaySeconds
                + (firstFrameIsSilent ? primingFrameSeconds : 0.0);

            sourceStartOffset = juce::jmax(
                0,
                static_cast<int>(
                    std::llround(
                        reader->sampleRate * delaySeconds)));
        }
    }

    // Load the whole stem (minus the MP3 priming): the song plays to the end
    // of the MIDI, not to a fixed one-minute window.
    const int samples = static_cast<int>(juce::jmin<int64_t>(
        reader->lengthInSamples,
        static_cast<int64_t>(std::numeric_limits<int>::max())));

    const int samplesToRead =
        juce::jmax(0, samples - sourceStartOffset);

    track.audio.setSize(channels, samplesToRead);
    track.audio.clear();

    if (samplesToRead <= 0)
    {
        track.audio.setSize(0, 0);
        appendAudioDebugLog("STEM[" + juce::String(index) + "] READ FAILED read="
                            + juce::String(samplesToRead)
                            + " offset=" + juce::String(sourceStartOffset));
        DBG("Failed reading external audio track: " + file.getFullPathName());
        return false;
    }

    // JUCE estimates MP3 length from the compressed file size. Without a
    // gapless header that estimate can overshoot the real number of decoded
    // samples, and a single read of the whole file then fails at the tail.
    // Read in blocks and keep everything the decoder actually produced.
    int writtenSamples = 0;
    constexpr int chunkSize = 4096;

    while (writtenSamples < samplesToRead)
    {
        const int toRead = juce::jmin(chunkSize, samplesToRead - writtenSamples);

        if (! reader->read(&track.audio, writtenSamples, toRead,
                           sourceStartOffset + writtenSamples, true, true))
            break;

        writtenSamples += toRead;
    }

    if (writtenSamples <= 0)
    {
        track.audio.setSize(0, 0);
        appendAudioDebugLog("STEM[" + juce::String(index) + "] READ FAILED read="
                            + juce::String(samplesToRead)
                            + " offset=" + juce::String(sourceStartOffset));
        DBG("Failed reading external audio track: " + file.getFullPathName());
        return false;
    }

    if (writtenSamples < samplesToRead)
    {
        track.audio.setSize(channels, writtenSamples, true, false, false);

        appendAudioDebugLog("STEM[" + juce::String(index) + "] PARTIAL samples="
                            + juce::String(writtenSamples) + "/"
                            + juce::String(samplesToRead));
    }

    track.sourceSampleRate = reader->sampleRate;
    track.loaded.store(true, std::memory_order_release);
    appendAudioDebugLog("STEM[" + juce::String(index) + "] OK samples="
                        + juce::String(writtenSamples));
    DBG("Loaded external audio track: " + file.getFullPathName()
        + " (" + juce::String(samplesToRead) + " samples, "
        + juce::String(reader->sampleRate) + " Hz, "
        + juce::String(static_cast<double>(samplesToRead) / reader->sampleRate, 2)
        + " seconds; full stem; trimmed "
        + juce::String(sourceStartOffset) + " MP3 priming samples)");
    return true;
}

void MainComponent::prepareToPlay(
    int samplesPerBlockExpected,
    double sampleRate)
{
    juce::ignoreUnused(
        samplesPerBlockExpected);

    // The audio callback receives one buffer channel per active input. Cache
    // that count and the active-index -> hardware-channel map so guitar capture
    // can select any active input without querying the device from the
    // realtime thread.
    cachedSampleRate.store(sampleRate, std::memory_order_relaxed);
    activeInputChannels.store(0, std::memory_order_relaxed);
    activeInputHardwareChannels.fill(-1);

    // -1 makes the first audio block choose the loudest active input instead
    // of always starting on hardware channel 1.
    currentInputChannel = -1;

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const auto names = device->getInputChannelNames();
        const auto active = device->getActiveInputChannels();
        const int maxActive = static_cast<int>(activeInputHardwareChannels.size());
        const int hardwareCount = juce::jmin(names.size(), maxActive);
        int activeIndex = 0;

        for (int hardware = 0;
            hardware < hardwareCount;
            ++hardware)
        {
            if (!active[hardware])
                continue;

            activeInputHardwareChannels[static_cast<size_t>(activeIndex)] = hardware;
            ++activeIndex;
        }

        activeInputChannels.store(activeIndex, std::memory_order_relaxed);
    }


    // Audio formats were registered in the constructor before the sample bank
    // was loaded.  Metronome clicks are decoded by loadMetronomeSamples() while
    // the device is stopped; prepareToPlay() must not touch the filesystem.

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

    int metronomeTriggerOffset = -1;

    if (gameState == GameState::Playing)
    {
        metronomeTriggerOffset = metronome.processScheduler(
            currentSample,
            bufferToFill.numSamples);
    }

    //==========================================================================
    // Playhead

    if (gameState == GameState::Playing)
    {
        const auto currentGameplayStart =
            gameplayStartSample.load(
                std::memory_order_relaxed);

        if (currentSample >= currentGameplayStart)
        {
            // Beat position follows the tempo anchor, so it stays continuous
            // when the player changes BPM mid-song.
            currentPlayheadBeat.store(
                getSongBeatForSample(currentSample),
                std::memory_order_relaxed);
        }
    }

    //==========================================================================
    // Input samples
    //
    // IMPORTANT:
    // Use startSample because AudioSourceChannelInfo can point to an
    // offset inside the AudioBuffer.
    //
    // JUCE fills channels 0..outputCount-1 with the output buffers (including
    // the first inputs) and appends input-only channels when the device has
    // more inputs than outputs. Never assume the guitar is on channel 0: the
    // user may have it on any input of a multi-channel interface.

    const int inputChannelCount = juce::jlimit(
        0,
        bufferToFill.buffer->getNumChannels(),
        activeInputChannels.load(std::memory_order_relaxed));

    const int outputChannelCount = juce::jmin(
        2,
        bufferToFill.buffer->getNumChannels());

    int inputChannel = 0;

    if (inputChannelCount > 0)
    {
        const int requestedHardware =
            preferredInputChannel.load(std::memory_order_relaxed);

        int requestedIndex = -1;

        if (requestedHardware >= 0)
        {
            const int mappedCount = juce::jmin(
                inputChannelCount,
                static_cast<int>(activeInputHardwareChannels.size()));

            for (int index = 0; index < mappedCount; ++index)
            {
                if (activeInputHardwareChannels[static_cast<size_t>(index)]
                    == requestedHardware)
                {
                    requestedIndex = index;
                    break;
                }
            }
        }

        if (requestedIndex >= 0)
        {
            inputChannel = requestedIndex;
            currentInputChannel = requestedIndex;
        }
        else
        {
            // Auto mode: follow the loudest active input. On the first block,
            // or while the current input is silent, switch to the strongest
            // input immediately. Once a channel is producing signal, require
            // a clear advantage before switching to avoid flapping between
            // noisy inputs during a performance.
            const int previousChannel = currentInputChannel;
            int bestChannel = -1;
            float bestRms = 0.0f;

            for (int channel = 0;
                channel < inputChannelCount;
                ++channel)
            {
                const float rms = bufferToFill.buffer->getRMSLevel(
                    channel,
                    bufferToFill.startSample,
                    bufferToFill.numSamples);

                if (rms > bestRms)
                {
                    bestRms = rms;
                    bestChannel = channel;
                }
            }

            if (bestChannel < 0)
                bestChannel = 0;

            const bool previousValid =
                previousChannel >= 0
                && previousChannel < inputChannelCount;

            if (! previousValid)
            {
                inputChannel = bestChannel;
            }
            else
            {
                const float previousRms =
                    bufferToFill.buffer->getRMSLevel(
                        previousChannel,
                        bufferToFill.startSample,
                        bufferToFill.numSamples);

                // Match the pitch detector's silence threshold: a channel
                // below this level cannot produce a usable note.
                constexpr float silenceThreshold = 0.0035f;

                const bool previousIsSilent =
                    previousRms < silenceThreshold;

                if (previousIsSilent
                    && bestChannel != previousChannel
                    && bestRms > silenceThreshold)
                {
                    inputChannel = bestChannel;
                }
                else if (bestRms > previousRms * 1.6f + 0.0015f)
                {
                    inputChannel = bestChannel;
                }
                else
                {
                    inputChannel = previousChannel;
                }
            }

            currentInputChannel = inputChannel;
        }
    }

    const auto* samples = inputChannelCount > 0
        ? bufferToFill.buffer->getReadPointer(
              inputChannel,
              bufferToFill.startSample)
        : nullptr;

    //==========================================================================
    // Input level metering
    //
    // Realtime-safe: plain arithmetic over the current block plus relaxed
    // atomic stores. The message thread reads these values at 30 Hz, applies
    // smoothing and paints the meter; nothing is allocated or locked here.

    if (samples != nullptr && bufferToFill.numSamples > 0)
    {
        double sum = 0.0;
        float peak = 0.0f;

        for (int i = 0; i < bufferToFill.numSamples; ++i)
        {
            const float value = samples[i];
            sum += static_cast<double>(value) * value;
            peak = juce::jmax(peak, std::abs(value));
        }

        const float rms = static_cast<float>(
            std::sqrt(sum / static_cast<double>(bufferToFill.numSamples)));

        inputMeterRms.store(rms, std::memory_order_relaxed);
        inputMeterPeak.store(peak, std::memory_order_relaxed);

        // A sample at or above full scale is the only clipping the client can
        // prove: an analogue overload of the interface is not observable here.
        if (peak >= 0.999f)
            inputClipEvents.fetch_add(1, std::memory_order_relaxed);

        const int mappedCount = juce::jmin(
            inputChannelCount,
            static_cast<int>(activeInputHardwareChannels.size()));

        inputMeterHardwareChannel.store(
            inputChannel < mappedCount
                ? activeInputHardwareChannels[static_cast<size_t>(inputChannel)]
                : -1,
            std::memory_order_relaxed);
    }
    else
    {
        inputMeterRms.store(0.0f, std::memory_order_relaxed);
        inputMeterPeak.store(0.0f, std::memory_order_relaxed);
        inputMeterHardwareChannel.store(-1, std::memory_order_relaxed);
    }

    //==========================================================================
    // Mono guitar -> stereo monitoring

    if (outputChannelCount > 0)
    {
        if (samples != nullptr)
        {
            auto* left = bufferToFill.buffer->getWritePointer(
                0,
                bufferToFill.startSample);

            for (int i = 0;
                i < bufferToFill.numSamples;
                ++i)
            {
                left[i] = samples[i];
            }

            if (outputChannelCount > 1)
            {
                auto* right = bufferToFill.buffer->getWritePointer(
                    1,
                    bufferToFill.startSample);

                for (int i = 0;
                    i < bufferToFill.numSamples;
                    ++i)
                {
                    right[i] = samples[i];
                }
            }
        }
        else
        {
            for (int channel = 0;
                channel < outputChannelCount;
                ++channel)
            {
                bufferToFill.buffer->clear(
                    channel,
                    bufferToFill.startSample,
                    bufferToFill.numSamples);
            }
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
            samples != nullptr ? samples[i] : 0.0f);

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

    int metronomeBlockOffset = 0;

    if (gameState == GameState::Playing
        && metronome.consumeTrigger()
        && metronomeTriggerOffset >= 0)
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

        // Start the click on the exact beat sample inside this block instead
        // of quantising it to the block start.
        metronomeBlockOffset = metronomeTriggerOffset;
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
        const int channels = outputChannelCount;
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
        const double outputRate = cachedSampleRate.load(std::memory_order_relaxed);
        if (outputRate > 0.0 && currentSample + static_cast<uint64_t>(bufferToFill.numSamples) > songStart)
        {
            const std::array<float, 4> gains = {
                midiDrumsEnabled.load(std::memory_order_relaxed) ? midiDrumsVolume.load(std::memory_order_relaxed) : 0.0f,
                midiBassEnabled.load(std::memory_order_relaxed) ? midiBassVolume.load(std::memory_order_relaxed) : 0.0f,
                midiRhythmGuitarEnabled.load(std::memory_order_relaxed) ? midiRhythmGuitarVolume.load(std::memory_order_relaxed) : 0.0f,
                midiPianoEnabled.load(std::memory_order_relaxed) ? midiPianoVolume.load(std::memory_order_relaxed) : 0.0f
            };
            const int outputChannels = outputChannelCount;
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
                    // The stems are time-stretched for the selected tempo and
                    // addressed by song beat, so a tempo change keeps the
                    // current musical position instead of restarting.
                    const double songBeat = getSongBeatForSample(absoluteSample);
                    if (songBeat < 0.0)
                        continue;
                    const double tempoBpm = bpm.load(std::memory_order_relaxed);
                    if (tempoBpm <= 0.0)
                        break;
                    const double sourcePos = songBeat
                        * track.sourceSampleRate * 60.0 / tempoBpm;
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
        const double sr = cachedSampleRate.load(std::memory_order_relaxed);
        const uint64_t songStart = gameplayStartSample.load(std::memory_order_relaxed);
        const float backingVolume = midiBackingVolume.load(std::memory_order_relaxed);
        const auto& notes = gameSession.getMidiNotes();
        if (sr > 0.0 && currentBpm > 0.0 && backingVolume > 0.0f
            && notes.size() > 0 && currentSample + static_cast<uint64_t>(bufferToFill.numSamples) > songStart)
        {
            const double samplesPerBeat = sr * 60.0 / currentBpm;
            const int channels = outputChannelCount;
            const int blockSamples = bufferToFill.numSamples;
            const double blockStart = static_cast<double>(currentSample);
            const double blockEnd = blockStart + static_cast<double>(blockSamples);

            // Iterate notes, not samples: only notes that overlap this block
            // are rendered. The previous sample-outer loop walked the whole
            // song for every sample, which stalled the audio thread (and the
            // UI) while sustained notes were sounding.
            for (const auto& note : notes)
            {
                const double noteStartSample =
                    static_cast<double>(getSongSampleForBeat(note.startBeat));
                const double noteLength =
                    juce::jmax(0.08, note.durationBeats * samplesPerBeat);
                const double noteEndSample = noteStartSample + noteLength;

                if (noteStartSample < static_cast<double>(songStart))
                    continue; // keep the count-in free of melody notes

                if (noteEndSample <= blockStart || noteStartSample >= blockEnd)
                    continue;

                const int first = juce::jmax(
                    0, static_cast<int>(std::ceil(noteStartSample - blockStart)));
                const int last = juce::jmin(
                    blockSamples,
                    static_cast<int>(std::ceil(noteEndSample - blockStart)));

                if (last <= first)
                    continue;

                for (int i = first; i < last; ++i)
                {
                    const double noteElapsedSamples =
                        (blockStart + static_cast<double>(i)) - noteStartSample;

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

                    const float sample =
                        juce::jlimit(-1.0f, 1.0f,
                                     noteSample * static_cast<float>(release) * backingVolume);

                    for (int channel = 0; channel < channels; ++channel)
                        bufferToFill.buffer->addSample(
                            channel, bufferToFill.startSample + i, sample);
                }
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
        const double sr = cachedSampleRate.load(std::memory_order_relaxed);
        const double currentBpm = bpm.load(std::memory_order_relaxed);
        const uint64_t songStart = gameplayStartSample.load(std::memory_order_relaxed);
        if (sr > 0.0 && currentBpm > 0.0 && currentSample + static_cast<uint64_t>(bufferToFill.numSamples) > songStart)
        {
            const double samplesPerBeat = sr * 60.0 / currentBpm;
            const int channels = outputChannelCount;
            // Beat at the start of the block: note timing follows the tempo
            // anchor, so a mid-song BPM change keeps the position continuous.
            const double blockBeat = getSongBeatForSample(currentSample);
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
                const double relativeStart = (note.startBeat - blockBeat) * samplesPerBeat;
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

        const int outputOffset =
            juce::jlimit(
                0,
                bufferToFill.numSamples,
                metronomeBlockOffset);

        const int samplesToPlay =
            std::min(
                metronomeSamplesRemaining,
                std::min(
                    availableSamples,
                    bufferToFill.numSamples - outputOffset));

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
                    channel < outputChannelCount;
                    ++channel)
                {
                    bufferToFill.buffer->addSample(
                        channel,
                        bufferToFill.startSample + outputOffset + i,
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

void MainComponent::initialiseAudio()
{
    //==========================================================================
    // Restore audio device settings and the server-hosted sound pack

    auto savedState =
        appProperties.getUserSettings()
        ->getXmlValue(
            "AudioDeviceState");

    // The sound pack is downloaded from the server, never read from a local
    // D:\GuitarBattle\... path.  A validated cache is decoded before the audio
    // device starts; otherwise the built-in fallbacks are used until the
    // background refresh finishes.
    const auto cachedManifest = ClientAssets::readCachedManifest();
    clientAssetsReady = ClientAssets::cacheMatches(cachedManifest);
    clientAssetsRoot = clientAssetsReady ? ClientAssets::currentRoot()
                                         : juce::File();

    loadGuitarSampleBank(clientAssetsRoot);
    loadBackingSampleBanks(clientAssetsRoot);
    loadMetronomeSamples(clientAssetsRoot);

    // Song stems are loaded when a catalogue entry is selected.

    deviceManager.addChangeListener(this);
    startAudioDevice(savedState.get());
    startClientAssetRefresh();

    //==========================================================================
    // Start pitch thread

    // Below normal priority: pitch analysis must yield to the message and
    // audio threads so dense analysis never stalls the interface.
    pitchThread->startThread(juce::Thread::Priority::low);
}
