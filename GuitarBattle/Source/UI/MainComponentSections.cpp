#include "../MainComponent.h"

#include "CampaignPanel.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    juce::String campaignBotName (int level)
    {
        return level <= 0 ? juce::String ("RUSTY")
                          : (level >= 2 ? juce::String ("VOLT")
                                        : juce::String ("MIDNIGHT"));
    }


}

int MainComponent::sectionSidebarWidth() const
{
    if (showAuthScreen)
        return 0;

    const auto state = gameState.load (std::memory_order_relaxed);

    // The in-game tuner (entered from a song/duel) still takes the whole
    // window; the hub Tuner section keeps the sidebar like every other tab.
    if (state == GameState::Tuner)
    {
        if (! (tunerStandalone && currentSection == AppSection::Tuner))
            return 0;
    }

    if (state != GameState::SongSelect && state != GameState::Tuner)
        return 0;

    const int width = getWidth();

    // Proportional like the design reference (~13.5% of the window), with hard
    // limits so the labels never look lost and the content keeps its room.
    return juce::jlimit (176, 224, juce::roundToInt (static_cast<float> (width) * 0.135f));
}

int MainComponent::campaignStageCount() const noexcept
{
    const auto* campaign = findServerCampaign (selectedCampaignId);
    return campaign != nullptr ? static_cast<int> (campaign->stages.size()) : 0;
}

const CampaignView* MainComponent::findServerCampaign (const juce::String& campaignId) const
{
    for (const auto& campaign : serverCampaigns)
        if (campaign.id == campaignId)
            return &campaign;

    return nullptr;
}

juce::String MainComponent::campaignBestKey() const
{
    return "CampaignBest:" + selectedCampaignId;
}

void MainComponent::selectCampaign (const juce::String& campaignId)
{
    selectedCampaignId = campaignId;

    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue ("SelectedCampaignId", selectedCampaignId);
        campaignStage = juce::jlimit (0, campaignStageCount(),
                                      settings->getIntValue (campaignBestKey(), 0));
        settings->saveIfNeeded();
    }
    else
    {
        campaignStage = 0;
    }

    applyServerCampaigns();
}

void MainComponent::resetCampaignProgress()
{
    campaignStage = 0;
    campaignStageWriteMs = juce::Time::getMillisecondCounterHiRes();

    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue (campaignBestKey(), 0);
        settings->saveIfNeeded();
    }

    applyServerCampaigns();

    // The server owns the run position: a reset that only cleared the local
    // cache was undone by the next stage result, so the tower jumped back to
    // "BATTLE COMPLETE" after the first song. Resetting there starts a new run
    // (best_stage = 0) and keeps the completion history for the reward rules.
    if (onlineAccessToken.isEmpty() || selectedCampaignId.isEmpty())
        return;

    const auto campaignId = selectedCampaignId;
    const auto token = onlineAccessToken;
    juce::Component::SafePointer<MainComponent> safeThis (this);

    juce::DynamicObject::Ptr body = new juce::DynamicObject();

    apiClient.postJson ("/campaigns/" + campaignId + "/reset",
                        juce::var (body.get()), token, 8000,
        [safeThis, campaignId] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            if (! reply.transportOk || reply.statusCode != 200
                || ! reply.json.isObject())
            {
                if (self->campaignPanel != nullptr)
                    self->campaignPanel->setStatus (
                        "Could not reset this battle on the server. "
                        "Check the connection and try again.");
                return;
            }

            const auto id = reply.json
                                .getProperty ("campaign_id", juce::var())
                                .toString();

            if (id.isNotEmpty())
                if (auto* settings = self->appProperties.getUserSettings())
                {
                    settings->setValue ("CampaignBest:" + id, 0);
                    settings->saveIfNeeded();
                }

            if (id.isEmpty() || id == self->selectedCampaignId)
            {
                self->campaignStage = 0;
                self->campaignStageWriteMs =
                    juce::Time::getMillisecondCounterHiRes();
            }

            self->applyServerCampaigns();
        });
}

void MainComponent::refreshServerCampaigns (bool silent)
{
    if (campaignRefreshInFlight)
        return;

    campaignRefreshInFlight = true;

    if (! silent)
    {
        campaignLoading = true;

        if (campaignPanel != nullptr)
            campaignPanel->setLoading (true);
    }

    juce::Component::SafePointer<MainComponent> safeThis (this);

    apiClient.getJson ("/campaigns", {}, 8000,
        [safeThis] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->campaignRefreshInFlight = false;
            self->campaignLoading = false;

            if (self->campaignPanel != nullptr)
                self->campaignPanel->setLoading (false);

            if (! reply.transportOk || reply.statusCode != 200
                || ! reply.json.isObject())
            {
                if (self->campaignPanel != nullptr)
                    self->campaignPanel->setStatus (
                        "Could not load battles. Check the connection and try again.");
                return;
            }

            std::vector<CampaignView> parsed;

            if (const auto* array = reply.json.getProperty ("items", juce::var()).getArray())
            {
                for (const auto& item : *array)
                {
                    if (! item.isObject())
                        continue;

                    CampaignView campaign;
                    campaign.id = item.getProperty ("id", "").toString();
                    campaign.name = item.getProperty ("name", "").toString();
                    // Additive card fields (server sends them once the tower
                    // artwork/description feature is deployed).
                    campaign.description =
                        item.getProperty ("description", juce::var()).toString();
                    campaign.coverImageUrl =
                        item.getProperty ("cover_image_url", juce::var()).toString();
                    campaign.rewardName =
                        item.getProperty ("reward_character_name", juce::var()).toString();
                    campaign.rewardCondition =
                        item.getProperty ("reward_condition", juce::var()).toString();
                    campaign.botLevel = juce::jlimit (
                        0, 2, static_cast<int> (item.getProperty ("bot_level", 1)));

                    if (const auto* songs = item.getProperty ("songs", juce::var()).getArray())
                    {
                        for (const auto& songItem : *songs)
                        {
                            if (! songItem.isObject())
                                continue;

                            CampaignStageView stage;
                            stage.song = songItem.getProperty ("title", "").toString();
                            stage.slug = songItem.getProperty ("slug", "").toString();
                            stage.serverId = songItem.getProperty ("song_id", "").toString();
                            stage.difficulty = static_cast<int> (
                                songItem.getProperty ("difficulty", 1));

                            // Per-stage bot level (server >= 1.5). Older servers
                            // do not send it, so the campaign default is kept.
                            int stageBotLevel = campaign.botLevel;

                            if (const auto* songObject = songItem.getDynamicObject())
                            {
                                const auto levelVar = songObject->getProperty ("bot_level");

                                if (levelVar.isInt() || levelVar.isInt64() || levelVar.isDouble())
                                {
                                    const int parsedLevel = static_cast<int> (levelVar);

                                    if (parsedLevel >= 0 && parsedLevel <= 2)
                                        stageBotLevel = parsedLevel;
                                }
                            }

                            stage.botLevel = stageBotLevel;
                            stage.opponent = campaignBotName (stageBotLevel);
                            campaign.stages.push_back (std::move (stage));
                        }
                    }

                    parsed.push_back (std::move (campaign));
                }
            }

            self->serverCampaigns = std::move (parsed);

            // The tower grid is a progression, so it reads easiest first. The
            // admin's order breaks ties inside the same difficulty tier.
            std::stable_sort (self->serverCampaigns.begin(),
                              self->serverCampaigns.end(),
                              [] (const CampaignView& a, const CampaignView& b)
                              {
                                  return a.botLevel < b.botLevel;
                              });

            if (auto* settings = self->appProperties.getUserSettings())
            {
                const auto saved = settings->getValue ("SelectedCampaignId");

                if (self->findServerCampaign (saved) != nullptr)
                    self->selectedCampaignId = saved;
            }

            // A previously selected campaign can disappear (deleted or renamed
            // id): fall back to the first one instead of keeping a stale id
            // that no row in the dropdown matches.
            if (self->selectedCampaignId.isEmpty()
                || self->findServerCampaign (self->selectedCampaignId) == nullptr)
            {
                if (! self->serverCampaigns.empty())
                    self->selectedCampaignId = self->serverCampaigns.front().id;
            }

            if (auto* settings = self->appProperties.getUserSettings())
                self->campaignStage = juce::jlimit (
                    0, self->campaignStageCount(),
                    settings->getIntValue (self->campaignBestKey(), 0));

            self->applyServerCampaigns();
        });
}

void MainComponent::requestCampaignCover (const juce::String& campaignId,
                                          const juce::String& url)
{
    if (url.isEmpty() || campaignCoverCache.count (url) > 0
        || campaignCoverInFlight.count (url) > 0)
        return;

    juce::ignoreUnused (campaignId);
    campaignCoverInFlight[url] = true;

    const auto absolute = url.startsWith ("http")
                              ? url
                              : juce::String (ApiClient::serverOrigin) + url;
    auto data = std::make_shared<juce::MemoryBlock>();
    auto image = std::make_shared<juce::Image>();
    juce::Component::SafePointer<MainComponent> safeThis (this);

    // Worker pool: download and decode the original tower artwork off the UI
    // thread. The card keeps the admin's colours untouched.
    apiClient.runAsync ([data, image, absolute]
                        {
                            *data = ApiClient::download (absolute, 15000);

                            if (data->getSize() > 0)
                                *image = juce::ImageFileFormat::loadFrom (data->getData(),
                                                                          data->getSize());
                        },
                        [safeThis, url, image]
                        {
                            auto* self = safeThis.getComponent();

                            if (self == nullptr)
                                return;

                            self->campaignCoverInFlight.erase (url);

                            if (! image->isValid())
                                return;

                            self->campaignCoverCache[url] = *image;
                            self->applyServerCampaigns();
                        });
}

void MainComponent::applyServerCampaigns()
{
    if (campaignPanel == nullptr)
        return;

    // The opponent caption must be the character the player actually faces on
    // that stage (the sprite assigned to the song in the admin catalogue), not
    // the fixed bot-difficulty name. The bot name stays as a fallback while the
    // catalogue or the song's character is unavailable.
    for (auto& campaign : serverCampaigns)
    {
        for (auto& stage : campaign.stages)
        {
            for (const auto& row : catalogSongs)
            {
                if ((stage.slug.isNotEmpty() && row.slug == stage.slug)
                    || (stage.serverId.isNotEmpty() && row.serverId == stage.serverId))
                {
                    if (row.characterName.isNotEmpty())
                        stage.opponent = row.characterName;

                    break;
                }
            }
        }
    }

    // Per-tower progress for the card grid: the mirror holds every campaign,
    // not only the selected one.
    {
        auto* settings = appProperties.getUserSettings();

        for (auto& campaign : serverCampaigns)
        {
            const int count = static_cast<int> (campaign.stages.size());
            const int stored = settings != nullptr
                                   ? settings->getIntValue ("CampaignBest:" + campaign.id, 0)
                                   : 0;
            campaign.bestStage = count > 0 ? juce::jlimit (0, count, stored) : 0;

            if (campaign.coverImageUrl.isNotEmpty())
            {
                const auto cached = campaignCoverCache.find (campaign.coverImageUrl);

                if (cached != campaignCoverCache.end())
                    campaign.cover = cached->second;
                else
                    requestCampaignCover (campaign.id, campaign.coverImageUrl);
            }
        }
    }

    const auto* campaign = findServerCampaign (selectedCampaignId);
    campaignSongIndices.clear();

    if (campaign != nullptr)
    {
        for (const auto& stage : campaign->stages)
        {
            int index = -1;

            for (int i = 0; i < static_cast<int> (catalogSongs.size()); ++i)
            {
                const auto& row = catalogSongs[static_cast<size_t> (i)];

                if ((stage.slug.isNotEmpty() && row.slug == stage.slug)
                    || (stage.serverId.isNotEmpty() && row.serverId == stage.serverId))
                {
                    index = i;
                    break;
                }
            }

            campaignSongIndices.push_back (index);
        }
    }

    campaignPanel->setPlayerLevel (juce::jmax (1, progressionLevel));
    campaignPanel->setCampaigns (serverCampaigns, selectedCampaignId, campaignStage);

    if (serverCampaigns.empty())
    {
        campaignPanel->setStatus (
            "No battles yet. Create one in the admin panel.");
    }
    else if (campaign == nullptr)
    {
        campaignPanel->setStatus ("Choose a battle.");
    }
    else if (campaign->stages.empty())
    {
        campaignPanel->setStatus (
            "This battle has no songs yet. Add songs in the admin panel.");
    }
    else
    {
        campaignPanel->setStatus (
            juce::String (static_cast<int> (campaign->stages.size()))
            + " stages - win each one to unlock the next song of the tower.");
    }
}

void MainComponent::refreshCampaignSection()
{
    if (campaignPanel == nullptr)
    {
        campaignPanel = std::make_unique<CampaignPanel>();
        campaignPanel->onCampaignSelected = [this] (const juce::String& campaignId)
        {
            selectCampaign (campaignId);
        };
        campaignPanel->onFight = [this] { startCampaignStage(); };
        campaignPanel->onReset = [this] { resetCampaignProgress(); };
        addChildComponent (*campaignPanel);
    }

    // Show the cached list straight away, then always re-read it: campaigns
    // are added in the admin panel while the client keeps running, and a list
    // fetched only once used to hide every newer campaign until a restart.
    if (! serverCampaigns.empty())
        applyServerCampaigns();

    refreshServerCampaigns (! serverCampaigns.empty());

    // The server owns the cleared-stage count (a defeat resets the run to the
    // first opponent). Re-read it every time Battle is opened so the panel can
    // never show a stage cleared from a stale local cache.
    syncServerCampaignProgress();
}

void MainComponent::startCampaignStage (bool skipTuner)
{
    const auto* campaign = findServerCampaign (selectedCampaignId);

    if (campaign == nullptr || campaign->stages.empty()
        || campaignStage >= static_cast<int> (campaign->stages.size())
        || campaignStage >= static_cast<int> (campaignSongIndices.size()))
        return;

    const int index = campaignSongIndices[static_cast<size_t> (campaignStage)];

    if (! juce::isPositiveAndBelow (index, static_cast<int> (catalogSongs.size())))
    {
        if (catalogSongs.empty())
        {
            // The catalogue has not arrived yet: ask for it instead of leaving
            // the player in front of a button that does nothing.
            if (campaignPanel != nullptr)
                campaignPanel->setStatus ("Loading the song list...");

            refreshOnlineCatalog();
            return;
        }

        if (campaignPanel != nullptr)
            campaignPanel->setStatus (
                "This stage's song is not in the catalogue. Refresh the catalogue.");

        return;
    }

    selectedCatalogIndex = index;
    campaignFightActive = true;
    matchOriginSection = AppSection::Campaign;
    skipTunerForNextStage = skipTuner;

    // One Battle attempt = one run id. The result report retries this exact id
    // on a transport failure, so the server cannot reward the same run twice.
    campaignStageRunId = juce::Uuid().toString();
    campaignStageServerXp = -1;
    campaignStageNormalizedScore = -1;
    campaignStageReportPending = false;
    campaignStageReportFailed = false;

    // Each stage carries its own bot level; the campaign-level value is only
    // the default for stages that do not override it.
    const auto& stage = campaign->stages[static_cast<size_t> (campaignStage)];
    requestDuelWithBot (stage.botLevel);
}

void MainComponent::syncServerCampaignProgress()
{
    if (onlineAccessToken.isEmpty() || campaignProgressSyncInFlight)
        return;

    campaignProgressSyncInFlight = true;
    juce::Component::SafePointer<MainComponent> safeThis (this);
    // A stage result reported after this request was sent makes this answer
    // stale: its GET can be served before the result POST commits.
    const double requestMs = juce::Time::getMillisecondCounterHiRes();

    // The server is the only owner of Battle progress: a defeat already moved
    // the run back to the first opponent there. ``CampaignBest:<id>`` is only a
    // cache for the panel, so this sync REPLACES it with the server's value and
    // never uploads it. Uploading the cached stage is what made a cleared stage
    // reappear after a defeat: the stale value was written back through the
    // monotonic /progress endpoint, and a delayed /progress response raised the
    // cache again, so the tower restarted at the wrong opponent.
    apiClient.getJson ("/campaigns/progress", onlineAccessToken, 8000,
        [safeThis, requestMs] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->campaignProgressSyncInFlight = false;

            if (! reply.transportOk || reply.statusCode != 200
                || ! reply.json.isObject())
                return;

            auto* settings = self->appProperties.getUserSettings();
            const auto* items =
                reply.json.getProperty ("items", juce::var()).getArray();

            if (settings == nullptr || items == nullptr)
                return;

            // A stage result applied after this request was sent owns the
            // selected campaign's value: this answer predates it.
            const bool staleForSelected =
                self->selectedCampaignId.isNotEmpty()
                && self->campaignStageWriteMs > requestMs;

            for (const auto& item : *items)
            {
                if (! item.isObject())
                    continue;

                const auto id = item.getProperty ("campaign_id", "").toString();

                if (id.isEmpty())
                    continue;

                if (staleForSelected && id == self->selectedCampaignId)
                    continue;

                const int stageCount = juce::jmax (
                    0, static_cast<int> (item.getProperty ("stage_count", 0)));
                const int best = juce::jlimit (
                    0, stageCount,
                    static_cast<int> (item.getProperty ("best_stage", 0)));

                settings->setValue ("CampaignBest:" + id, best);
            }

            settings->saveIfNeeded();

            if (self->selectedCampaignId.isNotEmpty() && ! staleForSelected)
            {
                const int stored = settings->getIntValue (self->campaignBestKey(), 0);
                const int stageCount = self->campaignStageCount();

                // The campaign list may not be loaded yet at sign-in: clamping
                // to an unknown count would silently drop the saved stage.
                self->campaignStage = stageCount > 0
                                          ? juce::jlimit (0, stageCount, stored)
                                          : juce::jmax (0, stored);
            }

            self->applyServerCampaigns();
        });
}

void MainComponent::notifyCharacterUnlocked (const juce::String& name)
{
    if (name.isEmpty())
        return;

    // The PROFILE dialog caches GET /characters; refresh it so the new
    // character moves from the locked list into the combo immediately.
    if (refreshOpenAccountCharacters != nullptr)
        refreshOpenAccountCharacters();

    juce::Component::SafePointer<MainComponent> safeThis (this);

    juce::MessageManager::callAsync ([safeThis, name]
    {
        if (safeThis.getComponent() == nullptr)
            return;

        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::InfoIcon,
            "New character unlocked",
            "New character unlocked: " + name
                + ".\nPick it in PROFILE before your next match.",
            "OK");
    });
}

void MainComponent::setSelectedCharacter (int id, const juce::String& name,
                                         const juce::String& sheetUrl,
                                         bool persistToServer)
{
    selectedCharacterId = id;
    selectedCharacterName = name;
    selectedCharacterSheetUrl = sheetUrl;

    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue ("SelectedCharacterId", id);
        settings->setValue ("SelectedCharacterName", name);
        settings->setValue ("SelectedCharacterSheetUrl", sheetUrl);
        settings->saveIfNeeded();
    }

    // Persist the choice on the account as well; the server sends it to the
    // opponent in online duels. The Profile section passes false: there the
    // character is written by SAVE PROFILE together with the other fields, so
    // the button can report whether the account really was updated.
    if (persistToServer && onlineAccessToken.isNotEmpty())
    {
        juce::DynamicObject::Ptr payload = new juce::DynamicObject();
        payload->setProperty ("character_id",
                              id > 0 ? juce::var (id) : juce::var());

        apiClient.patchJson ("/auth/me", juce::var (payload.get()),
                             onlineAccessToken, 8000,
                             [] (ApiClient::Response) {});
    }

    applyPlayerCharacterSheet();
}

void MainComponent::restoreSelectedCharacter()
{
    if (auto* settings = appProperties.getUserSettings())
    {
        selectedCharacterId = settings->getIntValue ("SelectedCharacterId", 0);
        selectedCharacterName = settings->getValue ("SelectedCharacterName");
        selectedCharacterSheetUrl = settings->getValue ("SelectedCharacterSheetUrl");
    }

    applyPlayerCharacterSheet();
}

void MainComponent::applyCharacterFromServer (int id, const juce::String& name,
                                              const juce::String& sheetUrl)
{
    if (id <= 0 || name.isEmpty() || sheetUrl.isEmpty())
        return;

    if (selectedCharacterId == id
        && selectedCharacterName == name
        && selectedCharacterSheetUrl == sheetUrl)
        return;

    selectedCharacterId = id;
    selectedCharacterName = name;
    selectedCharacterSheetUrl = sheetUrl;

    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue ("SelectedCharacterId", id);
        settings->setValue ("SelectedCharacterName", name);
        settings->setValue ("SelectedCharacterSheetUrl", sheetUrl);
        settings->saveIfNeeded();
    }

    applyPlayerCharacterSheet();
}

void MainComponent::applyDefaultCharacterIfNeeded (bool force)
{
    if ((! force && selectedCharacterSheetUrl.isNotEmpty())
        || defaultCharacterRequestInFlight)
        return;

    defaultCharacterRequestInFlight = true;

    juce::Component::SafePointer<MainComponent> safeThis (this);

    apiClient.getJson ("/characters", {}, 8000,
        [safeThis] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->defaultCharacterRequestInFlight = false;

            if (! reply.transportOk || reply.statusCode != 200
                || ! reply.json.isArray())
                return;

            for (const auto& item : *reply.json.getArray())
            {
                if (! item.isObject())
                    continue;

                const auto name = item.getProperty ("name", "").toString();

                if (! name.equalsIgnoreCase ("Ray"))
                    continue;

                const int id = static_cast<int> (item.getProperty ("id", 0));
                const auto sheet = item.getProperty ("sheet_url", "").toString();

                if (id > 0 && sheet.isNotEmpty())
                    self->setSelectedCharacter (id, name, sheet);

                break;
            }
        });
}

void MainComponent::applyPlayerCharacterSheet()
{
    if (selectedCharacterSheetUrl.isEmpty())
    {
        duelStage.clearPlayerSheet();
        return;
    }

    applyPlayerCharacterSheetFromUrl (selectedCharacterSheetUrl);
}

void MainComponent::applyPlayerCharacterSheetFromUrl (const juce::String& sheetUrl)
{
    if (sheetUrl.isEmpty())
        return;

    const auto url = sheetUrl.startsWith ("http")
        ? sheetUrl
        : juce::String (ApiClient::serverOrigin) + sheetUrl;

    juce::Component::SafePointer<MainComponent> safeThis (this);

    // Network + decode on the worker pool; the image is handed to the stage on
    // the message thread.
    apiClient.runAsync (
        [safeThis, url]
        {
            const auto data = ApiClient::download (url, 15000);

            juce::MessageManager::callAsync (
                [safeThis, data]
                {
                    if (auto* self = safeThis.getComponent())
                        self->duelStage.setPlayerSheet (data);
                });
        },
        nullptr);
}

void MainComponent::ensureVstSection()
{
    if (vstSectionPanel != nullptr)
        return;

    vstSectionPanel = std::make_unique<VstPanel> (vstHost);
    vstSectionPanel->setEmbedded (true);
    addChildComponent (*vstSectionPanel);
}

void MainComponent::openStandaloneTuner()
{
    if (gameState.load (std::memory_order_relaxed) != GameState::SongSelect)
        return;

    tunerStandalone = true;
    tuner.resetTunedStrings();
    syncTuningUi();
    tunerActive.store (true, std::memory_order_relaxed);
    tunerStartButton.setEnabled (false);
    gameState.store (GameState::Tuner, std::memory_order_relaxed);

    resized();
    repaint();
    overlayComponent.repaint();
}

void MainComponent::leaveStandaloneTuner()
{
    const bool returnToLobby = tunerReturnToLobby;
    tunerReturnToLobby = false;

    tunerActive.store (false, std::memory_order_relaxed);
    tunerStandalone = false;

    if (gameState.load (std::memory_order_relaxed) == GameState::Tuner)
        gameState.store (GameState::SongSelect, std::memory_order_relaxed);

    if (returnToLobby && onlineDuelRoomDialog != nullptr)
    {
        onlineDuelRoomDialog->setVisible (true);
        onlineDuelRoomDialog->toFront (true);
    }

    if (returnToLobby)
    {
        if (onlineDuelRoomDialog == nullptr && tunerLobbyId.isNotEmpty())
        {
            const auto id = tunerLobbyId;
            tunerLobbyId.clear();
            openOnlineDuelRoom (id);
            return;
        }

        tunerLobbyId.clear();
    }

    resized();
    repaint();
    overlayComponent.repaint();
}

void MainComponent::openLobbyTuner()
{
    if (onlineDuelRoomDialog == nullptr
        || tunerStandalone
        || gameState.load (std::memory_order_relaxed) != GameState::SongSelect)
        return;

    tunerReturnToLobby = true;
    tunerLobbyId = activeVoiceLobbyId;
    onlineDuelRoomDialog->setVisible (false);
    openStandaloneTuner();

    if (! tunerStandalone)
    {
        // openStandaloneTuner() refused (not in the hub state): restore the
        // room immediately instead of leaving it hidden.
        tunerReturnToLobby = false;
        tunerLobbyId.clear();
        onlineDuelRoomDialog->setVisible (true);
        return;
    }

    toFront (true);
    resized();
    repaint();
}

void MainComponent::closeLobbyTuner()
{
    const bool lobbyHidden = onlineDuelRoomDialog != nullptr
                             && ! onlineDuelRoomDialog->isVisible();

    if (! tunerReturnToLobby && ! lobbyHidden)
        return;

    const auto lobbyId = tunerLobbyId;
    tunerLobbyId.clear();
    tunerReturnToLobby = false;
    leaveStandaloneTuner();

    if (onlineDuelRoomDialog != nullptr)
    {
        onlineDuelRoomDialog->setVisible (true);
        onlineDuelRoomDialog->toFront (true);
    }
    else if (lobbyId.isNotEmpty())
    {
        // The dialog was torn down while hidden; rejoin the same server lobby.
        openOnlineDuelRoom (lobbyId);
    }

    resized();
    repaint();
}

void MainComponent::returnToHubSection()
{
    returnToSongSelect();
}

void MainComponent::setSection (AppSection section)
{
    if (showAuthScreen)
        return;

    if (publicProfileViewport != nullptr && publicProfileViewport->isVisible())
        publicProfileViewport->setVisible (false);

    const auto state = gameState.load (std::memory_order_relaxed);

    if (state != GameState::SongSelect && state != GameState::Tuner)
        return;

    if (section != currentSection)
        uiSounds.play ("nav.section");

    if (section == AppSection::Tuner)
    {
        currentSection = section;

        if (! tunerStandalone)
            openStandaloneTuner();
    }
    else
    {
        if (state == GameState::Tuner)
            leaveStandaloneTuner();

        tunerStandalone = false;
        gameState.store (GameState::SongSelect, std::memory_order_relaxed);
        currentSection = section;

        if (section == AppSection::Profile)
            ensureProfileSection();
        else if (section == AppSection::Settings)
            ensureSettingsSection();
        else if (section == AppSection::Vst)
            ensureVstSection();
        else if (section == AppSection::Campaign)
            refreshCampaignSection();
        else if (section == AppSection::Friends)
            ensureFriendsSection();
        else if (section == AppSection::Messages)
            ensureMessagesSection();
        else if (section == AppSection::OnlineDuel)
        {
            duelLobbyRefreshTicks = 0;
            refreshDuelLobby();
        }

        // HOME shows the administrator hero artwork and the player avatar, so
        // re-read the public config when the player returns to the dashboard
        // (images are cached by URL, only the small JSON is re-fetched).
        if (section == AppSection::Home)
            refreshSiteArtwork();
    }

    if (sectionSidebar != nullptr)
        sectionSidebar->setActiveSection (section);

    resized();
    repaint();
    overlayComponent.repaint();
}
