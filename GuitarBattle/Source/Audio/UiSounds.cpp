#include "UiSounds.h"

#include "../Network/ApiClient.h"

#include <algorithm>
#include <cmath>

namespace
{
    struct RegistryEntry
    {
        const char* key;
        const char* name;
        UiSounds::Category category;
    };

    // The single source of UI sound keys. The same keys are seeded on the
    // server, so an event can be uploaded without touching the client.
    const RegistryEntry kRegistry[] = {
        { "nav.menu", "Menu item", UiSounds::Category::Navigation },
        { "nav.section", "Section opened", UiSounds::Category::Navigation },
        { "nav.back", "Back", UiSounds::Category::Navigation },
        { "tab.switch", "Tab switched", UiSounds::Category::Navigation },
        { "menu.open", "Dropdown opened", UiSounds::Category::Navigation },
        { "menu.close", "Dropdown closed", UiSounds::Category::Navigation },
        { "button", "Button", UiSounds::Category::Action },
        { "button.primary", "Primary action", UiSounds::Category::Action },
        { "card.select", "Card selected", UiSounds::Category::Action },
        { "toggle", "Toggle", UiSounds::Category::Action },
        { "checkbox", "Checkbox", UiSounds::Category::Action },
        { "character.select", "Character selected", UiSounds::Category::Action },
        { "song.select", "Song selected", UiSounds::Category::Action },
        { "save.success", "Settings saved", UiSounds::Category::System },
        { "action.success", "Action succeeded", UiSounds::Category::System },
        { "error", "Error", UiSounds::Category::System },
        { "warning", "Warning", UiSounds::Category::System },
        { "notification", "Notification", UiSounds::Category::System },
        { "achievement", "Achievement unlocked", UiSounds::Category::System },
        { "level.up", "Level up", UiSounds::Category::System },
        { "character.unlock", "Character unlocked", UiSounds::Category::System },
        { "tower.complete", "Tower completed", UiSounds::Category::System },
        { "duel.start", "Duel started", UiSounds::Category::System },
        { "duel.finish", "Duel finished", UiSounds::Category::System },
    };

    constexpr int kRegistrySize = static_cast<int> (std::size (kRegistry));
}

juce::StringArray UiSounds::allKeys()
{
    juce::StringArray keys;

    for (const auto& entry : kRegistry)
        keys.add (entry.key);

    return keys;
}

juce::String UiSounds::keyName (const juce::String& key)
{
    for (const auto& entry : kRegistry)
        if (key == entry.key)
            return entry.name;

    return key;
}

UiSounds::Category UiSounds::keyCategory (const juce::String& key)
{
    for (const auto& entry : kRegistry)
        if (key == entry.key)
            return entry.category;

    return Category::Action;
}

UiSounds::UiSounds()
    : alive (std::make_shared<std::atomic<bool>> (true))
{
    formats.registerBasicFormats();
}

UiSounds::~UiSounds()
{
    alive->store (false);
}

int UiSounds::indexForKey (const juce::String& key) const
{
    for (int i = 0; i < maxEntries; ++i)
        if (entries[static_cast<size_t> (i)] != nullptr
            && entries[static_cast<size_t> (i)]->key == key)
            return i;

    return -1;
}

UiSounds::Entry* UiSounds::ensureEntry (const juce::String& key)
{
    const int existing = indexForKey (key);

    if (existing >= 0)
        return entries[static_cast<size_t> (existing)].get();

    for (int i = 0; i < maxEntries; ++i)
    {
        if (entries[static_cast<size_t> (i)] != nullptr)
            continue;

        auto entry = std::make_unique<Entry>();
        entry->key = key;
        entry->category = keyCategory (key);
        entries[static_cast<size_t> (i)] = std::move (entry);
        return entries[static_cast<size_t> (i)].get();
    }

    return nullptr;
}

double UiSounds::nowMs() const
{
    return juce::Time::getMillisecondCounterHiRes();
}

void UiSounds::setPreferences (bool soundsEnabled, float volume,
                               bool navigation, bool actions, bool system)
{
    enabled.store (soundsEnabled, std::memory_order_relaxed);
    masterGain.store (juce::jlimit (0.0f, 1.0f, volume), std::memory_order_relaxed);
    categoryEnabled[0].store (navigation, std::memory_order_relaxed);
    categoryEnabled[1].store (actions, std::memory_order_relaxed);
    categoryEnabled[2].store (system, std::memory_order_relaxed);
}

int UiSounds::loadedEventCount() const
{
    int count = 0;

    for (const auto& entry : entries)
        if (entry != nullptr
            && entry->buffer.load (std::memory_order_relaxed) != nullptr)
            ++count;

    return count;
}

void UiSounds::play (const juce::String& key)
{
    if (! enabled.load (std::memory_order_relaxed))
        return;

    const int index = indexForKey (key);

    if (index < 0)
        return;

    auto& entry = *entries[static_cast<size_t> (index)];

    if (! entry.active.load (std::memory_order_relaxed)
        || entry.buffer.load (std::memory_order_relaxed) == nullptr)
        return;

    const auto category = static_cast<size_t> (entry.category);

    if (category >= categoryEnabled.size()
        || ! categoryEnabled[category].load (std::memory_order_relaxed))
        return;

    // One action must not stack the same event: 30 ms is shorter than any
    // intentional double click and long enough to swallow repeated UI noise.
    const auto now = nowMs();

    if (now - lastPlayMs[static_cast<size_t> (index)] < 30.0)
        return;

    lastPlayMs[static_cast<size_t> (index)] = now;

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite (1, start1, size1, start2, size2);

    if (size1 + size2 < 1)
        return;

    fifoData[static_cast<size_t> (size1 > 0 ? start1 : start2)] = index;
    fifo.finishedWrite (1);
}

void UiSounds::prepare (double newSampleRate)
{
    sampleRate.store (newSampleRate > 0.0 ? newSampleRate : 44100.0,
                      std::memory_order_relaxed);

    for (auto& voice : voices)
        voice = {};
}

void UiSounds::startVoice (int index)
{
    if (! juce::isPositiveAndBelow (index, maxEntries))
        return;

    auto* entry = entries[static_cast<size_t> (index)].get();

    if (entry == nullptr)
        return;

    const auto* buffer = entry->buffer.load (std::memory_order_relaxed);

    if (buffer == nullptr || buffer->getNumSamples() <= 0)
        return;

    Voice* slot = nullptr;

    for (auto& voice : voices)
    {
        if (! voice.active)
        {
            slot = &voice;
            break;
        }
    }

    if (slot == nullptr)
    {
        // Steal the voice that is furthest through its sample.
        slot = &voices[0];

        for (auto& voice : voices)
            if (voice.position > slot->position)
                slot = &voice;
    }

    const auto sr = sampleRate.load (std::memory_order_relaxed);
    const auto srcRate = entry->sourceRate.load (std::memory_order_relaxed);

    slot->buffer = buffer;
    slot->position = 0.0;
    slot->step = (srcRate > 0.0 && sr > 0.0) ? srcRate / sr : 1.0;
    slot->gain = juce::jlimit (0.0f, 1.0f, entry->gain);
    slot->active = true;
}

void UiSounds::render (juce::AudioBuffer<float>& output, int startSample,
                       int numSamples)
{
    if (output.getNumChannels() <= 0 || numSamples <= 0)
        return;

    juce::ScopedNoDenormals noDenormals;

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToRead (fifo.getNumReady(), start1, size1, start2, size2);

    for (int i = 0; i < size1; ++i)
        startVoice (fifoData[static_cast<size_t> (start1 + i)]);

    for (int i = 0; i < size2; ++i)
        startVoice (fifoData[static_cast<size_t> (start2 + i)]);

    fifo.finishedRead (size1 + size2);

    if (! enabled.load (std::memory_order_relaxed))
        return;

    const float master = masterGain.load (std::memory_order_relaxed);

    if (master <= 0.0f)
        return;

    const int channels = output.getNumChannels();

    for (auto& voice : voices)
    {
        if (! voice.active || voice.buffer == nullptr)
            continue;

        const auto* source = voice.buffer;
        const int length = source->getNumSamples();

        for (int i = 0; i < numSamples; ++i)
        {
            const int position = static_cast<int> (voice.position);

            if (position >= length)
            {
                voice.active = false;
                break;
            }

            const float sample = source->getSample (0, position)
                                 * voice.gain * master;

            for (int channel = 0; channel < channels; ++channel)
                output.addSample (channel, startSample + i, sample);

            voice.position += voice.step;
        }
    }
}

void UiSounds::refreshManifest (const juce::String& token)
{
    if (api == nullptr)
        return;

    juce::Component::SafePointer<juce::Component> unused; // no component here
    juce::ignoreUnused (unused);

    auto aliveFlag = alive;

    api->getJson ("/ui-sounds/manifest", token, 6000,
        [this, aliveFlag] (ApiClient::Response reply)
        {
            if (! aliveFlag->load())
                return;

            if (! reply.transportOk || ! reply.json.isObject())
                return;

            const auto* items =
                reply.json.getProperty ("items", juce::var()).getArray();

            if (items == nullptr)
                return;

            for (const auto& item : *items)
            {
                if (! item.isObject())
                    continue;

                const auto key = item.getProperty ("key", juce::var()).toString();

                if (key.isEmpty())
                    continue;

                auto* entry = ensureEntry (key);

                if (entry == nullptr)
                    continue;

                entry->url = item.getProperty ("url", juce::var()).toString();
                entry->gain = juce::jlimit (
                    0.0f, 1.0f, static_cast<float> (item.getProperty ("gain", 1.0)));
                entry->active.store (
                    static_cast<bool> (item.getProperty ("active", true)),
                    std::memory_order_relaxed);

                const int index = indexForKey (key);
                const int version = static_cast<int> (item.getProperty ("version", 0));
                entry->version.store (version, std::memory_order_relaxed);

                if (entry->url.isEmpty())
                    continue;

                if (entry->loadedVersion.load (std::memory_order_relaxed) == version
                    || entry->inflightVersion.load (std::memory_order_relaxed)
                           == version)
                    continue;

                requestFile (index, version);
            }
        });
}

void UiSounds::requestFile (int index, int version)
{
    if (api == nullptr || ! juce::isPositiveAndBelow (index, maxEntries))
        return;

    auto* entry = entries[static_cast<size_t> (index)].get();

    if (entry == nullptr || entry->url.isEmpty())
        return;

    entry->inflightVersion.store (version, std::memory_order_relaxed);

    const auto url = entry->url;
    const auto absolute = url.startsWith ("http")
                              ? url
                              : juce::String (ApiClient::serverOrigin) + url;
    auto data = std::make_shared<juce::MemoryBlock>();
    auto aliveFlag = alive;

    api->runAsync ([data, absolute]
                   {
                       *data = ApiClient::download (absolute, 15000);
                   },
                   [this, aliveFlag, index, version, data]
                   {
                       if (! aliveFlag->load())
                           return;

                       auto* target = entries[static_cast<size_t> (index)].get();

                       if (target == nullptr)
                           return;

                       target->inflightVersion.store (-1, std::memory_order_relaxed);

                       if (data->getSize() == 0)
                           return;

                       std::unique_ptr<juce::AudioFormatReader> reader (
                           formats.createReaderFor (
                               std::make_unique<juce::MemoryInputStream> (*data,
                                                                         true)));

                       if (reader == nullptr || reader->lengthInSamples <= 0)
                           return;

                       const int length = static_cast<int> (
                           juce::jmin<juce::int64> (reader->lengthInSamples,
                                                    48000 * 8));

                       auto buffer = std::make_unique<juce::AudioBuffer<float>> (
                           1, length);
                       reader->read (buffer.get(), 0, length, 0, true, false);

                       target->sourceRate.store (reader->sampleRate,
                                                 std::memory_order_relaxed);

                       // The audio thread reads the raw pointer: keep every
                       // decoded buffer for the session so a replacement can
                       // never free memory under a playing voice.
                       ownedBuffers.push_back (std::move (buffer));
                       target->buffer.store (ownedBuffers.back().get(),
                                             std::memory_order_release);
                       target->loadedVersion.store (version,
                                                    std::memory_order_relaxed);
                   });
}
