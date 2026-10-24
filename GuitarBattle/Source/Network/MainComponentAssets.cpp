#include "../MainComponent.h"

#include <algorithm>
#include <memory>
#include <vector>

// Server-hosted sound-pack lifecycle. Downloads and extraction happen on
// worker threads; decoded buffers are swapped only while audio is stopped.

void MainComponent::startClientAssetRefresh()
{
    if (clientAssetsRefreshInFlight)
        return;

    clientAssetsRefreshInFlight = true;

    juce::Component::SafePointer<MainComponent> safeThis(this);

    ClientAssets::refresh(apiClient,
        [safeThis](ClientAssets::RefreshResult result)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->clientAssetsRefreshInFlight = false;

            if (! result.ok)
            {
                if (! self->clientAssetsReady)
                    self->onlineCatalogStatusLabel.setText(
                        "Sound pack unavailable: " + result.message
                            + " Built-in sounds will be used.",
                        juce::dontSendNotification);

                return;
            }

            const bool changed = result.updated
                || ! self->clientAssetsReady
                || self->clientAssetsRoot != ClientAssets::currentRoot();

            self->clientAssetsRoot = ClientAssets::currentRoot();
            self->clientAssetsReady = true;

            if (changed)
            {
                self->pendingClientAssetsReload = true;
                self->applyPendingClientAssetsReload();
            }
        });
}

void MainComponent::applyPendingClientAssetsReload()
{
    if (! pendingClientAssetsReload)
        return;

    // Never rebuild the sample banks under a running gameplay callback.  The
    // timer applies the pack as soon as the player is back in a menu screen.
    if (gameState == GameState::Playing || gameState == GameState::Paused)
        return;

    pendingClientAssetsReload = false;
    reloadClientAssets();
}

void MainComponent::reloadClientAssets()
{
    if (! clientAssetsRoot.isDirectory())
        return;

    std::unique_ptr<juce::XmlElement> deviceState;

    if (audioDeviceStarted)
    {
        deviceState = deviceManager.createStateXml();
        shutdownAudio();
        audioDeviceStarted = false;
    }

    loadGuitarSampleBank(clientAssetsRoot);
    loadBackingSampleBanks(clientAssetsRoot);
    loadMetronomeSamples(clientAssetsRoot);

    startAudioDevice(deviceState.get());

    resized();
    repaint();
}

