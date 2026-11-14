#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

/*
    Lobby voice chat.

    The client has no WebSocket/TLS socket in JUCE 9.0.2, so the transport is a
    small HTTPS relay: the microphone is captured as 16 kHz mono signed 16-bit
    PCM, sent in 200 ms chunks, and the other lobby members' chunks are polled
    and mixed into the main output.

    Sending and polling run on two independent worker threads. A single worker
    that did POST then GET took ~2 * TLS-round-trip per loop (300-500 ms on the
    production server), so it delivered only a fraction of real-time audio and
    the receiver never kept its jitter buffer full. With two workers the
    outgoing chunk rate and the incoming poll rate are independent.

    All audio-thread methods are realtime safe: they only use preallocated
    buffers, atomics and juce::AbstractFifo.

    Capture is one input channel of the current audio device, chosen in
    Settings (it may be the same channel as the guitar). The main audio
    callback passes that channel here; there is no second device manager.
*/
class VoiceChat final
{
public:
    VoiceChat();
    ~VoiceChat();

    /** Start the relay workers for one lobby. Voice is still disabled until
        setEnabled(true) is called from the lobby button. */
    void start (const juce::String& lobbyId, const juce::String& token);

    /** Stop the workers and release the relay state. Safe to call repeatedly. */
    void stop();

    void setEnabled (bool shouldBeEnabled);
    bool isEnabled() const noexcept { return enabled.load (std::memory_order_relaxed); }
    bool isStarted() const noexcept { return started.load (std::memory_order_relaxed); }

    /** Lobby the workers are bound to. Message thread only. */
    const juce::String& getLobbyId() const noexcept { return lobbyId; }

    /** Capture the selected channel for the Settings level meter even when
        no lobby is open. No network traffic is generated in this mode. */
    void setMonitorEnabled (bool shouldMonitor)
    {
        monitorEnabled.store (shouldMonitor, std::memory_order_release);
    }

    /** Called from MainComponent::getNextAudioBlock. Realtime safe. */
    void captureFromDeviceChannel (const float* samples,
                                   int numSamples,
                                   double sampleRate);

    /** Adds the received voice to the main output. Realtime safe. */
    void mixIntoOutput (juce::AudioBuffer<float>& output,
                        int startSample,
                        int numSamples,
                        int numOutputChannels,
                        double sampleRate);

    /** Resets the playback resampler after an output-device change. Realtime
        safe (called from prepareToPlay before the callback runs). */
    void prepare (double outputSampleRate);

    float getInputLevel() const noexcept
    {
        return inputLevel.load (std::memory_order_relaxed);
    }

    float getOutputLevel() const noexcept
    {
        return outputLevel.load (std::memory_order_relaxed);
    }

    /** Compact relay diagnostics for the room status label. */
    int getLastHttpStatus() const noexcept;
    int getSentChunks() const noexcept
    {
        return static_cast<int> (sentChunks.load (std::memory_order_relaxed));
    }
    int getReceivedChunks() const noexcept
    {
        return static_cast<int> (receivedChunks.load (std::memory_order_relaxed));
    }

    /** True while direct UDP packets from the peer arrive. */
    bool isPeerActive() const noexcept;

private:
    static constexpr int targetSampleRate = 16000;
    static constexpr int udpChunkSamples = 160;    // 10 ms, one UDP packet
    // Multiples of one 20 ms capture block. 120 ms is the floor: the server
    // allows 10 chunks/s and the 0.5/s signaling chunk must fit as well.
    static constexpr int minRelayChunkSamples = 1920;     // 120 ms
    static constexpr int initialRelayChunkSamples = 2240; // 140 ms
    static constexpr int maxRelayChunkSamples = 2560;     // 160 ms cap
    static constexpr int captureFifoSize = 32768;  // 2 s
    static constexpr int playbackFifoSize = 32768; // 2 s
    static constexpr int initialPrebufferSamples = 1600; // 100 ms
    static constexpr int maxBlock = 1024;
    static constexpr int requestTimeoutMs = 1500;

    struct CapturePath
    {
        CapturePath();

        juce::AbstractFifo fifo { captureFifoSize };
        std::vector<juce::int16> storage;
        std::vector<juce::int16> scratch;
        double phase = 0.0;
        float lowpass = 0.0f;
        bool haveSample = false;

        void reset();
    };

    class Worker : public juce::Thread
    {
    public:
        Worker (VoiceChat& ownerToUse, const char* threadName)
            : juce::Thread (threadName), owner (ownerToUse)
        {
        }

    protected:
        VoiceChat& owner;
    };

    class SendWorker final : public Worker
    {
    public:
        using Worker::Worker;

        void run() override { owner.runSend (*this); }
    };

    class PollWorker final : public Worker
    {
    public:
        using Worker::Worker;

        void run() override { owner.runPoll (*this); }
    };

    class UdpWorker final : public Worker
    {
    public:
        using Worker::Worker;

        void run() override { owner.runUdp (*this); }
    };

    class SignalWorker final : public Worker
    {
    public:
        using Worker::Worker;

        void run() override { owner.runSignal (*this); }
    };

    void runSend (juce::Thread& worker);
    void runPoll (juce::Thread& worker);
    void runUdp (juce::Thread& worker);
    void runSignal (juce::Thread& worker);

    void discoverUdpEndpoint();
    void resolvePublicIpFallback();
    bool refreshPeerEndpoint();
    bool sendUdpPacket (const juce::int16* samples, int numSamples);
    void sendUdpPunch();
    void handleUdpPacket (const void* data, int bytes);

    bool assembleChunk();
    void sendRelayChunk();
    void sendRelayControl();
    void pollChunks();

    void pushMono (CapturePath& path,
                   const float* mono,
                   int numSamples,
                   double sampleRate);

    static bool isFiniteRate (double sampleRate) noexcept
    {
        return sampleRate > 1000.0 && sampleRate < 400000.0;
    }

    CapturePath mainCapture;

    juce::AbstractFifo playbackFifo { playbackFifoSize };
    std::vector<juce::int16> playbackStorage;
    std::vector<juce::int16> playbackScratch;
    // The relay poll thread and the UDP receive thread are two producers for
    // the same FIFO. AbstractFifo is single-producer/single-consumer, so the
    // producers serialize on this lock; the audio thread never takes it.
    juce::CriticalSection playbackWriteLock;

    // 20 ms of audio, the direct-UDP packet payload. The relay batches ten of
    // these into one 200 ms POST.
    std::vector<juce::int16> pendingSend;
    std::vector<juce::int16> relayPending;

    // Only touched by the workers while they are running; start()/stop()
    // modify them only after the workers have been stopped.
    juce::String lobbyId;
    juce::String token;
    juce::int64 lastSeq = 0;

    SendWorker sendWorker { *this, "GuitarBattle voice send" };
    PollWorker pollWorker { *this, "GuitarBattle voice poll" };
    UdpWorker udpWorker { *this, "GuitarBattle voice udp" };
    SignalWorker signalWorker { *this, "GuitarBattle voice signal" };

    // Direct UDP path: the server exchanges each member's public endpoint.
    // DatagramSocket cannot be re-bound after shutdown(), so each start()
    // creates a fresh object; stop() destroys it after the workers exit.
    std::unique_ptr<juce::DatagramSocket> udpSocket;
    int boundUdpPort = 0;
    // Public port published to the lobby (STUN mapping or the bound port).
    // Written by the poll worker, read by the send worker.
    std::atomic<int> localUdpPort { 0 };
    std::atomic<bool> udpReady { false };
    juce::CriticalSection peerLock;
    juce::String peerHost;
    int peerPort = 0;
    std::atomic<int> lastEndpointStatus { 0 };
    // Relay chunk size adapts to the measured POST time: fast links get
    // 100 ms chunks (lower latency), slow links grow to 200 ms (no backlog).
    std::atomic<int> relayChunkTarget { initialRelayChunkSamples };
    double relayPostMsAverage = 0.0;
    // Public IPv4 reported by STUN, host byte order. Direct-UDP signaling is
    // also carried inside the existing relay chunks, so no new server route
    // is required for the low-latency path.
    std::atomic<juce::uint32> publicIpV4 { 0 };
    std::atomic<int> prebufferTarget { initialPrebufferSamples };
    std::atomic<std::uint32_t> udpPacketsSent { 0 };
    std::atomic<std::uint32_t> udpPacketsReceived { 0 };
    std::atomic<std::uint32_t> lastPeerPacketMs { 0 };
    std::atomic<std::uint32_t> lastPeerKnownMs { 0 };
    std::atomic<std::uint32_t> udpSequence { 0 };
    std::uint32_t lastPunchMs = 0; // send worker only

    double playbackPhase = 0.0;
    float playbackPrevious = 0.0f;
    float playbackCurrent = 0.0f;
    int playbackPos = 0;
    int playbackAvailable = 0;
    int starvedSamples = 0;

    std::atomic<bool> playbackPrimed { false };
    std::atomic<bool> enabled { false };
    std::atomic<bool> started { false };
    std::atomic<bool> monitorEnabled { false };
    std::atomic<bool> clearPlaybackRequested { false };
    std::atomic<bool> resetCaptureRequested { false };
    std::atomic<float> inputLevel { 0.0f };
    std::atomic<float> outputLevel { 0.0f };
    std::atomic<int> lastSendStatus { 0 };
    std::atomic<int> lastPollStatus { 0 };
    std::atomic<std::uint32_t> sentChunks { 0 };
    std::atomic<std::uint32_t> receivedChunks { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceChat)
};
