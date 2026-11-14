#include "VoiceChat.h"

#include "../Network/ApiClient.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    constexpr float voiceGain = 0.80f;

    // Direct-UDP signaling chunk carried inside the normal PCM relay. The
    // server treats it as opaque bytes, so no signaling route is required.
    constexpr juce::uint8 controlMagic[4] = { 'F', 'P', 'E', '1' };

    inline juce::int16 toPcm16 (float sample) noexcept
    {
        const auto scaled = juce::jlimit (-32768.0f, 32767.0f,
                                          sample * 32767.0f);
        return static_cast<juce::int16> (std::lround (scaled));
    }

    inline float fromPcm16 (juce::int16 sample) noexcept
    {
        return static_cast<float> (sample) / 32768.0f;
    }
}

//==============================================================================
VoiceChat::CapturePath::CapturePath()
    : storage (captureFifoSize),
      scratch (4096)
{
}

void VoiceChat::CapturePath::reset()
{
    // Only the producer touches these members. The FIFO itself is drained by
    // the network thread, never reset from the message thread.
    phase = 0.0;
    lowpass = 0.0f;
    haveSample = false;
}

//==============================================================================
VoiceChat::VoiceChat()
    : playbackStorage (playbackFifoSize),
      playbackScratch (8192),
      pendingSend(),
      relayPending()
{
    pendingSend.reserve (static_cast<size_t> (udpChunkSamples));
    relayPending.reserve (static_cast<size_t> (maxRelayChunkSamples * 2));
}

VoiceChat::~VoiceChat()
{
    stop();
}

void VoiceChat::start (const juce::String& lobbyIdToUse,
                       const juce::String& tokenToUse)
{
    if (started.load (std::memory_order_relaxed))
        stop();

    lobbyId = lobbyIdToUse;
    token = tokenToUse;
    lastSeq = 0;
    pendingSend.clear();
    relayPending.clear();

    // Direct UDP path. Port 0 asks the OS for an ephemeral port; the server
    // reports the public mapping to the other lobby member. If binding fails
    // the relay still works, so this is never a fatal error.
    //
    // DatagramSocket closes its handle permanently in shutdown(), so a new
    // object is required for every room; stop() resets the previous one.
    udpSocket = std::make_unique<juce::DatagramSocket>();
    boundUdpPort = 0;
    localUdpPort = 0;
    udpReady.store (false, std::memory_order_release);

    {
        const juce::ScopedLock lock (peerLock);
        peerHost.clear();
        peerPort = 0;
    }

    if (udpSocket->bindToPort (0))
    {
        boundUdpPort = udpSocket->getBoundPort();
        localUdpPort = boundUdpPort; // fallback until STUN answers
    }

    lastPeerPacketMs.store (0, std::memory_order_relaxed);
    lastPeerKnownMs.store (0, std::memory_order_relaxed);
    lastPunchMs = 0;
    lastEndpointStatus.store (0, std::memory_order_relaxed);
    publicIpV4.store (0, std::memory_order_relaxed);
    prebufferTarget.store (initialPrebufferSamples, std::memory_order_relaxed);
    relayChunkTarget.store (initialRelayChunkSamples, std::memory_order_relaxed);
    relayPostMsAverage = 0.0;
    udpPacketsSent.store (0, std::memory_order_relaxed);
    udpPacketsReceived.store (0, std::memory_order_relaxed);
    udpSequence.store (0, std::memory_order_relaxed);

    resetCaptureRequested.store (true, std::memory_order_release);
    clearPlaybackRequested.store (true, std::memory_order_release);

    enabled.store (false, std::memory_order_release);
    started.store (true, std::memory_order_release);

    sendWorker.startThread (juce::Thread::Priority::normal);
    pollWorker.startThread (juce::Thread::Priority::normal);
    udpWorker.startThread (juce::Thread::Priority::normal);
    signalWorker.startThread (juce::Thread::Priority::normal);
}

void VoiceChat::stop()
{
    enabled.store (false, std::memory_order_release);
    started.store (false, std::memory_order_release);

    sendWorker.signalThreadShouldExit();
    pollWorker.signalThreadShouldExit();
    udpWorker.signalThreadShouldExit();
    signalWorker.signalThreadShouldExit();
    sendWorker.notify();
    pollWorker.notify();
    udpWorker.notify();
    signalWorker.notify();

    // A request is bounded by requestTimeoutMs, so the HTTP workers exit well
    // inside this wait even when the network is unreachable.
    sendWorker.stopThread (5000);
    pollWorker.stopThread (5000);
    udpWorker.stopThread (5000);
    signalWorker.stopThread (5000);

    udpSocket.reset();
    udpReady.store (false, std::memory_order_release);
    boundUdpPort = 0;
    localUdpPort = 0;

    {
        const juce::ScopedLock lock (peerLock);
        peerHost.clear();
        peerPort = 0;
    }

    lastPeerPacketMs.store (0, std::memory_order_relaxed);
    lastPeerKnownMs.store (0, std::memory_order_relaxed);
    lastPunchMs = 0;
    publicIpV4.store (0, std::memory_order_relaxed);
    prebufferTarget.store (initialPrebufferSamples, std::memory_order_relaxed);
    relayChunkTarget.store (initialRelayChunkSamples, std::memory_order_relaxed);
    relayPostMsAverage = 0.0;
    pendingSend.clear();
    relayPending.clear();
    lastSeq = 0;
    clearPlaybackRequested.store (true, std::memory_order_release);
    resetCaptureRequested.store (true, std::memory_order_release);
}

void VoiceChat::setEnabled (bool shouldBeEnabled)
{
    if (shouldBeEnabled)
    {
        if (! started.load (std::memory_order_relaxed))
            return;

        resetCaptureRequested.store (true, std::memory_order_release);
        enabled.store (true, std::memory_order_release);
    }
    else
    {
        enabled.store (false, std::memory_order_release);
        resetCaptureRequested.store (true, std::memory_order_release);
    }
}

int VoiceChat::getLastHttpStatus() const noexcept
{
    // Prefer a failing request: a broken upload must not be hidden by a
    // successful poll, and vice versa.
    const int send = lastSendStatus.load (std::memory_order_relaxed);
    const int poll = lastPollStatus.load (std::memory_order_relaxed);

    if (send != 0 && send != 200)
        return send;

    if (poll != 0 && poll != 200)
        return poll;

    return juce::jmax (send, poll);
}

//==============================================================================
void VoiceChat::captureFromDeviceChannel (const float* samples,
                                          int numSamples,
                                          double sampleRate)
{
    const bool monitor = monitorEnabled.load (std::memory_order_relaxed);

    if ((! started.load (std::memory_order_relaxed) && ! monitor)
        || (! enabled.load (std::memory_order_relaxed) && ! monitor)
        || samples == nullptr
        || numSamples <= 0
        || ! isFiniteRate (sampleRate))
        return;

    if (! enabled.load (std::memory_order_relaxed))
    {
        // Settings meter: level only, no FIFO / network traffic.
        double sum = 0.0;

        for (int i = 0; i < numSamples; ++i)
            sum += static_cast<double> (samples[i]) * samples[i];

        inputLevel.store (static_cast<float> (std::sqrt (
                              sum / static_cast<double> (numSamples))),
                          std::memory_order_relaxed);
        return;
    }

    pushMono (mainCapture, samples, numSamples, sampleRate);
}

void VoiceChat::pushMono (CapturePath& path,
                          const float* mono,
                          int numSamples,
                          double sampleRate)
{
    if (mono == nullptr || numSamples <= 0 || ! isFiniteRate (sampleRate))
        return;

    if (resetCaptureRequested.exchange (false, std::memory_order_acq_rel))
        path.reset();

    const double ratio = sampleRate / static_cast<double> (targetSampleRate);
    const float alpha = static_cast<float> (
        1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 6500.0
                        / sampleRate));

    int produced = 0;
    float sumSquares = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float value = mono[i];
        sumSquares += value * value;

        path.lowpass += (value - path.lowpass) * alpha;

        if (! path.haveSample)
        {
            path.haveSample = true;
            path.phase = 0.0;
        }

        if (path.phase <= 0.0)
        {
            if (produced < static_cast<int> (path.scratch.size()))
                path.scratch[static_cast<size_t> (produced++)] =
                    toPcm16 (path.lowpass);

            path.phase += ratio;
        }

        path.phase -= 1.0;
    }

    if (produced > 0)
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        path.fifo.prepareToWrite (produced, start1, size1, start2, size2);

        if (size1 > 0)
            std::memcpy (path.storage.data() + start1,
                         path.scratch.data(),
                         sizeof (juce::int16) * static_cast<size_t> (size1));

        if (size2 > 0)
            std::memcpy (path.storage.data() + start2,
                         path.scratch.data() + size1,
                         sizeof (juce::int16) * static_cast<size_t> (size2));

        path.fifo.finishedWrite (size1 + size2);

        const float rms = std::sqrt (sumSquares
                                     / static_cast<float> (numSamples));
        inputLevel.store (rms, std::memory_order_relaxed);
    }
}

//==============================================================================
void VoiceChat::mixIntoOutput (juce::AudioBuffer<float>& output,
                               int startSample,
                               int numSamples,
                               int numOutputChannels,
                               double sampleRate)
{
    // Playback follows the transport, not the microphone: the player hears
    // the opponent as soon as the room opens, even while their own mic is off.
    if (! started.load (std::memory_order_relaxed)
        || numSamples <= 0
        || numOutputChannels <= 0
        || ! isFiniteRate (sampleRate))
        return;

    if (clearPlaybackRequested.exchange (false, std::memory_order_acq_rel))
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        playbackFifo.prepareToRead (playbackFifo.getNumReady(),
                                    start1, size1, start2, size2);
        playbackFifo.finishedRead (size1 + size2);

        playbackPrimed.store (false, std::memory_order_relaxed);
        playbackPhase = 0.0;
        playbackPrevious = 0.0f;
        playbackCurrent = 0.0f;
        playbackPos = 0;
        playbackAvailable = 0;
        starvedSamples = 0;
    }

    if (! playbackPrimed.load (std::memory_order_relaxed))
    {
        if (playbackFifo.getNumReady()
            < prebufferTarget.load (std::memory_order_relaxed))
            return;

        playbackPrimed.store (true, std::memory_order_relaxed);
        playbackPhase = 0.0;
        playbackPrevious = 0.0f;
        playbackCurrent = 0.0f;
        playbackPos = 0;
        playbackAvailable = 0;
        starvedSamples = 0;
    }

    const double ratio = static_cast<double> (targetSampleRate) / sampleRate;
    float peak = 0.0f;

    int done = 0;

    while (done < numSamples)
    {
        const int block = juce::jmin (maxBlock, numSamples - done);
        const int needed = static_cast<int> (
            std::ceil (static_cast<double> (block) * ratio)) + 3;

        // Move the unconsumed tail to the front, then top up from the FIFO.
        if (playbackAvailable > 0 && playbackPos > 0)
        {
            const int remaining = playbackAvailable - playbackPos;

            if (remaining > 0)
                std::memmove (playbackScratch.data(),
                              playbackScratch.data() + playbackPos,
                              sizeof (juce::int16)
                                  * static_cast<size_t> (remaining));

            playbackAvailable = remaining;
            playbackPos = 0;
        }

        const int wanted = needed - playbackAvailable;

        if (wanted > 0)
        {
            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            playbackFifo.prepareToRead (wanted, start1, size1, start2, size2);
            const int got = size1 + size2;

            if (got <= 0)
            {
                // A single late chunk must not discard the buffered audio:
                // stay primed and output silence until the next chunk arrives.
                // Only a long outage re-arms the prebuffer.
                starvedSamples += numSamples;

                if (starvedSamples > static_cast<int> (sampleRate * 1.5))
                {
                    playbackPrimed.store (false, std::memory_order_relaxed);
                    playbackAvailable = 0;
                    starvedSamples = 0;
                }

                return;
            }

            starvedSamples = 0;

            if (size1 > 0)
                std::memcpy (playbackScratch.data() + playbackAvailable,
                             playbackStorage.data() + start1,
                             sizeof (juce::int16) * static_cast<size_t> (size1));

            if (size2 > 0)
                std::memcpy (playbackScratch.data() + playbackAvailable + size1,
                             playbackStorage.data() + start2,
                             sizeof (juce::int16) * static_cast<size_t> (size2));

            playbackFifo.finishedRead (got);
            playbackAvailable += got;
        }

        if (playbackAvailable < needed)
        {
            starvedSamples += numSamples;

            if (starvedSamples > static_cast<int> (sampleRate * 1.5))
            {
                playbackPrimed.store (false, std::memory_order_relaxed);
                playbackAvailable = 0;
                starvedSamples = 0;
            }

            return;
        }

        for (int i = 0; i < block; ++i)
        {
            while (playbackPhase >= 1.0)
            {
                if (playbackPos >= playbackAvailable)
                {
                    playbackPrimed.store (false, std::memory_order_relaxed);
                    playbackAvailable = 0;
                    starvedSamples = 0;
                    return;
                }

                playbackPrevious = playbackCurrent;
                playbackCurrent = fromPcm16 (
                    playbackScratch[static_cast<size_t> (playbackPos++)]);
                playbackPhase -= 1.0;
            }

            const float sample = voiceGain
                * (playbackPrevious
                   + (playbackCurrent - playbackPrevious)
                         * static_cast<float> (playbackPhase));

            for (int channel = 0; channel < numOutputChannels; ++channel)
                output.addSample (channel, startSample + done + i, sample);

            peak = juce::jmax (peak, std::abs (sample));
            playbackPhase += ratio;
        }

        done += block;
    }

    outputLevel.store (peak, std::memory_order_relaxed);
}

void VoiceChat::prepare (double outputSampleRate)
{
    juce::ignoreUnused (outputSampleRate);

    playbackPrimed.store (false, std::memory_order_relaxed);
    playbackPhase = 0.0;
    playbackPrevious = 0.0f;
    playbackCurrent = 0.0f;
    playbackPos = 0;
    playbackAvailable = 0;
    starvedSamples = 0;
}

//==============================================================================
void VoiceChat::runSend (juce::Thread& worker)
{
    while (! worker.threadShouldExit())
    {
        if (! enabled.load (std::memory_order_relaxed))
        {
            pendingSend.clear();
            relayPending.clear();

            // Drop everything captured before the mic was switched off; it
            // must not be uploaded as a burst on the next enable.
            const int ready = mainCapture.fifo.getNumReady();

            if (ready > 0)
            {
                int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
                mainCapture.fifo.prepareToRead (ready, start1, size1,
                                                start2, size2);
                mainCapture.fifo.finishedRead (size1 + size2);
            }

            // Keep the NAT mapping to the peer open even while the mic is
            // muted, so unmuting does not need another hole-punch delay.
            const auto now = juce::Time::getMillisecondCounter();

            if (lastPunchMs == 0
                || static_cast<std::uint32_t> (now - lastPunchMs) >= 500)
            {
                sendUdpPunch();
                lastPunchMs = now;
            }

            juce::Thread::sleep (50);
            continue;
        }

        if (! assembleChunk())
        {
            juce::Thread::sleep (5);
            continue;
        }

        // Fast path: one 20 ms packet directly to the peer. The relay copy is
        // sent as well so the receiver can fall back without a renegotiation.
        sendUdpPacket (pendingSend.data(),
                       static_cast<int> (pendingSend.size()));

        relayPending.insert (relayPending.end(),
                             pendingSend.begin(),
                             pendingSend.end());

        if (static_cast<int> (relayPending.size())
            >= relayChunkTarget.load (std::memory_order_relaxed))
            sendRelayChunk();

        pendingSend.clear();
    }
}

void VoiceChat::runPoll (juce::Thread& worker)
{
    while (! worker.threadShouldExit())
    {
        if (! started.load (std::memory_order_relaxed))
        {
            juce::Thread::sleep (50);
            continue;
        }

        pollChunks();
        juce::Thread::sleep (10);
    }
}

void VoiceChat::runSignal (juce::Thread& worker)
{
    std::uint32_t lastSignal = 0;

    while (! worker.threadShouldExit())
    {
        if (! started.load (std::memory_order_relaxed))
        {
            juce::Thread::sleep (50);
            continue;
        }

        // The UDP socket is bound before the workers start, but the port the
        // peer must use is the NAT mapping, which only STUN reveals. The UDP
        // worker waits for this flag so the response is not stolen.
        if (! udpReady.load (std::memory_order_acquire))
        {
            discoverUdpEndpoint();

            // UDP STUN blocked? Fall back to an HTTPS IP lookup and the
            // local bound port; a port-preserving NAT is still enough for
            // direct voice.
            if (publicIpV4.load (std::memory_order_relaxed) == 0)
                resolvePublicIpFallback();

            continue;
        }

        const auto now = juce::Time::getMillisecondCounter();

        if (lastSignal == 0
            || static_cast<std::uint32_t> (now - lastSignal) >= 2000)
        {
            // Publish the direct-UDP endpoint through the existing relay
            // chunk route, so the low-latency path works even when the
            // dedicated /endpoint route is not deployed yet.
            sendRelayControl();

            // And use the dedicated route when the server has it: the peer
            // is then known one round trip sooner.
            refreshPeerEndpoint();
            lastSignal = now;
        }

        juce::Thread::sleep (20);
    }
}

void VoiceChat::runUdp (juce::Thread& worker)
{
    juce::uint8 buffer[2048];
    bool wasEnabled = false;

    while (! worker.threadShouldExit())
    {
        if (! started.load (std::memory_order_relaxed)
            || ! udpReady.load (std::memory_order_acquire)
            || udpSocket == nullptr)
        {
            wasEnabled = false;
            juce::Thread::sleep (10);
            continue;
        }

        // Drop packets that arrived while voice was off: they are seconds
        // old and would otherwise be played as a burst on re-enable.
        if (! wasEnabled)
        {
            while (udpSocket->read (buffer, static_cast<int> (sizeof (buffer)),
                                    false) > 0)
            {
            }

            wasEnabled = true;
        }

        for (;;)
        {
            const int bytes = udpSocket->read (buffer,
                                               static_cast<int> (sizeof (buffer)),
                                               false);

            if (bytes <= 0)
                break;

            handleUdpPacket (buffer, bytes);
        }

        juce::Thread::sleep (5);
    }
}

//==============================================================================
bool VoiceChat::assembleChunk()
{
    int have = static_cast<int> (pendingSend.size());

    if (have >= udpChunkSamples)
        return true;

    for (;;)
    {
        const int remaining = udpChunkSamples - have;

        if (remaining <= 0)
            break;

        const int ready = mainCapture.fifo.getNumReady();

        if (ready <= 0)
            break;

        const int want = juce::jmin (remaining, ready);
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        mainCapture.fifo.prepareToRead (want, start1, size1, start2, size2);

        if (size1 > 0)
            pendingSend.insert (pendingSend.end(),
                                mainCapture.storage.begin() + start1,
                                mainCapture.storage.begin() + start1 + size1);

        if (size2 > 0)
            pendingSend.insert (pendingSend.end(),
                                mainCapture.storage.begin() + start2,
                                mainCapture.storage.begin() + start2 + size2);

        mainCapture.fifo.finishedRead (size1 + size2);
        have += size1 + size2;

        if (size1 + size2 <= 0)
            break;
    }

    return have >= udpChunkSamples;
}

void VoiceChat::sendRelayChunk()
{
    const int targetSamples =
        relayChunkTarget.load (std::memory_order_relaxed);

    if (static_cast<int> (relayPending.size()) < targetSamples)
        return;

    if (lobbyId.isEmpty() || token.isEmpty())
    {
        relayPending.clear();
        relayPostMsAverage = 0.0;
        return;
    }

    const auto encoded = juce::Base64::toBase64 (
        relayPending.data(),
        sizeof (juce::int16) * static_cast<size_t> (targetSamples));

    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty ("data", encoded);

    const auto requestStart = juce::Time::getMillisecondCounterHiRes();

    const auto response = ApiClient::post (
        "/voice/lobbies/" + lobbyId + "/chunks",
        juce::var (body.get()), token, requestTimeoutMs);

    const double elapsedMs =
        juce::Time::getMillisecondCounterHiRes() - requestStart;

    relayPostMsAverage = relayPostMsAverage <= 0.0
        ? elapsedMs
        : relayPostMsAverage * 0.8 + elapsedMs * 0.2;

    lastSendStatus.store (response.transportOk ? response.statusCode : 0,
                          std::memory_order_relaxed);

    if (response.transportOk && response.statusCode == 200)
        sentChunks.fetch_add (1, std::memory_order_relaxed);

    // Keep the chunk just longer than one request. A link where POST takes
    // longer than the chunk cannot keep up and grows the chunk (up to
    // 200 ms); a fast link drops to 100 ms to cut the relay latency.
    const double chunkMs = targetSamples * 1000.0 / targetSampleRate;

    int nextTarget = targetSamples;

    if (relayPostMsAverage > chunkMs * 0.90)
        nextTarget = juce::jmin (maxRelayChunkSamples,
                                 targetSamples + udpChunkSamples);
    else if (relayPostMsAverage < chunkMs * 0.55)
        nextTarget = juce::jmax (minRelayChunkSamples,
                                 targetSamples - udpChunkSamples);

    relayChunkTarget.store (nextTarget, std::memory_order_relaxed);

    // Best effort: a lost chunk is never retried, the next one follows in
    // one chunk duration and stale audio is worse than a tiny gap. Keep any
    // samples that arrived while the request was running.
    relayPending.erase (relayPending.begin(),
                        relayPending.begin() + targetSamples);
}

void VoiceChat::sendRelayControl()
{
    const juce::uint32 ip = publicIpV4.load (std::memory_order_relaxed);
    const int port = localUdpPort.load (std::memory_order_relaxed);

    if (ip == 0 || port <= 0 || lobbyId.isEmpty() || token.isEmpty())
        return;

    juce::uint8 payload[12] = {};
    std::memcpy (payload, controlMagic, sizeof (controlMagic));
    payload[4] = static_cast<juce::uint8> ((ip >> 24) & 0xFF);
    payload[5] = static_cast<juce::uint8> ((ip >> 16) & 0xFF);
    payload[6] = static_cast<juce::uint8> ((ip >> 8) & 0xFF);
    payload[7] = static_cast<juce::uint8> (ip & 0xFF);
    payload[8] = static_cast<juce::uint8> ((port >> 8) & 0xFF);
    payload[9] = static_cast<juce::uint8> (port & 0xFF);

    const auto encoded = juce::Base64::toBase64 (
        payload, sizeof (payload));

    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty ("data", encoded);

    ApiClient::post ("/voice/lobbies/" + lobbyId + "/chunks",
                     juce::var (body.get()), token, requestTimeoutMs);
}

//==============================================================================
bool VoiceChat::sendUdpPacket (const juce::int16* samples, int numSamples)
{
    if (samples == nullptr || numSamples <= 0
        || udpSocket == nullptr
        || localUdpPort.load (std::memory_order_relaxed) <= 0)
        return false;

    juce::String host;
    int port = 0;

    {
        const juce::ScopedLock lock (peerLock);
        host = peerHost;
        port = peerPort;
    }

    if (host.isEmpty() || port <= 0)
        return false;

    struct Packet
    {
        juce::uint32 magic;
        juce::uint32 sequence;
        juce::uint16 sampleCount;
        juce::uint16 flags;
        juce::int16 samples[udpChunkSamples];
    };

    Packet packet {};
    packet.magic = 0x31465646u; // "FFV1" little-endian
    packet.sequence = udpSequence.fetch_add (1, std::memory_order_relaxed);
    packet.sampleCount = static_cast<juce::uint16> (
        juce::jmin (numSamples, udpChunkSamples));
    packet.flags = 0;

    std::memcpy (packet.samples, samples,
                 sizeof (juce::int16) * static_cast<size_t> (packet.sampleCount));

    const int written = udpSocket->write (host, port, &packet,
                                          static_cast<int> (sizeof (packet)));

    if (written <= 0)
        return false;

    udpPacketsSent.fetch_add (1, std::memory_order_relaxed);
    return true;
}

void VoiceChat::sendUdpPunch()
{
    if (udpSocket == nullptr
        || localUdpPort.load (std::memory_order_relaxed) <= 0)
        return;

    juce::String host;
    int port = 0;

    {
        const juce::ScopedLock lock (peerLock);
        host = peerHost;
        port = peerPort;
    }

    if (host.isEmpty() || port <= 0)
        return;

    struct Header
    {
        juce::uint32 magic;
        juce::uint32 sequence;
        juce::uint16 sampleCount;
        juce::uint16 flags;
    };

    Header header {};
    header.magic = 0x31465646u;
    header.sequence = udpSequence.fetch_add (1, std::memory_order_relaxed);
    header.sampleCount = 0;
    header.flags = 0;

    udpSocket->write (host, port, &header,
                      static_cast<int> (sizeof (header)));
}

void VoiceChat::handleUdpPacket (const void* data, int bytes)
{
    if (data == nullptr || bytes < 12)
        return;

    const auto* raw = static_cast<const juce::uint8*> (data);

    juce::uint32 magic = 0;
    juce::uint16 sampleCount = 0;
    std::memcpy (&magic, raw, sizeof (magic));
    std::memcpy (&sampleCount, raw + 8, sizeof (sampleCount));

    if (magic != 0x31465646u
        || sampleCount == 0
        || sampleCount > udpChunkSamples
        || bytes < 12 + static_cast<int> (sampleCount) * 2)
        return;

    const auto now = juce::Time::getMillisecondCounter();
    const auto previous = lastPeerPacketMs.exchange (now,
                                                     std::memory_order_relaxed);

    // First packet after a gap: drop relay audio that is already buffered so
    // the same words are never played twice.
    if (previous == 0
        || static_cast<std::uint32_t> (now - previous) > 2000)
        clearPlaybackRequested.store (true, std::memory_order_release);

    // Direct packets arrive every 10 ms: 40 ms of buffer is enough and keeps
    // the mouth-to-ear latency near 60-80 ms instead of the relay's 300+ ms.
    prebufferTarget.store (640, std::memory_order_relaxed);

    const juce::ScopedLock writeLock (playbackWriteLock);

    if (playbackFifo.getFreeSpace() < static_cast<int> (sampleCount))
        return;

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    playbackFifo.prepareToWrite (static_cast<int> (sampleCount),
                                 start1, size1, start2, size2);

    const auto* pcm = reinterpret_cast<const juce::int16*> (raw + 12);

    if (size1 > 0)
        std::memcpy (playbackStorage.data() + start1,
                     pcm,
                     sizeof (juce::int16) * static_cast<size_t> (size1));

    if (size2 > 0)
        std::memcpy (playbackStorage.data() + start2,
                     pcm + size1,
                     sizeof (juce::int16) * static_cast<size_t> (size2));

    playbackFifo.finishedWrite (size1 + size2);
    udpPacketsReceived.fetch_add (1, std::memory_order_relaxed);
}

bool VoiceChat::isPeerActive() const noexcept
{
    const auto last = lastPeerPacketMs.load (std::memory_order_relaxed);

    if (last == 0)
        return false;

    return static_cast<std::uint32_t> (
        juce::Time::getMillisecondCounter() - last) < 1200;
}

void VoiceChat::discoverUdpEndpoint()
{
    if (boundUdpPort <= 0 || udpSocket == nullptr)
    {
        udpReady.store (true, std::memory_order_release);
        return;
    }

    struct StunServer { const char* host; int port; };

    // IPv4 literals on purpose: juce::DatagramSocket is an AF_INET socket and
    // its hostname resolution may cache an IPv6 addrinfo first, which sendto()
    // then rejects. These are the long-lived anycast addresses of Google's
    // and Cloudflare's public STUN services.
    static const StunServer servers[] =
    {
        { "162.159.207.0", 3478 },     // Cloudflare
        { "46.225.95.169", 3478 },     // Nextcloud
        { "3.33.249.248", 3478 },      // Sipgate
        { "20.93.239.174", 3478 },     // BlackBerry
        { "74.125.250.129", 19302 }    // Google
    };

    juce::uint8 request[20] = {};
    request[0] = 0x00;
    request[1] = 0x01; // Binding Request
    request[4] = 0x21;
    request[5] = 0x12;
    request[6] = 0xA4;
    request[7] = 0x42; // magic cookie

    const auto seed = juce::Time::getHighResolutionTicks();
    std::memcpy (request + 8, &seed, sizeof (seed));
    std::memset (request + 16, 0x5A, 4);

    juce::uint8 buffer[512];

    for (const auto& server : servers)
    {
        if (udpSocket->write (server.host, server.port,
                              request, static_cast<int> (sizeof (request))) <= 0)
            continue;

        const auto deadline = juce::Time::getMillisecondCounter() + 200;

        while (juce::Time::getMillisecondCounter() < deadline)
        {
            const int bytes = udpSocket->read (buffer,
                                               static_cast<int> (sizeof (buffer)),
                                               false);

            if (bytes >= 20
                && buffer[0] == 0x01
                && buffer[1] == 0x01)
            {
                const int attributeBytes = juce::jmin (
                    bytes - 20, (static_cast<int> (buffer[2]) << 8) | buffer[3]);

                int offset = 20;

                while (offset + 4 <= 20 + attributeBytes)
                {
                    const int attributeType =
                        (static_cast<int> (buffer[offset]) << 8) | buffer[offset + 1];
                    const int attributeLength =
                        (static_cast<int> (buffer[offset + 2]) << 8) | buffer[offset + 3];
                    const int valueOffset = offset + 4;

                    if (valueOffset + attributeLength > bytes)
                        break;

                    // XOR-MAPPED-ADDRESS, IPv4 family.
                    if (attributeType == 0x0020
                        && attributeLength >= 8
                        && buffer[valueOffset + 1] == 0x01)
                    {
                        const int xorPort =
                            (static_cast<int> (buffer[valueOffset + 2]) << 8)
                            | buffer[valueOffset + 3];
                        const int port = xorPort ^ 0x2112;

                        const auto* ipBytes = buffer + valueOffset + 4;
                        constexpr juce::uint32 cookie = 0x2112A442u;
                        const juce::uint32 ip =
                            (static_cast<juce::uint32> (
                                 ipBytes[0] ^ ((cookie >> 24) & 0xFF)) << 24)
                            | (static_cast<juce::uint32> (
                                   ipBytes[1] ^ ((cookie >> 16) & 0xFF)) << 16)
                            | (static_cast<juce::uint32> (
                                   ipBytes[2] ^ ((cookie >> 8) & 0xFF)) << 8)
                            | static_cast<juce::uint32> (
                                  ipBytes[3] ^ (cookie & 0xFF));

                        if (port > 0 && port <= 65535 && ip != 0)
                        {
                            publicIpV4.store (ip, std::memory_order_relaxed);
                            localUdpPort = port;
                            udpReady.store (true, std::memory_order_release);
                            return;
                        }
                    }

                    offset = valueOffset + ((attributeLength + 3) & ~3);
                }
            }

            juce::Thread::sleep (5);
        }
    }

    // No STUN answer: keep the local port. A LAN peer or a port-preserving
    // NAT still connects; otherwise the HTTPS relay remains in use.
    localUdpPort = boundUdpPort;
    udpReady.store (true, std::memory_order_release);
}

void VoiceChat::resolvePublicIpFallback()
{
    // Some networks block UDP/3478 to public STUN servers. The port is then
    // still the local bound one; if the router preserves it (common), an
    // HTTPS public-IP lookup is enough for the peers to meet.
    static const char* services[] =
    {
        "https://api.ipify.org",
        "https://icanhazip.com"
    };

    for (const auto* service : services)
    {
        juce::URL url (service);
        const auto options =
            juce::URL::InputStreamOptions (
                juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs (2000);

        auto stream = url.createInputStream (options);

        if (stream == nullptr)
            continue;

        const auto text = stream->readEntireStreamAsString().trim();

        juce::StringArray parts;
        parts.addTokens (text, ".", "");

        if (parts.size() != 4)
            continue;

        juce::uint32 ip = 0;
        bool valid = true;

        for (const auto& part : parts)
        {
            if (! part.containsOnly ("0123456789"))
            {
                valid = false;
                break;
            }

            const int value = part.getIntValue();

            if (value < 0 || value > 255)
            {
                valid = false;
                break;
            }

            ip = (ip << 8) | static_cast<juce::uint32> (value);
        }

        if (valid && ip != 0)
        {
            publicIpV4.store (ip, std::memory_order_relaxed);
            return;
        }
    }
}

bool VoiceChat::refreshPeerEndpoint()
{
    const int publishedPort = localUdpPort.load (std::memory_order_relaxed);

    if (lobbyId.isEmpty() || token.isEmpty() || publishedPort <= 0)
        return false;

    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty ("port", publishedPort);

    const auto response = ApiClient::post (
        "/voice/lobbies/" + lobbyId + "/endpoint",
        juce::var (body.get()), token, requestTimeoutMs);

    lastEndpointStatus.store (response.transportOk ? response.statusCode : 0,
                              std::memory_order_relaxed);

    if (! response.transportOk
        || response.statusCode != 200
        || ! response.json.isObject())
        return false;

    const auto peer = response.json.getProperty ("peer", juce::var());

    if (! peer.isObject())
        return false;

    const auto host = peer.getProperty ("ip", juce::var()).toString().trim();
    const int port = static_cast<int> (peer.getProperty ("port", juce::var()));

    if (host.isEmpty() || port <= 0 || port > 65535)
        return false;

    {
        const juce::ScopedLock lock (peerLock);
        peerHost = host;
        peerPort = port;
    }

    lastPeerKnownMs.store (juce::Time::getMillisecondCounter(),
                           std::memory_order_relaxed);
    return true;
}

void VoiceChat::pollChunks()
{
    if (lobbyId.isEmpty() || token.isEmpty())
        return;

    const auto response = ApiClient::get (
        "/voice/lobbies/" + lobbyId + "/chunks?since="
            + juce::String (lastSeq),
        token, requestTimeoutMs);

    lastPollStatus.store (response.transportOk ? response.statusCode : 0,
                          std::memory_order_relaxed);

    if (! response.transportOk
        || response.statusCode != 200
        || ! response.json.isObject())
        return;

    // Control chunks are always parsed, even while direct UDP is active: they
    // keep the peer endpoint fresh. Relay audio is discarded while UDP flows,
    // but the cursor still advances so a fallback resumes at the live edge.
    const bool useRelay = ! isPeerActive();

    if (const auto* chunks = response.json.getProperty ("chunks", juce::var())
                                  .getArray())
    {
        for (const auto& chunk : *chunks)
        {
            const auto data = chunk.getProperty ("data", juce::var()).toString();

            if (data.isEmpty())
                continue;

            juce::MemoryOutputStream decoded;
            decoded.preallocate (8192);

            if (! juce::Base64::convertFromBase64 (decoded, data))
                continue;

            const auto bytes = decoded.getDataSize();

            if (bytes < 2 || bytes > 16384)
                continue;

            const auto* raw = static_cast<const juce::uint8*> (decoded.getData());

            if (bytes == 12
                && std::memcmp (raw, controlMagic, sizeof (controlMagic)) == 0)
            {
                const juce::uint32 ip =
                    (static_cast<juce::uint32> (raw[4]) << 24)
                    | (static_cast<juce::uint32> (raw[5]) << 16)
                    | (static_cast<juce::uint32> (raw[6]) << 8)
                    | static_cast<juce::uint32> (raw[7]);
                const int port =
                    (static_cast<int> (raw[8]) << 8) | raw[9];

                if (ip != 0 && port > 0 && port <= 65535)
                {
                    const auto host =
                        juce::String ((ip >> 24) & 0xFF) + "."
                        + juce::String ((ip >> 16) & 0xFF) + "."
                        + juce::String ((ip >> 8) & 0xFF) + "."
                        + juce::String (ip & 0xFF);

                    const juce::ScopedLock lock (peerLock);
                    peerHost = host;
                    peerPort = port;
                    lastPeerKnownMs.store (
                        juce::Time::getMillisecondCounter(),
                        std::memory_order_relaxed);
                }

                continue;
            }

            const int samples = static_cast<int> (bytes / 2);

            // Relay audio arrives in one chunk; half a chunk is enough to
            // start and keeps the fallback well below the old 400 ms. Update
            // it even while UDP is active so a later fallback is ready.
            prebufferTarget.store (juce::jmax (800, samples / 2),
                                   std::memory_order_relaxed);

            if (! useRelay)
                continue;

            const juce::ScopedLock writeLock (playbackWriteLock);

            if (playbackFifo.getFreeSpace() < samples)
                continue;

            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            playbackFifo.prepareToWrite (samples, start1, size1, start2, size2);

            auto writeRange = [raw] (juce::int16* target, int offset, int count)
            {
                for (int i = 0; i < count; ++i)
                {
                    const int index = (offset + i) * 2;
                    target[i] = static_cast<juce::int16> (
                        static_cast<juce::uint16> (raw[index])
                        | (static_cast<juce::uint16> (raw[index + 1]) << 8));
                }
            };

            if (size1 > 0)
                writeRange (playbackStorage.data() + start1, 0, size1);

            if (size2 > 0)
                writeRange (playbackStorage.data() + start2, size1, size2);

            playbackFifo.finishedWrite (size1 + size2);
            receivedChunks.fetch_add (1, std::memory_order_relaxed);
        }
    }

    const auto next = response.json.getProperty ("next_since", juce::var());

    if (! next.isVoid())
        lastSeq = static_cast<juce::int64> (next);
}
