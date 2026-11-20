#include "../MainComponent.h"
#include "../UI/HomeView.h"
#include "../UI/HubHeader.h"

#include <functional>
#include <memory>

// Admin-uploaded artwork for the desktop client:
//
//   GET /site/config  -> { hero_battle_url, hero_duel_url, default_avatar_url }
//   GET /auth/me      -> avatar_image_url of the signed-in player
//   leaderboard rows  -> avatar_image_url of every listed player
//
// Every request and every image download runs on ApiClient's worker pool; the
// results are applied on the message thread only. An empty URL or an invalid
// image keeps the built-in fallback (procedural hero art, initial letter), so a
// server without these fields looks exactly as before.

namespace
{
    juce::String absoluteServerUrl (const juce::String& url)
    {
        return url.startsWith ("http") ? url
                                       : juce::String (ApiClient::serverOrigin) + url;
    }
}

void MainComponent::refreshSiteArtwork()
{
    // One request at a time: entering HOME repeatedly must not flood the pool.
    if (siteArtworkRequestInFlight)
        return;

    siteArtworkRequestInFlight = true;
    juce::Component::SafePointer<MainComponent> safeThis (this);

    apiClient.getJson ("/site/config", {}, 5000,
        [safeThis] (ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->siteArtworkRequestInFlight = false;

            if (! response.transportOk || ! response.json.isObject())
                return; // older server: keep the procedural artwork

            const auto battleUrl = response.json
                                       .getProperty ("hero_battle_url", juce::var())
                                       .toString();
            const auto duelUrl = response.json
                                     .getProperty ("hero_duel_url", juce::var())
                                     .toString();
            const auto avatarUrl = response.json
                                       .getProperty ("default_avatar_url", juce::var())
                                       .toString();

            self->applySiteArtwork (battleUrl, duelUrl, avatarUrl);
        });
}

void MainComponent::applySiteArtwork (const juce::String& battleUrl,
                                      const juce::String& duelUrl,
                                      const juce::String& newDefaultAvatarUrl)
{
    heroBattleUrl = battleUrl;
    heroDuelUrl = duelUrl;
    defaultAvatarUrl = newDefaultAvatarUrl;

    // A removed image clears the cached picture, so the next paint returns to
    // the procedural fallback instead of the previous upload.
    if (heroBattleUrl.isEmpty())
        heroBattleImage = {};

    if (heroDuelUrl.isEmpty())
        heroDuelImage = {};

    if (defaultAvatarUrl.isEmpty())
        defaultAvatarImage = {};

    juce::Component::SafePointer<MainComponent> safeThis (this);

    if (heroBattleUrl.isNotEmpty())
        loadArtworkImage (heroBattleUrl,
                          [safeThis] (const juce::Image& image)
                          {
                              if (auto* self = safeThis.getComponent())
                              {
                                  self->heroBattleImage = image;

                                  if (self->homeView != nullptr)
                                      self->homeView->setHeroBackgrounds (
                                          self->heroBattleImage,
                                          self->heroDuelImage);
                              }
                          });

    if (heroDuelUrl.isNotEmpty())
        loadArtworkImage (heroDuelUrl,
                          [safeThis] (const juce::Image& image)
                          {
                              if (auto* self = safeThis.getComponent())
                              {
                                  self->heroDuelImage = image;

                                  if (self->homeView != nullptr)
                                      self->homeView->setHeroBackgrounds (
                                          self->heroBattleImage,
                                          self->heroDuelImage);
                              }
                          });

    if (defaultAvatarUrl.isNotEmpty())
        loadArtworkImage (defaultAvatarUrl,
                          [safeThis] (const juce::Image& image)
                          {
                              if (auto* self = safeThis.getComponent())
                              {
                                  self->defaultAvatarImage = image;
                                  // The default is the fallback of every
                                  // player without an own picture.
                                  self->applyOwnAvatar();
                              }
                          });
    else
        applyOwnAvatar();

    // Applying the current state unconditionally also covers a slot the admin
    // just cleared: the member images are empty again and HomeView returns to
    // its procedural artwork.
    if (homeView != nullptr)
        homeView->setHeroBackgrounds (heroBattleImage, heroDuelImage);
}

void MainComponent::loadArtworkImage (
    const juce::String& url,
    std::function<void (const juce::Image&)> onLoaded)
{
    if (url.isEmpty())
    {
        if (onLoaded != nullptr)
            onLoaded (juce::Image());

        return;
    }

    const auto cached = artworkImageCache.find (url);

    if (cached != artworkImageCache.end())
    {
        if (onLoaded != nullptr)
            onLoaded (cached->second);

        return;
    }

    // A download of the same URL is already running: its callback owns the
    // result, so a duplicate request is simply ignored.
    if (artworkImageInFlight.count (url) > 0)
        return;

    artworkImageInFlight[url] = true;

    const auto absolute = absoluteServerUrl (url);
    auto data = std::make_shared<juce::MemoryBlock>();
    auto image = std::make_shared<juce::Image>();
    juce::Component::SafePointer<MainComponent> safeThis (this);

    // Worker pool: download and decode off the UI thread, exactly like the
    // tower covers.
    apiClient.runAsync ([data, image, absolute]
                        {
                            *data = ApiClient::download (absolute, 15000);

                            if (data->getSize() > 0)
                                *image = juce::ImageFileFormat::loadFrom (
                                    data->getData(), data->getSize());
                        },
                        [safeThis, url, image, onLoaded]
                        {
                            auto* self = safeThis.getComponent();

                            if (self == nullptr)
                                return;

                            self->artworkImageInFlight.erase (url);

                            if (! image->isValid())
                                return;

                            self->artworkImageCache[url] = *image;

                            if (onLoaded != nullptr)
                                onLoaded (*image);
                        });
}

void MainComponent::setOwnAvatarUrl (const juce::String& url)
{
    ownAvatarUrl = url;
    juce::Component::SafePointer<MainComponent> safeThis (this);

    if (url.isEmpty())
    {
        applyOwnAvatar();
        return;
    }

    loadArtworkImage (url,
                      [safeThis] (const juce::Image&)
                      {
                          if (auto* self = safeThis.getComponent())
                              self->applyOwnAvatar();
                      });
}

void MainComponent::applyOwnAvatar()
{
    if (hubHeader == nullptr)
        return;

    juce::Image image;

    if (ownAvatarUrl.isNotEmpty())
    {
        const auto cached = artworkImageCache.find (ownAvatarUrl);

        // Still downloading: keep whatever the chip shows now instead of
        // flashing the initial letter.
        if (cached == artworkImageCache.end())
            return;

        image = cached->second;
    }

    // The administrator default is the second fallback; an invalid image makes
    // the header draw the first letter of the username.
    if (! image.isValid())
        image = defaultAvatarImage;

    hubHeader->setAvatarImage (image);
}

void MainComponent::registerAvatarUrl (const juce::String& userId,
                                       const juce::String& url)
{
    if (userId.isEmpty() || url.isEmpty())
        return;

    avatarUrlByUser[userId] = url;

    juce::Component::SafePointer<MainComponent> safeThis (this);
    loadArtworkImage (url,
                      [safeThis, userId] (const juce::Image&)
                      {
                          auto* self = safeThis.getComponent();

                          if (self == nullptr)
                              return;

                          // The chip of the signed-in player has its own
                          // fallback chain.
                          if (userId == self->onlineUserId)
                              self->applyOwnAvatar();
                      });
}

juce::Image MainComponent::avatarFor (const juce::String& userId) const
{
    if (userId.isEmpty())
        return {};

    const auto url = avatarUrlByUser.find (userId);

    if (url == avatarUrlByUser.end())
        return defaultAvatarImage;

    const auto image = artworkImageCache.find (url->second);

    if (image != artworkImageCache.end())
        return image->second;

    return defaultAvatarImage;
}
