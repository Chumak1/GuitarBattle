#pragma once

#include <JuceHeader.h>
#include "Gameplay/GameplayStats.h"
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "Gameplay/ScoringSystem.h"
#include "Gameplay/GameplayEngine.h"
#include "Gameplay/DuelMatch.h"
#include "Midi/MidiParser.h"
#include "Midi/TablatureComponent.h"
#include "Audio/PitchDetector.h"
#include "Audio/MusicTrackPlayer.h"
#include "Audio/VstHost.h"
#include "Tuner/Tuner.h"
#include "Gameplay/Metronome.h"
#include "Gameplay/GameSession.h"
#include "Audio/UiSounds.h"
#include "Audio/VoiceChat.h"
#include "Generation/MidiMelodyGenerator.h"
#include "Network/ApiClient.h"
#include "Network/DuelClient.h"
#include "Network/ClientAssets.h"
#include "UI/GameStyle.h"
#include "UI/GuitarInputMonitor.h"
#include "UI/DuelLobby.h"
#include "UI/DuelStageComponent.h"
#include "UI/SectionSidebar.h"
#include "UI/CampaignPanel.h"
#include "UI/VstPanel.h"
#include "UI/PublicProfilePanel.h"
#include "Network/SessionStore.h"

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

        // Legacy rating points. Kept for servers that do not expose Guitar
        // Skills yet; the table falls back to it when guitarSkills < 0.
        int rating = 0;

        // Guitar Skills (0..100, one decimal) and the numbers behind it.
        double guitarSkills = -1.0;
        double accuracyPercent = 0.0;
        double duelWinRate = 0.0;
        int duelWins = 0;
        int duelLosses = 0;
        int duelDraws = 0;
        int xpTotal = 0;
        bool provisional = false;
        int positionChange = 0;
        bool hasPositionChange = false;
    };

    std::function<void(const juce::String&)> onRowClicked;

    // The signed-in player is highlighted exactly like in the account window's
    // global rating table.
    void setCurrentUsername(juce::String username)
    {
        meUsername = std::move(username);
    }

    void setRows(std::vector<Row> newRows)
    {
        rows = std::move(newRows);
    }

    // The page shows the top three as a podium and the table below it, so the
    // table starts after those rows instead of repeating them.
    void setFirstRow(int firstRowToUse)
    {
        firstRow = juce::jmax(0, firstRowToUse);
    }

    const std::vector<Row>& getRows() const { return rows; }

    int getNumRows() override
    {
        return juce::jmax(0, static_cast<int>(rows.size()) - firstRow);
    }

    void paintRowBackground(juce::Graphics& g, int rowNumber, int, int, bool) override
    {
        const int index = rowNumber + firstRow;

        if (! juce::isPositiveAndBelow(rowNumber, getNumRows())
            || ! juce::isPositiveAndBelow(index,
                                          static_cast<int>(rows.size())))
        {
            g.fillAll(juce::Colours::transparentBlack);
            return;
        }

        const auto& row = rows[static_cast<size_t>(index)];
        const bool isMe = meUsername.isNotEmpty()
            && row.username == meUsername;

        g.fillAll(isMe
                      ? UiColors::accent.withAlpha(0.08f)
                      : (index % 2 == 0
                             ? UiColors::surface.withAlpha(0.35f)
                             : UiColors::surface2.withAlpha(0.25f)));
    }

    void paintCell(juce::Graphics& g, int rowNumber, int columnId,
                   int width, int height, bool) override
    {
        const int index = rowNumber + firstRow;

        if (! juce::isPositiveAndBelow(rowNumber, getNumRows())
            || ! juce::isPositiveAndBelow(index,
                                          static_cast<int>(rows.size())))
            return;

        const auto& row = rows[static_cast<size_t>(index)];
        const bool isMe = meUsername.isNotEmpty()
            && row.username == meUsername;

        juce::String text;
        auto justification = juce::Justification::centredLeft;
        auto colour = isMe ? UiColors::accent : UiColors::text;

        switch (columnId)
        {
        case 1:
            text = juce::String(row.rank);
            justification = juce::Justification::centred;
            colour = row.rank <= 3 ? UiColors::accent : UiColors::muted;
            break;
        case 2:
            // One name per row: the display name when the player set one, the
            // username otherwise. The "@username" suffix was noise in the list.
            text = row.displayName.isNotEmpty()
                       ? row.displayName
                       : row.username;
            break;
        case 3:
            text = "LVL " + juce::String(row.level);
            justification = juce::Justification::centred;
            colour = UiColors::secondaryText;
            break;
        case 4:
            // Guitar Skills is the competitive metric. Older servers send only
            // rating_points, so fall back to it until the field arrives.
            text = row.guitarSkills >= 0.0 ? formatSkills(row.guitarSkills)
                                           : formatPoints(row.rating);
            justification = juce::Justification::centredRight;
            colour = isMe ? UiColors::accent : UiColors::gold;
            break;
        case 5:
            // Win rate plus the raw duel record in one cell: "75.0% · 12-4".
            text = row.duelWins + row.duelLosses + row.duelDraws > 0
                       ? formatPercent(row.duelWinRate) + "  "
                             + juce::String(row.duelWins) + "-"
                             + juce::String(row.duelLosses)
                             + (row.duelDraws > 0
                                    ? "-" + juce::String(row.duelDraws)
                                    : juce::String())
                       : juce::String ("-");
            justification = juce::Justification::centredRight;
            colour = UiColors::secondaryText;
            break;
        case 6:
            // No guitar_skills means the server predates the rollout: do not
            // claim the row is ranked by a metric it did not send.
            text = row.guitarSkills < 0.0
                       ? juce::String ("-")
                       : (row.provisional ? juce::String ("PROV")
                                          : juce::String ("RANKED"));
            justification = juce::Justification::centred;
            colour = row.guitarSkills < 0.0
                         ? UiColors::muted
                         : (row.provisional ? UiColors::muted : UiColors::green);
            break;
        default:
            break;
        }

        g.setColour(colour);
        g.setFont(GameUi::font(12.0f, isMe && columnId == 2));
        g.drawText(text, 6, 0, width - 12, height, justification, true);
    }

    void cellClicked(int rowNumber, int, const juce::MouseEvent&) override
    {
        openRow (rowNumber);
    }

    /** Index of the first row shown (the podium owns the top three). */
    int getFirstRow() const noexcept { return firstRow; }

    void openRow (int rowNumber)
    {
        const int index = rowNumber + firstRow;

        if (! juce::isPositiveAndBelow(rowNumber, getNumRows())
            || ! juce::isPositiveAndBelow(index,
                                          static_cast<int>(rows.size())))
            return;

        const auto& row = rows[static_cast<size_t>(index)];

        if (row.userId.isNotEmpty() && onRowClicked != nullptr)
            onRowClicked(row.userId);
    }

private:
    // Same thousands separator the account window uses for rating points.
    static juce::String formatPoints(int value)
    {
        auto text = juce::String(value);

        for (int i = text.length() - 3; i > 0; i -= 3)
            text = text.substring(0, i) + " " + text.substring(i);

        return text;
    }

    static juce::String formatSkills(double value)
    {
        return juce::String(value, 1);
    }

    static juce::String formatPercent(double value)
    {
        return juce::String(value, 1) + "%";
    }

    std::vector<Row> rows;
    juce::String meUsername;
    int firstRow = 0;
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
    void mouseUp (const juce::MouseEvent& event) override;

    void resized() override;

    // Podium hit areas on HOME, filled by paint() so a click on a medal card
    // opens that player's profile like a table row does.
    std::array<juce::Rectangle<int>, 3> podiumHitBounds {};
    std::array<juce::String, 3> podiumUserIds {};

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

    // Count-in before the song timeline begins. The length is one full bar of
    // the song's meter, so 4/4 counts four clicks and 7/8 counts seven.
    std::atomic<bool> countInActive{ false };
    std::atomic<int> countInRemaining{ 0 };
    std::atomic<int> countInTotal{ 3 };

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
    // Pixel-art duel backdrop, below the overlay and the tablature.
    DuelStageComponent duelStage;

    // Hub navigation and section panels. The sidebar is the only persistent
    // chrome; each section reuses the existing screens/components.
    std::unique_ptr<SectionSidebar> sectionSidebar;
    std::unique_ptr<CampaignPanel> campaignPanel;
    std::unique_ptr<juce::Viewport> profileViewport;
    std::unique_ptr<juce::Component> profileContent;
    std::unique_ptr<juce::Viewport> settingsViewport;
    std::unique_ptr<juce::Component> settingsContent;
    std::unique_ptr<juce::Viewport> friendsViewport;
    std::unique_ptr<juce::Component> friendsContent;
    std::unique_ptr<juce::Viewport> messagesViewport;
    std::unique_ptr<juce::Component> messagesContent;
    std::unique_ptr<VstPanel> vstSectionPanel;
    // Full profile of another player: an overlay above the current section so
    // BACK can return to exactly where the card was clicked.
    // Declared before the viewport so the viewport is destroyed first and never
    // holds a dangling viewed-component pointer.
    std::unique_ptr<PublicProfilePanel> publicProfilePanel;
    std::unique_ptr<juce::Viewport> publicProfileViewport;
    AppSection publicProfileReturnSection = AppSection::Home;

    AppSection currentSection = AppSection::Home;
    AppSection matchOriginSection = AppSection::Home;
    bool tunerStandalone = false;

    // Admin-defined campaigns ("tower riffs"): each has a name, one bot level
    // for the whole run and an ordered list of songs. Progress is persisted per
    // campaign id; the actual fights reuse the bot duel.
    int campaignStage = 0;
    // High-resolution time of the last local write of ``campaignStage``. A
    // progress sync that was requested before this point carries stale data
    // (its GET may be answered before the stage-result POST commits), so its
    // selected-campaign value must not overwrite the newer local one.
    double campaignStageWriteMs = 0.0;
    bool campaignFightActive = false;
    bool campaignStageWon = false;
    // Set after a campaign victory: the next stage goes straight to the Ready
    // screen without the pre-flight tuner.
    bool skipTunerForNextStage = false;
    std::vector<int> campaignSongIndices;
    std::vector<CampaignView> serverCampaigns;
    juce::String selectedCampaignId;
    bool campaignRefreshInFlight = false;
    bool campaignLoading = false;
    // Server-side tower progress, mirrored into ``CampaignBest:<id>`` as a
    // read-only cache. The stage-result endpoint owns the XP and the cleared
    // stage count (a defeat resets the run to the first opponent), so the sync
    // replaces the cache with the server's value and never uploads it.
    bool campaignProgressSyncInFlight = false;
    void syncServerCampaignProgress();
    // Screen 1.8.0: a Battle (tower) run is reported to
    // POST /campaigns/{id}/stages/{position}/result, which owns the XP. The run
    // id is generated once per attempt and reused by the transport retry, so
    // the server stays idempotent.
    void reportCampaignStageResult (const juce::String& campaignId, int position,
                                    const juce::String& runId, bool won);
    juce::String campaignStageRunId;
    // -1 = not reported yet, -2 = report failed (no fake XP), >= 0 = server XP.
    int campaignStageServerXp = -1;
    // -1 = no normalized value returned, >= 0 = server normalized score.
    int campaignStageNormalizedScore = -1;
    bool campaignStageReportPending = false;
    bool campaignStageReportFailed = false;
    void notifyCharacterUnlocked (const juce::String& name);

    MainOverlayComponent overlayComponent{ *this };
    MainModalOverlayComponent modalOverlayComponent{ *this };

    // UI

    juce::Label songTitleLabel;
    juce::Label gameSongTitleLabel;

    juce::TextButton songPridumayButton{ "START SONG" };
    juce::TextButton onlineCatalogButton{ "REFRESH CATALOG" };
    // A catalogue request is in flight. Prevents duplicate fetches when several
    // sections ask for the list at the same time (Battle FIGHT, song select).
    bool onlineCatalogInFlight = false;
    juce::Label onlineCatalogStatusLabel;

    // Song asset download feedback. The ApiClient worker publishes the overall
    // fraction into a shared atomic; timerCallback() copies it into the double
    // the ProgressBar reads, so the network thread never touches a component.
    // The shared_ptr keeps the state alive if the window closes mid-download.
    double songDownloadProgressValue = 0.0;
    juce::ProgressBar songDownloadProgressBar{ songDownloadProgressValue };
    std::shared_ptr<std::atomic<float>> songDownloadProgressState;
    juce::Label leaderboardTitleLabel;
    juce::Label leaderboardLabel;
    juce::TableListBox leaderboardTable{ "Global rating", nullptr };
    std::unique_ptr<MainLeaderboardModel> leaderboardModel;
    juce::Label progressionSummaryLabel;
    int progressionLevel = 1;
    int progressionXp = 0;
    int progressionAchievements = 0;
    int progressionAchievementsTotal = 0;
    int progressionXpForNext = 0;
    int progressionXpRemaining = 0;

    // Account summary shown on HOME above the global leaderboard (moved out of
    // the personal account page). Filled from /auth/me, /games/best-scores,
    // /leaderboard/global and /profile/progression.
    juce::String accountEmail;
    juce::String accountScoresText { "-" };
    juce::String accountRankText { "-" };
    juce::String accountRankNote { "not in the top 50" };
    juce::String accountSongsText { "-" };
    juce::String accountAttemptsText { "-" };
    juce::String accountLevelText { "-" };
    juce::String accountXpText { "-" };
    juce::String accountXpNote { "total experience" };
    juce::String accountAccuracyText { "-" };
    juce::String accountTimingText { "-" };
    void refreshHomeAccountRank();
    float levelBarShown = 0.0f;
    juce::TextButton dailyChallengeButton{ "DAILY CHALLENGE" };
    juce::Label dailyChallengeStatusLabel;
    juce::String dailyChallengeSongSlug;
    juce::String dailyChallengeSongId;
    bool currentSessionIsDaily = false;
    // Admin draft preview. ``onlineIsAdmin`` mirrors ``is_admin`` from
    // /auth/me; ``adminPreviewMode`` is the ADMIN toggle in the header and is
    // remembered in the user settings. A draft is always played as a preview:
    // the server stores the score as history and ranks nothing.
    bool onlineIsAdmin = false;
    bool adminPreviewMode = false;
    bool currentSessionIsPreview = false;
    // Practice-section run: the server stores the score for audit only and
    // awards no XP/rating/personal best. Set when a solo run starts (not a
    // campaign stage, duel, online match or official daily attempt) and kept
    // until the results screen is left.
    bool currentSessionIsPractice = false;
    juce::TextButton adminPreviewButton{ "ADMIN" };
    void setAdminPreviewMode(bool enabled);
    // True when the catalogue/detail requests may ask for drafts.
    bool isAdminPreviewActive() const
    {
        return adminPreviewMode && onlineIsAdmin && onlineAccessToken.isNotEmpty();
    }
    bool dailyOfficialUsed = false;
    void refreshDailyChallenge();
    void onDailyChallengeClicked();
    int ratingPoints = 0;
    void refreshProgressionSummary();
    void openPublicProfile(const juce::String& userId);
    void closePublicProfile();
    void openNotifications();
    void refreshSocialNotifications();
    void refreshMessageUnread();
    void openMessagesFor (const juce::String& userId,
                          const juce::String& displayName = {});
    int socialUnreadNotifications = 0;
    int notificationPollTicks = 0;
    juce::uint32 lastNotificationPollMs = 0;
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
    juce::TextButton fullScreenButton{ "FULLSCREEN" };
    bool showAuthScreen = true;
    juce::String currentUsername;

    // Profile character selection (admin-managed UI levels). The chosen 4x2
    // spritesheet replaces the built-in rocker in every duel mode.
    int selectedCharacterId = 0;
    juce::String selectedCharacterName;
    juce::String selectedCharacterSheetUrl;
    void setSelectedCharacter (int id, const juce::String& name,
                               const juce::String& sheetUrl,
                               bool persistToServer = true);
    void restoreSelectedCharacter();
    // New profiles start with Ray selected: when nothing is stored locally (and
    // the account has no character), pick the admin-uploaded character named
    // Ray from GET /characters and persist it.
    void applyDefaultCharacterIfNeeded (bool force = false);
    void applyCharacterFromServer (int id, const juce::String& name,
                                   const juce::String& sheetUrl);
    bool defaultCharacterRequestInFlight = false;
    void applyPlayerCharacterSheet();
    // Same, for an explicit sheet URL (the server's authoritative copy of this
    // player's character in an online duel).
    void applyPlayerCharacterSheetFromUrl(const juce::String& sheetUrl);
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
    // Self-update: once per launch the client asks the public landing endpoint
    // for the active portable build of its own platform. A newer build is
    // downloaded automatically, then a tiny platform helper replaces the
    // running app and relaunches it; no installer UI is involved. A read-only
    // per-machine install (Program Files) falls back to the installer, which
    // can elevate and upgrade in place. Never touches the audio thread.
    void checkForUpdatesOnLaunch();
    void startAutomaticUpdate(const juce::String& version,
                              const juce::String& downloadUrl,
                              const juce::String& filename,
                              juce::int64 sizeBytes,
                              bool installerMode = false);
    void applyDownloadedUpdate(const juce::File& downloadedFile,
                               const juce::String& targetVersion);
    void runDownloadedInstaller(const juce::File& installerFile);
    // A newer build found at launch is announced, never installed silently:
    // the player sees the version, the release notes and two buttons. The
    // prompt waits for the main screen so it cannot cover the sign-in card.
    void showUpdatePrompt();
    bool updateCheckStarted = false;
    bool updateDownloadInProgress = false;
    bool updatePromptPending = false;
    juce::String updatePromptVersion;
    juce::String updatePromptUrl;
    juce::String updatePromptFilename;
    juce::String updatePromptNotes;
    juce::int64 updatePromptSize = 0;
    bool updatePromptInstallerMode = false;
    // Last hit/miss counts the overlay painted. The live accuracy and the
    // judgement strip live in paintOverlay(), which a label repaint does not
    // invalidate, so the timer compares these and repaints on a change.
    int hudShownHits = -1;
    int hudShownMisses = -1;
    // Areas of the gameplay HUD whose painted values change during a run.
    // resized() fills them; the timer repaints only these, so a hit never
    // forces a full-window (and tablature) repaint.
    juce::Rectangle<int> hudStatsBounds;
    juce::Rectangle<int> hudAccuracyBounds;
    // All server communication is asynchronous: requests run on this client's
    // worker threads and their results arrive on the message thread.
    ApiClient apiClient;
    // Central UI sound system: registry + manifest + realtime-safe mixer. It
    // never touches the guitar/note path; its voices are added at the end of
    // the audio callback.
    UiSounds uiSounds;
    void loadUiSoundPreferences();
    // REST client for /api/v1/duels. It shares ApiClient's worker pool, so no
    // HTTP call can ever touch the message thread.
    DuelClient duelClient{ apiClient };
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
                            int requestId,
                            bool silent = false);
    // Clears the local session and returns to the sign-in screen. Used by the
    // LOG OUT button and by the account window.
    void logoutCurrentUser();
    void startOnlineGameSession();
    void submitOnlineGameResult();
    // Debug helper: writes stem/MIDI loading details to
    // %APPDATA%/GuitarBattle/audio-debug.log. Never called from the audio thread.
    void appendAudioDebugLog(const juce::String& line) const;
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
        // Time signature published by the server. 0 means "unknown" (local
        // songs, or a server that predates the field); loadSongMidi() then
        // falls back to the MIDI meta event and finally to 4/4.
        int timeSignatureNumerator = 0;
        int timeSignatureDenominator = 0;
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
        juce::String genre;      // server genre, free text ("Power metal", "Jazz")
        juce::String previewUrl; // full-song MP3 for the in-app preview, optional
        double bpm = 120.0;
        int difficulty = 1;      // server difficulty, 1-10
        int version = 0;
        bool online = false;
        bool downloaded = false; // assets present in the local cache
        // ``is_published`` from the server (additive field). Older servers do
        // not send it, so the default keeps every row a published song.
        bool published = true;
        // Time signature from the server; 0 keeps the 4/4 default for older
        // servers and for rows that have not been downloaded yet.
        int timeSignatureNumerator = 0;
        int timeSignatureDenominator = 0;

        // Stage/opponent visuals assigned to this song in the admin panel.
        // The client downloads them from the server like the song assets, so
        // no duel artwork is bundled into the executable.
        juce::String characterName;
        juce::String characterSheetUrl;  // origin-relative /media/characters/<uuid>.png
        juce::String stageName;
        juce::String stageBackgroundUrl; // origin-relative /media/stages/<uuid>.png
    };

    // Every catalogue song is playable: access no longer depends on the
    // player level. The row is only dimmed when it is not selected.
    //
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
    void refreshOnlineBestScores();    void rebuildCatalogRows(const juce::var& serverItems);
    void updateCatalogRows();
    void selectCatalogSong(int index);
    // Keeps the START button in sync with the catalogue selection.
    void updateStartButtonState();
    void startSelectedSong();
    void downloadOnlineSong(const juce::String& slug,
                            const juce::String& serverId,
                            bool startAfterDownload,
                            bool forDuel = false);
    SongEntry* findOnlineLibraryEntry(const juce::String& slug);
    int getBestScoreForSong(const juce::String& slug, const juce::String& title);

    juce::Label gameSessionLabel;

    juce::Label pitchLabel;

    juce::Label comboLabel;

    juce::Label beatLabel;

    juce::Label barBeatLabel;

    juce::Label bpmLabel;

    juce::Label tempoLabel;

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

    // Shown only when the tuner was opened from an Online room, so the player
    // can return to the room without closing and reopening it.
    juce::TextButton tunerReturnButton{ "BACK TO LOBBY" };
    bool tunerReturnToLobby = false;
    // Preserved while the room dialog is hidden for the tuner; if the dialog
    // was torn down, this lets us reopen the same server lobby.
    juce::String tunerLobbyId;

    // Geometry of the two full-width tuner blocks (STRINGS above LIVE INPUT),
    // shared by paint() and resized() so the drawing and the widgets can never
    // drift apart.
    struct TunerGeometry
    {
        juce::Rectangle<float> stringsPanel, inputPanel;
        float tileTop = 0.0f, tileH = 0.0f, tileW = 0.0f, tileGap = 12.0f;
        int columns = 1, rows = 1, stringSlots = 0;
        float rightColX = 0.0f, rightColW = 0.0f;
        float dialX = 0.0f, dialY = 0.0f, dialRadius = 0.0f;
    };
    TunerGeometry computeTunerGeometry() const;

    // Open-string tuning selected on the tuner screen. The same value is used
    // by GuitarTuner (targets) and MidiParser (tab positions), so the tuner
    // and the tabs can never disagree. The instrument may have more or fewer
    // than six strings, and a slot can be marked as empty.
    GuitarTuning::Tuning currentTuning = GuitarTuning::standardTuning();
    juce::String currentTuningName{ "STANDARD" };
    juce::ComboBox tuningPresetCombo;
    juce::ComboBox stringCountCombo;
    juce::ComboBox fretCountCombo;
    std::array<juce::ComboBox, GuitarTuning::maxStrings> stringTuningCombos;
    juce::File currentMidiFile;
    bool tuningUiUpdating = false;

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

    // Live monitored guitar. The fader only affects what the player hears:
    // note detection and scoring always read the dry input.
    juce::TextButton liveGuitarButton{ "LIVE GTR ON" };
    juce::Slider liveGuitarVolumeSlider;
    juce::Label liveGuitarVolumeLabel;
    std::atomic<bool> liveGuitarEnabled{ true };
    std::atomic<float> liveGuitarVolume{ 0.5f };
    // The guitar input is monitored only after sign-in: the login/registration
    // screen must play background music alone. Written on the message thread
    // from showAuthScreen, read by the audio callback (realtime-safe).
    std::atomic<bool> guitarMonitoringAllowed{ false };

    juce::TextButton midiBackingButton{ "SOLO GTR OFF" };
    juce::Slider midiBackingVolumeSlider;
    juce::Label midiBackingVolumeLabel;
    std::atomic<bool> midiBackingEnabled{ false };
    std::atomic<float> midiBackingVolume{ 0.22f };

    juce::TextButton midiRhythmGuitarButton{ "RHYTHM GTR ON" };
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

    juce::TextButton metronomeMixerButton{ "METRONOME OFF" };
    juce::Slider metronomeMixerVolumeSlider;
    juce::Label metronomeMixerVolumeLabel;
    std::atomic<bool> metronomeMixerEnabled{ false };
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
    //: Hardware input channel used by voice chat on the current audio device.
    //: -1 = same channel as the guitar input.
    std::atomic<int> preferredMicChannel{ -1 };

    // Device latency in milliseconds, refreshed on the message thread when the
    // device, its buffer or its sample rate changes. The scoring path subtracts
    // the output latency (what the player hears after the chart position) plus
    // the manual timing offset from every detected onset, so a large buffer or
    // a different sample rate does not turn on-time hits into late misses.
    std::atomic<double> deviceInputLatencyMs{ 0.0 };
    std::atomic<double> deviceOutputLatencyMs{ 0.0 };
    std::atomic<int> timingOffsetMs{ 0 };
    std::atomic<int> activeInputChannels{ 0 };
    std::array<int, 32> activeInputHardwareChannels{};
    int currentInputChannel = -1;

    // Sample rate cached in prepareToPlay(); the audio callback must not query
    // the device manager from the realtime thread.
    std::atomic<double> cachedSampleRate{ 0.0 };

    // Lobby voice chat. The manager is declared after the engine so it is
    // destroyed first and can never call a dangling audio callback.
    std::unique_ptr<VoiceChat> voiceChat;
    juce::String activeVoiceLobbyId;

    //==========================================================================
    // Input monitor
    //
    // The audio callback only writes these atomics; the message thread reads
    // them at the UI tick rate. No locks, no allocation, no UI calls from the
    // audio thread.
    std::atomic<float> inputMeterRms{ 0.0f };
    std::atomic<float> inputMeterPeak{ 0.0f };
    std::atomic<int> inputClipEvents{ 0 };
    // Hardware channel actually captured by the last audio block (-1 = none).
    std::atomic<int> inputMeterHardwareChannel{ -1 };
    // Detector sensitivity, 0..1, 0.5 = original thresholds.
    std::atomic<float> inputSensitivity{ 0.5f };
    // Reject singing and other non-guitar input. On by default; the audio
    // settings dialog exposes it because an unusual tone may need to bypass
    // the pluck gate. Read by the pitch-analysis thread only.
    std::atomic<bool> guitarInputOnly{ true };

    // Latest guitar-only gate decision, published by the pitch-analysis
    // thread for the input monitor. 0 = none yet, 1 = pluck accepted,
    // 2 = rejected, 3 = not enough data to decide.
    std::atomic<int> gateDecision{ 0 };
    std::atomic<float> gateAttackMs{ 0.0f };
    std::atomic<float> gateClimbFraction{ 0.0f };
    std::atomic<float> gateRiseDb{ 0.0f };

    // Per-frame analysis values for the diagnostics readout: the RMS the
    // detector actually sees (4096-sample window) and the raw detected pitch.
    std::atomic<float> analysisRms{ 0.0f };
    std::atomic<float> analysisPitch{ 0.0f };

    std::unique_ptr<GuitarInputMonitor> inputMonitor;

    void setInputSensitivity(float normalised);
    void setGuitarInputOnly(bool only);
    void updateInputMonitor();

    //==========================================================================
    // Persistent sign-in
    //
    // The access token stays in memory. The refresh token lives in the Windows
    // Credential Manager and is used once at startup to restore the session.
    juce::String onlineRefreshToken;
    bool sessionRestoreInFlight = false;
    bool sessionOfflineRetryAvailable = false;
    juce::TextButton retryConnectionButton{ "RETRY CONNECTION" };

    void tryRestoreSavedSession();
    void storeRefreshToken(const juce::String& token);
    void clearSavedSession();
    void revokeSavedSession();

    //==========================================================================
    // Preview song and background music
    //
    // One decoded track per role. The audio callback mixes at most one of them
    // (game audio always wins), and the message thread decides which one is
    // allowed through updateMusicState().
    MusicTrackPlayer previewPlayer;
    MusicTrackPlayer backgroundPlayer;
    juce::String previewTrackUrl;
    juce::String backgroundMusicUrl;
    int previewRequestId = 0;
    int backgroundRequestId = 0;
    std::atomic<float> backgroundMusicVolume{ 0.2f };
    std::atomic<bool> backgroundMusicEnabled{ true };
    bool previewButtonPlaying = false;
    juce::TextButton previewButton{ "PREVIEW" };

    static juce::File musicCacheDirectory();
    void preparePreviewForSelectedSong();
    void togglePreviewPlayback();
    void stopPreviewPlayback();
    void fetchBackgroundMusic();
    void updateMusicState();
    void updatePreviewButton();
    void applyBackgroundMusicSettings();
    void setBackgroundMusicVolume(float volume);
    void setBackgroundMusicEnabled(bool enabled);
    void restoreMusicSettings();

    void setPreferredInputChannel(int channel);
    void setPreferredMicChannel(int channel);
    juce::String getVoiceStatusText();

    // Timing compensation. refreshDeviceLatency() runs outside the audio
    // callback (prepareToPlay and device-change notifications); the scoring
    // thread only reads the atomics.
    void refreshDeviceLatency();
    void setTimingOffsetMs(int offsetMs);
    int getTimingOffsetMs() const noexcept
    {
        return timingOffsetMs.load(std::memory_order_relaxed);
    }
    juce::String getLatencyDiagnosticsText() const;
    uint64_t getScoringLatencySamples(double sampleRate) const;
    bool ensureInputChannelEnabled(int hardwareChannel);
    void rebuildGuitarInputChoices();
    void refreshGuitarInputCombo();
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    std::atomic<uint64_t> audioSampleCounter{ 0 };

    std::atomic<double> currentPlayheadBeat{ 0.0 };

    // Beat the player actually hears right now: currentPlayheadBeat is the
    // render position, which reaches the speakers one output latency later.
    // The tablature uses this value so the notes and the music stay aligned
    // with a large buffer.
    std::atomic<double> audiblePlayheadBeat{ 0.0 };

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

    // Meter of the loaded song, normalised by Metronome. Read on the message
    // thread only: the audio thread reads the metronome's own atomics.
    int songSignatureNumerator = 4;
    int songSignatureDenominator = 4;

    // Applies the meter to the metronome and the tablature in one place, so a
    // song load, the daily challenge and the 4/4 default cannot disagree.
    void applySongTimeSignature(int numerator, int denominator);

    // "7/8" for a valid meter, "4/4" when the value is unknown.
    static juce::String formatTimeSignature(int numerator, int denominator);

    //==========================================================================
    // Difficulty / scoring

    Difficulty difficulty = Difficulty::Normal;

    // Published 1-10 difficulty of the song being played. Read by the pitch
    // worker, written when a song starts, so it is an atomic.
    std::atomic<int> songLevel{ 5 };





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
    void setLobbyVoiceChat (bool shouldBeEnabled);
    void setLobbyMicrophoneEnabled (bool shouldBeEnabled);
    void stopLobbyVoiceChat();
    void openLobbyTuner();
    void closeLobbyTuner();

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


    // Pitch analysis window. It is sized from the device sample rate in
    // prepareToPlay() so 44.1, 48, 88.2 and 96 kHz analyse the same ~93 ms of
    // signal (a fixed 4096-sample window shrank to 42 ms at 96 kHz and made
    // low notes unstable). The buffers are allocated once at the maximum size;
    // the active window is published atomically for the audio callback and the
    // analysis thread.
    static constexpr int maxPitchAnalysisSize = 16384;
    std::atomic<int> pitchAnalysisWindow{ 4096 };

    // Accepted pitch window for a detected guitar note. The 21st fret on the
    // high E string is ~1109 Hz and the 24th fret is ~1319 Hz, so the old
    // 1000 Hz ceiling silently rejected the top of the fretboard. The ceiling
    // sits above the highest playable fret and below the second harmonic of
    // the low E string.
    static constexpr double minDetectedPitchHz = 20.0;
    static constexpr double maxDetectedPitchHz = 1500.0;

    PitchDetector pitchDetector;
    GuitarTuner tuner;
    Metronome metronome;
    GameSession gameSession;
    GameplayStats gameplayStats;

    //==========================================================================
    // Guitar Duels
    //
    // The client runs the same rules as the server (DuelEngine). Bot matches
    // are simulated locally: the bot's quality for every chart note is derived
    // from the note index and a match seed, so it always plays the selected
    // song and can be replayed. Online matches will feed the server's
    // authoritative results through the same DuelMatch state.

    std::unique_ptr<DuelMatch> duelMatch;
    juce::SpinLock duelLock;
    std::atomic<bool> duelActive{ false };
    std::atomic<int> duelPlayerHealthMilli{ 100000 };
    std::atomic<int> duelOpponentHealthMilli{ 100000 };
    std::atomic<int> duelLastExchange{ 0 };
    std::atomic<int> duelLastDamageMilli{ 0 };
    std::atomic<double> duelFlashUntilMs{ 0.0 };
    std::atomic<int> duelBotLevelValue{ 0 };

    bool duelPending = false;
    DuelBotLevel duelPendingLevel = DuelBotLevel::Easy;

    //==========================================================================
    // Online duels (server-authoritative)
    //
    // The server freezes the chart and owns HP, damage, counters, XP and Elo.
    // The client only reports (note_index, quality) batches and copies the
    // authoritative snapshot into the same HUD the bot match uses. JUCE 9.0.2
    // has no juce::WebSocket, so a match is polled with GET /duels/{id} about
    // once a second; the REST callbacks arrive on the message thread.
    enum class DuelOnlinePhase
    {
        None,
        Searching,   // POST /duels in flight
        Waiting,     // in the queue / waiting for the opponent to become ready
        Playing,
        Finishing,   // chart ended locally, waiting for the server result
        Finished,
        Preparing    // downloading / preloading the song before `ready`
    };

    DuelOnlinePhase duelOnlinePhase = DuelOnlinePhase::None;
    DuelMatchSnapshot duelOnlineMatch;
    juce::String duelOnlineMatchId;
    juce::String duelOnlineSongId;
    juce::String duelOnlineSongSlug;
    // Room (lobby) the match belongs to; empty for the quick queue and bots.
    // When known, the client never shows the post-download "Start the duel?"
    // prompt (the room already asked) and an abort returns to the room.
    juce::String duelOnlineLobbyId;
    // True when this match was confirmed inside the room window. It is the
    // authoritative "already confirmed" marker: the queue-style ready prompt
    // must never appear for a room match even when the lobby id is lost during
    // the handoff (a state reset, a join snapshot without lobby_id). Set by the
    // room start callback and as soon as the server reports a lobby id.
    bool duelOnlineConfirmedInRoom = false;
    // Last match that already reached its result card on this side. A new start
    // must never reuse it: a stale room snapshot from the previous round used
    // to rejoin the finished match instead of the fresh one.
    juce::String duelOnlineFinishedMatchId;
    // Slug of the chart actually loaded by preloadOnlineDuelSong(), so a ready
    // flag from a previous match can never be credited to the new song.
    juce::String duelOnlinePreloadedSlug;
    juce::String duelOnlineOpponentName;
    // Sheet URLs currently applied to the stage, so a poll does not restart the
    // image downloads on every snapshot.
    juce::String onlineCharactersAppliedKey;
    int duelOnlineExpectedNotes = 0;
    int duelOnlineGeneration = 0;
    int duelOnlinePollFailures = 0;
    int duelOnlineFinishAttempts = 0;

    bool duelOnlinePlaybackPending = false;
    bool duelOnlineChartReady = false;
    // A room match may arrive with a missing or stale local chart. The first
    // preload failure triggers one forced re-download (with the progress
    // dialog) before the match is cancelled.
    bool duelOnlineChartReloadAttempted = false;
    bool duelOnlineStartAborted = false;
    bool duelStageResultActive = false;
    bool duelOnlinePollInFlight = false;
    bool duelOnlineReadyInFlight = false;
    // Both players must explicitly confirm after their stems are ready; the
    // server countdown only starts when both confirmations arrive.
    bool duelOnlineReadyConfirmed = false;
    bool duelOnlineReadyPromptShown = false;
    bool duelOnlineReadyPromptClosing = false;
    juce::Component::SafePointer<juce::AlertWindow> duelOnlineReadyDialog;
    bool duelOnlineEventInFlight = false;
    bool duelOnlineFinishRequested = false;
    bool duelOnlineFinishSent = false;
    bool duelOnlineFinishInFlight = false;
    bool duelOnlineResultInFlight = false;
    bool duelOnlineEventSendingStopped = false;

    // Pre-duel preparation: the stem download and the chart/stem preload run
    // before POST /ready, so the 3-second server countdown is never spent on
    // decoding audio or on a network transfer.
    bool duelOnlineDownloadInProgress = false;
    bool duelOnlineDownloadJoinAfter = false;
    bool duelOnlinePreloadInProgress = false;
    juce::String duelOnlineDownloadMatchId;
    juce::String duelOnlineDownloadSlug;
    int duelOnlineDownloadPercentShown = -1;

    double duelOnlineNextPollMs = 0.0;
    double duelOnlineNextEventMs = 0.0;
    double duelOnlineNextReadyMs = 0.0;
    double duelOnlineSnapshotAtMs = 0.0;
    // Half of the smallest measured poll round trip: the server computed
    // ``starts_in_ms`` one one-way latency before the snapshot arrived, so the
    // local beat zero must be pulled that much earlier to sit on the server's
    // timeline. Without it the transport started ~30-100 ms late.
    double duelOnlineOneWayMs = 0.0;
    void noteOnlineDuelRoundTrip(double sentMs);

    // Read by the pitch-analysis thread (note resolved) and the message thread
    // (auto miss + timer drain). The event queue itself is guarded by duelLock.
    std::atomic<bool> duelOnlineActive{ false };
    std::atomic<int> duelOnlineNoteCount{ 0 };
    int duelOnlineLastQueuedIndex = -1;
    std::deque<DuelEventOut> duelOnlinePendingEvents;
    std::vector<DuelEventOut> duelOnlineInFlightEvents;

    // Modal "SEARCHING FOR OPPONENT..." window with a CANCEL button.
    juce::Component::SafePointer<juce::AlertWindow> duelOnlineDialog;

    void requestDuelWithBot(int botLevel);

    // Aborts a bot duel that could not prepare its chart (missing assets,
    // locked song) and keeps the previous result card usable.
    void abortPendingDuelStart(const juce::String& reason);
    void showDuelModeMenu();
    void paintDuelOverlay(juce::Graphics& g);

    // Bot duel result rendered inside the big session card (Victory / Defeat),
    // instead of a second small card on top of it.
    void paintDuelResultInSessionCard (juce::Graphics& g,
                                       juce::Rectangle<float> card);
    // Campaign: the character the player faces on the current stage's song
    // (the sprite assigned to it in the admin catalogue). Empty outside a
    // campaign or when the song has no character.
    juce::String campaignOpponentCharacterName() const;
    // Online duel result: the same campaign-style card, fed by the server's
    // authoritative participant snapshot.
    void paintOnlineDuelResultInSessionCard (juce::Graphics& g,
                                             juce::Rectangle<float> card);
    bool duelResultCardInSession = false;
    void repaintDuelHud();
    void refreshDuelLobby();
    void joinOnlineDuelRoom(const juce::String& matchId,
                            const juce::String& lobbyId = {});
    // Aborts of a room match go back to the room window instead of dead-ending:
    // the server rolled the room back to `waiting` (song and chat kept).
    void returnOnlineDuelToRoom(const juce::String& lobbyId,
                                const juce::String& matchId);
    // Result screen of a room match: GO TO LOBBY keeps the room (chat and song
    // preserved by the server), LEAVE LOBBY drops the room and lands on the
    // Online section (the lobby list, refreshed).
    void goToOnlineDuelLobby();
    void leaveOnlineDuelLobby();
    bool isOnlineRoomMatch() const
    {
        return duelOnlineLobbyId.isNotEmpty() || duelOnlineConfirmedInRoom;
    }

    // Online duel rooms (create with a password, shared chat, song picker).
    void openCreateDuelRoomDialog();
    void openOnlineDuelRoom (const juce::String& lobbyId);
    void joinDuelLobbyRoom (const juce::String& lobbyId,
                            const juce::String& password);
    std::unique_ptr<juce::DialogWindow> duelRoomDialog;
    std::unique_ptr<juce::DialogWindow> createRoomDialog;
    juce::Component::SafePointer<juce::DialogWindow> onlineDuelRoomDialog;

    // Hub navigation and sections. Reuse the existing screens/components; the
    // sidebar only changes which of them is visible.
    void setSection(AppSection section);
    AppSection getCurrentSection() const noexcept { return currentSection; }
    int sectionSidebarWidth() const;
    void ensureProfileSection();
    void ensureSettingsSection();
    void ensureFriendsSection();
    void ensureMessagesSection();
    void ensureVstSection();
    void refreshCampaignSection();
    // Re-reads /campaigns. ``silent`` keeps the current list interactive while
    // the request is in flight (used for the periodic background refresh).
    void refreshServerCampaigns (bool silent = false);
    // Tower card artwork, downloaded once per URL and kept for the session.
    std::map<juce::String, juce::Image> campaignCoverCache;
    std::map<juce::String, bool> campaignCoverInFlight;
    void requestCampaignCover (const juce::String& campaignId,
                               const juce::String& url);
    void applyServerCampaigns();
    void selectCampaign(const juce::String& campaignId);
    const CampaignView* findServerCampaign(const juce::String& campaignId) const;
    juce::String campaignBestKey() const;
    void startCampaignStage (bool skipTuner = false);
    void resetCampaignProgress();
    int campaignStageCount() const noexcept;
    void openStandaloneTuner();
    void leaveStandaloneTuner();
    void returnToHubSection();

    std::unique_ptr<DuelLobbyComponent> duelLobby;
    bool duelLobbyInFlight = false;
    int duelLobbyRefreshTicks = 0;
    int campaignRefreshTicks = 0;
    void paintOnlineDuelOverlay(juce::Graphics& g);
    void beginPendingDuelIfNeeded();
    void onDuelNoteResolved(int noteIndex, int hitQuality);
    void finishDuel();
    void resetDuelState();

    // Online duel flow.
    void updateOnlineDuel();
    void startOnlineDuel();
    void cancelOnlineDuel(bool sendLeave);
    void clearOnlineDuelState();
    void showOnlineDuelDialog(const juce::String& message);
    void closeOnlineDuelDialog();
    void pollOnlineDuel();
    void sendOnlineDuelReady();
    // Readiness gate: the song's cached chart AND audio stems must be complete
    // on this machine before `ready` is reported. The server starts the shared
    // countdown as soon as both sides confirm, so a client that confirms during
    // a download makes the song start while the other side (or itself) cannot
    // play it yet.
    bool onlineDuelContentComplete(const juce::String& slug);
    void promptOnlineDuelReady();
    void applyOnlineDuelState(const DuelMatchSnapshot& state);
    void applyOnlineDuelExchangeFeedback(const DuelEventsReply& reply);
    void handleOnlineDuelReplyError(const DuelReply& reply, const juce::String& stage);
    void offerBotAfterNoOpponent();
    void abortOnlineDuelForChartMismatch(int localNotes);
    void queueOnlineDuelNote(int noteIndex, int hitQuality);
    void flushOnlineDuelEvents();
    void sendOnlineDuelFinish();
    void requestOnlineDuelResult();
    void finishOnlineDuelFromServer();
    void enterOnlineDuelResultView();
    void beginOnlineDuelPlayback();
    void startOnlineDuelTimeline();
    void stopOnlineDuelTransport();
    void updateOnlineDuelHud();
    bool isDuelStageActive() const;
    juce::Rectangle<int> getDuelHealthStripBounds() const;

    // Downloads the stage background and opponent spritesheet assigned to the
    // song in the admin panel (the same server media as songs) and applies
    // them to the pixel-art stage. Images are cached by URL and downloaded on
    // the worker pool; no duel artwork is embedded in the client.
    void applySongStageVisuals(const juce::String& songSlug,
                               const juce::String& opponentSheetUrl = {});
    // Online: the stage shows each player's own character from the profile
    // snapshot (the opponent's sprite is the one they picked, not the song's).
    void applyOnlineDuelCharacters();
    void requestDuelImage(const juce::String& relativeUrl,
                          bool background,
                          int generation);
    std::map<juce::String, juce::MemoryBlock> duelImageCache;
    int duelVisualGeneration = 0;

    // Pre-duel preparation (tempo, download, chart/stem preload).
    bool prepareDuelTempo();
    bool preloadOnlineDuelSong(const juce::String& slug);
    void beginOnlineDuelPreload(const juce::String& slug, bool joinAfter);
    void beginOnlineDuelSongPreparation(const juce::String& slug,
                                        const juce::String& serverId,
                                        bool joinAfterDownload,
                                        const juce::String& joinMatchId);
    void completeOnlineDuelSongPreparation(bool downloadOk,
                                           const juce::String& slug);
    void startOnlineDuelCreate();
    void updateOnlineDuelPreparationDialog();

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

    // Tuning selector shown at the tuner stage. The presets live in
    // MainComponentUI.cpp; changing any string combo switches to Custom.
    void initialiseTuningSelector();
    void syncTuningUi();
    void applyPresetTuning(int presetItemId);
    void applyStringCountFromUi();
    void applyFretCountFromUi();
    void applyCustomTuningFromUi();
    void setCurrentTuning(const GuitarTuning::Tuning& tuning,
                          const juce::String& name,
                          bool persist);
    void rebuildTablatureForTuning();
    static int presetItemIdForTuning(const GuitarTuning::Tuning& tuning);
    static GuitarTuning::Tuning presetNotes(int presetIndex);
    static const char* presetName(int presetIndex);

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
    // Practice mode
    //
    // Training run: no score, no server session, no results. The transport
    // freezes at every target note until the player performs it correctly.

    bool practiceMode = false;
    std::atomic<bool> practiceActive{ false };
    std::atomic<bool> practiceHoldActive{ false };
    std::atomic<uint64_t> practiceHoldStartSample{ 0 };

    // Sample-accurate stop scheduled by the message thread and executed by
    // the audio callback. The audio thread stops the musical layers on the
    // exact target sample, so the freeze does not depend on the 30 Hz UI
    // timer or on the device output latency.
    std::atomic<uint64_t> practiceGateSample{ 0 };
    std::atomic<bool> practiceGateArmed{ false };

    std::atomic<double> practiceLastPitch{ 0.0 };
    // End sample of the analysis window that produced practiceLastPitch. The
    // message thread uses it to ignore pitches captured before the current
    // freeze, so a ringing note from earlier cannot release the hold.
    std::atomic<uint64_t> practicePitchSample{ 0 };
    uint64_t practiceLastOnsetSample = 0;
    // Sample at which the previous hold was released. A ringing note from
    // before the resume must not release the next target.
    uint64_t practiceResumeSample = 0;
    // True while the hold waits for the remaining notes of a chord. A strum
    // produces one onset for several pitches, so the live pitch may advance a
    // chord note without a fresh attack. Every chord note has a different
    // pitch, so a ringing note cannot skip one.
    bool practiceChordHold = false;
    bool practiceHoldUiApplied = false;
    bool practiceCompleted = false;

    juce::TextButton practiceModeButton{ "STUDY" };

    // Guitar Duels entry on the song screen.
    juce::TextButton duelModeButton{ "DUEL" };

    void setPracticeMode(bool enabled);
    void beginPracticeRun();
    void updatePracticeMode();
    void schedulePracticeFreeze();
    void applyPracticeHoldUi();
    void releasePracticeTransport();
    void evaluatePracticeNote();
    void advancePracticeTarget(int index);
    void finishPracticeSession();

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
    // VST3 monitor plugin

    // Processes only the monitored guitar output. The audio callback fills the
    // pitch-analysis buffers from the dry input before this insert runs, so
    // note detection and scoring never see the processed signal.
    VstHost vstHost;

    //==========================================================================
    // Frontend extension point. The frontend agent owns MainComponentUi.inc and
    // may add UI widgets/state/helpers there without editing this header.
#include "UI/MainComponentUi.inc"

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(
        MainComponent)
};
