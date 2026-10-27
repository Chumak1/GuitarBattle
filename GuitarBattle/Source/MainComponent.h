#pragma once

#include <JuceHeader.h>
#include "Gameplay/GameplayStats.h"
#include <array>
#include <map>
#include <memory>
#include <vector>

#include "Gameplay/ScoringSystem.h"
#include "Gameplay/GameplayEngine.h"
#include "Midi/MidiParser.h"
#include "Midi/TablatureComponent.h"
#include "Audio/PitchDetector.h"
#include "Tuner/Tuner.h"
#include "Gameplay/Metronome.h"
#include "Gameplay/GameSession.h"
#include "Generation/MidiMelodyGenerator.h"
#include "Network/ApiClient.h"
#include "Network/ClientAssets.h"
#include "UI/GameStyle.h"

class MainComponent;
class PitchDetectionThread;

// Transparent drawing layer above the video background and below the controls.
class MainOverlayComponent : public juce::Component
{
public:
    explicit MainOverlayComponent(MainComponent& ownerToUse) : owner(ownerToUse)
    {
        setInterceptsMouseClicks(false, false);
    }
    void paint(juce::Graphics& g) override;
private:
    MainComponent& owner;
};

// Transparent, non-intercepting layer above every control for pause and
// count-in cards. It must be added after the controls so modal content is not
// hidden behind the HUD or mixer.
class MainModalOverlayComponent : public juce::Component
{
public:
    explicit MainModalOverlayComponent(MainComponent& ownerToUse) : owner(ownerToUse)
    {
        setInterceptsMouseClicks(false, false);
    }
    void paint(juce::Graphics& g) override;
private:
    MainComponent& owner;
};

// Table model for the global rating shown on the song-select screen. Lives in
// this header so both the UI and the network module can fill it. Rows are
// clickable: the owner opens the public player card for the selected user.
class MainLeaderboardModel final : public juce::TableListBoxModel
{
public:
    struct Row
    {
        juce::String userId;
        juce::String username;
        juce::String displayName;
        int rank = 0;
        int level = 1;
        int rating = 0;
    };

    std::function<void(const juce::String&)> onRowClicked;

    void setRows(std::vector<Row> newRows)
    {
        rows = std::move(newRows);
    }

    int getNumRows() override
    {
        return static_cast<int>(rows.size());
    }

    void paintRowBackground(juce::Graphics& g, int rowNumber, int width,
                            int height, bool rowIsSelected) override
    {
        juce::ignoreUnused(rowNumber);

        g.setColour(rowIsSelected ? juce::Colour(0xFF2A3140)
                                  : juce::Colour(0xFF161B24).withAlpha(0.55f));
        g.fillRect(0, 0, width, height);
    }

    void paintCell(juce::Graphics& g, int rowNumber, int columnId,
                   int width, int height, bool) override
    {
        if (! juce::isPositiveAndBelow(rowNumber,
                                       static_cast<int>(rows.size())))
            return;

        const auto& row = rows[static_cast<size_t>(rowNumber)];

        juce::String text;

        switch (columnId)
        {
        case 1: text = juce::String(row.rank); break;
        case 2: text = row.displayName.isNotEmpty()
                        ? row.displayName
                        : row.username; break;
        case 3: text = "LVL " + juce::String(row.level); break;
        case 4: text = juce::String(row.rating); break;
        default: break;
        }

        g.setColour(juce::Colour(0xFFE8ECF4));
        g.setFont(juce::Font(juce::FontOptions().withHeight(columnId == 3 ? 11.0f : 12.0f)));
        g.drawText(text, 8, 0, width - 12, height,
                   columnId == 1 || columnId == 4
                       ? juce::Justification::centredRight
                       : juce::Justification::centredLeft,
                   true);
    }

    void cellClicked(int rowNumber, int, const juce::MouseEvent&) override
    {
        if (! juce::isPositiveAndBelow(rowNumber,
                                       static_cast<int>(rows.size())))
            return;

        const auto& row = rows[static_cast<size_t>(rowNumber)];

        if (row.userId.isNotEmpty() && onRowClicked != nullptr)
            onRowClicked(row.userId);
    }

private:
    std::vector<Row> rows;
};

class MainComponent : public juce::AudioAppComponent,
    private juce::Timer,
    private juce::ChangeListener
{
public:

    enum class GameState
    {
        SongSelect,
        Tuner,
        // Prepared screen: song loaded, no count-in yet. Playback starts only
        // after the player presses START.
        Ready,
        Playing,
        Paused,
        // Managed pause while the tempo is re-rendered; playback is stopped
        // and the overlay explains what is happening.
        Preparing,
        Finished
    };

    MainComponent();
    ~MainComponent() override;

    void prepareToPlay(
        int samplesPerBlockExpected,
        double sampleRate) override;

    void getNextAudioBlock(
        const juce::AudioSourceChannelInfo& bufferToFill) override;

    void timerCallback() override;

    void releaseResources() override;

    void paint(juce::Graphics& g) override;
    void paintOverlay(juce::Graphics& g);
    void paintModalOverlay(juce::Graphics& g);

    void resized() override;

    friend class MainOverlayComponent;
    friend class MainModalOverlayComponent;
    friend class PitchDetectionThread;

private:

    //==========================================================================
    // Game state

    // Read from the audio and pitch worker threads, written by the message
    // thread: the value must be atomic so pause/resume takes effect
    // immediately and without a data race.
    std::atomic<GameState> gameState{ GameState::SongSelect };

    GameState stateBeforePause = GameState::Playing;

    // Four-beat count-in before the song timeline begins.
    std::atomic<bool> countInActive{ false };
    std::atomic<int> countInRemaining{ 0 };

    std::atomic<bool> tunerActive{ false };

    // Real audio sample at the exact moment START is pressed.
    std::atomic<uint64_t> gameplayStartSample{ 0 };

    // Tempo anchor: the song position stays continuous when the tempo changes
    // mid-song. Beat 0 is at tempoAnchorSample; after a tempo change the
    // anchor moves to the current position with the new BPM.
    std::atomic<double> tempoAnchorBeat{ 0.0 };
    std::atomic<uint64_t> tempoAnchorSample{ 0 };
    double getSongBeatForSample(uint64_t sample) const;
    uint64_t getSongSampleForBeat(double beat) const;
    void applyTempoPercent(double percent);

    // Total number of audio samples spent paused.
    std::atomic<uint64_t> pausedSamples{ 0 };

    // Audio sample at which the current pause started.
    std::atomic<uint64_t> pauseStartSample{ 0 };

    //==========================================================================
    // Tuner

    int tunerDetectedString = -1;

    double tunerFrequency = 0.0;

    double tunerCents = 0.0;


    // Shared dark navy/gold theme. Declared before the child controls so it
    // outlives every component that inherits it.
    GameLookAndFeel gameLookAndFeel;

    //==========================================================================
    // Decoded JPEG frame sequence avoids JUCE VideoComponent's native Windows
    // playback window, which would otherwise cover all JUCE controls.
    MainOverlayComponent overlayComponent{ *this };
    MainModalOverlayComponent modalOverlayComponent{ *this };

    // UI

    juce::Label songTitleLabel;
    juce::Label gameSongTitleLabel;

    juce::TextButton songPridumayButton{ "START SONG" };
    juce::TextButton onlineCatalogButton{ "REFRESH CATALOG" };
    juce::Label onlineCatalogStatusLabel;
    juce::Label leaderboardTitleLabel;
    juce::Label leaderboardLabel;
    juce::TableListBox leaderboardTable{ "Global rating", nullptr };
    std::unique_ptr<MainLeaderboardModel> leaderboardModel;
    juce::Label progressionSummaryLabel;
    int ratingPoints = 0;
    void refreshProgressionSummary();
    void openPublicProfile(const juce::String& userId);
    juce::TextButton refreshLeaderboardButton{ "REFRESH RATING" };
    juce::TextButton accountLoginButton{ "LOGIN" };
    juce::TextButton accountRegisterButton{ "REGISTER" };
    juce::TextButton verifyEmailButton{ "VERIFY EMAIL" };
    juce::TextButton forgotPasswordButton{ "FORGOT PASSWORD?" };
    juce::Label accountStatusLabel;
    juce::Label loginScreenTitleLabel;
    juce::Label currentUserLabel;
    juce::TextButton logoutButton{ "LOG OUT" };
    juce::TextButton quitButton{ "EXIT" };
    bool showAuthScreen = true;
    juce::String currentUsername;
    juce::String onlineAccessToken;
    juce::String onlineSongUuid;
    juce::String onlineGameSessionUuid;
    // Internal activity telemetry. The session id is regenerated on every
    // successful sign-in; requests run on ApiClient's worker pool and never
    // block the message thread or touch the audio thread.
    juce::String telemetryClientSessionId;
    int telemetryTickCounter = 0;
    // Progression data returned by the last successful score submission.
    bool lastProgressionValid = false;
    int lastXpEarned = 0;
    int lastXpTotal = 0;
    int lastLevel = 0;
    int lastXpForNext = 0;
    juce::String lastAchievementsText;
    void sendActivityHeartbeat(bool finalEvent);
    void sendGameLaunchTelemetry();
    // All server communication is asynchronous: requests run on this client's
    // worker threads and their results arrive on the message thread.
    ApiClient apiClient;
    // Guards against stale responses from an older request overwriting newer UI.
    int authRequestId = 0;
    int leaderboardRequestId = 0;
    int onlineSessionRequestId = 0;
    void refreshGlobalLeaderboard();
    void authenticateOnlineAccount(bool createAccount);
    void showEmailVerificationDialog(const juce::String& email,
                                     const juce::String& fallbackName,
                                     bool accountCreated);
    void recoverOnlineAccountPassword();
    void showPasswordResetDialog(const juce::String& email);
    void finishOnlineSignIn(bool createAccount,
                            const juce::String& fallbackName,
                            const juce::String& accessToken,
                            int requestId);
    // Clears the local session and returns to the sign-in screen. Used by the
    // LOG OUT button and by the account window.
    void logoutCurrentUser();
    void startOnlineGameSession();
    void submitOnlineGameResult();
    // Legacy song buttons are retained in the class for minimal UI churn, but hidden.
    juce::TextButton songMistButton{ "" };
    juce::TextButton songChordsButton{ "" };
    juce::TextButton aiSoloButton{ "" };
    juce::TextButton oldGeneratorArchiveButton{ "OLD GENERATOR (ARCHIVE)" };
    bool oldGeneratorArchiveOpen = false;
    juce::ComboBox melodyStyleCombo;
    juce::TextButton generateMelodyButton{ "Daily Challenge Monday" };
    juce::TextButton newGenerationButton{ "NEW GENERATION" };
    juce::Label songPridumayBestLabel;
    juce::Label songMistBestLabel;
    juce::Label songChordsBestLabel;
    juce::Label dailyChallengeBestLabel;
    juce::String selectedSongId;

    struct SongEntry
    {
        juce::String title;
        // Server slug for songs cached under %APPDATA%/GuitarBattle/OnlineSongs.
        juce::String slug;
        juce::String artist;
        juce::File folder;
        juce::File soloMidi;
        juce::File drums;
        juce::File bass;
        juce::File rhythmGuitar;
        juce::File others;
        double bpm = 120.0;
    };
    std::vector<SongEntry> songLibrary;
    SongEntry selectedSong;
    // Slug of the loaded server song; empty for the daily challenge.
    juce::String selectedSongSlug;
    bool loadSongAudio(const SongEntry& song);

    //==========================================================================
    // Song-select catalogue: every published song from the server. Rows do not
    // carry audio files - they are downloaded on demand when the player presses
    // START SONG. Local Songs/ folders are intentionally not scanned.

    struct CatalogSong
    {
        juce::String title;
        juce::String slug;      // server slug
        juce::String serverId;  // server UUID, used to open a game session
        juce::String artist;
        double bpm = 120.0;
        int version = 0;
        bool online = false;
        bool downloaded = false; // assets present in the local cache
    };

    // List model for the song list. Lives in this header so no new compile
    // unit has to be registered in GuitarBattle.jucer.
    struct SongListModel : public juce::ListBoxModel
    {
        explicit SongListModel(MainComponent& ownerToUse) : owner(ownerToUse) {}

        int getNumRows() override;

        void paintListBoxItem(int rowNumber,
                              juce::Graphics& g,
                              int width,
                              int height,
                              bool rowIsSelected) override;

        void listBoxItemClicked(int rowNumber, const juce::MouseEvent&) override;

        // Keyboard navigation and click selection both keep the highlighted
        // catalogue row in sync with selectedCatalogIndex.
        void selectedRowsChanged(int lastRowSelected) override;

        void returnKeyPressed(int rowNumber) override;

        MainComponent& owner;
    };

    std::vector<CatalogSong> catalogSongs;
    SongListModel songListModel { *this };
    juce::ListBox songListBox { "Song catalogue", &songListModel };
    int selectedCatalogIndex = 0;

    // Best scores reported by /games/best-scores, keyed by song slug. They are
    // kept in memory only: after a sign-out the locally saved values remain.
    std::map<juce::String, int> remoteBestScores;
    int bestScoresRequestId = 0;

    void refreshOnlineCatalog();
    void refreshOnlineBestScores();
    void rebuildCatalogRows(const juce::var& serverItems);
    void updateCatalogRows();
    void selectCatalogSong(int index);
    void startSelectedSong();
    void downloadOnlineSong(const juce::String& slug,
                            const juce::String& serverId,
                            bool startAfterDownload);
    SongEntry* findOnlineLibraryEntry(const juce::String& slug);
    int getBestScoreForSong(const juce::String& slug, const juce::String& title);

    juce::Label gameSessionLabel;

    juce::Label pitchLabel;

    juce::Label comboLabel;

    juce::Label beatLabel;

    juce::Label barBeatLabel;

    juce::Label bpmLabel;

    juce::Label scoreLabel;

    juce::Label hitLabel;

    juce::Label targetNoteLabel;

    juce::Label tunerTitleLabel;

    juce::Label tunerNoteLabel;

    juce::Label tunerFrequencyLabel;

    juce::Label tunerStringLabel;

    juce::Label tunerCentsLabel;

    juce::Label tunerStatusLabel;

    juce::TextButton tunerStartButton{ "START GAME" };

    juce::TextButton tunerSkipButton{ "SKIP TUNING" };

    juce::TextButton gameStartButton{ "START" };

    // Shown on the Ready screen; the count-in and playback start only here.
    juce::TextButton readyStartButton{ "START" };

    // Shown on the pause card; kept above the modal overlay so it always
    // receives the click.
    juce::TextButton resumeOverlayButton{ "RESUME" };

    juce::TextButton pauseButton{ "PAUSE" };

    juce::TextButton menuButton{ "MAIN MENU" };

    juce::TextButton speed50Button{ "50%" };

    juce::TextButton speed75Button{ "75%" };

    juce::TextButton speed100Button{ "100%" };

    juce::TextButton speed120Button{ "120%" };

    juce::TextButton audioSettingsButton{ "Audio Settings" };
    juce::TextButton distortionButton{ "DISTORTION OFF" };
    juce::TextButton midiBackingButton{ "FULL MIX OFF" };
    juce::Slider midiBackingVolumeSlider;
    juce::Label midiBackingVolumeLabel;
    std::atomic<bool> midiBackingEnabled{ true };
    std::atomic<float> midiBackingVolume{ 0.22f };

    juce::TextButton midiRhythmGuitarButton{ "RHYTHM GUITAR ON" };
    juce::Slider midiRhythmGuitarVolumeSlider;
    juce::Label midiRhythmGuitarVolumeLabel;
    std::atomic<bool> midiRhythmGuitarEnabled{ true };
    std::atomic<float> midiRhythmGuitarVolume{ 0.20f };

    juce::TextButton midiBassButton{ "BASS ON" };
    juce::Slider midiBassVolumeSlider;
    juce::Label midiBassVolumeLabel;
    std::atomic<bool> midiBassEnabled{ true };
    std::atomic<float> midiBassVolume{ 0.45f };
    juce::TextButton midiDrumsButton{ "DRUMS ON" };
    juce::Slider midiDrumsVolumeSlider;
    juce::Label midiDrumsVolumeLabel;
    std::atomic<bool> midiDrumsEnabled{ true };
    std::atomic<float> midiDrumsVolume{ 0.45f };
    juce::TextButton midiPianoButton{ "OTHERS ON" };
    juce::Slider midiPianoVolumeSlider;
    juce::Label midiPianoVolumeLabel;
    std::atomic<bool> midiPianoEnabled{ true };
    std::atomic<float> midiPianoVolume{ 0.30f };

    juce::TextButton metronomeMixerButton{ "METRONOME ON" };
    juce::Slider metronomeMixerVolumeSlider;
    juce::Label metronomeMixerVolumeLabel;
    std::atomic<bool> metronomeMixerEnabled{ true };
    std::atomic<float> metronomeMixerVolume{ 0.70f };

    struct BackingNote
    {
        double startBeat = 0.0;
        double durationBeats = 0.1;
        int midiNote = 60;
        int velocity = 80;
        int instrument = 0; // 1=bass, 2=drums, 3=piano, 4=rhythm guitar
    };
    std::vector<BackingNote> backingNotes;
    std::atomic<bool> dailyChallengeLoaded{ false };
    bool loadBackingMidiEvents(const juce::File& directory);

    // Sample-based instruments for the generated backing MIDI. These are loaded
    // once before playback; the audio callback only reads immutable buffers.
    struct BackingSampleRegion
    {
        juce::AudioBuffer<float> audio;
        double sourceSampleRate = 44100.0;
        int rootKey = 60, lowKey = 0, highKey = 127;
        double loopStart = -1.0, loopEnd = -1.0;
        bool looping = false;
    };
    std::array<std::vector<BackingSampleRegion>, 4> backingSampleRegions;
    std::array<std::array<int, 128>, 4> backingSampleMap{};
    std::array<std::atomic<bool>, 4> backingSamplesLoaded{};
    void loadBackingSampleBanks(const juce::File& assetsRoot);
    float renderBackingSample(int instrument, int midiNote, double elapsedOutputSamples, double outputSampleRate) const;
    juce::Slider distortionDriveSlider;
    juce::Slider distortionToneSlider;
    juce::Label distortionDriveLabel;
    juce::Label distortionToneLabel;
    std::atomic<bool> distortionEnabled{ false };
    std::atomic<float> distortionDrive{ 2.5f };
    std::atomic<float> distortionTone{ 0.65f };
    float distortionFilterState[2] { 0.0f, 0.0f };

    // Difficulty remains available to the scoring system.
    // Buttons are intentionally hidden from the UI.

    juce::TextButton easyButton{ "EASY" };

    juce::TextButton normalButton{ "NORMAL" };

    juce::TextButton hardButton{ "HARD" };

    TablatureComponent tablatureComponent;

    //==========================================================================
    // Pitch analysis buffers

    juce::AudioBuffer<float> pitchBuffer;

    int pitchBufferPosition = 0;

    // Guitar capture channel. -1 means "auto": follow the loudest active input.
    // A non-negative value is a hardware input-channel index, which stays stable
    // even if other channels are enabled or disabled later.
    std::atomic<int> preferredInputChannel{ -1 };
    std::atomic<int> activeInputChannels{ 0 };
    std::array<int, 32> activeInputHardwareChannels{};
    int currentInputChannel = -1;

    // Sample rate cached in prepareToPlay(); the audio callback must not query
    // the device manager from the realtime thread.
    std::atomic<double> cachedSampleRate{ 0.0 };

    // Compact input selector shown on the tuner screen so the player can
    // choose any available hardware input without opening gameplay.
    juce::Label guitarInputLabel;
    juce::ComboBox guitarInputCombo;

    void setPreferredInputChannel(int channel);
    bool ensureInputChannelEnabled(int hardwareChannel);
    void rebuildGuitarInputChoices();
    void refreshGuitarInputCombo();
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    std::atomic<uint64_t> audioSampleCounter{ 0 };

    std::atomic<double> currentPlayheadBeat{ 0.0 };

    juce::AudioBuffer<float> pitchAnalysisBuffer;

    std::atomic<bool> pitchBufferReady{ false };

    juce::AudioBuffer<float> pitchAnalysisBuffer2;

    std::atomic<bool> pitchBufferReady2{ false };

    std::atomic<uint64_t> pitchAnalysisStartSample{ 0 };

    std::atomic<uint64_t> pitchAnalysisStartSample2{ 0 };

    //==========================================================================
    // Note / onset detection

    std::atomic<bool> noteOn{ false };

    std::atomic<uint64_t> lastNoteOnSample{ 0 };

    std::atomic<uint64_t> lastOnsetSample{ 0 };

    double previousRmsValue = 0.0;

    double onsetNoiseFloor = 0.0;

    uint64_t lastDetectedOnsetSample = 0;

    double previousRmsValue2 = 0.0;

    double lastPitch = 0.0;

    //==========================================================================
    // Tempo

    std::atomic<double> bpm{ 120.0 };
    // Playback-rate multiplier for all external WAV stems; 1.0 = original speed.
    std::atomic<double> audioPlaybackRate{ 1.0 };

    std::atomic<double> speedPercent{ 100.0 };

    double baseBpm = 140.0;

    //==========================================================================
    // Difficulty / scoring

    Difficulty difficulty = Difficulty::Normal;





    int metronomeSamplesRemaining = 0;

    int metronomePlaybackPosition = 0;

    bool metronomePlayingAccent = false;

    // Clean electric-guitar sample bank (SFZ + WAV/FLAC samples).
    // The bank is loaded outside the real-time audio callback.
    struct GuitarSampleRegion
    {
        juce::AudioBuffer<float> audio;
        double sourceSampleRate = 44100.0;
        int rootKey = 60;
        int lowKey = 0;
        int highKey = 127;
        double loopStart = -1.0;
        double loopEnd = -1.0;
        bool looping = false;
    };

    std::vector<GuitarSampleRegion> guitarSampleRegions;
    std::array<int, 128> guitarRegionForMidi{};
    std::atomic<bool> guitarSamplesLoaded{ false };

    void loadGuitarSampleBank(const juce::File& assetsRoot);
    float renderGuitarSample(int midiNote, double elapsedOutputSamples, double outputSampleRate) const;

    juce::AudioFormatManager audioFormatManager;

    // Server-hosted sound pack.  clientAssetsRoot points at the unpacked
    // "current" directory and is empty until a validated cache exists.  The
    // buffers above are only rebuilt while the audio device is stopped.
    juce::File clientAssetsRoot;
    bool clientAssetsReady = false;
    bool clientAssetsRefreshInFlight = false;
    bool pendingClientAssetsReload = false;
    bool audioDeviceStarted = false;
    void initialiseAudio();
    void startClientAssetRefresh();
    void applyPendingClientAssetsReload();
    void reloadClientAssets();
    void startAudioDevice(const juce::XmlElement* savedState);

    // Song stems are decoded before playback; the real-time callback only reads and mixes samples.
    struct ExternalAudioTrack
    {
        juce::AudioBuffer<float> audio; // decoded original stem
        std::shared_ptr<juce::AudioBuffer<float>> playbackAudio; // WSOLA time-stretched, same pitch
        double sourceSampleRate = 44100.0;
        std::atomic<bool> loaded{ false };
    };
    std::array<ExternalAudioTrack, 4> externalAudioTracks;
    bool loadExternalAudioTrack(size_t index, const juce::File& file);
    void prepareExternalAudioForSpeed(double speedPercent);
    // Keep the previous MIDI generator implementation in the source, but use
    // the external stem mixer during normal gameplay.
    const bool externalAudioMixerActive = true;

    // Loaded from the server-hosted sound pack before the audio device starts
    // (or while it is stopped for a sound-pack refresh).
    void loadMetronomeSamples(const juce::File& assetsRoot);

    juce::AudioBuffer<float> metronomeAccentBuffer;

    juce::AudioBuffer<float> metronomeNormalBuffer;


    static constexpr int pitchAnalysisSize = 4096;
    PitchDetector pitchDetector;
    GuitarTuner tuner;
    Metronome metronome;
    GameSession gameSession;
    GameplayStats gameplayStats;

    int feedbackType = 0; // 1 PERFECT, 2 GREAT, 3 GOOD, 4 MISS, 5 combo milestone
    uint32_t feedbackStartMs = 0;
    float feedbackBaseFontSize = 24.0f;



    //==========================================================================
    // Target helpers

    void setTargetFromMidiIndex(
        int index);

    bool loadSongMidi(const juce::String& songId);
    bool generateDailyChallengeMidi(bool forceRandomSeed = false);
    bool loadGeneratedMelody();
    juce::File dailyChallengeMidiFile;
    void beginTuningForSelectedSong();
    int getBestScore(const juce::String& songId);
    void saveBestScoreForSelectedSong(int score);
    void updateSongBestScoreLabels();

    uint64_t getTargetSample(
        int index,
        double sampleRate) const;

    // Frequency window used by pitch analysis. During gameplay it follows the
    // current target so high frets above 700 Hz are detected.
    juce::Range<double> getPitchAnalysisRange() const;

    void advanceTarget();

    //==========================================================================
    // Game helpers

    void pauseGame();

    void resumeGame();

    // Starts the four-beat count-in and gameplay from the Ready screen.
    void beginGameplayCountIn();

    // Managed tempo change: playback pauses while the stems are re-rendered,
    // then continues from the same beat.
    std::atomic<bool> tempoChangeInProgress{ false };
    bool tempoChangeQueued = false;
    double queuedTempoPercent = 100.0;
    GameState tempoResumeState = GameState::Playing;
    double pendingTempoBeat = 0.0;
    uint64_t pendingTempoSample = 0;
    double pendingTempoPercent = 100.0;
    void beginManagedTempoChange(double percent);
    void finishManagedTempoChange();

    void returnToSongSelect();

    // Animated feedback for note quality and combo milestones.
    void showHitFeedback(const juce::String& text, int type);
    void updateHitFeedbackAnimation();

    //==========================================================================
    // Pitch worker

    // Audio analysis worker lives in Audio/PitchDetectionThread.h/.cpp.
    std::unique_ptr<class PitchDetectionThread> pitchThread;

    //==========================================================================
    // Application properties

    juce::ApplicationProperties appProperties;

    //==========================================================================
    // Frontend extension point. The frontend agent owns MainComponentUi.inc and
    // may add UI widgets/state/helpers there without editing this header.
#include "UI/MainComponentUi.inc"

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
        MainComponent)
};