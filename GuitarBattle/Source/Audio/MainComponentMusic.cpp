#include "../MainComponent.h"

#include <memory>
#include <vector>

// Preview song and background music.
//
// Two decoded tracks are mixed by the existing audio callback (see
// MainComponentAudio.cpp). This file owns everything that happens on the
// message thread: fetching URLs, downloading and decoding on a worker thread,
// and the single priority rule that decides which track may be audible.

namespace
{
    // A cached file is reused only when it was downloaded from the same URL,
    // so replacing a preview on the server invalidates the local copy.
    bool ensureCachedTrack(const juce::String& url, const juce::File& file)
    {
        if (url.isEmpty())
            return false;

        auto marker = file.withFileExtension("url");

        if (file.existsAsFile() && marker.existsAsFile()
            && marker.loadFileAsString().trim() == url)
            return true;

        if (ApiClient::downloadToFile(url, file, 20000) <= 0)
        {
            file.deleteFile();
            return false;
        }

        marker.replaceWithText(url);
        return true;
    }
}

juce::File MainComponent::musicCacheDirectory()
{
    auto root = juce::File::getSpecialLocation(
        juce::File::userApplicationDataDirectory).getChildFile("GuitarBattle");

    auto dir = root.getChildFile("Music");
    dir.createDirectory();
    return dir;
}

void MainComponent::restoreMusicSettings()
{
    if (auto* settings = appProperties.getUserSettings())
    {
        backgroundMusicVolume.store(
            static_cast<float>(juce::jlimit(0.0, 1.0,
                settings->getDoubleValue("BackgroundMusicVolume", 0.6))),
            std::memory_order_relaxed);

        backgroundMusicEnabled.store(
            settings->getBoolValue("BackgroundMusicEnabled", true),
            std::memory_order_relaxed);
    }

    backgroundPlayer.setLooping(true);
    backgroundPlayer.setGain(
        backgroundMusicEnabled.load(std::memory_order_relaxed)
            ? backgroundMusicVolume.load(std::memory_order_relaxed)
            : 0.0f);
}

void MainComponent::setBackgroundMusicVolume(float volume)
{
    volume = juce::jlimit(0.0f, 1.0f, volume);
    backgroundMusicVolume.store(volume, std::memory_order_relaxed);

    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue("BackgroundMusicVolume", static_cast<double>(volume));
        settings->saveIfNeeded();
    }

    applyBackgroundMusicSettings();
}

void MainComponent::setBackgroundMusicEnabled(bool enabled)
{
    backgroundMusicEnabled.store(enabled, std::memory_order_relaxed);

    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue("BackgroundMusicEnabled", enabled);
        settings->saveIfNeeded();
    }

    applyBackgroundMusicSettings();
}

void MainComponent::applyBackgroundMusicSettings()
{
    const bool enabled = backgroundMusicEnabled.load(std::memory_order_relaxed);

    backgroundPlayer.setGain(enabled
        ? backgroundMusicVolume.load(std::memory_order_relaxed)
        : 0.0f);

    updateMusicState();
}

void MainComponent::fetchBackgroundMusic()
{
    const int requestId = ++backgroundRequestId;

    // Public endpoint: no token needed, the player is not signed in yet when
    // the application starts.
    apiClient.getJson("/app/background-music", {}, 5000,
        [safeThis = juce::Component::SafePointer<MainComponent>(this), requestId]
        (ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr || requestId != self->backgroundRequestId)
                return;

            if (! response.transportOk || response.statusCode != 200)
                return;

            const auto url = response.json.getProperty("url", juce::var()).toString();

            if (url.isEmpty())
            {
                self->backgroundMusicUrl.clear();
                self->backgroundPlayer.clearTrack();
                self->updateMusicState();
                return;
            }

            self->backgroundMusicUrl = url;

            auto result = std::make_shared<std::shared_ptr<MusicTrackPlayer::Track>>();
            const auto target = musicCacheDirectory().getChildFile("background.mp3");

            self->apiClient.runAsync(
                [url, target, result]
                {
                    if (! ensureCachedTrack(url, target))
                        return;

                    *result = MusicTrackPlayer::decodeFile(target);
                },
                [safeThis, requestId, result]
                {
                    auto* window = safeThis.getComponent();

                    if (window == nullptr || requestId != window->backgroundRequestId)
                        return;

                    if (*result == nullptr)
                    {
                        window->backgroundPlayer.clearTrack();
                    }
                    else
                    {
                        window->backgroundPlayer.setTrack(*result);
                        window->backgroundPlayer.setLooping(true);
                        window->backgroundPlayer.setGain(
                            window->backgroundMusicEnabled.load(std::memory_order_relaxed)
                                ? window->backgroundMusicVolume.load(std::memory_order_relaxed)
                                : 0.0f);
                    }

                    window->updateMusicState();
                });
        });
}

void MainComponent::preparePreviewForSelectedSong()
{
    // Any new selection silences the previous preview immediately, even if the
    // new one has no file or is still downloading.
    previewPlayer.stop();

    const int requestId = ++previewRequestId;
    previewTrackUrl.clear();

    if (gameState != GameState::SongSelect
        || ! juce::isPositiveAndBelow(selectedCatalogIndex,
                                      static_cast<int>(catalogSongs.size())))
    {
        updateMusicState();
        updatePreviewButton();
        return;
    }

    const auto& song = catalogSongs[static_cast<size_t>(selectedCatalogIndex)];

    if (song.previewUrl.isEmpty())
    {
        previewPlayer.clearTrack();
        updateMusicState();
        updatePreviewButton();
        return;
    }

    previewTrackUrl = song.previewUrl;

    auto result = std::make_shared<std::shared_ptr<MusicTrackPlayer::Track>>();
    const auto url = song.previewUrl;
    const auto slug = song.slug;
    const auto target = musicCacheDirectory().getChildFile("preview-" + slug + ".mp3");

    apiClient.runAsync(
        [url, target, result]
        {
            if (! ensureCachedTrack(url, target))
                return;

            *result = MusicTrackPlayer::decodeFile(target);
        },
        [safeThis = juce::Component::SafePointer<MainComponent>(this),
         requestId, result]()
        {
            auto* self = safeThis.getComponent();

            // A late answer for an old selection (or after leaving the screen)
            // must never start audio.
            if (self == nullptr
                || requestId != self->previewRequestId
                || self->gameState != GameState::SongSelect)
                return;

            if (*result == nullptr)
            {
                self->previewPlayer.clearTrack();
                self->updateMusicState();
                self->updatePreviewButton();
                return;
            }

            self->previewPlayer.setTrack(*result);
            self->previewPlayer.setLooping(false);
            self->previewPlayer.setGain(1.0f);
            self->previewPlayer.playFromStart();

            self->updateMusicState();
            self->updatePreviewButton();
        });
}

void MainComponent::togglePreviewPlayback()
{
    if (previewPlayer.isPlaying())
    {
        previewPlayer.stop();
    }
    else if (previewPlayer.hasTrack())
    {
        previewPlayer.playFromStart();
    }
    else
    {
        preparePreviewForSelectedSong();
        return;
    }

    updateMusicState();
    updatePreviewButton();
}

void MainComponent::stopPreviewPlayback()
{
    previewPlayer.stop();
    updateMusicState();
    updatePreviewButton();
}

void MainComponent::updateMusicState()
{
    // Single source of truth for audio priority:
    // game audio > preview song > background music.
    const bool gameAudioActive =
        gameState == GameState::Playing
        || gameState == GameState::Paused
        || gameState == GameState::Ready
        || gameState == GameState::Preparing;

    if (gameAudioActive)
    {
        if (previewPlayer.isPlaying())
            previewPlayer.stop();

        if (backgroundPlayer.isPlaying())
            backgroundPlayer.stop();

        updatePreviewButton();
        return;
    }

    if (previewPlayer.isPlaying())
    {
        if (backgroundPlayer.isPlaying())
            backgroundPlayer.stop();

        updatePreviewButton();
        return;
    }

    const bool backgroundAllowed =
        (gameState == GameState::SongSelect || gameState == GameState::Finished)
        && backgroundMusicEnabled.load(std::memory_order_relaxed)
        && backgroundPlayer.hasTrack();

    if (backgroundAllowed)
    {
        if (! backgroundPlayer.isPlaying())
            backgroundPlayer.playFromStart();
    }
    else if (backgroundPlayer.isPlaying())
    {
        backgroundPlayer.stop();
    }

    updatePreviewButton();
}

void MainComponent::updatePreviewButton()
{
    const bool hasTrack = previewPlayer.hasTrack();
    const bool playing = previewPlayer.isPlaying();

    const juce::String text = playing
        ? juce::String("STOP PREVIEW")
        : (hasTrack ? juce::String("PLAY PREVIEW") : juce::String("NO PREVIEW"));

    if (previewButton.getButtonText() != text)
        previewButton.setButtonText(text);

    if (previewButton.isEnabled() != hasTrack)
        previewButton.setEnabled(hasTrack);

    if (previewButtonPlaying != playing)
    {
        previewButtonPlaying = playing;

        previewButton.setColour(juce::TextButton::buttonColourId,
                                playing ? UiColors::accentStrong
                                        : UiColors::surface2);
        previewButton.setColour(juce::TextButton::textColourOffId,
                                playing ? UiColors::onAccent : UiColors::text);
    }
}
