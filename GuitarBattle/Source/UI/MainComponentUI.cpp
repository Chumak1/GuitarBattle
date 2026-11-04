#include "../MainComponent.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "GameStyle.h"
#include "BrandAssets.h"
#include "VstPanel.h"

// UI-only implementation for MainComponent. Frontend work belongs in this
// file and in UI/MainComponentUi.inc; do not add audio, network or gameplay
// logic here.

void MainComponent::showHitFeedback(const juce::String& text, int type)
{
    // Called on the message thread (all pitch-thread callers use callAsync).
    feedbackType = type;
    feedbackStartMs = juce::Time::getMillisecondCounter();
    feedbackBaseFontSize = 27.0f;
    hitLabel.setText(text, juce::dontSendNotification);
    updateHitFeedbackAnimation();

    // Repaint only the feedback label. A full-window repaint here forced the
    // tablature to repaint on every hit and during the whole animation, which
    // made dense songs look like they were stalling.
    hitLabel.repaint();
}

void MainComponent::updateHitFeedbackAnimation()
{
    if (feedbackType == 0)
        return;

    const uint32_t elapsed = juce::Time::getMillisecondCounter() - feedbackStartMs;
    constexpr uint32_t animationDurationMs = 620;
    if (elapsed >= animationDurationMs)
    {
        feedbackType = 0;
        hitLabel.setFont(GameUi::font(feedbackBaseFontSize, true));
        hitLabel.setColour(juce::Label::textColourId, UiColors::blue);
        hitLabel.repaint();
        return;
    }

    const float progress = static_cast<float>(elapsed) / static_cast<float>(animationDurationMs);
    const float pulse = std::sin(progress * juce::MathConstants<float>::pi);
    const float fontSize = feedbackBaseFontSize + 10.0f * pulse;

    juce::Colour feedbackColour = UiColors::blue;
    switch (feedbackType)
    {
    case 1: feedbackColour = UiColors::orange; break; // PERFECT: gold
    case 2: feedbackColour = juce::Colour(0xFF36C98F); break; // GREAT: green
    case 3: feedbackColour = juce::Colour(0xFF55A7FF); break; // GOOD: blue
    case 4: feedbackColour = juce::Colour(0xFFFF5A67); break; // MISS: red
    case 5: feedbackColour = UiColors::orange; break; // combo milestone
    default: break;
    }

    const float alpha = 1.0f - 0.35f * progress;
    hitLabel.setFont(GameUi::font(fontSize, true));
    hitLabel.setColour(juce::Label::textColourId, feedbackColour.withAlpha(alpha));
    hitLabel.repaint();
}

void MainComponent::showEmailVerificationDialog(const juce::String& email,
                                                const juce::String& fallbackName,
                                                bool accountCreated)
{
    const auto normalizedEmail = email.trim().toLowerCase();
    auto* prompt = new juce::AlertWindow(
        "Verify your email",
        normalizedEmail.isNotEmpty()
            ? "Enter the 6-digit code sent to " + normalizedEmail + "."
            : "Enter your email and the 6-digit code. Use RESEND CODE if you need a new one.",
        juce::AlertWindow::NoIcon);

    prompt->addTextEditor("email", normalizedEmail, "Email");
    prompt->addTextEditor("code", "", "6-digit confirmation code");
    prompt->addButton("VERIFY", 1);
    prompt->addButton("RESEND CODE", 2);
    prompt->addButton("CANCEL", 0);

    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::Component::SafePointer<juce::AlertWindow> safePrompt(prompt);
    const int attemptId = ++authRequestId;

    auto setSubmitEnabled = [safePrompt](bool shouldBeEnabled)
    {
        if (auto* dialog = safePrompt.getComponent())
            if (auto* submit = dialog->getButton(0))
                submit->setEnabled(shouldBeEnabled);
    };

    auto showError = [safeThis, safePrompt](const juce::String& title,
                                            const juce::String& message)
    {
        if (safePrompt.getComponent() == nullptr)
            return;

        juce::MessageManager::callAsync([safeThis, safePrompt, title, message]()
        {
            auto* self = safeThis.getComponent();
            auto* dialog = safePrompt.getComponent();
            if (self == nullptr || dialog == nullptr)
                return;

            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                title,
                message,
                "OK",
                dialog);
        });
    };

    auto verify = [safeThis, safePrompt, attemptId, fallbackName, accountCreated,
                   setSubmitEnabled, showError]()
    {
        auto* self = safeThis.getComponent();
        auto* dialog = safePrompt.getComponent();
        if (self == nullptr || dialog == nullptr || attemptId != self->authRequestId)
            return;

        const auto emailAddress = dialog->getTextEditorContents("email").trim().toLowerCase();
        const auto code = dialog->getTextEditorContents("code").trim();

        if (emailAddress.isEmpty() || ! emailAddress.containsChar('@'))
        {
            showError("Verification failed", "Enter the email address used for registration.");
            return;
        }

        if (code.length() != 6 || ! code.containsOnly("0123456789"))
        {
            showError("Verification failed", "Enter the 6-digit code from the email.");
            return;
        }

        juce::DynamicObject::Ptr payload = new juce::DynamicObject();
        payload->setProperty("email", emailAddress);
        payload->setProperty("code", code);

        setSubmitEnabled(false);

        self->apiClient.postJson("/auth/verify-email", juce::var(payload.get()), {}, 5000,
            [safeThis, safePrompt, attemptId, fallbackName, accountCreated,
             emailAddress, setSubmitEnabled, showError](ApiClient::Response response)
            {
                auto* component = safeThis.getComponent();
                if (component == nullptr || attemptId != component->authRequestId)
                    return;

                const auto accessToken =
                    response.json.getProperty("access_token", juce::var()).toString();
                if (accessToken.isNotEmpty())
                {
                    component->onlineAccessToken = accessToken;
                    if (auto* dialog = safePrompt.getComponent())
                        dialog->exitModalState(1);

                    component->finishOnlineSignIn(
                        accountCreated,
                        fallbackName.isNotEmpty() ? fallbackName : emailAddress,
                        accessToken,
                        attemptId);
                    return;
                }

                setSubmitEnabled(true);
                component->accountStatusLabel.setText(
                    "Verification failed. Check the code and try again.",
                    juce::dontSendNotification);

                const auto detail =
                    response.json.getProperty("detail", juce::var()).toString();
                if (response.statusCode == 400 || response.statusCode == 422)
                {
                    showError("Verification failed",
                              detail.isNotEmpty()
                                  ? detail
                                  : "The code is invalid or expired. Request a new code and try again.");
                }
                else
                {
                    showError("Verification failed",
                              "Could not verify the email. Check the server connection and try again.");
                }
            });
    };

    auto resend = [safeThis, safePrompt, attemptId, showError]()
    {
        auto* self = safeThis.getComponent();
        auto* dialog = safePrompt.getComponent();
        if (self == nullptr || dialog == nullptr || attemptId != self->authRequestId)
            return;

        const auto emailAddress = dialog->getTextEditorContents("email").trim().toLowerCase();
        if (emailAddress.isEmpty() || ! emailAddress.containsChar('@'))
        {
            showError("Resend failed", "Enter the email address used for registration.");
            return;
        }

        juce::DynamicObject::Ptr payload = new juce::DynamicObject();
        payload->setProperty("email", emailAddress);

        self->apiClient.postJson("/auth/resend-verification", juce::var(payload.get()), {}, 5000,
            [safeThis, safePrompt, attemptId, emailAddress, showError](ApiClient::Response response)
            {
                auto* component = safeThis.getComponent();
                if (component == nullptr || attemptId != component->authRequestId)
                    return;

                if (response.statusCode >= 200 && response.statusCode < 300)
                {
                    component->accountStatusLabel.setText(
                        "A new code was sent to " + emailAddress + ".",
                        juce::dontSendNotification);
                    showError("Code sent",
                              "If this account needs verification, a new code is on its way to "
                                  + emailAddress + ".");
                }
                else
                {
                    showError("Resend failed",
                              "Could not request a new code. Check the server connection and try again.");
                }
            });
    };

    if (auto* verifyButton = prompt->getButton(0))
        verifyButton->onClick = verify;
    if (auto* resendButton = prompt->getButton(1))
        resendButton->onClick = resend;

    prompt->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis](int result)
        {
            if (result != 1)
                if (auto* self = safeThis.getComponent())
                    ++self->authRequestId;
        }), true);
}

void MainComponent::showPasswordResetDialog(const juce::String& email)
{
    const auto normalizedEmail = email.trim().toLowerCase();
    auto* prompt = new juce::AlertWindow(
        "Choose a new password",
        "Enter the 6-digit code sent to " + normalizedEmail
            + " and choose a new password of at least 6 characters.",
        juce::AlertWindow::NoIcon);

    prompt->addTextEditor("code", "", "6-digit reset code");
    prompt->addTextEditor("new_password", "", "New password (6+ characters)", true);
    prompt->addTextEditor("confirm_password", "", "Repeat new password", true);
    prompt->addButton("RESET PASSWORD", 1);
    prompt->addButton("RESEND CODE", 2);
    prompt->addButton("CANCEL", 0);

    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::Component::SafePointer<juce::AlertWindow> safePrompt(prompt);
    const int attemptId = ++authRequestId;

    auto setSubmitEnabled = [safePrompt](bool shouldBeEnabled)
    {
        if (auto* dialog = safePrompt.getComponent())
            if (auto* submit = dialog->getButton(0))
                submit->setEnabled(shouldBeEnabled);
    };

    auto showError = [safeThis, safePrompt](const juce::String& title,
                                            const juce::String& message)
    {
        if (safePrompt.getComponent() == nullptr)
            return;

        juce::MessageManager::callAsync([safeThis, safePrompt, title, message]()
        {
            auto* self = safeThis.getComponent();
            auto* dialog = safePrompt.getComponent();
            if (self == nullptr || dialog == nullptr)
                return;

            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                title,
                message,
                "OK",
                dialog);
        });
    };

    auto reset = [safeThis, safePrompt, attemptId, normalizedEmail,
                  setSubmitEnabled, showError]()
    {
        auto* self = safeThis.getComponent();
        auto* dialog = safePrompt.getComponent();
        if (self == nullptr || dialog == nullptr || attemptId != self->authRequestId)
            return;

        const auto code = dialog->getTextEditorContents("code").trim();
        const auto newPassword = dialog->getTextEditorContents("new_password");
        const auto confirmPassword = dialog->getTextEditorContents("confirm_password");

        if (code.length() != 6 || ! code.containsOnly("0123456789"))
        {
            showError("Reset failed", "Enter the 6-digit code from the email.");
            return;
        }

        if (newPassword.length() < 6)
        {
            showError("Reset failed", "The new password must be at least 6 characters.");
            return;
        }

        if (newPassword != confirmPassword)
        {
            showError("Reset failed", "The new passwords do not match.");
            return;
        }

        juce::DynamicObject::Ptr payload = new juce::DynamicObject();
        payload->setProperty("email", normalizedEmail);
        payload->setProperty("code", code);
        payload->setProperty("new_password", newPassword);

        setSubmitEnabled(false);

        self->apiClient.postJson("/auth/reset-password", juce::var(payload.get()), {}, 5000,
            [safeThis, safePrompt, attemptId, normalizedEmail,
             setSubmitEnabled, showError](ApiClient::Response response)
            {
                auto* component = safeThis.getComponent();
                if (component == nullptr || attemptId != component->authRequestId)
                    return;

                const auto accessToken =
                    response.json.getProperty("access_token", juce::var()).toString();
                if (accessToken.isNotEmpty())
                {
                    component->onlineAccessToken = accessToken;
                    if (auto* dialog = safePrompt.getComponent())
                        dialog->exitModalState(1);

                    component->finishOnlineSignIn(false, normalizedEmail, accessToken, attemptId);
                    return;
                }

                setSubmitEnabled(true);
                component->accountStatusLabel.setText(
                    "Password reset failed. Check the code and try again.",
                    juce::dontSendNotification);

                const auto detail =
                    response.json.getProperty("detail", juce::var()).toString();
                if (response.statusCode == 400 || response.statusCode == 422)
                {
                    showError("Reset failed",
                              detail.isNotEmpty()
                                  ? detail
                                  : "The code is invalid or expired. Request a new code and try again.");
                }
                else
                {
                    showError("Reset failed",
                              "Could not reset the password. Check the server connection and try again.");
                }
            });
    };

    auto resend = [safeThis, safePrompt, attemptId, normalizedEmail, showError]()
    {
        auto* self = safeThis.getComponent();
        auto* dialog = safePrompt.getComponent();
        if (self == nullptr || dialog == nullptr || attemptId != self->authRequestId)
            return;

        juce::DynamicObject::Ptr payload = new juce::DynamicObject();
        payload->setProperty("email", normalizedEmail);

        self->apiClient.postJson("/auth/forgot-password", juce::var(payload.get()), {}, 5000,
            [safeThis, safePrompt, attemptId, normalizedEmail, showError](ApiClient::Response response)
            {
                auto* component = safeThis.getComponent();
                if (component == nullptr || attemptId != component->authRequestId)
                    return;

                if (response.statusCode >= 200 && response.statusCode < 300)
                {
                    component->accountStatusLabel.setText(
                        "A new reset code was sent to " + normalizedEmail + ".",
                        juce::dontSendNotification);
                    showError("Code sent",
                              "If the account exists, a new reset code is on its way to "
                                  + normalizedEmail + ".");
                }
                else
                {
                    showError("Resend failed",
                              "Could not request a new code. Check the server connection and try again.");
                }
            });
    };

    if (auto* resetButton = prompt->getButton(0))
        resetButton->onClick = reset;
    if (auto* resendButton = prompt->getButton(1))
        resendButton->onClick = resend;

    prompt->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis](int result)
        {
            if (result != 1)
                if (auto* self = safeThis.getComponent())
                    ++self->authRequestId;
        }), true);
}

int MainComponent::SongListModel::getNumRows()
{
    return static_cast<int>(owner.catalogSongs.size());
}

void MainComponent::SongListModel::paintListBoxItem(int rowNumber,
                                                    juce::Graphics& g,
                                                    int width,
                                                    int height,
                                                    bool rowIsSelected)
{
    if (! juce::isPositiveAndBelow(rowNumber,
                                   static_cast<int>(owner.catalogSongs.size())))
        return;

    const auto& song = owner.catalogSongs[static_cast<size_t>(rowNumber)];
    const auto row = juce::Rectangle<int>(0, 0, width, height).reduced(3, 4);

    // A locked tier sits on the page background with a faint border, so it
    // reads as "not yet" even before the eye reaches the requirement text.
    const bool unlocked = owner.isSongUnlocked(song);

    if (rowIsSelected)
    {
        g.setColour(UiColors::accent.withAlpha(0.22f));
    }
    else if (unlocked)
    {
        // Gold wash: every playable song reads as a gold tile, while a locked
        // one stays on the dark page background.
        g.setColour(UiColors::accent.withAlpha(0.12f));
    }
    else
    {
        g.setColour(UiColors::background.withAlpha(0.85f));
    }

    g.fillRoundedRectangle(row.toFloat(), 12.0f);

    g.setColour(rowIsSelected ? UiColors::accent.withAlpha(0.75f)
                              : (unlocked ? UiColors::accent.withAlpha(0.28f)
                                          : UiColors::border.withAlpha(0.45f)));
    g.drawRoundedRectangle(row.toFloat().reduced(0.5f), 12.0f, 1.0f);

    if (rowIsSelected)
    {
        const juce::Rectangle<int> selectionBar(row.getX() + 2, row.getY() + 11,
                                                4, juce::jmax(4, row.getHeight() - 22));
        g.setColour(UiColors::accent);
        g.fillRoundedRectangle(selectionBar.toFloat(), 2.0f);
    }

    // The best score sits on the right; the metadata column shrinks with the card.
    const int bestWidth = juce::jlimit(96, 168, width / 3);
    const auto bestArea = row.withTrimmedRight(18).removeFromRight(bestWidth);
    auto textArea = row.withTrimmedLeft(18).withTrimmedRight(bestWidth + 30);

    juce::StringArray meta;
    if (song.artist.isNotEmpty())
        meta.add(song.artist);
    meta.add(juce::String(juce::roundToInt(song.bpm)) + " BPM");
    meta.add(MainComponent::formatTimeSignature(song.timeSignatureNumerator,
                                                song.timeSignatureDenominator));
    meta.add(song.downloaded ? "ONLINE / CACHED" : "ONLINE");

    const auto titleFont = GameUi::font(14.0f, true);
    auto titleLine = textArea.removeFromTop(juce::jmax(18, height * 3 / 5));

    // A locked tier keeps its row visible, so the player can see what is
    // waiting for them, but the title is dimmed like a disabled item.
    g.setColour(unlocked ? UiColors::text : UiColors::muted);
    g.setFont(titleFont);
    g.drawFittedText(song.title, titleLine,
                     juce::Justification::centredLeft, 1, 0.8f);

    g.setColour(UiColors::muted);
    g.setFont(GameUi::font(10.5f));
    g.drawFittedText(meta.joinIntoString("   |   "),
                     textArea.toNearestInt(),
                     juce::Justification::centredLeft, 1, 0.7f);

    const int best = owner.getBestScoreForSong(song.slug, song.title);

    // Difficulty, genre and best score share the right column: both tags sit on
    // the same line, directly above the score, so the numbers read as one block.
    auto scoreColumn = bestArea;
    auto tagLine = scoreColumn.removeFromTop(juce::jmax(18, scoreColumn.getHeight() / 2));
    scoreColumn.removeFromTop(2);

    const auto tagFont = GameUi::font(9.5f, true);
    const int levelWidth = juce::jmin(46, juce::jmax(30, tagLine.getWidth()));
    const int tagGap = 6;

    auto levelSlot = tagLine.removeFromRight(levelWidth);
    const auto levelPill = levelSlot.withSizeKeepingCentre(
        levelWidth, juce::jmin(16, levelSlot.getHeight())).toFloat();

    // The level tag is gold only for a playable row; a locked one is neutral,
    // so the gold in the list always means "you can play this now".
    g.setColour(unlocked ? UiColors::accent.withAlpha(rowIsSelected ? 0.22f : 0.14f)
                         : UiColors::border.withAlpha(0.35f));
    g.fillRoundedRectangle(levelPill, 6.0f);
    g.setColour(unlocked ? UiColors::accent : UiColors::muted);
    g.setFont(tagFont);
    g.drawFittedText("LVL " + juce::String(juce::jlimit(1, 10, song.difficulty)),
                     levelPill.toNearestInt().reduced(3, 0),
                     juce::Justification::centred, 1, 0.8f);

    // Genre tag uses the same pill geometry, immediately left of the level tag.
    const auto genreText = song.genre.trim().toUpperCase();

    if (genreText.isNotEmpty())
    {
        tagLine = tagLine.withTrimmedRight(tagGap);

        const int wanted = juce::roundToInt(
            juce::GlyphArrangement::getStringWidth(tagFont, genreText)) + 14;
        const int genreWidth = juce::jmin(wanted, tagLine.getWidth());

        if (genreWidth >= 30)
        {
            auto genreSlot = tagLine.removeFromRight(genreWidth);
            const auto genrePill = genreSlot.withSizeKeepingCentre(
                genreWidth, juce::jmin(16, genreSlot.getHeight())).toFloat();

            g.setColour(UiColors::border.withAlpha(0.22f));
            g.fillRoundedRectangle(genrePill, 6.0f);
            g.setColour(UiColors::secondaryText);
            g.setFont(tagFont);
            g.drawFittedText(genreText, genrePill.toNearestInt().reduced(4, 0),
                             juce::Justification::centred, 1, 0.75f);
        }
    }

    // Unpublished drafts exist only in the admin preview: the DRAFT badge keeps
    // them apart from the playable catalogue. Same pill geometry as LVL/genre.
    if (! song.published)
    {
        tagLine = tagLine.withTrimmedRight(tagGap);

        const int draftWidth = juce::jmin(48, tagLine.getWidth());

        if (draftWidth >= 34)
        {
            auto draftSlot = tagLine.removeFromRight(draftWidth);
            const auto draftPill = draftSlot.withSizeKeepingCentre(
                draftWidth, juce::jmin(16, draftSlot.getHeight())).toFloat();

            g.setColour(UiColors::red.withAlpha(rowIsSelected ? 0.26f : 0.18f));
            g.fillRoundedRectangle(draftPill, 6.0f);
            g.setColour(UiColors::red);
            g.setFont(tagFont);
            g.drawFittedText("DRAFT", draftPill.toNearestInt().reduced(3, 0),
                             juce::Justification::centred, 1, 0.8f);
        }
    }

    if (! unlocked)
    {
        // Locked rows explain the requirement instead of the best score.
        g.setColour(UiColors::red.withAlpha(0.85f));
        g.setFont(GameUi::font(9.5f, true));
        g.drawFittedText("REQUIRES LVL "
                             + juce::String(owner.requiredPlayerLevelForSong(song.difficulty)),
                         scoreColumn, juce::Justification::centredRight, 1, 0.6f);
        return;
    }

    // Fitted text with a horizontal-scale floor: never truncate the digits.
    g.setColour(best > 0 ? UiColors::accent : UiColors::muted.withAlpha(0.55f));
    g.setFont(GameUi::font(10.0f, true));
    g.drawFittedText(best > 0 ? "BEST: " + juce::String(best) : juce::String("NO SCORE"),
                     scoreColumn, juce::Justification::centredRight, 1, 0.65f);
}

void MainComponent::SongListModel::listBoxItemClicked(int rowNumber, const juce::MouseEvent&)
{
    // Selecting a row only highlights it; START SONG performs the download.
    owner.selectCatalogSong(rowNumber);
}

void MainComponent::SongListModel::selectedRowsChanged(int lastRowSelected)
{
    // Guard against the re-entry caused by selectCatalogSong() selecting the row.
    if (lastRowSelected >= 0 && lastRowSelected != owner.selectedCatalogIndex)
        owner.selectCatalogSong(lastRowSelected);
}

void MainComponent::SongListModel::returnKeyPressed(int rowNumber)
{
    owner.selectCatalogSong(rowNumber);
    owner.startSelectedSong();
}

void MainComponent::updateCatalogRows()
{
    songListBox.updateContent();
    songListBox.repaint();
    updateSongBestScoreLabels();
}

void MainComponent::selectCatalogSong(int index)
{
    if (! juce::isPositiveAndBelow(index, static_cast<int>(catalogSongs.size())))
        return;

    selectedCatalogIndex = index;
    const auto& song = catalogSongs[static_cast<size_t>(index)];

    // Every catalogue row is a server song; the session is opened on START.
    onlineSongUuid = song.serverId;
    onlineGameSessionUuid.clear();

    // Selecting a row only highlights it: audio files and MIDI are loaded when
    // START SONG is pressed, so clicking through the list stays instant.
    updateSongBestScoreLabels();
    songListBox.selectRow(index);
    resized();
    repaint();

    // Selecting another row swaps the preview: the old track stops inside
    // preparePreviewForSelectedSong() before the new file is requested.
    preparePreviewForSelectedSong();

    refreshSongUnlockState();
}

void MainComponent::refreshSongUnlockState()
{
    const bool hasSelection = juce::isPositiveAndBelow(
        selectedCatalogIndex, static_cast<int>(catalogSongs.size()));

    songPridumayButton.setEnabled(
        hasSelection
        && isSongUnlocked(catalogSongs[static_cast<size_t>(selectedCatalogIndex)]));
}

void MainComponent::updateSongBestScoreLabels()
{
    juce::String selectedBestText = "BEST: -";

    if (juce::isPositiveAndBelow(selectedCatalogIndex,
                                 static_cast<int>(catalogSongs.size())))
    {
        const auto& row = catalogSongs[static_cast<size_t>(selectedCatalogIndex)];
        selectedBestText = "BEST: " + juce::String(getBestScoreForSong(row.slug, row.title));
    }

    songPridumayBestLabel.setText(selectedBestText, juce::dontSendNotification);
    songMistBestLabel.setText(juce::String(), juce::dontSendNotification);
    songChordsBestLabel.setText(juce::String(), juce::dontSendNotification);
    dailyChallengeBestLabel.setText(
        "BEST: " + juce::String(getBestScore("daily_challenge_monday")),
        juce::dontSendNotification);

    // Row text is resolved at paint time; a repaint is enough to show a new best.
    songListBox.repaint();
}

// Width of the shared GuitarInputMonitor card shown on the song screen. Kept
// in one place so paint and layout always agree.
static int inputMonitorCardWidth(int windowWidth)
{
    return juce::jlimit(340, 470, windowWidth * 28 / 100);
}

void MainComponent::paint(juce::Graphics& g){
    const auto area = getLocalBounds().toFloat();
    g.fillAll(UiColors::background);

    auto drawGlow = [&g, &area](float centreX, float centreY, float radius,
                                juce::Colour colour)
    {
        juce::ColourGradient glow(colour, centreX, centreY,
                                  colour.withAlpha(0.0f),
                                  centreX, centreY + radius, true);
        g.setGradientFill(glow);
        g.fillRect(area);
    };

    drawGlow(area.getWidth() * 0.10f, -area.getHeight() * 0.08f,
             area.getHeight() * 0.78f, UiColors::accent.withAlpha(0.115f));
    drawGlow(area.getWidth() * 0.94f, area.getHeight() * 0.03f,
             area.getHeight() * 0.62f, UiColors::blue.withAlpha(0.065f));
    drawGlow(area.getWidth() * 0.50f, area.getHeight() * 1.08f,
             area.getHeight() * 0.48f, UiColors::green.withAlpha(0.035f));

    juce::ColourGradient vignette(
        juce::Colours::transparentBlack, 0.0f, area.getHeight() * 0.44f,
        UiColors::bgSoft.withAlpha(0.82f), 0.0f, area.getHeight(), false);
    g.setGradientFill(vignette);
    g.fillRect(area);
}

void MainOverlayComponent::paint(juce::Graphics& g)
{
    owner.paintOverlay(g);
}

void MainComponent::paintOverlay(juce::Graphics& g)
{
    const float width = static_cast<float>(getWidth());
    const float height = static_cast<float>(getHeight());

    if (width <= 0.0f || height <= 0.0f)
        return;

    auto drawBrand = [&g](float x, float y)
    {
        BrandAssets::drawLogo(g, { x, y, 32.0f, 32.0f });

        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(16.0f));
        g.drawText("FRETFIGHT",
                   juce::Rectangle<float>(x + 44.0f, y + 1.0f, 190.0f, 30.0f),
                   juce::Justification::centredLeft, false);
    };

    //==========================================================================
    // Authentication / landing screen
    //==========================================================================

    if (gameState == GameState::SongSelect && showAuthScreen)
    {
        drawBrand(32.0f, 26.0f);

        const float cardWidth = juce::jmin(560.0f, width - 48.0f);
        const float cardHeight = 364.0f;
        const float cardX = (width - cardWidth) * 0.5f;
        const float cardY = juce::jmax(150.0f, (height - cardHeight) * 0.5f);
        const juce::Rectangle<float> card(cardX, cardY, cardWidth, cardHeight);

        GameUi::drawCard(g, card, 24.0f, true);
        g.setColour(UiColors::accent);
        g.fillRoundedRectangle(cardX + 24.0f, cardY, 78.0f, 3.0f, 1.5f);

        const float iconSize = 58.0f;
        const juce::Rectangle<float> icon(cardX + (cardWidth - iconSize) * 0.5f,
                                          cardY + 30.0f, iconSize, iconSize);
        g.setColour(UiColors::accent.withAlpha(0.08f));
        g.fillEllipse(icon.expanded(8.0f));
        BrandAssets::drawLogo(g, icon);

        g.setColour(UiColors::accent);
        g.setFont(GameUi::eyebrow(10.0f));
        g.drawText("PLAYER ACCESS",
                   juce::Rectangle<float>(cardX, cardY + 94.0f, cardWidth, 16.0f),
                   juce::Justification::centred, false);

        GameUi::drawDivider(g, cardX + 42.0f, cardX + cardWidth - 42.0f,
                            cardY + 178.0f);

        return;
    }

    //==========================================================================
    // Song select
    //==========================================================================

    if (gameState == GameState::SongSelect)
    {
        const juce::Rectangle<float> nav(24.0f, 20.0f, width - 48.0f, 78.0f);
        GameUi::drawCard(g, nav, 18.0f);
        drawBrand(44.0f, 40.0f);

        if (currentUsername.isNotEmpty() && currentUserLabel.isVisible())
        {
            const auto userBounds = currentUserLabel.getBounds().toFloat();
            const float avatarSize = 30.0f;
            const float avatarX = userBounds.getX() - avatarSize - 8.0f;

            // On narrow windows the account row needs all of its horizontal
            // space for the three equally-sized buttons. The avatar is
            // decorative, so drop it before it can collide with the brand.
            if (avatarX > 286.0f)
            {
                const juce::Rectangle<float> avatar(avatarX,
                                                    userBounds.getY() - 6.0f,
                                                    avatarSize, avatarSize);
                g.setColour(UiColors::accent.withAlpha(0.16f));
                g.fillEllipse(avatar);
                g.setColour(UiColors::accent.withAlpha(0.55f));
                g.drawEllipse(avatar, 1.0f);
                g.setColour(UiColors::accent);
                g.setFont(GameUi::heading(13.0f));
                g.drawText(currentUsername.substring(0, 1).toUpperCase(),
                           avatar, juce::Justification::centred, false);
            }

            g.setColour(UiColors::muted);
            g.setFont(GameUi::eyebrow(8.0f));
            g.drawText("SIGNED IN AS",
                       juce::Rectangle<float>(userBounds.getX(),
                                              userBounds.getY() - 13.0f,
                                              userBounds.getWidth(), 13.0f),
                       juce::Justification::centredLeft, false);
        }

        const float contentY = 250.0f;
        const float availableH = juce::jmax(320.0f, height - 68.0f - 20.0f - contentY);
        // Fill the window instead of capping the content height: on large
        // displays the old 560 px cap left the lower third of the screen empty.
        const float contentH = availableH;
        const float footerY = contentY + contentH + 20.0f;
        const float contentW = width - 96.0f;
        const float gap = 20.0f;
        const float leftW = contentW * 0.56f;
        const float rightX = 48.0f + leftW + gap;
        const float rightW = contentW - leftW - gap;

        // Level panel: current level, XP earned inside the level, the cost of
        // the next level, what is left and an animated progress bar. Every
        // number comes from the server-side progression payload; the bar is a
        // clamped view of the same ratio, so text and fill never disagree.
        {
            const juce::Rectangle<float> panel(
                48.0f, 108.0f,
                width - 96.0f - inputMonitorCardWidth(juce::roundToInt(width)) - 14.0f,
                124.0f);
            GameUi::drawCard(g, panel, 18.0f, true);

            const auto inner = panel.reduced(24.0f, 16.0f);

            const int xpForNext = juce::jmax(1, progressionXpForNext);
            const int xpRemaining = juce::jmax(0, progressionXpRemaining);
            const int xpIntoLevel =
                juce::jlimit(0, xpForNext, xpForNext - xpRemaining);
            const float ratio = juce::jlimit(0.0f, 1.0f, levelBarShown);

            // Left column: identity of the block.
            const float leftColumnW =
                juce::jmin(320.0f, inner.getWidth() * 0.42f);

            g.setColour(UiColors::accent);
            g.setFont(GameUi::eyebrow(10.0f));
            g.drawText("PLAYER LEVEL",
                       juce::Rectangle<float>(inner.getX(), inner.getY(),
                                              leftColumnW, 14.0f),
                       juce::Justification::centredLeft, false);

            g.setColour(UiColors::text);
            g.setFont(GameUi::heading(30.0f));
            g.drawText("LEVEL " + juce::String(progressionLevel),
                       juce::Rectangle<float>(inner.getX(), inner.getY() + 14.0f,
                                              leftColumnW, 38.0f),
                       juce::Justification::centredLeft, false);

            g.setColour(UiColors::muted);
            g.setFont(GameUi::font(12.5f));
            g.drawFittedText("Current XP: " + juce::String(xpIntoLevel)
                                 + "   |   Total XP: " + juce::String(progressionXp),
                             juce::Rectangle<int>(static_cast<int>(inner.getX()),
                                                  static_cast<int>(inner.getY() + 54.0f),
                                                  static_cast<int>(leftColumnW), 18),
                             juce::Justification::centredLeft, 1);

            g.setColour(UiColors::muted);
            g.setFont(GameUi::font(12.5f));
            g.drawFittedText("Rating " + juce::String(ratingPoints)
                                 + "   |   Achievements "
                                 + juce::String(progressionAchievements) + "/"
                                 + juce::String(progressionAchievementsTotal),
                             juce::Rectangle<int>(static_cast<int>(inner.getX()),
                                                  static_cast<int>(inner.getY() + 74.0f),
                                                  static_cast<int>(leftColumnW), 18),
                             juce::Justification::centredLeft, 1);

            // Right column: numbers above a wide bar.
            const float barX = inner.getX() + leftColumnW + 24.0f;
            const float barW = inner.getRight() - barX;
            const float barY = inner.getY() + 48.0f;
            const float barH = 30.0f;

            if (barW > 120.0f)
            {
                const juce::String progressText =
                    juce::String(xpIntoLevel) + " / " + juce::String(xpForNext) + " XP";

                g.setColour(UiColors::text);
                g.setFont(GameUi::font(15.0f, true));
                g.drawText(progressText,
                           juce::Rectangle<float>(barX, inner.getY() + 16.0f,
                                                  barW, 24.0f),
                           juce::Justification::centredLeft, false);

                const float progressWidth = juce::GlyphArrangement::getStringWidth(
                    g.getCurrentFont(), progressText);

                g.setColour(UiColors::muted);
                g.setFont(GameUi::font(12.5f));
                g.drawText(juce::String(xpRemaining) + " XP TO NEXT LEVEL   |   "
                               + juce::String(juce::roundToInt(ratio * 100.0f)) + "%",
                           juce::Rectangle<float>(barX + progressWidth + 28.0f,
                                                  inner.getY() + 16.0f,
                                                  juce::jmax(120.0f, barW - progressWidth - 28.0f),
                                                  24.0f),
                           juce::Justification::centredLeft, false);

                g.setColour(UiColors::border);
                g.fillRoundedRectangle(barX, barY, barW, barH, barH * 0.5f);

                const float filled = ratio * barW;

                if (filled > 1.0f)
                {
                    g.setColour(UiColors::accent);
                    g.fillRoundedRectangle(barX, barY, filled, barH, barH * 0.5f);
                }
            }
        }

        const juce::Rectangle<float> leftCard(48.0f, contentY, leftW, contentH);
        const juce::Rectangle<float> rightCard(rightX, contentY, rightW, contentH);
        GameUi::drawCard(g, leftCard, 20.0f, true);
        GameUi::drawCard(g, rightCard, 20.0f);

        // Song artwork. The server stores only an optional cover key and the
        // client does not download cover art yet, so the brand icon stands in
        // for every song: it is the same picture the executable and the
        // desktop shortcut carry.
        const juce::Rectangle<float> art(leftCard.getX() + 24.0f,
                                         leftCard.getY() + 24.0f,
                                         118.0f, 118.0f);

        if (const auto& artwork = BrandAssets::getAppIconImage(); artwork.isValid())
        {
            // drawImage multiplies the artwork by the alpha of the current
            // colour, and the card left a 3.5% white brush behind. Reset it to
            // an opaque brush so the icon is drawn at full strength.
            g.setColour(juce::Colours::white);
            g.drawImage(artwork, art, juce::RectanglePlacement::fillDestination);
        }
        else
        {
            // Fallback when the icon cannot be decoded: the old gold plate.
            juce::ColourGradient artGradient(
                UiColors::accentStrong, art.getX(), art.getY(),
                UiColors::accent.darker(0.45f), art.getRight(), art.getBottom(),
                false);
            g.setGradientFill(artGradient);
            g.fillRoundedRectangle(art, 18.0f);
            g.setColour(UiColors::onAccent.withAlpha(0.12f));
            g.drawRoundedRectangle(art.reduced(1.0f), 18.0f, 1.0f);
            g.setColour(UiColors::onAccent);
            g.setFont(GameUi::heading(40.0f));
            g.drawText("FF", art, juce::Justification::centred, false);
        }

        // The header shows whichever catalogue row is highlighted, so the card
        // always previews the song START SONG will load.
        const CatalogSong* headerSong = juce::isPositiveAndBelow(
            selectedCatalogIndex, static_cast<int>(catalogSongs.size()))
            ? &catalogSongs[static_cast<size_t>(selectedCatalogIndex)]
            : nullptr;

        const juce::String headerTitle = headerSong != nullptr
            ? headerSong->title
            : juce::String("NO SONG SELECTED");
        const juce::String headerKind = headerSong != nullptr
            ? juce::String("ONLINE SONG")
            : juce::String("SERVER CATALOG");
        const double headerBpm = headerSong != nullptr ? headerSong->bpm : baseBpm;

        const juce::String headerMeter = headerSong != nullptr
            ? formatTimeSignature(headerSong->timeSignatureNumerator,
                                  headerSong->timeSignatureDenominator)
            : juce::String("4/4");

        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(10.0f, true));
        g.drawText(headerKind + "  /  " + juce::String(juce::roundToInt(headerBpm)) + " BPM"
                       + "  /  " + headerMeter,
                   juce::Rectangle<float>(art.getRight() + 20.0f,
                                          leftCard.getY() + 26.0f,
                                          leftW - 360.0f, 16.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(30.0f));
        g.drawText(headerTitle,
                   juce::Rectangle<float>(art.getRight() + 20.0f,
                                          leftCard.getY() + 42.0f,
                                          leftW - 360.0f, 44.0f),
                   juce::Justification::centredLeft, true);

        GameUi::drawDivider(g, leftCard.getX() + 24.0f,
                            leftCard.getRight() - 24.0f,
                            leftCard.getY() + 164.0f);

        // Catalogue heading: the ListBox below paints the rows themselves.
        GameUi::drawEyebrow(g, "SONG CATALOG",
                            juce::Rectangle<float>(leftCard.getX() + 24.0f,
                                                   leftCard.getY() + 170.0f,
                                                   leftW - 48.0f, 14.0f));
        g.setColour(UiColors::muted.withAlpha(0.8f));
        g.setFont(GameUi::font(9.0f, true));
        g.drawText(juce::String(static_cast<int>(catalogSongs.size())) + " TRACKS",
                   juce::Rectangle<float>(leftCard.getX() + 24.0f,
                                          leftCard.getY() + 170.0f,
                                          leftW - 48.0f, 14.0f),
                   juce::Justification::centredRight, false);

        g.setColour(UiColors::accent.withAlpha(0.75f));
        g.setFont(GameUi::eyebrow(9.0f));
        g.drawText("GLOBAL LEADERBOARD",
                   juce::Rectangle<float>(rightCard.getX() + 24.0f,
                                          rightCard.getY() + 24.0f,
                                          rightW - 48.0f, 16.0f),
                   juce::Justification::centredLeft, false);
        GameUi::drawDivider(g, rightCard.getX() + 24.0f,
                            rightCard.getRight() - 24.0f,
                            rightCard.getY() + 104.0f);

        if (leaderboardLabel.getText().isEmpty())
        {
            const float centreX = rightCard.getCentreX();
            const float iconY = rightCard.getCentreY() - 34.0f;
            g.setColour(UiColors::accent.withAlpha(0.08f));
            g.fillEllipse(centreX - 44.0f, iconY - 44.0f, 88.0f, 88.0f);
            g.setColour(UiColors::accent.withAlpha(0.32f));
            g.drawEllipse(centreX - 44.0f, iconY - 44.0f, 88.0f, 88.0f, 1.0f);

            g.setColour(UiColors::accent.withAlpha(0.82f));
            g.fillRoundedRectangle(centreX - 24.0f, iconY + 4.0f, 13.0f, 24.0f, 3.0f);
            g.fillRoundedRectangle(centreX - 7.0f, iconY - 8.0f, 13.0f, 36.0f, 3.0f);
            g.fillRoundedRectangle(centreX + 10.0f, iconY - 20.0f, 13.0f, 48.0f, 3.0f);

            g.setColour(UiColors::text);
            g.setFont(GameUi::font(13.0f, true));
            g.drawText("NO SCORES YET",
                       juce::Rectangle<float>(centreX - 160.0f, iconY + 62.0f,
                                              320.0f, 20.0f),
                       juce::Justification::centred, false);
            g.setColour(UiColors::muted);
            g.setFont(GameUi::font(11.0f));
            g.drawText("Press REFRESH to load the global rating.",
                       juce::Rectangle<float>(centreX - 180.0f, iconY + 84.0f,
                                              360.0f, 18.0f),
                       juce::Justification::centred, false);
        }

        // Footer status strip.
        const juce::Rectangle<float> footer(24.0f, footerY, width - 48.0f, 44.0f);
        g.setColour(UiColors::surface.withAlpha(0.62f));
        g.fillRoundedRectangle(footer, 14.0f);
        g.setColour(UiColors::border);
        g.drawRoundedRectangle(footer, 14.0f, 1.0f);
        g.setColour(currentUsername.isNotEmpty() ? UiColors::green : UiColors::accent);
        g.fillEllipse(footer.getX() + 18.0f, footer.getY() + 18.0f, 7.0f, 7.0f);

        return;
    }

    //==========================================================================
    // Tuner
    //==========================================================================

    if (gameState == GameState::Tuner)
    {
        drawBrand(42.0f, 24.0f);
        const int tunedMaskTop = tuner.getTunedMask();
        const int activeStrings = juce::jmax(1, tuner.getActiveStringCount());
        int tunedCountTop = 0;
        for (int slot = 0; slot < currentTuning.count; ++slot)
            tunedCountTop += (tunedMaskTop & (1 << slot)) != 0 ? 1 : 0;
        const bool allTunedTop = tunedCountTop >= activeStrings;

        GameUi::drawPill(g,
                         juce::Rectangle<float>(width - 178.0f, 29.0f, 136.0f, 22.0f),
                         allTunedTop ? juce::String("READY")
                                     : "TUNING " + juce::String(tunedCountTop)
                                           + "/" + juce::String(activeStrings),
                         allTunedTop ? UiColors::green : UiColors::accent,
                         allTunedTop);

        GameUi::drawEyebrow(g, "PRE-FLIGHT CHECK",
                            juce::Rectangle<float>(42.0f, 82.0f, 360.0f, 16.0f));
        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(34.0f));
        g.drawText("Tune your guitar",
                   juce::Rectangle<float>(42.0f, 100.0f, width - 84.0f, 44.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(13.0f));
        const juce::String tuningHint =
            currentTuningName + " TUNING  /  "
            + GuitarTuning::label(currentTuning, false)
            + ". Play each string and wait for the green light.";
        g.drawText(tuningHint,
                   juce::Rectangle<float>(42.0f, 148.0f, width - 84.0f, 24.0f),
                   juce::Justification::centredLeft, false);

        const float margin = 42.0f;
        const float gap = 20.0f;
        const float panelY = 190.0f;
        const float panelH = juce::jmin(560.0f,
                                        juce::jmax(380.0f, height - 190.0f - 120.0f));
        const float leftW = (width - 84.0f - gap) * 0.44f;
        const float rightX = margin + leftW + gap;
        const float rightW = width - margin - rightX;
        const juce::Rectangle<float> leftPanel(margin, panelY, leftW, panelH);
        const juce::Rectangle<float> rightPanel(rightX, panelY, rightW, panelH);
        GameUi::drawCard(g, leftPanel, 20.0f);
        GameUi::drawCard(g, rightPanel, 20.0f, true);

        g.setColour(UiColors::accent);
        g.setFont(GameUi::eyebrow(10.0f));
        g.drawText("STRINGS",
                   juce::Rectangle<float>(leftPanel.getX() + 24.0f,
                                          leftPanel.getY() + 24.0f,
                                          leftW - 48.0f, 16.0f),
                   juce::Justification::centredLeft, false);
        const int stringSlots = juce::jlimit(1, GuitarTuning::maxStrings,
                                             currentTuning.count);
        // Six strings keep the familiar 3x2 grid; a seven- or eight-string
        // instrument uses four columns so the tiles still fit above the input
        // monitor without shrinking the note combos.
        const int tileColumns = stringSlots > 6 ? 4 : 3;
        const int tunedMask = tuner.getTunedMask();
        const float tileGap = 12.0f;
        const float tileW = (leftW - 48.0f - tileGap * static_cast<float>(tileColumns - 1))
            / static_cast<float>(tileColumns);
        const float tileH = 112.0f;

        for (int slot = 0; slot < stringSlots; ++slot)
        {
            const int column = slot % tileColumns;
            const int row = slot / tileColumns;
            const float x = leftPanel.getX() + 24.0f
                + static_cast<float>(column) * (tileW + tileGap);
            const float y = leftPanel.getY() + 82.0f
                + static_cast<float>(row) * (tileH + tileGap);
            const bool active = GuitarTuning::isActive(currentTuning, slot);
            const bool tuned = active && (tunedMask & (1 << slot)) != 0;
            const auto colour = tuned ? UiColors::green
                                      : (active ? UiColors::accent : UiColors::muted);

            g.setColour(tuned ? UiColors::green.withAlpha(0.10f)
                              : UiColors::surface2.withAlpha(active ? 0.72f : 0.34f));
            g.fillRoundedRectangle(x, y, tileW, tileH, 14.0f);
            g.setColour(colour.withAlpha(tuned ? 0.46f : 0.22f));
            g.drawRoundedRectangle(x, y, tileW, tileH, 14.0f, 1.0f);

            // The note itself is a real ComboBox placed over the tile by
            // resized(); the paint layer only provides the card and status.
            g.setColour(UiColors::muted);
            g.setFont(GameUi::eyebrow(8.5f));
            g.drawText("STRING " + juce::String(GuitarTuning::stringNumber(currentTuning, slot)),
                       juce::Rectangle<float>(x, y + 52.0f, tileW, 16.0f),
                       juce::Justification::centred, false);
            GameUi::drawPill(g,
                             juce::Rectangle<float>(x + 10.0f, y + 77.0f,
                                                    tileW - 20.0f, 20.0f),
                             active ? (tuned ? juce::String("IN TUNE")
                                             : juce::String("WAITING"))
                                    : juce::String("EMPTY"),
                             colour,
                             tuned);
        }

        if (panelH > 470.0f && panelH <= 520.0f)
        {
            g.setColour(UiColors::muted.withAlpha(0.72f));
            g.setFont(GameUi::font(11.0f));
            g.drawText("Play each string clearly and let the note ring.",
                       juce::Rectangle<float>(leftPanel.getX() + 24.0f,
                                              leftPanel.getBottom() - 48.0f,
                                              leftW - 48.0f, 22.0f),
                       juce::Justification::centred, false);
        }


        if (panelH > 520.0f)
        {
            const bool allReady = tuner.areAllStringsTuned();
            int tunedCount = 0;
            for (int slot = 0; slot < currentTuning.count; ++slot)
                tunedCount += (tunedMask & (1 << slot)) != 0 ? 1 : 0;

            const auto readyColour = allReady ? UiColors::green : UiColors::accent;
            const juce::Rectangle<float> readyCard(
                leftPanel.getX() + 24.0f, leftPanel.getY() + 474.0f,
                leftW - 48.0f, 72.0f);
            g.setColour(UiColors::surface2.withAlpha(0.56f));
            g.fillRoundedRectangle(readyCard, 14.0f);
            g.setColour(readyColour.withAlpha(0.42f));
            g.drawRoundedRectangle(readyCard, 14.0f, 1.0f);

            const juce::Rectangle<float> readyIcon(
                readyCard.getX() + 16.0f, readyCard.getCentreY() - 15.0f,
                30.0f, 30.0f);
            g.setColour(readyColour.withAlpha(0.16f));
            g.fillEllipse(readyIcon);
            g.setColour(readyColour);
            g.setFont(GameUi::font(9.5f, true));
            g.drawText(allReady ? juce::String("OK") : juce::String(tunedCount),
                       readyIcon.toNearestInt(), juce::Justification::centred, false);

            g.setColour(UiColors::text);
            g.setFont(GameUi::font(12.5f, true));
            g.drawText(allReady ? juce::String("ALL STRINGS READY")
                                : juce::String(tunedCount) + " / "
                                      + juce::String(activeStrings) + " STRINGS READY",
                       juce::Rectangle<float>(readyCard.getX() + 58.0f,
                                              readyCard.getY() + 14.0f,
                                              readyCard.getWidth() - 72.0f, 18.0f),
                       juce::Justification::centredLeft, false);
            g.setColour(UiColors::muted);
            g.setFont(GameUi::font(10.5f));
            g.drawText(allReady ? "All strings are in tune."
                                : "Keep tuning the remaining strings.",
                       juce::Rectangle<float>(readyCard.getX() + 58.0f,
                                              readyCard.getY() + 38.0f,
                                              readyCard.getWidth() - 72.0f, 16.0f),
                       juce::Justification::centredLeft, false);
        }

        // Tuner arc: a shallow gauge from flat (b) to sharp (#). The triangle
        // marks the target note at the top; the gold/green bar shows how far
        // the detected pitch is from it. The note itself is a real Label.
        const float dialX = rightPanel.getCentreX();
        const float dialY = rightPanel.getY() + 196.0f;
        const float dialRadius = juce::jmin(120.0f, rightW * 0.30f);
        constexpr float startDeg = 200.0f;
        constexpr float endDeg = 340.0f;
        constexpr int dashCount = 13;
        constexpr float dashLength = 11.0f;
        constexpr float dashThickness = 7.0f;

        const bool hasReading = tunerDetectedString >= 0;
        const double cents = juce::jlimit(-50.0, 50.0, tunerCents);
        const bool inTune = hasReading && std::abs(cents) <= 10.0;

        for (int i = 0; i < dashCount; ++i)
        {
            const float t = static_cast<float>(i)
                / static_cast<float>(dashCount - 1);
            const float angleDeg = startDeg + t * (endDeg - startDeg);
            const float angle = juce::degreesToRadians(angleDeg);
            const float x = dialX + dialRadius * std::cos(angle);
            const float y = dialY + dialRadius * std::sin(angle);

            juce::Path dash;
            dash.addRoundedRectangle(-dashLength * 0.5f,
                                     -dashThickness * 0.5f,
                                     dashLength, dashThickness,
                                     dashThickness * 0.5f);
            dash.applyTransform(
                juce::AffineTransform::rotation(
                    angle + juce::MathConstants<float>::halfPi)
                    .translated(x, y));

            g.setColour(UiColors::muted.withAlpha(
                i == dashCount / 2 ? 0.55f : 0.30f));
            g.fillPath(dash);
        }

        // Target pointer at the top: green once the string is in tune.
        const float triangleTipY = dialY - dialRadius - 4.0f;
        juce::Path triangle;
        triangle.addTriangle(dialX, triangleTipY,
                             dialX - 9.0f, triangleTipY - 16.0f,
                             dialX + 9.0f, triangleTipY - 16.0f);
        g.setColour(inTune ? UiColors::green : UiColors::muted);
        g.fillPath(triangle);

        // Flat / sharp ends of the scale.
        const float leftAngle = juce::degreesToRadians(startDeg);
        const float rightAngle = juce::degreesToRadians(endDeg);
        const float labelRadius = dialRadius + 24.0f;
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(18.0f, true));
        g.drawText("b",
                   juce::Rectangle<float>(
                       dialX + labelRadius * std::cos(leftAngle) - 14.0f,
                       dialY + labelRadius * std::sin(leftAngle) - 12.0f,
                       28.0f, 24.0f),
                   juce::Justification::centred, false);
        g.drawText("#",
                   juce::Rectangle<float>(
                       dialX + labelRadius * std::cos(rightAngle) - 14.0f,
                       dialY + labelRadius * std::sin(rightAngle) - 12.0f,
                       28.0f, 24.0f),
                   juce::Justification::centred, false);

        // Current pitch marker: moves from b to #, green when in tune.
        const float markerDeg = 270.0f
            + static_cast<float>(cents / 50.0) * 70.0f;
        const float markerAngle = juce::degreesToRadians(markerDeg);
        const float markerX = dialX + dialRadius * std::cos(markerAngle);
        const float markerY = dialY + dialRadius * std::sin(markerAngle);

        juce::Path marker;
        marker.addRoundedRectangle(-9.0f, -5.0f, 18.0f, 10.0f, 4.0f);
        marker.applyTransform(
            juce::AffineTransform::rotation(
                markerAngle + juce::MathConstants<float>::halfPi)
                .translated(markerX, markerY));
        g.setColour(hasReading
                        ? (inTune ? UiColors::green : UiColors::accent)
                        : UiColors::muted.withAlpha(0.55f));
        g.fillPath(marker);

        GameUi::drawDivider(g, rightPanel.getX() + 24.0f,
                            rightPanel.getRight() - 24.0f,
                            rightPanel.getY() + 352.0f);

        g.setColour(UiColors::muted);
        g.setFont(GameUi::eyebrow(8.5f));
        g.drawText("DETECTED STRING",
                   juce::Rectangle<float>(rightPanel.getX() + 24.0f,
                                          rightPanel.getY() + 286.0f,
                                          220.0f, 15.0f),
                   juce::Justification::centredLeft, false);
        g.drawText("OFFSET",
                   juce::Rectangle<float>(rightPanel.getX() + 24.0f,
                                          rightPanel.getY() + 318.0f,
                                          150.0f, 15.0f),
                   juce::Justification::centredLeft, false);

        const auto statusColour = tuner.areAllStringsTuned()
            ? UiColors::green : UiColors::accent;
        g.setColour(statusColour.withAlpha(0.10f));
        g.fillRoundedRectangle(rightPanel.getX() + 40.0f,
                               rightPanel.getY() + 368.0f,
                               rightW - 80.0f, 40.0f, 20.0f);
        g.setColour(statusColour.withAlpha(0.38f));
        g.drawRoundedRectangle(rightPanel.getX() + 40.0f,
                               rightPanel.getY() + 368.0f,
                               rightW - 80.0f, 40.0f, 20.0f, 1.0f);

        if (panelH > 520.0f)
        {
            GameUi::drawDivider(g, rightPanel.getX() + 24.0f,
                                rightPanel.getRight() - 24.0f,
                                rightPanel.getY() + 432.0f);
            GameUi::drawEyebrow(g, "QUICK TIPS",
                                juce::Rectangle<float>(rightPanel.getX() + 24.0f,
                                                       rightPanel.getY() + 448.0f,
                                                       rightW - 48.0f, 14.0f),
                                UiColors::muted);
            g.setColour(UiColors::muted.withAlpha(0.82f));
            g.setFont(GameUi::font(11.0f));
            g.drawText("Pluck one string at a time.",
                       juce::Rectangle<float>(rightPanel.getX() + 24.0f,
                                              rightPanel.getY() + 468.0f,
                                              rightW - 48.0f, 18.0f),
                       juce::Justification::centredLeft, false);
            g.drawText("Let the note ring before reading.",
                       juce::Rectangle<float>(rightPanel.getX() + 24.0f,
                                              rightPanel.getY() + 488.0f,
                                              rightW - 48.0f, 18.0f),
                       juce::Justification::centredLeft, false);
        }

        if (panelH > 500.0f)
        {
            g.setColour(UiColors::muted.withAlpha(0.65f));
            g.setFont(GameUi::font(10.5f));
            g.drawText("TIP  /  USE THE NECK PICKUP AND MUTE THE OTHER STRINGS",
                       juce::Rectangle<float>(rightPanel.getX() + 24.0f,
                                              rightPanel.getBottom() - 48.0f,
                                              rightW - 48.0f, 22.0f),
                       juce::Justification::centred, false);
        }

        return;
    }

    //==========================================================================
    // Results
    //==========================================================================

    if (gameState == GameState::Finished)
    {
        drawBrand(48.0f, 32.0f);
        GameUi::drawPill(g,
                         juce::Rectangle<float>(width - 154.0f, 36.0f, 106.0f, 22.0f),
                         "RESULTS",
                         UiColors::accent,
                         true);
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(10.5f));
        g.drawText("BEST ON THIS TRACK  " + juce::String(getBestScoreForSong(selectedSongSlug, selectedSongId)),
                   juce::Rectangle<float>(width - 460.0f, 36.0f, 278.0f, 22.0f),
                   juce::Justification::centredRight, false);

        const int score = gameplayStats.getScore();
        const int hits = gameplayStats.getHitCount();
        const int misses = gameplayStats.getMissCount();
        const int total = hits + misses;
        const double accuracy = total > 0
            ? 100.0 * static_cast<double>(hits) / static_cast<double>(total)
            : 0.0;

        const juce::Rectangle<float> card(
            48.0f, 82.0f, width - 96.0f,
            juce::jmax(520.0f, height - 170.0f));
        GameUi::drawCard(g, card, 24.0f, true);

        GameUi::drawEyebrow(g, "SESSION COMPLETE",
                            juce::Rectangle<float>(card.getX() + 32.0f,
                                                   card.getY() + 26.0f,
                                                   340.0f, 16.0f));
        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(38.0f));
        g.drawText("SONG COMPLETE",
                   juce::Rectangle<float>(card.getX() + 32.0f,
                                          card.getY() + 46.0f,
                                          card.getWidth() - 64.0f, 48.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(13.0f));
        g.drawText("Here is how your run landed. Replay the track to push the score higher.",
                   juce::Rectangle<float>(card.getX() + 32.0f,
                                          card.getY() + 92.0f,
                                          card.getWidth() - 64.0f, 22.0f),
                   juce::Justification::centredLeft, false);

        if (currentSessionIsPreview)
        {
            // The results screen repeats the notice: a draft attempt is saved
            // as history only and never becomes a best score or rating row.
            GameUi::drawPill(g,
                             juce::Rectangle<float>(card.getRight() - 232.0f,
                                                    card.getY() + 26.0f,
                                                    200.0f, 24.0f),
                             "PREVIEW - NOT RANKED",
                             UiColors::red,
                             false);
        }

        const juce::Rectangle<float> scorePanel(
            card.getX() + 32.0f, card.getY() + 132.0f,
            card.getWidth() - 64.0f, 176.0f);
        juce::ColourGradient scoreGradient(
            UiColors::accentStrong, scorePanel.getX(), scorePanel.getY(),
            UiColors::accent.darker(0.46f), scorePanel.getRight(), scorePanel.getBottom(),
            false);
        g.setGradientFill(scoreGradient);
        g.fillRoundedRectangle(scorePanel, 20.0f);
        g.setColour(juce::Colours::white.withAlpha(0.16f));
        g.drawRoundedRectangle(scorePanel, 20.0f, 1.0f);

        g.setColour(UiColors::onAccent.withAlpha(0.72f));
        g.setFont(GameUi::eyebrow(9.5f));
        g.drawText("TOTAL SCORE",
                   juce::Rectangle<float>(scorePanel.getX() + 24.0f,
                                          scorePanel.getY() + 20.0f,
                                          300.0f, 16.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::onAccent);
        g.setFont(GameUi::heading(72.0f));
        g.drawText(juce::String(score),
                   juce::Rectangle<float>(scorePanel.getX() + 24.0f,
                                          scorePanel.getY() + 38.0f,
                                          scorePanel.getWidth() - 48.0f, 92.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::onAccent.withAlpha(0.70f));
        g.setFont(GameUi::font(12.0f));
        g.drawText("POINTS",
                   juce::Rectangle<float>(scorePanel.getX() + 26.0f,
                                          scorePanel.getY() + 134.0f,
                                          180.0f, 20.0f),
                   juce::Justification::centredLeft, false);

        const int columnWidth = juce::jmax(120, (juce::roundToInt(card.getWidth()) - 88) / 3);
        const float gridX = card.getX() + 32.0f;
        const float gridY = card.getY() + 334.0f;
        const std::array<juce::String, 6> values
        {
            juce::String(juce::roundToInt(accuracy)) + "%",
            juce::String(hits),
            juce::String(misses),
            juce::String(gameplayStats.getPerfectCount()),
            juce::String(gameplayStats.getGreatCount()),
            juce::String(gameplayStats.getGoodCount())
        };
        const std::array<juce::String, 6> captions
        {
            "ACCURACY", "HITS", "MISSES", "PERFECT", "GREAT", "GOOD"
        };

        for (int i = 0; i < 6; ++i)
        {
            const int column = i % 3;
            const int row = i / 3;
            const float x = gridX + static_cast<float>(column) * static_cast<float>(columnWidth);
            const float y = gridY + static_cast<float>(row) * 94.0f;
            const juce::Rectangle<float> tile(x, y, static_cast<float>(columnWidth) - 12.0f, 82.0f);
            g.setColour(UiColors::surface2.withAlpha(0.72f));
            g.fillRoundedRectangle(tile, 14.0f);
            g.setColour(UiColors::border);
            g.drawRoundedRectangle(tile, 14.0f, 1.0f);

            auto valueColour = UiColors::text;
            if (i == 2) valueColour = UiColors::red;
            else if (i == 3) valueColour = UiColors::accent;
            else if (i == 4) valueColour = UiColors::green;
            else if (i == 5) valueColour = UiColors::info;

            g.setColour(valueColour);
            g.setFont(GameUi::heading(28.0f));
            g.drawText(values[static_cast<size_t>(i)],
                       juce::Rectangle<float>(tile.getX() + 16.0f,
                                              tile.getY() + 8.0f,
                                              tile.getWidth() - 32.0f, 38.0f),
                       juce::Justification::centredLeft, false);
            g.setColour(UiColors::muted);
            g.setFont(GameUi::eyebrow(8.5f));
            g.drawText(captions[static_cast<size_t>(i)],
                       juce::Rectangle<float>(tile.getX() + 16.0f,
                                              tile.getY() + 50.0f,
                                              tile.getWidth() - 32.0f, 16.0f),
                       juce::Justification::centredLeft, false);
        }

        GameUi::drawPill(g,
                         juce::Rectangle<float>(card.getX() + 32.0f,
                                                card.getY() + 548.0f,
                                                180.0f, 30.0f),
                         "MAX COMBO  x" + juce::String(gameplayStats.getMaxCombo()),
                         UiColors::accent,
                         true);
        GameUi::drawPill(g,
                         juce::Rectangle<float>(card.getX() + 224.0f,
                                                card.getY() + 548.0f,
                                                210.0f, 30.0f),
                         "COMBO BONUS  +" + juce::String(gameplayStats.getComboBonusPoints()),
                         UiColors::green,
                         false);

        // Progression earned by this attempt (server-authoritative).
        if (lastProgressionValid && card.getHeight() > 640.0f)
        {
            GameUi::drawPill(g,
                             juce::Rectangle<float>(card.getX() + 32.0f,
                                                    card.getY() + 590.0f,
                                                    250.0f, 30.0f),
                             "LEVEL " + juce::String(lastLevel)
                                 + "   +" + juce::String(lastXpEarned) + " XP",
                             UiColors::accent,
                             true);
            GameUi::drawPill(g,
                             juce::Rectangle<float>(card.getX() + 294.0f,
                                                    card.getY() + 590.0f,
                                                    270.0f, 30.0f),
                             juce::String(lastXpForNext) + " XP TO NEXT LEVEL",
                             UiColors::surface2,
                             false);

            if (lastAchievementsText.isNotEmpty())
            {
                g.setColour(UiColors::green);
                g.setFont(GameUi::font(11.0f, true));
                g.drawFittedText("ACHIEVEMENT  " + lastAchievementsText,
                                 juce::Rectangle<int>(
                                     static_cast<int>(card.getX() + 32.0f),
                                     static_cast<int>(card.getY() + 630.0f),
                                     static_cast<int>(card.getWidth() - 64.0f),
                                     20),
                                 juce::Justification::centredLeft, 1);
            }
        }

        if (card.getHeight() > 720.0f)
        {
            const int perfect = gameplayStats.getPerfectCount();
            const int great = gameplayStats.getGreatCount();
            const int good = gameplayStats.getGoodCount();
            const int judged = juce::jmax(1, perfect + great + good + misses);

            GameUi::drawEyebrow(g, "JUDGEMENT BREAKDOWN",
                                juce::Rectangle<float>(card.getX() + 32.0f,
                                                       card.getY() + 596.0f,
                                                       420.0f, 14.0f),
                                UiColors::muted);

            const juce::Rectangle<float> bar(card.getX() + 32.0f,
                                             card.getY() + 620.0f,
                                             card.getWidth() - 64.0f,
                                             14.0f);
            g.setColour(UiColors::surface2);
            g.fillRoundedRectangle(bar, 7.0f);

            {
                juce::Graphics::ScopedSaveState saved(g);
                juce::Path clip;
                clip.addRoundedRectangle(bar, 7.0f);
                g.reduceClipRegion(clip);

                const std::array<int, 4> counts { perfect, great, good, misses };
                const std::array<juce::Colour, 4> colours
                {
                    UiColors::accent, UiColors::green, UiColors::info, UiColors::red
                };
                float segmentX = bar.getX();

                for (size_t i = 0; i < counts.size(); ++i)
                {
                    const float segmentW = bar.getWidth()
                        * static_cast<float>(counts[i]) / static_cast<float>(judged);
                    if (segmentW > 1.5f)
                    {
                        g.setColour(colours[i]);
                        g.fillRect(segmentX, bar.getY(),
                                   juce::jmax(0.0f, segmentW - 2.0f), bar.getHeight());
                    }
                    segmentX += segmentW;
                }
            }

            const std::array<juce::String, 4> legend
            {
                "PERFECT  " + juce::String(perfect),
                "GREAT  " + juce::String(great),
                "GOOD  " + juce::String(good),
                "MISS  " + juce::String(misses)
            };
            const std::array<juce::Colour, 4> legendColours
            {
                UiColors::accent, UiColors::green, UiColors::info, UiColors::red
            };
            const float legendWidth = (card.getWidth() - 64.0f) / 4.0f;

            for (int i = 0; i < 4; ++i)
            {
                const float x = card.getX() + 32.0f
                    + static_cast<float>(i) * legendWidth;
                const float y = card.getY() + 649.0f;
                g.setColour(legendColours[static_cast<size_t>(i)]);
                g.fillEllipse(x, y + 4.0f, 7.0f, 7.0f);
                g.setColour(UiColors::muted);
                g.setFont(GameUi::font(10.5f));
                g.drawText(legend[static_cast<size_t>(i)],
                           juce::Rectangle<float>(x + 13.0f, y,
                                                   legendWidth - 20.0f, 16.0f),
                           juce::Justification::centredLeft, false);
            }
        }

        return;
    }

    //==========================================================================
    // Gameplay
    //==========================================================================

    const float topY = 20.0f;
    const float topH = 88.0f;
    const float hudY = 120.0f;
    const float hudH = 96.0f;
    const float bottomH = juce::jlimit(124.0f, 160.0f, height / 6.0f);
    const float bottomY = height - 24.0f - bottomH;
    // Compact single-row mixer: six channels side by side instead of a
    // three-column / two-row block. The freed height goes to the tablature.
    const float studioH = juce::jlimit(132.0f, 146.0f, height / 7.0f);
    const float studioY = bottomY - 14.0f - studioH;
    const float tabY = hudY + hudH + 12.0f;
    const float tabH = juce::jmax(150.0f, studioY - 12.0f - tabY);

    const juce::Rectangle<float> topBar(24.0f, topY, width - 48.0f, topH);
    GameUi::drawCard(g, topBar, 18.0f);
    drawBrand(44.0f, 32.0f);

    g.setColour(UiColors::muted);
    g.setFont(GameUi::eyebrow(8.0f));
    g.drawText("NOW PLAYING",
               juce::Rectangle<float>(210.0f, 31.0f, 260.0f, 12.0f),
               juce::Justification::centredLeft, false);

    // Tempo pills: BEAT / BAR / BPM.
    auto drawTempoPill = [&g](juce::Rectangle<float> area,
                              const juce::String& caption)
    {
        g.setColour(UiColors::surface2.withAlpha(0.72f));
        g.fillRoundedRectangle(area, 9.0f);
        g.setColour(UiColors::border);
        g.drawRoundedRectangle(area, 9.0f, 1.0f);
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(8.0f, true));
        g.drawText(caption,
                   juce::Rectangle<float>(area.getX() + 9.0f, area.getY() + 3.0f,
                                          36.0f, 14.0f),
                   juce::Justification::centredLeft, false);
    };

    drawTempoPill({ 44.0f, 82.0f, 80.0f, 24.0f }, "BEAT");
    drawTempoPill({ 132.0f, 82.0f, 72.0f, 24.0f }, "BAR");
    drawTempoPill({ 212.0f, 82.0f, 88.0f, 24.0f }, "BPM");

    constexpr float hudGap = 12.0f;
    const float comboW = 124.0f;
    const float pitchW = 214.0f;
    const float targetW = 190.0f;

    const juce::Rectangle<float> comboCard(24.0f, hudY, comboW, hudH);
    const juce::Rectangle<float> pitchCard(comboCard.getRight() + hudGap, hudY,
                                           pitchW, hudH);
    const juce::Rectangle<float> targetCard(width - 24.0f - targetW, hudY,
                                            targetW, hudH);
    const float statsX = pitchCard.getRight() + hudGap;
    const float statsW = juce::jmax(180.0f, targetCard.getX() - hudGap - statsX);
    const juce::Rectangle<float> statsCard(statsX, hudY, statsW, hudH);

    GameUi::drawCard(g, comboCard, 16.0f);
    GameUi::drawCard(g, pitchCard, 16.0f);
    GameUi::drawCard(g, statsCard, 16.0f);
    GameUi::drawCard(g, targetCard, 16.0f);

    GameUi::drawEyebrow(g, "COMBO",
                        juce::Rectangle<float>(comboCard.getX() + 14.0f,
                                               comboCard.getY() + 14.0f,
                                               comboW - 28.0f, 15.0f));

    GameUi::drawEyebrow(g, "PITCH",
                        juce::Rectangle<float>(pitchCard.getX() + 16.0f,
                                               pitchCard.getY() + 14.0f,
                                               pitchW - 32.0f, 15.0f),
                        UiColors::green);

    g.setColour(UiColors::accent);
    g.setFont(GameUi::eyebrow(9.0f));
    g.drawText("NEXT NOTE",
               juce::Rectangle<float>(targetCard.getX() + 18.0f,
                                      targetCard.getY() + 14.0f,
                                      targetW - 30.0f, 15.0f),
               juce::Justification::centredLeft, false);

    if (practiceActive.load(std::memory_order_relaxed))
    {
        // The live judgement strip is meaningless in a training run, so the
        // card states the mode instead of showing scores that stay at zero.
        GameUi::drawPill(g,
                         juce::Rectangle<float>(statsCard.getX() + 16.0f,
                                                statsCard.getY() + 12.0f,
                                                104.0f, 24.0f),
                         "PRACTICE",
                         UiColors::accent,
                         true);
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(11.0f, true));
        g.drawFittedText("NO SCORE - PLAY THE HIGHLIGHTED NOTE",
                         juce::Rectangle<int>(
                             static_cast<int>(statsCard.getX() + 132.0f),
                             static_cast<int>(statsCard.getY() + 14.0f),
                             static_cast<int>(statsCard.getWidth() - 150.0f),
                             22),
                         juce::Justification::centredLeft, 1);
    }
    else
    {
        if (currentSessionIsPreview)
        {
            // A draft played in admin preview mode is not ranked: the strip
            // states it instead of the usual "LIVE PERFORMANCE" caption.
            GameUi::drawPill(g,
                             juce::Rectangle<float>(statsCard.getX() + 16.0f,
                                                    statsCard.getY() + 8.0f,
                                                    196.0f, 22.0f),
                             "PREVIEW - NOT RANKED",
                             UiColors::red,
                             false);
        }
        else
        {
            GameUi::drawEyebrow(g, "LIVE PERFORMANCE",
                                juce::Rectangle<float>(statsCard.getX() + 18.0f,
                                                       statsCard.getY() + 9.0f,
                                                       statsCard.getWidth() - 36.0f, 14.0f),
                                UiColors::accent);
        }

        const float cellWidth = statsCard.getWidth() / 4.0f;
        const std::array<juce::String, 4> values
        {
            juce::String(gameplayStats.getMissCount()),
            juce::String(gameplayStats.getPerfectCount()),
            juce::String(gameplayStats.getGreatCount()),
            juce::String(gameplayStats.getGoodCount())
        };
        const std::array<juce::String, 4> captions { "MISS", "PERFECT", "GREAT", "GOOD" };

        for (int i = 0; i < 4; ++i)
        {
            if (i > 0)
            {
                g.setColour(UiColors::border);
                g.fillRect(statsCard.getX() + cellWidth * static_cast<float>(i),
                           statsCard.getY() + 28.0f, 1.0f, 52.0f);
            }
            // The LIVE PERFORMANCE strip is monochrome gold: every judgement
            // keeps the same accent colour instead of red/green/blue coding.
            g.setColour(UiColors::accent);
            g.setFont(GameUi::heading(24.0f));
            g.drawText(values[static_cast<size_t>(i)],
                       juce::Rectangle<float>(statsCard.getX() + cellWidth * static_cast<float>(i),
                                              statsCard.getY() + 28.0f,
                                              cellWidth, 34.0f),
                       juce::Justification::centred, false);
            g.setColour(UiColors::accent);
            g.setFont(GameUi::eyebrow(8.0f));
            g.drawText(captions[static_cast<size_t>(i)],
                       juce::Rectangle<float>(statsCard.getX() + cellWidth * static_cast<float>(i),
                                              statsCard.getY() + 65.0f,
                                              cellWidth, 14.0f),
                       juce::Justification::centred, false);
        }
    }

    // Tablature sits on its own opaque surface; the shadow below it is painted here.
    g.setColour(juce::Colours::black.withAlpha(0.25f));
    g.fillRoundedRectangle(24.0f, tabY + 3.0f, width - 48.0f, tabH, 16.0f);

    // Guitar-effect pedals are intentionally not shown yet; the track mixer
    // uses the full width until the effects UI is implemented.
    const juce::Rectangle<float> mixer(24.0f, studioY,
                                       width - 48.0f, studioH);

    GameUi::drawCard(g, mixer, 18.0f);
    GameUi::drawEyebrow(g, "MIXER",
                        juce::Rectangle<float>(mixer.getX() + 22.0f,
                                               mixer.getY() + 14.0f,
                                               180.0f, 14.0f));
    g.setColour(UiColors::text);
    g.setFont(GameUi::heading(17.0f));
    g.drawText("TRACK MIX",
               juce::Rectangle<float>(mixer.getX() + 22.0f,
                                      mixer.getY() + 29.0f,
                                      mixer.getWidth() - 190.0f, 22.0f),
               juce::Justification::centredLeft, false);
    GameUi::drawPill(g,
                     juce::Rectangle<float>(mixer.getRight() - 122.0f,
                                            mixer.getY() + 17.0f,
                                            100.0f, 20.0f),
                     "7 CHANNELS",
                     UiColors::green,
                     false);
    GameUi::drawDivider(g, mixer.getX() + 22.0f,
                        mixer.getRight() - 22.0f,
                        mixer.getY() + 54.0f);

    {
        const int mixerCellGap = 8;
        const int mixerCellWidth =
            (juce::roundToInt(mixer.getWidth()) - 28 - mixerCellGap * 6) / 7;
        for (int i = 1; i < 7; ++i)
        {
            const float dividerX = mixer.getX() + 14.0f
                + static_cast<float>(i * (mixerCellWidth + mixerCellGap)) - 4.0f;
            g.setColour(UiColors::border.withAlpha(0.55f));
            g.fillRect(dividerX, mixer.getY() + 60.0f, 1.0f,
                       mixer.getHeight() - 72.0f);
        }
    }

    const juce::Rectangle<float> bottomCard(24.0f, bottomY, width - 48.0f, bottomH);
    GameUi::drawCard(g, bottomCard, 18.0f, true);

    const float judgementW = bottomCard.getWidth() * 0.40f;
    const float accuracyX = bottomCard.getX() + judgementW;
    const float scoreX = bottomCard.getX() + bottomCard.getWidth() * 0.66f;
    GameUi::drawEyebrow(g, "JUDGEMENT",
                        juce::Rectangle<float>(bottomCard.getX() + 24.0f,
                                               bottomCard.getY() + 15.0f,
                                               judgementW - 48.0f, 14.0f));
    GameUi::drawEyebrow(g, "ACCURACY",
                        juce::Rectangle<float>(accuracyX + 22.0f,
                                               bottomCard.getY() + 15.0f,
                                               160.0f, 14.0f),
                        UiColors::muted);
    GameUi::drawEyebrow(g, "SCORE",
                        juce::Rectangle<float>(scoreX + 22.0f,
                                               bottomCard.getY() + 15.0f,
                                               120.0f, 14.0f));

    const int hits = gameplayStats.getHitCount();
    const int misses = gameplayStats.getMissCount();
    const int total = hits + misses;
    const double accuracy = total > 0
        ? 100.0 * static_cast<double>(hits) / static_cast<double>(total)
        : 0.0;
    g.setColour(UiColors::text);
    g.setFont(GameUi::heading(30.0f));
    g.drawText(juce::String(juce::roundToInt(accuracy)) + "%",
               juce::Rectangle<float>(accuracyX + 22.0f,
                                      bottomCard.getY() + 34.0f,
                                      160.0f, 38.0f),
               juce::Justification::centredLeft, false);
    g.setColour(UiColors::muted);
    g.setFont(GameUi::font(9.5f));
    g.drawText(juce::String(hits) + " / " + juce::String(total) + " NOTES",
               juce::Rectangle<float>(accuracyX + 22.0f,
                                      bottomCard.getY() + bottomH - 34.0f,
                                      180.0f, 16.0f),
               juce::Justification::centredLeft, false);

    g.setColour(UiColors::border);
    g.fillRect(accuracyX, bottomCard.getY() + 20.0f, 1.0f, bottomH - 40.0f);
    g.fillRect(scoreX, bottomCard.getY() + 20.0f, 1.0f, bottomH - 40.0f);

    g.setColour(UiColors::muted.withAlpha(0.72f));
    g.setFont(GameUi::font(9.0f, true));
    g.drawText("POINTS",
               juce::Rectangle<float>(scoreX + 22.0f,
                                      bottomCard.getY() + bottomH - 34.0f,
                                      120.0f, 16.0f),
               juce::Justification::centredRight, false);

}

void MainModalOverlayComponent::paint(juce::Graphics& g)
{
    owner.paintModalOverlay(g);
}

void MainComponent::paintModalOverlay(juce::Graphics& g)
{
    const float width = static_cast<float>(getWidth());
    const float height = static_cast<float>(getHeight());

    if (width <= 0.0f || height <= 0.0f)
        return;

    const bool ready = gameState == GameState::Ready;
    const bool preparing = gameState == GameState::Preparing;

    // Ready and count-in share the same card. Before START the card shows the
    // prompt; after START the same card shows 3-2-1.
    if (ready
        || (gameState == GameState::Playing
            && countInActive.load(std::memory_order_acquire)))
    {
        const float cardWidth = 280.0f;
        const float cardHeight = 240.0f;
        const juce::Rectangle<float> countCard(
            (width - cardWidth) * 0.5f,
            (height - cardHeight) * 0.5f,
            cardWidth, cardHeight);

        // Light dim only: the tablature behind stays readable so the player
        // can look at the first notes before starting.
        g.setColour(UiColors::background.withAlpha(ready ? 0.28f : 0.32f));
        g.fillAll();

        GameUi::drawCard(g, countCard, 24.0f, true);
        g.setColour(UiColors::accent);
        g.fillRoundedRectangle(countCard.getX() + 24.0f, countCard.getY(),
                               78.0f, 3.0f, 1.5f);
        g.setColour(UiColors::accent);
        g.setFont(GameUi::eyebrow(10.0f));
        g.drawText("GET READY",
                   juce::Rectangle<float>(countCard.getX(), countCard.getY() + 28.0f,
                                          cardWidth, 18.0f),
                   juce::Justification::centred, false);

        if (ready)
        {
            g.setColour(UiColors::text);
            g.setFont(GameUi::heading(34.0f));
            g.drawText("READY",
                       juce::Rectangle<float>(countCard.getX(),
                                              countCard.getY() + 50.0f,
                                              cardWidth, 44.0f),
                       juce::Justification::centred, false);

            g.setColour(UiColors::muted);
            g.setFont(GameUi::font(12.0f));
            g.drawFittedText("The count-in starts when you press START.",
                             juce::Rectangle<int>(static_cast<int>(countCard.getX() + 20.0f),
                                                  static_cast<int>(countCard.getY() + 100.0f),
                                                  static_cast<int>(cardWidth - 40.0f),
                                                  40),
                             juce::Justification::centred, 2);

            if (currentSessionIsPreview)
            {
                // The player must know before pressing START that a draft
                // attempt is not ranked. Sits between the hint text and the
                // START button (card bottom - 74).
                GameUi::drawPill(g,
                                 juce::Rectangle<float>(countCard.getX() + 40.0f,
                                                        countCard.getY() + 140.0f,
                                                        cardWidth - 80.0f, 22.0f),
                                 "PREVIEW - NOT RANKED",
                                 UiColors::red,
                                 false);
            }
        }
        else
        {
            const int count = juce::jmax(1, countInRemaining.load(std::memory_order_relaxed));
            const int total = juce::jlimit(1, 16,
                                           countInTotal.load(std::memory_order_relaxed));

            g.setColour(count <= 1 ? UiColors::accentStrong : UiColors::accent);
            g.setFont(GameUi::heading(92.0f));
            g.drawText(juce::String(count),
                       juce::Rectangle<float>(countCard.getX(),
                                              countCard.getY() + 48.0f,
                                              cardWidth, 110.0f),
                       juce::Justification::centred, false);

            // One dot per click of the bar, so a 7/8 count-in is visibly
            // longer than a 4/4 one. The spacing shrinks for long bars
            // (12/8, 16/8) instead of overflowing the card.
            const float spacing = total > 1
                ? juce::jmin(22.0f, (cardWidth - 80.0f) / static_cast<float>(total - 1))
                : 0.0f;

            const float firstDotX = countCard.getCentreX()
                - spacing * static_cast<float>(total - 1) * 0.5f;

            for (int i = 0; i < total; ++i)
            {
                const float dotX = firstDotX + static_cast<float>(i) * spacing;
                g.setColour(i < count ? UiColors::accent : UiColors::borderStrong);
                g.fillEllipse(dotX - 3.0f, countCard.getBottom() - 42.0f, 6.0f, 6.0f);
            }
        }
    }

    // Managed tempo change: a clear, non-freezing progress card.
    if (preparing)
    {
        const float cardWidth = 360.0f;
        const float cardHeight = 170.0f;
        const juce::Rectangle<float> prepareCard(
            (width - cardWidth) * 0.5f,
            (height - cardHeight) * 0.5f,
            cardWidth, cardHeight);

        g.setColour(UiColors::background.withAlpha(0.62f));
        g.fillAll();

        GameUi::drawCard(g, prepareCard, 24.0f, true);
        g.setColour(UiColors::accent);
        g.fillRoundedRectangle(prepareCard.getX() + 24.0f, prepareCard.getY(),
                               78.0f, 3.0f, 1.5f);
        GameUi::drawEyebrow(g, "TEMPO CHANGE",
                            juce::Rectangle<float>(prepareCard.getX() + 24.0f,
                                                   prepareCard.getY() + 26.0f,
                                                   260.0f, 16.0f));
        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(26.0f));
        g.drawText("PREPARING AUDIO",
                   juce::Rectangle<float>(prepareCard.getX() + 24.0f,
                                          prepareCard.getY() + 52.0f,
                                          prepareCard.getWidth() - 48.0f, 34.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(12.0f));
        g.drawText("Re-rendering the backing tracks. Playback resumes "
                   "automatically from the same beat.",
                   juce::Rectangle<float>(prepareCard.getX() + 24.0f,
                                          prepareCard.getY() + 94.0f,
                                          prepareCard.getWidth() - 48.0f, 40.0f),
                   juce::Justification::topLeft, true);
    }

    // Pause overlay.
    if (gameState == GameState::Paused)
    {
        g.setColour(UiColors::background.withAlpha(0.76f));
        g.fillAll();

        const juce::Rectangle<float> pauseCard(
            width * 0.5f - 250.0f,
            height * 0.5f - 170.0f,
            500.0f, 340.0f);
        GameUi::drawCard(g, pauseCard, 24.0f, true);
        g.setColour(UiColors::accent);
        g.fillRoundedRectangle(pauseCard.getX() + 28.0f, pauseCard.getY(),
                               84.0f, 3.0f, 1.5f);
        GameUi::drawEyebrow(g, "SESSION PAUSED",
                            juce::Rectangle<float>(pauseCard.getX() + 28.0f,
                                                   pauseCard.getY() + 32.0f,
                                                   300.0f, 16.0f));
        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(32.0f));
        g.drawText("Take a breath.",
                   juce::Rectangle<float>(pauseCard.getX() + 28.0f,
                                          pauseCard.getY() + 52.0f,
                                          pauseCard.getWidth() - 56.0f, 44.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(13.0f));
        g.drawText("Your timing and score are frozen exactly where you left them.",
                   juce::Rectangle<float>(pauseCard.getX() + 28.0f,
                                          pauseCard.getY() + 102.0f,
                                          pauseCard.getWidth() - 56.0f, 24.0f),
                   juce::Justification::centredLeft, false);

        GameUi::drawDivider(g, pauseCard.getX() + 28.0f,
                            pauseCard.getRight() - 28.0f,
                            pauseCard.getY() + 142.0f);

        GameUi::drawEyebrow(g, "SCORE",
                            juce::Rectangle<float>(pauseCard.getX() + 28.0f,
                                                   pauseCard.getY() + 164.0f,
                                                   160.0f, 14.0f),
                            UiColors::muted);
        g.setColour(UiColors::accent);
        g.setFont(GameUi::heading(38.0f));
        g.drawText(juce::String(gameplayStats.getScore()),
                   juce::Rectangle<float>(pauseCard.getX() + 28.0f,
                                          pauseCard.getY() + 182.0f,
                                          240.0f, 46.0f),
                   juce::Justification::centredLeft, false);
        GameUi::drawEyebrow(g, "COMBO",
                            juce::Rectangle<float>(pauseCard.getX() + 280.0f,
                                                   pauseCard.getY() + 164.0f,
                                                   160.0f, 14.0f),
                            UiColors::muted);
        g.setColour(UiColors::green);
        g.setFont(GameUi::heading(38.0f));
        g.drawText("x" + juce::String(gameplayStats.getCombo()),
                   juce::Rectangle<float>(pauseCard.getX() + 280.0f,
                                          pauseCard.getY() + 182.0f,
                                          180.0f, 46.0f),
                   juce::Justification::centredLeft, false);

        GameUi::drawPill(g,
                         juce::Rectangle<float>(pauseCard.getX() + 28.0f,
                                                pauseCard.getBottom() - 56.0f,
                                                pauseCard.getWidth() - 56.0f, 30.0f),
                         "PRESS RESUME IN THE TOP BAR TO CONTINUE",
                         UiColors::accent,
                         true);
    }
}

void MainComponent::resized()
{
    overlayComponent.setBounds(getLocalBounds());
    modalOverlayComponent.setBounds(getLocalBounds());

    const int width = getWidth();
    const int height = getHeight();

    const bool authScreen = showAuthScreen;
    const bool songSelect = gameState == GameState::SongSelect && !authScreen;
    const bool tunerScreen = gameState == GameState::Tuner;
    const bool finishedScreen = gameState == GameState::Finished;
    const bool preparingScreen = gameState == GameState::Preparing;
    const bool showGame = gameState == GameState::Playing
        || gameState == GameState::Paused
        || preparingScreen;
    const bool readyScreen = gameState == GameState::Ready;

    //==========================================================================
    // Visibility
    //==========================================================================

    songTitleLabel.setVisible(false);
    previewButton.setVisible(false);
    practiceModeButton.setVisible(false);
    vstButton.setVisible(false);
    retryConnectionButton.setVisible(sessionOfflineRetryAvailable && authScreen);
    loginScreenTitleLabel.setVisible(authScreen);
    currentUserLabel.setVisible(songSelect && currentUsername.isNotEmpty());
    // Signing out is available only from the profile window; the main screen
    // shows EXIT in this row instead.
    logoutButton.setVisible(false);
    accountSettingsButton.setVisible(songSelect && currentUsername.isNotEmpty());
    // The ADMIN toggle exists only for an administrator on the song screen.
    adminPreviewButton.setVisible(songSelect && onlineIsAdmin);
    quitButton.setVisible(songSelect);
    supportButton.setVisible(songSelect);
    gameSongTitleLabel.setVisible(showGame || readyScreen);

    songPridumayButton.setVisible(songSelect);
    // The daily challenge is hidden for now: the schedule has no song for most
    // weekdays yet, and an always-disabled button only confuses players.
    dailyChallengeButton.setVisible(false);
    dailyChallengeStatusLabel.setVisible(false);
    onlineCatalogButton.setVisible(songSelect);
    onlineCatalogStatusLabel.setVisible(songSelect);
    leaderboardTitleLabel.setVisible(songSelect);
    leaderboardTable.setVisible(songSelect);
    progressionSummaryLabel.setVisible(false);
    leaderboardLabel.setVisible(songSelect);
    refreshLeaderboardButton.setVisible(songSelect);
    accountStatusLabel.setVisible(songSelect || authScreen);
    accountLoginButton.setVisible(authScreen);
    accountRegisterButton.setVisible(authScreen);
    verifyEmailButton.setVisible(authScreen);
    forgotPasswordButton.setVisible(authScreen);
    songMistButton.setVisible(false);
    songChordsButton.setVisible(false);
    aiSoloButton.setVisible(false);
    oldGeneratorArchiveButton.setVisible(false);
    melodyStyleCombo.setVisible(false);
    generateMelodyButton.setVisible(false);
    newGenerationButton.setVisible(false);
    songPridumayBestLabel.setVisible(songSelect);
    songListBox.setVisible(songSelect);
    songMistBestLabel.setVisible(false);
    songChordsBestLabel.setVisible(false);
    dailyChallengeBestLabel.setVisible(false);

    tunerTitleLabel.setVisible(tunerScreen);
    tunerNoteLabel.setVisible(tunerScreen);
    tunerFrequencyLabel.setVisible(tunerScreen);
    tunerStringLabel.setVisible(tunerScreen);
    tunerCentsLabel.setVisible(tunerScreen);
    tunerStatusLabel.setVisible(tunerScreen);
    tunerStartButton.setVisible(tunerScreen);
    tunerSkipButton.setVisible(tunerScreen);
    tuningPresetCombo.setVisible(tunerScreen);
    stringCountCombo.setVisible(tunerScreen);
    fretCountCombo.setVisible(tunerScreen);
    for (int slot = 0; slot < GuitarTuning::maxStrings; ++slot)
        stringTuningCombos[static_cast<size_t>(slot)].setVisible(
            tunerScreen && slot < currentTuning.count);

    // One shared input monitor: full size on the song screen and in the tuner,
    // compact in the gameplay HUD.
    if (inputMonitor != nullptr)
    {
        // Guitar Input lives on the song screen and in the tuner only; the
        // gameplay HUD keeps its full width for the tablature and stats.
        inputMonitor->setVisible(songSelect || tunerScreen);
        inputMonitor->setCompact(false);
    }

    pitchLabel.setVisible(showGame);
    comboLabel.setVisible(showGame);
    beatLabel.setVisible(showGame);
    barBeatLabel.setVisible(showGame);
    bpmLabel.setVisible(showGame);
    gameSessionLabel.setVisible(showGame);
    hitLabel.setVisible(showGame);
    targetNoteLabel.setVisible(showGame);

    gameStartButton.setVisible((showGame && !preparingScreen) || finishedScreen);
    readyStartButton.setVisible(readyScreen);
    resumeOverlayButton.setVisible(gameState == GameState::Paused);
    pauseButton.setVisible(gameState == GameState::Playing
                           || gameState == GameState::Paused);
    menuButton.setVisible((showGame && !preparingScreen)
                          || finishedScreen || readyScreen);
    distortionButton.setVisible(false);
    liveGuitarButton.setVisible(showGame);
    liveGuitarVolumeSlider.setVisible(showGame);
    liveGuitarVolumeLabel.setVisible(showGame);
    midiBackingButton.setVisible(showGame);
    midiBackingVolumeSlider.setVisible(showGame);
    midiBackingVolumeLabel.setVisible(showGame);
    midiRhythmGuitarButton.setVisible(showGame);
    midiRhythmGuitarVolumeSlider.setVisible(showGame);
    midiRhythmGuitarVolumeLabel.setVisible(showGame);
    midiBassButton.setVisible(showGame);
    midiBassVolumeSlider.setVisible(showGame);
    midiBassVolumeLabel.setVisible(showGame);
    midiDrumsButton.setVisible(showGame);
    midiDrumsVolumeSlider.setVisible(showGame);
    midiDrumsVolumeLabel.setVisible(showGame);
    midiPianoButton.setVisible(showGame);
    midiPianoVolumeSlider.setVisible(showGame);
    midiPianoVolumeLabel.setVisible(showGame);
    metronomeMixerButton.setVisible(showGame);
    metronomeMixerVolumeSlider.setVisible(showGame);
    metronomeMixerVolumeLabel.setVisible(showGame);
    distortionDriveSlider.setVisible(false);
    distortionToneSlider.setVisible(false);
    distortionDriveLabel.setVisible(false);
    distortionToneLabel.setVisible(false);
    speed50Button.setVisible(showGame);
    speed75Button.setVisible(showGame);
    speed100Button.setVisible(showGame);
    speed120Button.setVisible(showGame);
    audioSettingsButton.setVisible(songSelect || showGame || tunerScreen);
    easyButton.setVisible(false);
    normalButton.setVisible(false);
    hardButton.setVisible(false);
    tablatureComponent.setVisible(showGame || readyScreen);

    //==========================================================================
    // Authentication
    //==========================================================================

    if (authScreen)
    {
        const int cardWidth = juce::jmin(560, width - 48);
        const int cardHeight = 364;
        const int cardX = (width - cardWidth) / 2;
        const int cardY = juce::jmax(150, (height - cardHeight) / 2);

        loginScreenTitleLabel.setBounds(cardX + 40, cardY + 112, cardWidth - 80, 52);
        accountStatusLabel.setJustificationType(juce::Justification::centred);
        accountStatusLabel.setBounds(cardX + 40, cardY + 322, cardWidth - 80, 28);
        retryConnectionButton.setBounds(cardX + 40, cardY + 288, cardWidth - 80, 30);

        const int buttonWidth = (cardWidth - 92) / 2;
        accountLoginButton.setBounds(cardX + 40, cardY + 194, buttonWidth, 44);
        accountRegisterButton.setBounds(cardX + 52 + buttonWidth, cardY + 194,
                                        buttonWidth, 44);

        verifyEmailButton.setBounds(cardX + 40, cardY + 250, buttonWidth, 28);
        forgotPasswordButton.setBounds(cardX + 52 + buttonWidth, cardY + 250,
                                       buttonWidth, 28);
    }

    //==========================================================================
    // Song select
    //==========================================================================

    if (songSelect)
    {
        accountStatusLabel.setJustificationType(juce::Justification::centredLeft);

        // Top-right account row: plain username label, then SUPPORT, PROFILE,
        // ADMIN (administrators only), VST, AUDIO SETTINGS and EXIT. Signing out
        // lives only inside the profile window.
        const int topRight = width - 44;
        const int topGap = 8;
        const int accountButtonH = 32;
        const int usernameW = 150;

        // The ADMIN toggle joins the same row, but only for an administrator.
        // Six buttons no longer fit at 128 px each: the minimum shrinks to
        // 92 px and the gap tightens so the row still clears the wordmark.
        const bool showAdminToggle = onlineIsAdmin;
        const int headerButtonCount = showAdminToggle ? 6 : 5;
        const int headerGap = showAdminToggle ? 6 : topGap;
        constexpr int headerRowMinX = 300;
        const int accountButtonW = juce::jlimit(
            showAdminToggle ? 92 : 104, 128,
            (topRight - headerRowMinX - headerGap * (headerButtonCount - 1))
                / headerButtonCount);

        const int exitX = topRight - accountButtonW;
        const int audioX = exitX - headerGap - accountButtonW;
        const int vstX = audioX - headerGap - accountButtonW;
        const int settingsX = vstX - headerGap - accountButtonW;
        const int supportX = settingsX - headerGap - accountButtonW;
        const int adminX = supportX - headerGap - accountButtonW;
        const int usernameX = (showAdminToggle ? adminX : supportX) - 12 - usernameW;

        // The name stays available in the account window, so hide the label
        // before it can collide with the brand.
        currentUserLabel.setVisible(currentUsername.isNotEmpty() && usernameX >= 230);
        currentUserLabel.setBounds(usernameX, 48, usernameW, 22);
        currentUserLabel.setJustificationType(juce::Justification::centredLeft);
        supportButton.setBounds(supportX, 42, accountButtonW, accountButtonH);
        accountSettingsButton.setBounds(settingsX, 42, accountButtonW, accountButtonH);
        if (showAdminToggle)
            adminPreviewButton.setBounds(adminX, 42, accountButtonW, accountButtonH);
        vstButton.setVisible(true);
        vstButton.setBounds(vstX, 42, accountButtonW, accountButtonH);
        audioSettingsButton.setButtonText("AUDIO SETTINGS");
        audioSettingsButton.setBounds(audioX, 42, accountButtonW, accountButtonH);
        quitButton.setBounds(exitX, 42, accountButtonW, accountButtonH);
        songTitleLabel.setBounds(48, 140, width - 96, 44);

        const int contentY = 250;
        const int availableH = juce::jmax(320, height - 68 - 20 - contentY);
        // Must match paintOverlay(): the cards fill the window height.
        const int contentH = availableH;
        const int footerY = contentY + contentH + 20;
        const int contentW = width - 96;
        const int gap = 20;
        const int leftW = contentW * 56 / 100;
        const int rightX = 48 + leftW + gap;
        const int rightW = contentW - leftW - gap;

        // Best score sits under the title block, clear of the preview and mode
        // buttons on the right, so a long song title can never cover it.
        songPridumayBestLabel.setBounds(48 + 24 + 138, contentY + 108,
                                        leftW - 360, 20);

        const int buttonY = contentY + contentH - 64;
        const int available = leftW - 48;
        const int startW = juce::jmin(180, (available - 12) / 2);
        const int onlineW = juce::jmin(240, available - startW - 12);
        songPridumayButton.setBounds(48 + 24, buttonY, startW, 42);
        onlineCatalogButton.setBounds(48 + 24 + startW + 12, buttonY, onlineW, 42);
        onlineCatalogStatusLabel.setBounds(48 + 24, buttonY - 34, leftW - 48, 20);
        songDownloadProgressBar.setBounds(48 + 24, buttonY - 12, leftW - 48, 8);

        dailyChallengeButton.setBounds(48 + 24, contentY + 186, leftW - 48, 44);
        dailyChallengeStatusLabel.setBounds(48 + 24, contentY + 232,
                                            leftW - 48, 14);

        // Catalogue list between the daily card and the action buttons.
        const int listTop = contentY + 252;
        const int listBottom = buttonY - 38;
        songListBox.setBounds(48 + 18, listTop, leftW - 36,
                              juce::jmax(56, listBottom - listTop));

        leaderboardTitleLabel.setBounds(rightX + 24, contentY + 38, rightW - 48, 30);
        refreshLeaderboardButton.setBounds(rightX + rightW - 142, contentY + 68,
                                           118, 32);
        leaderboardLabel.setBounds(rightX + 24, contentY + 106,
                                   rightW - 48, 20);
        leaderboardTable.setBounds(rightX + 24, contentY + 130,
                                   rightW - 48,
                                   juce::jmax(120, contentH - 152));

        progressionSummaryLabel.setBounds(48, 78, juce::jmax(200, width - 96), 22);

        accountStatusLabel.setBounds(24 + 34, footerY + 7, width - 48 - 58, 30);

        // Shared input monitor: top-right of the song screen, level panel on
    // its left.
        if (inputMonitor != nullptr)
        {
            const int monitorW = inputMonitorCardWidth(width);
            inputMonitor->setBounds(width - 48 - monitorW, 108, monitorW, 124);
        }

        // Preview control inside the selected-song card, right of the header.
        previewButton.setVisible(songSelect);
        previewButton.setBounds(48 + leftW - 24 - 170, contentY + 56, 170, 34);

        // Mode switch above START SONG: the player always sees what starts.
        practiceModeButton.setVisible(songSelect);
        practiceModeButton.setBounds(48 + leftW - 24 - 170, contentY + 96, 170, 34);
    }

    //==========================================================================
    // Tuner
    //==========================================================================

    if (tunerScreen)
    {
        const int margin = 42;
        const int gap = 20;
        const int panelY = 190;
        const int panelH = juce::jmin(560, juce::jmax(380, height - 190 - 120));
        const int leftW = (width - 84 - gap) * 44 / 100;
        const int rightX = margin + leftW + gap;
        const int rightW = width - margin - rightX;

        tunerTitleLabel.setBounds(rightX + 24, panelY + 24, rightW - 48, 20);
        tunerNoteLabel.setBounds(rightX + 24, panelY + 142, rightW - 48, 96);
        tunerFrequencyLabel.setBounds(rightX + 24, panelY + 242, rightW - 48, 26);
        tunerStringLabel.setBounds(rightX + rightW - 180, panelY + 278, 160, 30);
        tunerCentsLabel.setBounds(rightX + rightW - 180, panelY + 310, 160, 30);
        tunerStringLabel.setJustificationType(juce::Justification::centredRight);
        tunerCentsLabel.setJustificationType(juce::Justification::centredRight);
        tunerStatusLabel.setBounds(rightX + 40, panelY + 368, rightW - 80, 40);
        tunerStatusLabel.setJustificationType(juce::Justification::centred);

        // Tuning selector occupies the old static "STANDARD TUNING" line; the
        // per-string note combos sit on top of the string tiles. String count
        // and fret count share the row so a bass, a balalaika or an
        // eight-string guitar is a single click away.
        const int selectorWidth = leftW - 48;
        const int presetWidth = selectorWidth * 46 / 100;
        const int stringsWidth = selectorWidth * 27 / 100;
        const int fretsWidth = selectorWidth - presetWidth - stringsWidth - 16;

        tuningPresetCombo.setBounds(margin + 24, panelY + 44, presetWidth, 28);
        stringCountCombo.setBounds(margin + 24 + presetWidth + 8, panelY + 44,
                                   stringsWidth, 28);
        fretCountCombo.setBounds(margin + 24 + presetWidth + stringsWidth + 16,
                                 panelY + 44, fretsWidth, 28);

        const int stringSlots = juce::jlimit(1, GuitarTuning::maxStrings,
                                             currentTuning.count);
        const int tileColumns = stringSlots > 6 ? 4 : 3;
        const float tileGap = 12.0f;
        const float tileW = (static_cast<float>(leftW) - 48.0f
                             - tileGap * static_cast<float>(tileColumns - 1))
            / static_cast<float>(tileColumns);
        const float tileH = 112.0f;

        for (int slot = 0; slot < GuitarTuning::maxStrings; ++slot)
        {
            auto& combo = stringTuningCombos[static_cast<size_t>(slot)];

            if (slot >= stringSlots)
            {
                combo.setVisible(false);
                continue;
            }

            const int column = slot % tileColumns;
            const int row = slot / tileColumns;
            const float x = static_cast<float>(margin + 24)
                + static_cast<float>(column) * (tileW + tileGap);
            const float y = static_cast<float>(panelY + 82)
                + static_cast<float>(row) * (tileH + tileGap);

            combo.setBounds(
                juce::Rectangle<float>(x + 8.0f, y + 8.0f,
                                       tileW - 16.0f, 34.0f).toNearestInt());
        }

        // The input monitor takes the free space at the bottom of the strings
        // panel, where the static reference-pitch list used to be.
        if (inputMonitor != nullptr)
        {
            const int monitorH = juce::jlimit(84, 124, panelH - 340 - 24);
            inputMonitor->setBounds(margin + 24, panelY + 340,
                                    juce::jmax(260, leftW - 48), monitorH);
        }

        // Quick access to the input/device settings from the tuner. The button
        // sits immediately to the left of the tuning-status pill, so the rest
        // of the tuner layout stays untouched.
        const int audioW = 128;
        const int audioH = 30;
        const int audioX = width - 44 - 136 - 12 - audioW;
        audioSettingsButton.setButtonText("AUDIO SETTINGS");
        audioSettingsButton.setBounds(audioX, 25, audioW, audioH);

        const int buttonWidth = juce::jmin(230, (width - 100) / 2);
        const int buttonGap = 16;
        const int buttonsX = (width - (buttonWidth * 2 + buttonGap)) / 2;
        const int buttonY = panelY + panelH + 24;
        tunerSkipButton.setBounds(buttonsX, buttonY, buttonWidth, 46);
        tunerStartButton.setBounds(buttonsX + buttonWidth + buttonGap,
                                   buttonY, buttonWidth, 46);
    }

    //==========================================================================
    // Results
    //==========================================================================

    // Ready screen: the START button lives inside the same count-in card.
    if (readyScreen)
    {
        gameSongTitleLabel.setBounds(210, 45,
                                     juce::jmax(200, width - 640), 28);
        menuButton.setBounds(24, 46, 100, 36);

        const int cardW = 280;
        const int cardH = 240;
        const int cardX = (width - cardW) / 2;
        const int cardY = (height - cardH) / 2;
        readyStartButton.setBounds(cardX + 40, cardY + cardH - 74, 200, 44);
        // The tablature is visible before the start so the player can see the
        // first notes while getting ready.
        const int hudY = 120;
        const int hudH = 96;
        const int bottomH = juce::jlimit(124, 160, height / 6);
        const int bottomY = height - 24 - bottomH;
        const int studioH = juce::jlimit(176, 218, height / 4);
        const int studioY = bottomY - 14 - studioH;
        const int tabY = hudY + hudH + 12;
        const int tabH = juce::jmax(150, studioY - 12 - tabY);
        tablatureComponent.setBounds(24, tabY, width - 48, tabH);
        // The modal overlay paints the card above regular controls; keep the
        // button on top so it is both visible and clickable.
        readyStartButton.toFront(false);
    }

    // Pause card: a real RESUME button above the score summary.
    if (gameState == GameState::Paused)
    {
        const int cardY = height / 2 - 170;
        resumeOverlayButton.setBounds((width - 200) / 2, cardY + 340 - 104,
                                      200, 40);
        resumeOverlayButton.toFront(false);

        // The pause card and its dim layer are painted by the modal overlay,
        // which sits above the regular controls. Keep the mixer and tempo
        // controls above that layer so the player can still balance the
        // backing tracks and change the tempo while the transport is frozen.
        juce::Component* pauseControls[] = {
            &liveGuitarButton, &liveGuitarVolumeSlider, &liveGuitarVolumeLabel,
            &midiBackingButton, &midiBackingVolumeSlider, &midiBackingVolumeLabel,
            &midiRhythmGuitarButton, &midiRhythmGuitarVolumeSlider,
            &midiRhythmGuitarVolumeLabel,
            &midiBassButton, &midiBassVolumeSlider, &midiBassVolumeLabel,
            &midiDrumsButton, &midiDrumsVolumeSlider, &midiDrumsVolumeLabel,
            &midiPianoButton, &midiPianoVolumeSlider, &midiPianoVolumeLabel,
            &metronomeMixerButton, &metronomeMixerVolumeSlider,
            &metronomeMixerVolumeLabel,
            &speed50Button, &speed75Button, &speed100Button, &speed120Button,
            &bpmLabel, &audioSettingsButton,
        };

        for (auto* control : pauseControls)
            control->toFront(false);
    }

    if (finishedScreen)
    {
        const int buttonWidth = juce::jmin(180, (width - 100) / 2);
        const int gap = 20;
        const int totalWidth = buttonWidth * 2 + gap;
        const int left = (width - totalWidth) / 2;
        const int cardBottom = 82 + juce::jmax(520, height - 170);
        const int y = height >= 820 ? cardBottom - 56 : height - 76;
        gameStartButton.setBounds(left, y, buttonWidth, 44);
        menuButton.setBounds(left + buttonWidth + gap, y, buttonWidth, 44);
        gameStartButton.setButtonText("PLAY AGAIN");
        menuButton.setButtonText("MAIN MENU");
    }

    //==========================================================================
    // Gameplay
    //==========================================================================

    if (showGame)
    {
        menuButton.setButtonText(gameState == GameState::Paused ? "BACK" : "MENU");
        audioSettingsButton.setButtonText("AUDIO");
        const int hudY = 120;
        const int hudH = 96;
        const int bottomH = juce::jlimit(124, 160, height / 6);
        const int bottomY = height - 24 - bottomH;
        const int studioH = juce::jlimit(132, 146, height / 7);
        const int studioY = bottomY - 14 - studioH;
        const int tabY = hudY + hudH + 12;
        const int tabH = juce::jmax(150, studioY - 12 - tabY);

        const int audioW = 112;
        const int menuW = 100;
        const int pauseW = 82;
        const int startW = 76;
        const int actionGap = 8;
        const int actionsW = audioW + menuW + pauseW + startW + actionGap * 3;
        const int actionY = 46;
        const int actionH = 36;
        int actionX = width - 24 - actionsW;
        gameStartButton.setBounds(actionX, actionY, startW, actionH);
        actionX += startW + actionGap;
        pauseButton.setBounds(actionX, actionY, pauseW, actionH);
        actionX += pauseW + actionGap;
        menuButton.setBounds(actionX, actionY, menuW, actionH);
        actionX += menuW + actionGap;
        audioSettingsButton.setBounds(actionX, actionY, audioW, actionH);

        gameSongTitleLabel.setBounds(210, 45, juce::jmax(200, width - 640), 28);

        beatLabel.setBounds(44 + 46, 83, 30, 22);
        barBeatLabel.setBounds(132 + 40, 83, 28, 22);
        bpmLabel.setBounds(212 + 42, 83, 44, 22);

        speed50Button.setBounds(310, 72, 68, 36);
        speed75Button.setBounds(386, 72, 68, 36);
        speed100Button.setBounds(462, 72, 68, 36);
        speed120Button.setBounds(538, 72, 68, 36);

        const int hudGap = 12;
        const int comboW = 124;
        const int pitchW = 214;
        const int targetW = 190;
        const int comboX = 24;
        const int pitchX = comboX + comboW + hudGap;
        const int targetX = width - 24 - targetW;
        const int statsX = pitchX + pitchW + hudGap;
        const int statsW = juce::jmax(180, targetX - hudGap - statsX);

        // The judgement counters are painted by paintOverlay(), not by a Label.
        // Remember their area so the timer can invalidate exactly that strip
        // when a note is judged instead of repainting the whole window (which
        // also redraws the tablature and makes dense songs stutter).
        hudStatsBounds = juce::Rectangle<int>(statsX, hudY, statsW, hudH);

        comboLabel.setBounds(comboX + 8, hudY + 32, comboW - 16, 46);
        pitchLabel.setBounds(pitchX + 10, hudY + 32, pitchW - 20, 46);
        targetNoteLabel.setBounds(targetX + 10, hudY + 32, targetW - 20, 46);

        tablatureComponent.setBounds(24, tabY, width - 48, tabH);

        const int mixerX = 24;
        const int mixerW = width - 48;
        const int channelX = mixerX + 14;
        const int channelTop = studioY + 60;
        const int cellGap = 8;
        const int cellW = (mixerW - 28 - cellGap * 6) / 7;

        auto layoutMixerChannel = [&](juce::TextButton& button, juce::Label& label,
                                      juce::Slider& slider, int column)
        {
            const int x = channelX + column * (cellW + cellGap);
            button.setBounds(x, channelTop, cellW, 22);
            slider.setBounds(x, channelTop + 26, cellW, 18);
            label.setBounds(x, channelTop + 46, cellW, 14);
        };

        layoutMixerChannel(liveGuitarButton, liveGuitarVolumeLabel,
                           liveGuitarVolumeSlider, 0);
        layoutMixerChannel(midiBackingButton, midiBackingVolumeLabel,
                           midiBackingVolumeSlider, 1);
        layoutMixerChannel(midiDrumsButton, midiDrumsVolumeLabel,
                           midiDrumsVolumeSlider, 2);
        layoutMixerChannel(midiBassButton, midiBassVolumeLabel,
                           midiBassVolumeSlider, 3);
        layoutMixerChannel(midiRhythmGuitarButton, midiRhythmGuitarVolumeLabel,
                           midiRhythmGuitarVolumeSlider, 4);
        layoutMixerChannel(midiPianoButton, midiPianoVolumeLabel,
                           midiPianoVolumeSlider, 5);
        layoutMixerChannel(metronomeMixerButton, metronomeMixerVolumeLabel,
                           metronomeMixerVolumeSlider, 6);

        const int bottomW = width - 48;
        const int judgementW = bottomW * 40 / 100;
        const int scoreX = 24 + bottomW * 66 / 100;
        hitLabel.setBounds(24 + 24, bottomY + 34, judgementW - 48, bottomH - 44);
        gameSessionLabel.setBounds(scoreX + 16, bottomY + 24,
                                   width - 24 - scoreX - 32, bottomH - 30);

        // Same geometry paintOverlay() uses for the ACCURACY value and the
        // "hits / total NOTES" line, with a small margin for antialiasing.
        const int accuracyX = 24 + judgementW;
        hudAccuracyBounds = juce::Rectangle<int>(accuracyX + 16, bottomY + 26,
                                                200, juce::jmax(0, bottomH - 38));
    }
}

//==============================================================================
// Shared input-channel selector contents.
//
// Lists every hardware input, including channels that the device selector has
// currently switched off. Selecting one of those enables it; this is what lets
// a guitar plugged into any interface input be captured without requiring the
// player to reconfigure the device first.

static void populateInputChannelCombo(juce::ComboBox& box,
                                      juce::AudioDeviceManager& manager,
                                      int preferredHardwareChannel)
{
    box.clear(juce::dontSendNotification);
    box.addItem("Auto (any active input)", 1);

    if (auto* device = manager.getCurrentAudioDevice())
    {
        const auto names = device->getInputChannelNames();
        const auto active = device->getActiveInputChannels();
        const int maxChannels = juce::jmin(names.size(), 32);

        for (int hardware = 0; hardware < maxChannels; ++hardware)
        {
            auto name = names[hardware].trim();

            if (name.isEmpty())
                name = "Input " + juce::String(hardware + 1);

            if (! active[hardware])
                name << " (off)";

            box.addItem(name, hardware + 2);
        }
    }

    const int requestedId = preferredHardwareChannel < 0
        ? 1
        : preferredHardwareChannel + 2;

    box.setSelectedId(
        box.indexOfItemId(requestedId) >= 0 ? requestedId : 1,
        juce::dontSendNotification);
}

//==============================================================================
// Branded audio settings panel used by the Audio Settings dialog.
// Keeps JUCE's native device selector and its existing deviceManager bindings,
// while placing it inside the app's dark navy/gold visual system.
class AudioSettingsPanel final : public juce::Component,
                                 private juce::ChangeListener
{
public:
    AudioSettingsPanel(juce::AudioDeviceManager& manager,
                       int initialInputChannel,
                       std::function<void(int)> onInputChannelChanged,
                       bool initialGuitarInputOnly,
                       std::function<void(bool)> onGuitarInputOnlyChanged,
                       float initialBackgroundVolume,
                       bool initialBackgroundEnabled,
                       std::function<void(float)> onBackgroundVolumeChanged,
                       std::function<void(bool)> onBackgroundEnabledChanged)
        : selector(manager, 0, 32, 0, 2, false, false, true, false),
          deviceManager(manager),
          inputChannelChanged(onInputChannelChanged),
          guitarInputOnlyChanged(onGuitarInputOnlyChanged),
          backgroundVolumeChanged(onBackgroundVolumeChanged),
          backgroundEnabledChanged(onBackgroundEnabledChanged)
    {
        setSize(760, 680);
        addAndMakeVisible(selector);
        addAndMakeVisible(inputChannelLabel);
        addAndMakeVisible(inputChannelBox);

        // Guitar-only input filter. A voice at the chart's pitch is detected
        // exactly like a guitar note, so the client rejects non-plucked
        // attacks. The switch lives here because an unusual tone may need the
        // gate off.
        inputFilterLabel.setText("INPUT FILTER", juce::dontSendNotification);
        inputFilterLabel.setJustificationType(juce::Justification::centredLeft);
        inputFilterLabel.setColour(juce::Label::textColourId, UiColors::text);
        inputFilterLabel.setFont(juce::FontOptions().withHeight(12.0f).withStyle("Bold"));
        addAndMakeVisible(inputFilterLabel);

        inputFilterToggle.setClickingTogglesState(true);
        inputFilterToggle.setToggleState(initialGuitarInputOnly,
                                         juce::dontSendNotification);
        inputFilterToggle.setButtonText(initialGuitarInputOnly ? "GUITAR ONLY"
                                                               : "ANY INPUT");
        inputFilterToggle.setTooltip(
            "Ignore singing and other instruments. The detector expects a "
            "plucked string. Turn this off if your guitar is not recognised.");
        inputFilterToggle.setColour(juce::TextButton::buttonOnColourId,
                                    UiColors::accentStrong);
        inputFilterToggle.setColour(juce::TextButton::textColourOnId,
                                    UiColors::onAccent);
        inputFilterToggle.onClick = [this]
        {
            const bool only = inputFilterToggle.getToggleState();
            inputFilterToggle.setButtonText(only ? "GUITAR ONLY" : "ANY INPUT");

            if (guitarInputOnlyChanged)
                guitarInputOnlyChanged(only);
        };
        addAndMakeVisible(inputFilterToggle);

        inputFilterStatusLabel.setText("Rejects voice and other instruments.",
                                       juce::dontSendNotification);
        inputFilterStatusLabel.setJustificationType(juce::Justification::centredLeft);
        inputFilterStatusLabel.setColour(juce::Label::textColourId, UiColors::muted);
        inputFilterStatusLabel.setFont(juce::FontOptions().withHeight(11.0f));
        addAndMakeVisible(inputFilterStatusLabel);

        // Background music controls: one global loop for the non-game screens.
        backgroundLabel.setText("BACKGROUND MUSIC", juce::dontSendNotification);
        backgroundLabel.setJustificationType(juce::Justification::centredLeft);
        backgroundLabel.setColour(juce::Label::textColourId, UiColors::text);
        backgroundLabel.setFont(juce::FontOptions().withHeight(12.0f).withStyle("Bold"));
        addAndMakeVisible(backgroundLabel);

        backgroundVolume.setSliderStyle(juce::Slider::LinearHorizontal);
        backgroundVolume.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 20);
        backgroundVolume.setTextValueSuffix(" %");
        backgroundVolume.setRange(0.0, 100.0, 1.0);
        backgroundVolume.setNumDecimalPlacesToDisplay(0);
        backgroundVolume.setValue(initialBackgroundVolume * 100.0,
                                  juce::dontSendNotification);
        backgroundVolume.setTooltip("Volume of the looped menu music. "
                                    "Game instruments and the metronome are unaffected.");
        backgroundVolume.onValueChange = [this]
        {
            if (backgroundVolumeChanged)
                backgroundVolumeChanged(
                    static_cast<float>(backgroundVolume.getValue() / 100.0));
        };
        addAndMakeVisible(backgroundVolume);

        backgroundToggle.setClickingTogglesState(true);
        backgroundToggle.setToggleState(initialBackgroundEnabled,
                                        juce::dontSendNotification);
        backgroundToggle.setButtonText(initialBackgroundEnabled ? "ON" : "OFF");
        backgroundToggle.setTooltip("Enable or disable the looped menu music.");
        backgroundToggle.setColour(juce::TextButton::buttonOnColourId,
                                   UiColors::accentStrong);
        backgroundToggle.setColour(juce::TextButton::textColourOnId,
                                   UiColors::onAccent);
        backgroundToggle.onClick = [this]
        {
            const bool enabled = backgroundToggle.getToggleState();
            backgroundToggle.setButtonText(enabled ? "ON" : "OFF");

            if (backgroundEnabledChanged)
                backgroundEnabledChanged(enabled);
        };
        addAndMakeVisible(backgroundToggle);

        backgroundStatusLabel.setText("Menu screens only.",
                                      juce::dontSendNotification);
        backgroundStatusLabel.setJustificationType(juce::Justification::centredLeft);
        backgroundStatusLabel.setColour(juce::Label::textColourId, UiColors::muted);
        backgroundStatusLabel.setFont(juce::FontOptions().withHeight(11.0f));
        addAndMakeVisible(backgroundStatusLabel);

        // Keep the native selector readable on the dark theme.
        std::function<void(juce::Component&)> applyReadableAudioColours;
        applyReadableAudioColours = [&applyReadableAudioColours](juce::Component& component)
        {
            if (auto* label = dynamic_cast<juce::Label*>(&component))
                label->setColour(juce::Label::textColourId, UiColors::text);

            if (auto* combo = dynamic_cast<juce::ComboBox*>(&component))
            {
                combo->setColour(juce::ComboBox::textColourId, UiColors::text);
                combo->setColour(juce::ComboBox::backgroundColourId, UiColors::card);
                combo->setColour(juce::ComboBox::outlineColourId, UiColors::border);
                combo->setColour(juce::ComboBox::arrowColourId, UiColors::accent);
            }

            if (auto* button = dynamic_cast<juce::Button*>(&component))
                button->setColour(juce::TextButton::textColourOffId, UiColors::text);

            for (int i = 0; i < component.getNumChildComponents(); ++i)
                applyReadableAudioColours(*component.getChildComponent(i));
        };
        applyReadableAudioColours(selector);

        inputChannelLabel.setText("GUITAR INPUT", juce::dontSendNotification);
        inputChannelLabel.setJustificationType(juce::Justification::centredLeft);
        inputChannelLabel.setColour(juce::Label::textColourId, UiColors::text);
        inputChannelLabel.setFont(juce::FontOptions().withHeight(12.0f).withStyle("Bold"));

        // Explicit buffer-size control. The device selector's own combo does
        // not always push the value to the ASIO driver, so the size is applied
        // here and then verified against what the driver actually opened.
        bufferLabel.setText("BUFFER SIZE", juce::dontSendNotification);
        bufferLabel.setJustificationType(juce::Justification::centredLeft);
        bufferLabel.setColour(juce::Label::textColourId, UiColors::text);
        bufferLabel.setFont(juce::FontOptions().withHeight(12.0f).withStyle("Bold"));
        addAndMakeVisible(bufferLabel);

        for (int size : { 16, 32, 64, 128, 256, 512, 1024, 2048 })
            bufferBox.addItem(juce::String(size) + " samples", size);

        bufferBox.setColour(juce::ComboBox::textColourId, UiColors::text);
        bufferBox.setColour(juce::ComboBox::backgroundColourId, UiColors::card);
        bufferBox.setColour(juce::ComboBox::outlineColourId, UiColors::border);
        bufferBox.setColour(juce::ComboBox::arrowColourId, UiColors::accent);
        bufferBox.setTooltip("Audio buffer size. The value is sent to the driver; "
                             "if the driver rejects it, the previous size is kept.");
        bufferBox.onChange = [this]
        {
            if (! syncingBufferBox)
                applySelectedBufferSize();
        };
        addAndMakeVisible(bufferBox);

        bufferStatusLabel.setColour(juce::Label::textColourId, UiColors::muted);
        bufferStatusLabel.setFont(juce::FontOptions().withHeight(11.0f));
        bufferStatusLabel.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(bufferStatusLabel);
        syncBufferBoxFromDevice();

        inputChannelBox.setColour(juce::ComboBox::textColourId, UiColors::text);
        inputChannelBox.setColour(juce::ComboBox::backgroundColourId, UiColors::card);
        inputChannelBox.setColour(juce::ComboBox::outlineColourId, UiColors::border);
        inputChannelBox.setColour(juce::ComboBox::arrowColourId, UiColors::accent);
        inputChannelBox.setTooltip("Choose the input channel that carries your guitar. "
                                   "Auto follows the loudest active input.");
        inputChannelBox.onChange = [this]
        {
            if (inputChannelChanged)
                inputChannelChanged(inputChannelBox.getSelectedId() - 2);
        };

        deviceManager.addChangeListener(this);
        rebuildInputChannelChoices(initialInputChannel);
    }

    ~AudioSettingsPanel() override
    {
        deviceManager.removeChangeListener(this);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UiColors::background);

        auto bounds = getLocalBounds().toFloat();
        GameUi::drawCard(g, bounds.reduced(12.0f), 20.0f, true);

        g.setColour(UiColors::accent);
        g.fillRoundedRectangle(juce::Rectangle<float>(30.0f, 29.0f, 5.0f, 36.0f), 2.5f);

        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(25.0f));
        g.drawText("AUDIO SETTINGS", 48, 26, getWidth() - 160, 34,
                   juce::Justification::centredLeft);

        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(13.0f));
        g.drawText("Choose your guitar input, input filter, buffer size and menu music.",
                   48, 59, getWidth() - 80, 24, juce::Justification::centredLeft);

        GameUi::drawPill(g,
                         juce::Rectangle<float>(getWidth() - 134.0f, 31.0f, 96.0f, 22.0f),
                         "AUDIO I/O",
                         UiColors::green,
                         true);

        g.setColour(UiColors::border);
        g.drawLine(30.0f, 96.0f, static_cast<float>(getWidth() - 30), 96.0f, 1.0f);
        g.drawLine(30.0f, 156.0f, static_cast<float>(getWidth() - 30), 156.0f, 1.0f);
        g.drawLine(30.0f, static_cast<float>(getHeight() - 80),
                   static_cast<float>(getWidth() - 30),
                   static_cast<float>(getHeight() - 80), 1.0f);

        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(10.5f));
        g.drawText("Auto follows the loudest active input. Enable extra inputs above for other channels.",
                   30, getHeight() - 34, getWidth() - 60, 20,
                   juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        backgroundLabel.setBounds(30, 112, 150, 24);
        backgroundToggle.setBounds(190, 110, 72, 28);
        backgroundVolume.setBounds(272, 110, 300, 28);
        backgroundStatusLabel.setBounds(582, 112, getWidth() - 612, 24);

        selector.setBounds(30, 164, getWidth() - 60,
                           juce::jmax(80, getHeight() - 324));
        inputFilterLabel.setBounds(30, getHeight() - 150, 130, 24);
        inputFilterToggle.setBounds(168, getHeight() - 152, 132, 28);
        inputFilterStatusLabel.setBounds(310, getHeight() - 150,
                                         getWidth() - 340, 24);
        bufferLabel.setBounds(30, getHeight() - 110, 130, 24);
        bufferBox.setBounds(168, getHeight() - 112, 200, 28);
        bufferStatusLabel.setBounds(380, getHeight() - 112, getWidth() - 410, 28);
        inputChannelLabel.setBounds(30, getHeight() - 64, 130, 24);
        inputChannelBox.setBounds(168, getHeight() - 66, 320, 28);
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        rebuildInputChannelChoices(-1);
        syncBufferBoxFromDevice();
    }

    // Sends the selected size to the driver and verifies the result. A driver
    // may reject the request or silently substitute a different size; in both
    // cases the UI must show what is actually running.
    void applySelectedBufferSize()
    {
        const int requested = bufferBox.getSelectedId();

        if (requested <= 0)
            return;

        auto* device = deviceManager.getCurrentAudioDevice();

        if (device == nullptr)
            return;

        auto setup = deviceManager.getAudioDeviceSetup();
        const auto previousSetup = setup;

        setup.bufferSize = requested;

        const auto error = deviceManager.setAudioDeviceSetup(setup, true);

        if (error.isNotEmpty())
        {
            deviceManager.setAudioDeviceSetup(previousSetup, true);
            bufferStatusLabel.setText(
                "Driver rejected " + juce::String(requested)
                    + " samples. Kept the previous size.",
                juce::dontSendNotification);
            syncBufferBoxFromDevice();
            return;
        }

        const int actual = device->getCurrentBufferSizeSamples();

        if (actual > 0 && actual != requested)
        {
            bufferStatusLabel.setText(
                "Driver opened " + juce::String(actual)
                    + " samples instead of " + juce::String(requested) + ".",
                juce::dontSendNotification);
        }
        else
        {
            bufferStatusLabel.setText(
                "Running with " + juce::String(actual) + " samples.",
                juce::dontSendNotification);
        }

        syncBufferBoxFromDevice();
    }

    void syncBufferBoxFromDevice()
    {
        const juce::ScopedValueSetter<bool> guard(syncingBufferBox, true);

        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            const int actual = device->getCurrentBufferSizeSamples();

            if (actual > 0)
            {
                if (bufferBox.indexOfItemId(actual) < 0)
                    bufferBox.addItem(juce::String(actual) + " samples", actual);

                bufferBox.setSelectedId(actual, juce::dontSendNotification);
            }
        }
    }

    void rebuildInputChannelChoices(int initialChannel)
    {
        const int selected = (inputChannelBox.getSelectedId() > 0)
            ? inputChannelBox.getSelectedId() - 2
            : initialChannel;

        populateInputChannelCombo(inputChannelBox, deviceManager, selected);
    }

    juce::AudioDeviceSelectorComponent selector;
    juce::AudioDeviceManager& deviceManager;
    std::function<void(int)> inputChannelChanged;
    std::function<void(bool)> guitarInputOnlyChanged;
    std::function<void(float)> backgroundVolumeChanged;
    std::function<void(bool)> backgroundEnabledChanged;
    juce::Label inputChannelLabel;
    juce::ComboBox inputChannelBox;
    juce::Label inputFilterLabel;
    juce::TextButton inputFilterToggle;
    juce::Label inputFilterStatusLabel;
    juce::Label bufferLabel;
    juce::ComboBox bufferBox;
    juce::Label bufferStatusLabel;
    juce::Label backgroundLabel;
    juce::Slider backgroundVolume;
    juce::TextButton backgroundToggle;
    juce::Label backgroundStatusLabel;
    bool syncingBufferBox = false;
};

//==============================================================================
// Personal account window.
//
// The layout mirrors the web account page: rating summary, global top-50,
// attempt history, profile/password forms and account details. Requests share
// the main window's ApiClient; every async callback is guarded by a
// Component::SafePointer.
//==============================================================================
namespace AccountWindowDetail
{
    juce::String formatCount(int value)
    {
        auto text = juce::String(value);
        for (int i = text.length() - 3; i > 0; i -= 3)
            text = text.substring(0, i) + " " + text.substring(i);
        return text;
    }

    juce::String formatPercent(double value)
    {
        return juce::String(value, 1).replace(".", ",") + "%";
    }

    juce::String formatDate(const juce::String& iso)
    {
        if (iso.isEmpty())
            return "-";

        auto text = iso;
        const int dot = text.indexOfChar('.');
        if (dot > 0)
        {
            const int z = text.indexOfChar('Z');
            text = z > dot ? text.substring(0, dot) + text.substring(z)
                           : text.substring(0, dot);
        }

        const auto time = juce::Time::fromISO8601(text);
        if (time.toMilliseconds() == 0)
            return iso;

        const auto formatted = time.formatted("%d.%m.%y %H:%M");
        return formatted.isNotEmpty() ? formatted : iso;
    }

    juce::String apiErrorMessage(const ApiClient::Response& response)
    {
        if (!response.transportOk)
            return "No connection to the server.";

        const auto detail = response.json.getProperty("detail", juce::var());
        if (detail.isString())
        {
            const auto text = detail.toString();
            if (text == "Username is already taken")
                return "This username is already taken.";
            if (text == "Current password is incorrect")
                return "The current password is incorrect.";
            if (text == "Display name cannot be blank")
                return "Display name cannot be blank.";
            return text;
        }

        if (detail.isArray())
            return "Check the fields and try again.";

        switch (response.statusCode)
        {
            case 401: return "Session expired. Please sign in again.";
            case 403: return "You do not have access to this action.";
            case 409: return "This value is already taken.";
            case 422: return "Check the fields and try again.";
            case 429: return "Too many attempts. Try again later.";
            case 500: return "The server could not process the request.";
            case 503: return "The service is temporarily unavailable.";
            default: break;
        }

        return "Request failed (" + juce::String(response.statusCode) + ").";
    }

    void styleTextEditor(juce::TextEditor& editor,
                         const juce::String& placeholder,
                         int maxCharacters,
                         const juce::String& allowedCharacters = {})
    {
        editor.setFont(GameUi::font(13.0f));
        editor.setColour(juce::TextEditor::backgroundColourId, UiColors::surface2);
        editor.setColour(juce::TextEditor::textColourId, UiColors::text);
        editor.setColour(juce::TextEditor::outlineColourId, UiColors::border);
        editor.setColour(juce::TextEditor::focusedOutlineColourId, UiColors::accent);
        editor.setColour(juce::TextEditor::highlightColourId, UiColors::accent.withAlpha(0.25f));
        editor.setColour(juce::TextEditor::highlightedTextColourId, UiColors::text);
        editor.setColour(juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
        editor.setTextToShowWhenEmpty(placeholder, UiColors::muted);
        editor.setInputRestrictions(maxCharacters, allowedCharacters);
        editor.setMultiLine(false);
        editor.setReturnKeyStartsNewLine(false);
        editor.setIndents(8, 6);
    }

    void styleButton(juce::TextButton& button, bool primary)
    {
        button.setColour(juce::TextButton::buttonColourId,
                         primary ? UiColors::accent : UiColors::surface2);
        button.setColour(juce::TextButton::buttonOnColourId,
                         primary ? UiColors::accentStrong : UiColors::surface);
        button.setColour(juce::TextButton::textColourOffId,
                         primary ? UiColors::onAccent : UiColors::text);
        button.setColour(juce::TextButton::textColourOnId,
                         primary ? UiColors::onAccent : UiColors::text);
        button.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    struct AchievementCard
    {
        juce::String code;
        juce::String title;
        juce::String description;
        juce::String iconUrl;
        juce::String awardedAt;
        int xpBonus = 0;
        int progress = 0;
        int goal = 0;
        bool unlocked = false;
    };

    // One badge tile. The bundled artwork is a dark glyph on a transparent
    // background, exactly like on the site, so it is drawn on the same light
    // rounded plate instead of being recoloured into the accent colour.
    void paintAchievementTile(juce::Graphics& g,
                              const juce::Rectangle<int>& tile,
                              const AchievementCard& card,
                              const juce::Image* icon)
    {
        const bool unlocked = card.unlocked;

        g.setColour(unlocked ? UiColors::surface2.withAlpha(0.92f)
                             : UiColors::surface2.withAlpha(0.45f));
        g.fillRoundedRectangle(tile.toFloat(), 12.0f);
        g.setColour(unlocked ? UiColors::accent.withAlpha(0.55f)
                             : UiColors::border);
        g.drawRoundedRectangle(tile.toFloat(), 12.0f, 1.0f);

        constexpr int iconSize = 52;
        const int iconX = tile.getX() + 12;
        const int iconY = tile.getY() + (tile.getHeight() - iconSize) / 2;
        const juce::Rectangle<float> iconTile(
            static_cast<float>(iconX), static_cast<float>(iconY),
            static_cast<float>(iconSize), static_cast<float>(iconSize));

        g.setColour(unlocked ? juce::Colour(0xFFE9EFF6).withAlpha(0.92f)
                             : juce::Colour(0xFF8D9CAB).withAlpha(0.16f));
        g.fillRoundedRectangle(iconTile, 13.0f);

        if (icon != nullptr)
        {
            g.setOpacity(unlocked ? 1.0f : 0.35f);
            g.drawImage(*icon, iconTile,
                        juce::RectanglePlacement::centred, false);
            g.setOpacity(1.0f);
        }

        const int textX = iconX + iconSize + 12;
        const int textW = tile.getRight() - textX - 12;

        g.setColour(unlocked ? UiColors::text : UiColors::muted);
        g.setFont(GameUi::font(12.5f, true));
        g.drawFittedText(card.title,
                         juce::Rectangle<int>(textX, tile.getY() + 8,
                                              textW, 16),
                         juce::Justification::centredLeft, 1);

        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(9.5f));
        g.drawFittedText(card.description,
                         juce::Rectangle<int>(textX, tile.getY() + 26,
                                              textW, 26),
                         juce::Justification::topLeft, 2);

        juce::String status;

        if (unlocked)
        {
            status = "Unlocked";

            if (card.awardedAt.isNotEmpty())
                status << "  " << card.awardedAt.substring(0, 10);

            status << "   +" << card.xpBonus << " XP";
        }
        else
        {
            status = "Locked";

            if (card.goal > 0)
                status << "   " << card.progress << "/" << card.goal;

            status << "   +" << card.xpBonus << " XP";
        }

        g.setColour(unlocked ? UiColors::green : UiColors::muted);
        g.setFont(GameUi::font(9.0f, true));
        g.drawFittedText(status,
                         juce::Rectangle<int>(textX, tile.getBottom() - 20,
                                              textW, 14),
                         juce::Justification::centredLeft, 1);
    }

    // Scrolling list modelled on the website profile: every achievement is
    // listed with its icon, title, description and progress, so the account
    // window shows the same badge artwork the browser does. Two tiles share a
    // row; the ListBox owns scrolling and clipping.
    class AchievementListModel final : public juce::ListBoxModel
    {
    public:
        void setData(const std::vector<AchievementCard>* cardsToUse,
                     const std::map<juce::String, juce::Image>* iconsToUse)
        {
            cards = cardsToUse;
            icons = iconsToUse;
        }

        int getNumRows() override
        {
            const int count = cards != nullptr
                ? static_cast<int>(cards->size()) : 0;
            return (count + 1) / 2;
        }

        void paintListBoxItem(int rowNumber, juce::Graphics& g,
                              int width, int height, bool) override
        {
            if (cards == nullptr || rowNumber < 0)
                return;

            constexpr int gap = 8;
            const int tileWidth = juce::jmax(80, (width - gap) / 2);
            const int firstIndex = rowNumber * 2;

            for (int offset = 0; offset < 2; ++offset)
            {
                const int index = firstIndex + offset;

                if (index >= static_cast<int>(cards->size()))
                    break;

                const auto& card = (*cards)[static_cast<size_t>(index)];
                const juce::Image* icon = nullptr;

                if (icons != nullptr)
                {
                    const auto found = icons->find(card.code);

                    if (found != icons->end() && found->second.isValid())
                        icon = &found->second;
                }

                paintAchievementTile(
                    g,
                    juce::Rectangle<int>(offset * (tileWidth + gap), 0,
                                         tileWidth, height),
                    card, icon);
            }
        }

    private:
        const std::vector<AchievementCard>* cards = nullptr;
        const std::map<juce::String, juce::Image>* icons = nullptr;
    };
}

class AccountWindow final : public juce::Component
{
public:
    struct Callbacks
    {
        std::function<void(const juce::String& username,
                           const juce::String& displayName)> onProfileUpdated;
        std::function<void()> onLogout;
        std::function<void()> onSessionExpired;
    };

    AccountWindow(ApiClient& apiToUse, const juce::String& accessToken,
                  Callbacks callbacksToUse)
        : api(apiToUse),
          token(accessToken),
          callbacks(std::move(callbacksToUse)),
          ratingModel(*this),
          historyModel(*this)
    {
        setSize(1180, 900);
        setOpaque(true);
        configureUi();
        resized();
        refreshAll();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UiColors::background);

        // Top bar.
        BrandAssets::drawLogo(g, { 24.0f, 20.0f, 34.0f, 34.0f });

        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(18.0f));
        g.drawText("FRETFIGHT", juce::Rectangle<float>(68.0f, 20.0f, 240.0f, 22.0f),
                   juce::Justification::centredLeft, false);

        g.setColour(UiColors::accent);
        g.setFont(GameUi::eyebrow(9.0f));
        g.drawText("PERSONAL ACCOUNT",
                   juce::Rectangle<float>(68.0f, 42.0f, 300.0f, 14.0f),
                   juce::Justification::centredLeft, false);

        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(12.0f));
        g.drawText(welcomeText,
                   juce::Rectangle<float>(24.0f, 64.0f,
                                          static_cast<float>(getWidth()) - 280.0f, 18.0f),
                   juce::Justification::centredLeft, true);

        // Score summary cards.
        const juce::String statLabels[] = { "SCORES", "RANK", "SONGS", "ATTEMPTS",
                                            "LEVEL", "XP", "ACCURACY", "TIMING" };
        const juce::String statValues[] = { ratingValue, rankValue, songsValue, attemptsValue,
                                            levelValue, xpValue, accuracyValue, timingValue };
        const juce::String statNotes[] = { "best score per song", rankNote,
                                           "songs with an attempt", "total attempts",
                                           xpNote, "total experience",
                                           "last 100 attempts", "avg timing error" };

        const int statWidth = (statsArea.getWidth() - 7 * 12) / 8;
        for (int i = 0; i < 8; ++i)
        {
            const juce::Rectangle<int> card(statsArea.getX() + i * (statWidth + 12),
                                            statsArea.getY(),
                                            statWidth,
                                            statsArea.getHeight());
            GameUi::drawCard(g, card.toFloat(), 14.0f);
            GameUi::drawEyebrow(g, statLabels[i],
                                juce::Rectangle<int>(card.getX() + 16, card.getY() + 12,
                                                     card.getWidth() - 32, 14).toFloat(),
                                UiColors::accent);

            g.setColour(UiColors::text);
            g.setFont(GameUi::heading(25.0f));
            g.drawText(statValues[i],
                       juce::Rectangle<int>(card.getX() + 16, card.getY() + 32,
                                            card.getWidth() - 32, 30).toFloat(),
                       juce::Justification::centredLeft, false);

            g.setColour(UiColors::muted);
            g.setFont(GameUi::font(10.5f));
            g.drawText(statNotes[i],
                       juce::Rectangle<int>(card.getX() + 16, card.getBottom() - 24,
                                            card.getWidth() - 32, 16).toFloat(),
                       juce::Justification::centredLeft, true);
        }

        auto drawCardHeader = [&g](const juce::Rectangle<int>& card,
                                   const juce::String& title,
                                   const juce::String& hint)
        {
            g.setColour(UiColors::text);
            g.setFont(GameUi::heading(17.0f));
            g.drawText(title,
                       juce::Rectangle<int>(card.getX() + 20, card.getY() + 16,
                                            card.getWidth() - 40, 22).toFloat(),
                       juce::Justification::centredLeft, false);

            if (hint.isNotEmpty())
            {
                g.setColour(UiColors::muted);
                g.setFont(GameUi::font(11.0f));
                g.drawText(hint,
                           juce::Rectangle<int>(card.getX() + 20, card.getY() + 38,
                                                card.getWidth() - 40, 18).toFloat(),
                           juce::Justification::centredLeft, true);
            }
        };

        GameUi::drawCard(g, ratingCard.toFloat(), 16.0f);
        drawCardHeader(ratingCard, "GLOBAL SCORES",
                       "Sum of the best score on every song. Top 50 players.");

        GameUi::drawCard(g, historyCard.toFloat(), 16.0f);
        drawCardHeader(historyCard, "ATTEMPT HISTORY",
                       "Your latest games: score, accuracy and max combo.");

        GameUi::drawCard(g, profileCard.toFloat(), 16.0f);
        drawCardHeader(profileCard, "PROFILE", {});

        g.setColour(UiColors::muted);
        g.setFont(GameUi::eyebrow(9.0f));
        g.drawText("USERNAME",
                   juce::Rectangle<int>(usernameEditorArea.getX(),
                                        usernameEditorArea.getY() - 15,
                                        usernameEditorArea.getWidth(), 13).toFloat(),
                   juce::Justification::centredLeft, false);
        g.drawText("DISPLAY NAME",
                   juce::Rectangle<int>(displayNameEditorArea.getX(),
                                        displayNameEditorArea.getY() - 15,
                                        displayNameEditorArea.getWidth(), 13).toFloat(),
                   juce::Justification::centredLeft, false);

        GameUi::drawCard(g, passwordCard.toFloat(), 16.0f);
        drawCardHeader(passwordCard, "PASSWORD", {});

        g.setColour(UiColors::muted);
        g.setFont(GameUi::eyebrow(9.0f));
        g.drawText("CURRENT PASSWORD",
                   juce::Rectangle<int>(currentPasswordEditorArea.getX(),
                                        currentPasswordEditorArea.getY() - 15,
                                        currentPasswordEditorArea.getWidth(), 13).toFloat(),
                   juce::Justification::centredLeft, false);
        g.drawText("NEW PASSWORD",
                   juce::Rectangle<int>(newPasswordEditorArea.getX(),
                                        newPasswordEditorArea.getY() - 15,
                                        newPasswordEditorArea.getWidth(), 13).toFloat(),
                   juce::Justification::centredLeft, false);

        GameUi::drawCard(g, accountCard.toFloat(), 16.0f);
        drawCardHeader(accountCard, "ACCOUNT", {});

        const auto accountInner = accountCard.reduced(20);
        constexpr int accountInfoW = 340;

        g.setColour(UiColors::muted);
        g.setFont(GameUi::eyebrow(9.0f));
        g.drawText("EMAIL",
                   juce::Rectangle<int>(accountInner.getX(), accountInner.getY() + 28,
                                        accountInfoW, 13).toFloat(),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::text);
        g.setFont(GameUi::font(12.0f));
        g.drawText(meEmail.isNotEmpty() ? meEmail : "-",
                   juce::Rectangle<int>(accountInner.getX(), accountInner.getY() + 42,
                                        accountInfoW, 18).toFloat(),
                   juce::Justification::centredLeft, true);

        g.setColour(UiColors::muted);
        g.setFont(GameUi::eyebrow(9.0f));
        g.drawText("MEMBER SINCE",
                   juce::Rectangle<int>(accountInner.getX(), accountInner.getY() + 70,
                                        accountInfoW, 13).toFloat(),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::text);
        g.setFont(GameUi::font(12.0f));
        g.drawText(AccountWindowDetail::formatDate(meCreatedAt),
                   juce::Rectangle<int>(accountInner.getX(), accountInner.getY() + 84,
                                        accountInfoW, 18).toFloat(),
                   juce::Justification::centredLeft, true);

        // Account icon: a large yellow user glyph in the card's top-right.
        {
            const float iconSize = 56.0f;
            const auto iconArea = juce::Rectangle<float>(
                accountInner.getRight() - iconSize, accountInner.getY() + 18.0f,
                iconSize, iconSize);

            g.setColour(UiColors::accent);
            g.fillEllipse(iconArea.getX() + iconSize * 0.30f,
                          iconArea.getY() + iconSize * 0.10f,
                          iconSize * 0.40f, iconSize * 0.40f);
            g.fillRoundedRectangle(iconArea.getX() + iconSize * 0.10f,
                                   iconArea.getY() + iconSize * 0.56f,
                                   iconSize * 0.80f, iconSize * 0.36f,
                                   iconSize * 0.18f);
        }

        if (achievementsTotalCount > 0)
        {
            const int gridX = accountInner.getX();
            const int gridY = accountInner.getY() + 146;
            const int gridW = accountInner.getWidth();

            g.setColour(UiColors::muted);
            g.setFont(GameUi::eyebrow(9.5f));
            g.drawText("ACHIEVEMENTS "
                           + juce::String(achievementsUnlockedCount) + "/"
                           + juce::String(achievementsTotalCount),
                       juce::Rectangle<int>(gridX, gridY - 18, gridW, 14).toFloat(),
                       juce::Justification::centredLeft, false);
        }
    }

    void resized() override
    {
        const int margin = 24;
        const int gap = 14;

        refreshButton.setBounds(getWidth() - margin - 104, 24, 104, 32);
        logoutButton.setBounds(getWidth() - margin - 104 - 12 - 104, 24, 104, 32);

        statsArea = { margin, 96, getWidth() - margin * 2, 92 };

        const int contentTop = statsArea.getBottom() + 16;
        const int contentBottom = getHeight() - 56;
        const int contentWidth = getWidth() - margin * 2;
        const int contentHeight = juce::jmax(300, contentBottom - contentTop);
        const int sideWidth = juce::jmax(300, contentWidth * 34 / 100);
        const int mainWidth = contentWidth - sideWidth - gap;

        const juce::Rectangle<int> mainColumn(margin, contentTop, mainWidth, contentHeight);
        const juce::Rectangle<int> sideColumn(margin + mainWidth + gap,
                                              contentTop, sideWidth, contentHeight);

        const int ratingHeight = juce::jmax(220, contentHeight * 42 / 100);
        ratingCard = { mainColumn.getX(), mainColumn.getY(),
                       mainColumn.getWidth(), ratingHeight };
        accountCard = { mainColumn.getX(), mainColumn.getY() + ratingHeight + gap,
                        mainColumn.getWidth(), contentHeight - ratingHeight - gap };

        constexpr int profileHeight = 184;
        constexpr int passwordHeight = 184;
        const int historyHeight = juce::jmax(200, contentHeight - profileHeight
                                                        - passwordHeight - 2 * gap);

        profileCard = { sideColumn.getX(), sideColumn.getY(),
                        sideColumn.getWidth(), profileHeight };
        passwordCard = { sideColumn.getX(), sideColumn.getY() + profileHeight + gap,
                         sideColumn.getWidth(), passwordHeight };
        historyCard = { sideColumn.getX(),
                        sideColumn.getY() + profileHeight + passwordHeight + 2 * gap,
                        sideColumn.getWidth(), historyHeight };

        const auto ratingInner = ratingCard.reduced(20);
        ratingTable.setBounds(ratingInner.getX(), ratingInner.getY() + 56,
                              ratingInner.getWidth(),
                              juce::jmax(80, ratingInner.getHeight() - 56));
        ratingEmptyLabel.setBounds(ratingTable.getBounds().withTrimmedTop(28));

        const auto historyInner = historyCard.reduced(20);
        constexpr int showMoreHeight = 28;
        const int historyTableHeight = juce::jmax(60, historyInner.getHeight()
                                                         - 56 - showMoreHeight - 8);
        historyTable.setBounds(historyInner.getX(), historyInner.getY() + 56,
                               historyInner.getWidth(), historyTableHeight);
        historyEmptyLabel.setBounds(historyTable.getBounds().withTrimmedTop(28));

        showMoreButtonArea = { historyInner.getRight() - 104,
                               historyInner.getBottom() - showMoreHeight,
                               104, showMoreHeight };
        showMoreButton.setBounds(showMoreButtonArea);

        const auto profileInner = profileCard.reduced(20);
        usernameEditorArea = { profileInner.getX(), profileInner.getY() + 42,
                               profileInner.getWidth(), 28 };
        displayNameEditorArea = { profileInner.getX(), profileInner.getY() + 92,
                                  profileInner.getWidth(), 28 };
        saveProfileButtonArea = { profileInner.getX(), profileInner.getY() + 128,
                                  profileInner.getWidth(), 30 };
        usernameEditor.setBounds(usernameEditorArea);
        displayNameEditor.setBounds(displayNameEditorArea);
        saveProfileButton.setBounds(saveProfileButtonArea);

        const auto passwordInner = passwordCard.reduced(20);
        currentPasswordEditorArea = { passwordInner.getX(), passwordInner.getY() + 42,
                                      passwordInner.getWidth(), 28 };
        newPasswordEditorArea = { passwordInner.getX(), passwordInner.getY() + 92,
                                  passwordInner.getWidth(), 28 };
        changePasswordButtonArea = { passwordInner.getX(), passwordInner.getY() + 128,
                                     passwordInner.getWidth(), 30 };
        currentPasswordEditor.setBounds(currentPasswordEditorArea);
        newPasswordEditor.setBounds(newPasswordEditorArea);
        changePasswordButton.setBounds(changePasswordButtonArea);

        const auto accountInner = accountCard.reduced(20);
        const int achievementsTop = accountInner.getY() + 146;
        achievementList.setBounds(
            accountInner.getX(), achievementsTop, accountInner.getWidth(),
            juce::jmax(80, accountInner.getBottom() - achievementsTop));

        alertLabel.setBounds(margin, getHeight() - 40,
                             getWidth() - margin * 2, 24);
    }

private:
    struct RatingRow
    {
        int rank = 0;
        juce::String username;
        juce::String displayName;
        int ratingPoints = 0;
        int gamesPlayed = 0;
        double bestAccuracy = 0.0;
    };

    struct HistoryRow
    {
        juce::String songTitle;
        juce::String startedAt;
        juce::String status;
        int scorePoints = 0;
        double accuracy = 0.0;
        int maxCombo = 0;
        bool hasScore = false;
        bool hasAccuracy = false;
        bool hasCombo = false;
    };

    class RatingModel final : public juce::TableListBoxModel
    {
    public:
        explicit RatingModel(AccountWindow& ownerToUse) : owner(ownerToUse) {}

        int getNumRows() override
        {
            return static_cast<int>(owner.ratingRows.size());
        }

        void paintRowBackground(juce::Graphics& g, int rowNumber, int, int, bool) override
        {
            if (!juce::isPositiveAndBelow(rowNumber, getNumRows()))
            {
                g.fillAll(juce::Colours::transparentBlack);
                return;
            }

            const auto& row = owner.ratingRows[static_cast<size_t>(rowNumber)];
            g.fillAll(row.username == owner.meUsername
                          ? UiColors::accent.withAlpha(0.08f)
                          : (rowNumber % 2 == 0
                                 ? UiColors::surface.withAlpha(0.35f)
                                 : UiColors::surface2.withAlpha(0.25f)));
        }

        void paintCell(juce::Graphics& g, int rowNumber, int columnId,
                       int width, int height, bool) override
        {
            if (!juce::isPositiveAndBelow(rowNumber, getNumRows()))
                return;

            const auto& row = owner.ratingRows[static_cast<size_t>(rowNumber)];
            const bool isMe = row.username == owner.meUsername;
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
                    // Same rule as the in-game leaderboard: a single name, no
                    // "@username" suffix.
                    text = row.displayName.isNotEmpty() ? row.displayName : row.username;
                    break;
                case 3:
                    text = AccountWindowDetail::formatCount(row.ratingPoints);
                    justification = juce::Justification::centredRight;
                    break;
                case 4:
                    text = AccountWindowDetail::formatCount(row.gamesPlayed);
                    justification = juce::Justification::centredRight;
                    colour = UiColors::secondaryText;
                    break;
                case 5:
                    text = AccountWindowDetail::formatPercent(row.bestAccuracy);
                    justification = juce::Justification::centredRight;
                    colour = UiColors::secondaryText;
                    break;
                default:
                    break;
            }

            g.setColour(colour);
            g.setFont(GameUi::font(12.0f, isMe));
            g.drawText(text, 6, 0, width - 12, height, justification, true);
        }

    private:
        AccountWindow& owner;
    };

    class HistoryModel final : public juce::TableListBoxModel
    {
    public:
        explicit HistoryModel(AccountWindow& ownerToUse) : owner(ownerToUse) {}

        int getNumRows() override
        {
            return static_cast<int>(owner.historyRows.size());
        }

        void paintRowBackground(juce::Graphics& g, int rowNumber, int, int, bool) override
        {
            if (!juce::isPositiveAndBelow(rowNumber, getNumRows()))
            {
                g.fillAll(juce::Colours::transparentBlack);
                return;
            }

            g.fillAll(rowNumber % 2 == 0
                          ? UiColors::surface.withAlpha(0.35f)
                          : UiColors::surface2.withAlpha(0.25f));
        }

        void paintCell(juce::Graphics& g, int rowNumber, int columnId,
                       int width, int height, bool) override
        {
            if (!juce::isPositiveAndBelow(rowNumber, getNumRows()))
                return;

            const auto& row = owner.historyRows[static_cast<size_t>(rowNumber)];
            juce::String text;
            auto justification = juce::Justification::centredLeft;
            auto colour = UiColors::text;

            switch (columnId)
            {
                case 1:
                    text = row.songTitle.isNotEmpty() ? row.songTitle : "-";
                    break;
                case 2:
                    text = row.startedAt;
                    colour = UiColors::secondaryText;
                    break;
                case 3:
                {
                    const auto status = row.status.toLowerCase();
                    const auto label = status == "finished"
                                           ? juce::String("FINISHED")
                                           : (status == "started" ? juce::String("IN GAME")
                                                                  : row.status.toUpperCase());
                    const auto pillColour = status == "finished"
                                                ? UiColors::green
                                                : (status == "started" ? UiColors::accent
                                                                       : UiColors::muted);
                    const auto pillWidth = juce::jmin(width - 12, 84);
                    GameUi::drawPill(g,
                                     juce::Rectangle<float>(
                                         6.0f,
                                         (static_cast<float>(height) - 19.0f) * 0.5f,
                                         static_cast<float>(pillWidth),
                                         19.0f),
                                     label,
                                     pillColour,
                                     false);
                    return;
                }
                case 4:
                    text = row.hasScore ? AccountWindowDetail::formatCount(row.scorePoints)
                                        : "-";
                    justification = juce::Justification::centredRight;
                    break;
                case 5:
                    text = row.hasAccuracy ? AccountWindowDetail::formatPercent(row.accuracy)
                                           : "-";
                    justification = juce::Justification::centredRight;
                    colour = UiColors::secondaryText;
                    break;
                case 6:
                    text = row.hasCombo
                               ? "x" + AccountWindowDetail::formatCount(row.maxCombo)
                               : "-";
                    justification = juce::Justification::centredRight;
                    colour = UiColors::secondaryText;
                    break;
                default:
                    break;
            }

            g.setColour(colour);
            g.setFont(GameUi::font(12.0f));
            g.drawText(text, 6, 0, width - 12, height, justification, true);
        }

    private:
        AccountWindow& owner;
    };

    void configureUi()
    {
        auto configureTable = [](juce::TableListBox& table)
        {
            table.setColour(juce::ListBox::backgroundColourId,
                            juce::Colours::transparentBlack);
            table.setColour(juce::ListBox::outlineColourId,
                            juce::Colours::transparentBlack);
            table.setOutlineThickness(0);
            table.setRowHeight(32);
            table.setHeaderHeight(28);
            table.getViewport()->setScrollBarsShown(true, false);

            auto& header = table.getHeader();
            header.setColour(juce::TableHeaderComponent::backgroundColourId,
                             UiColors::surface2);
            header.setColour(juce::TableHeaderComponent::textColourId,
                             UiColors::muted);
            header.setColour(juce::TableHeaderComponent::outlineColourId,
                             UiColors::border);
            header.setColour(juce::TableHeaderComponent::highlightColourId,
                             UiColors::accent.withAlpha(0.25f));
            header.setStretchToFitActive(true);
        };

        configureTable(ratingTable);
        configureTable(historyTable);

        ratingTable.getHeader().addColumn(
            "#", 1, 46, 40, 70, juce::TableHeaderComponent::notSortable);
        ratingTable.getHeader().addColumn(
            "PLAYER", 2, 220, 120, 420, juce::TableHeaderComponent::notSortable);
        ratingTable.getHeader().addColumn(
            "SCORES", 3, 100, 70, 160, juce::TableHeaderComponent::notSortable);
        ratingTable.getHeader().addColumn(
            "SONGS", 4, 80, 60, 120, juce::TableHeaderComponent::notSortable);
        ratingTable.getHeader().addColumn(
            "ACCURACY", 5, 100, 80, 150, juce::TableHeaderComponent::notSortable);
        addAndMakeVisible(ratingTable);

        historyTable.getHeader().addColumn(
            "SONG", 1, 170, 120, 420, juce::TableHeaderComponent::notSortable);
        historyTable.getHeader().addColumn(
            "STARTED", 2, 106, 90, 180, juce::TableHeaderComponent::notSortable);
        historyTable.getHeader().addColumn(
            "STATUS", 3, 84, 70, 120, juce::TableHeaderComponent::notSortable);
        historyTable.getHeader().addColumn(
            "SCORE", 4, 66, 60, 120, juce::TableHeaderComponent::notSortable);
        historyTable.getHeader().addColumn(
            "ACCURACY", 5, 76, 70, 130, juce::TableHeaderComponent::notSortable);
        historyTable.getHeader().addColumn(
            "COMBO", 6, 66, 60, 120, juce::TableHeaderComponent::notSortable);
        addAndMakeVisible(historyTable);

        for (auto* label : { &ratingEmptyLabel, &historyEmptyLabel })
        {
            label->setFont(GameUi::font(12.0f));
            label->setColour(juce::Label::textColourId, UiColors::muted);
            label->setJustificationType(juce::Justification::centred);
            label->setInterceptsMouseClicks(false, false);
            addAndMakeVisible(label);
            label->setVisible(false);
        }
        ratingEmptyLabel.setText("No scores yet.", juce::dontSendNotification);
        historyEmptyLabel.setText(
            "No attempts yet. Play a song and the result will appear here.",
            juce::dontSendNotification);

        AccountWindowDetail::styleTextEditor(
            usernameEditor, "username", 32,
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-");
        AccountWindowDetail::styleTextEditor(displayNameEditor, "display name", 64);
        AccountWindowDetail::styleTextEditor(currentPasswordEditor, "current password", 128);
        currentPasswordEditor.setPasswordCharacter(0x2022);
        AccountWindowDetail::styleTextEditor(
            newPasswordEditor, "new password (min 6 characters)", 128);
        newPasswordEditor.setPasswordCharacter(0x2022);

        addAndMakeVisible(usernameEditor);
        addAndMakeVisible(displayNameEditor);
        addAndMakeVisible(currentPasswordEditor);
        addAndMakeVisible(newPasswordEditor);

        usernameEditor.onTextChange = [this] { profileDirty = true; };
        displayNameEditor.onTextChange = [this] { profileDirty = true; };

        AccountWindowDetail::styleButton(saveProfileButton, true);
        AccountWindowDetail::styleButton(changePasswordButton, true);
        AccountWindowDetail::styleButton(refreshButton, false);
        AccountWindowDetail::styleButton(logoutButton, false);
        AccountWindowDetail::styleButton(showMoreButton, false);

        saveProfileButton.onClick = [this] { saveProfile(); };
        changePasswordButton.onClick = [this] { changePassword(); };
        refreshButton.onClick = [this] { refreshAll(); };
        logoutButton.onClick = [this] { logout(); };
        showMoreButton.onClick = [this] { loadHistory(false); };
        showMoreButton.setVisible(false);

        addAndMakeVisible(saveProfileButton);
        addAndMakeVisible(changePasswordButton);
        addAndMakeVisible(refreshButton);
        addAndMakeVisible(logoutButton);
        addAndMakeVisible(showMoreButton);

        // The achievement list scrolls inside the account card, so every badge
        // is visible with the same artwork the website profile shows.
        achievementList.setRowHeight(88);
        achievementList.setOutlineThickness(0);
        achievementList.setColour(juce::ListBox::backgroundColourId,
                                  juce::Colours::transparentBlack);
        achievementList.setColour(juce::ListBox::outlineColourId,
                                  juce::Colours::transparentBlack);
        achievementList.setWantsKeyboardFocus(false);
        achievementModel.setData(&achievementCards, &achievementIcons);
        achievementList.setVisible(false);
        addAndMakeVisible(achievementList);

        alertLabel.setFont(GameUi::font(12.0f));
        alertLabel.setColour(juce::Label::textColourId, UiColors::muted);
        alertLabel.setJustificationType(juce::Justification::centredLeft);
        alertLabel.setVisible(false);
        addAndMakeVisible(alertLabel);
    }

    void refreshAll()
    {
        if (sessionExpired)
            return;

        refreshHadError = false;
        showAlert("Loading account...", UiColors::muted);
        loadMe();
        loadSummary();
        loadRating();
        loadHistory(true);
        loadProgression();
        loadAchievements();
    }

    // Unlocked achievements shown in the account card.
    void loadAchievements()
    {
        const int requestId = ++achievementsRequestId;
        juce::Component::SafePointer<AccountWindow> safeThis(this);

        api.getJson("/achievements", token, 5000,
            [safeThis, requestId](ApiClient::Response response)
            {
                auto* self = safeThis.getComponent();

                if (self == nullptr || requestId != self->achievementsRequestId)
                    return;

                if (! response.transportOk || ! response.json.isObject())
                    return;

                const auto* items =
                    response.json.getProperty("items", juce::var()).getArray();

                if (items == nullptr)
                    return;

                self->achievementCards.clear();
                self->achievementsUnlockedCount = 0;
                self->achievementsTotalCount = items->size();

                std::vector<std::pair<juce::String, juce::String>> iconJobs;

                for (const auto& item : *items)
                {
                    AccountWindowDetail::AchievementCard card;
                    card.code = item.getProperty("code", juce::var()).toString();
                    card.title = item.getProperty("title", juce::var()).toString();
                    card.description =
                        item.getProperty("description", juce::var()).toString();
                    card.iconUrl =
                        item.getProperty("icon_url", juce::var()).toString();
                    card.awardedAt =
                        item.getProperty("awarded_at", juce::var()).toString();
                    card.xpBonus =
                        static_cast<int>(item.getProperty("xp_bonus", 0));
                    card.progress =
                        static_cast<int>(item.getProperty("progress", 0));
                    card.goal =
                        static_cast<int>(item.getProperty("goal", 0));
                    card.unlocked =
                        static_cast<int>(item.getProperty("unlocked", 0)) != 0;

                    if (card.unlocked)
                        ++self->achievementsUnlockedCount;

                    if (card.iconUrl.isNotEmpty() && iconJobs.size() < 64)
                        iconJobs.push_back({ card.code, card.iconUrl });

                    self->achievementCards.push_back(card);
                }

                // Unlocked first, then the server order, so the compact grid in
                // the account card always shows the most relevant cards.
                std::stable_sort(self->achievementCards.begin(),
                                 self->achievementCards.end(),
                                 [](const AccountWindowDetail::AchievementCard& a,
                                    const AccountWindowDetail::AchievementCard& b)
                                 {
                                     return a.unlocked && ! b.unlocked;
                                 });

                // Hand the full list to the scrolling grid. The cards are
                // rendered there, so the fetched icons and the summary always
                // reflect the latest response.
                self->achievementModel.setData(&self->achievementCards,
                                               &self->achievementIcons);
                self->achievementList.setVisible(
                    self->achievementsTotalCount > 0);
                self->achievementList.updateContent();

                if (auto* viewport = self->achievementList.getViewport())
                    viewport->setViewPosition(0, 0);

                self->achievementList.repaint();

                if (iconJobs.empty())
                {
                    self->repaint();
                    return;
                }

                struct IconResult
                {
                    juce::String code;
                    juce::MemoryBlock bytes;
                };

                auto results = std::make_shared<std::vector<IconResult>>();
                auto& apiRef = self->api;

                apiRef.runAsync(
                    [iconJobs, results]()
                    {
                        for (const auto& job : iconJobs)
                        {
                            auto bytes = ApiClient::download(job.second, 4000);

                            if (bytes.getSize() > 0)
                                results->push_back({ job.first, bytes });
                        }
                    },
                    [safeThis, results]()
                    {
                        auto* window = safeThis.getComponent();

                        if (window == nullptr)
                            return;

                        for (const auto& result : *results)
                        {
                            auto drawable = juce::Drawable::createFromImageData(
                                result.bytes.getData(), result.bytes.getSize());

                            if (drawable == nullptr)
                                continue;

                            // Rasterise at the same size the website uses: a
                            // 52 px plate with 9 px padding around the glyph.
                            juce::Image image(juce::Image::ARGB, 52, 52, true);
                            {
                                juce::Graphics imageGraphics(image);
                                drawable->drawWithin(
                                    imageGraphics,
                                    juce::Rectangle<float>(9.0f, 9.0f, 34.0f, 34.0f),
                                    juce::RectanglePlacement::centred, 1.0f);
                            }

                            window->achievementIcons[result.code] = image;
                        }

                        window->achievementList.repaint();
                        window->repaint();
                    });
            });
    }

    // Server-authoritative progression: level, total XP and the last-100
    // attempt statistics shown in the summary cards.
    void loadProgression()
    {
        const int requestId = ++progressionRequestId;
        juce::Component::SafePointer<AccountWindow> safeThis(this);

        api.getJson("/profile/progression", token, 5000,
            [safeThis, requestId](ApiClient::Response response)
            {
                auto* self = safeThis.getComponent();

                if (self == nullptr || requestId != self->progressionRequestId)
                    return;

                if (! response.transportOk
                    || response.json.getProperty("xp_total", juce::var()).isVoid())
                    return;

                const auto& json = response.json;
                const int level =
                    static_cast<int>(json.getProperty("level", 1));
                const int xpTotal =
                    static_cast<int>(json.getProperty("xp_total", 0));
                const int xpForNext =
                    static_cast<int>(json.getProperty("xp_for_next", 0));

                self->levelValue = juce::String(level);
                self->xpValue = juce::String(xpTotal);
                self->xpNote = juce::String(xpForNext) + " to level "
                    + juce::String(level + 1);

                const auto stats = json.getProperty("stats", juce::var());
                const auto accuracy = stats.getProperty("accuracy", juce::var());

                if (! accuracy.isVoid())
                    self->accuracyValue =
                        juce::String(juce::roundToInt(
                            static_cast<double>(accuracy))) + "%";

                const auto timing = stats.getProperty("timing", juce::var());

                if (timing.isVoid())
                    self->timingValue = "n/a";
                else
                    self->timingValue =
                        juce::String(juce::roundToInt(
                            static_cast<double>(timing))) + "%";

                self->repaint();
            });
    }

    void loadMe()
    {
        const int requestId = ++meRequestId;
        beginRequest();
        juce::Component::SafePointer<AccountWindow> safeThis(this);

        api.getJson("/auth/me", token, 5000,
            [safeThis, requestId](ApiClient::Response response)
            {
                auto* self = safeThis.getComponent();
                if (self == nullptr)
                    return;

                self->endRequest();
                if (requestId != self->meRequestId)
                    return;

                if (self->handleAuthFailure(response))
                    return;

                if (!response.transportOk || !response.json.isObject())
                {
                    self->refreshHadError = true;
                    self->showAlert(AccountWindowDetail::apiErrorMessage(response),
                                    UiColors::red);
                    return;
                }

                const auto& me = response.json;
                self->meUsername = me.getProperty("username", juce::var()).toString();
                self->meDisplayName = me.getProperty("display_name", juce::var()).toString();
                self->meEmail = me.getProperty("email", juce::var()).toString();
                self->meCreatedAt = me.getProperty("created_at", juce::var()).toString();

                if (!self->profileDirty)
                {
                    self->usernameEditor.setText(self->meUsername, false);
                    self->displayNameEditor.setText(self->meDisplayName, false);
                }

                self->welcomeText = "Signed in as " + self->meUsername;
                if (self->meEmail.isNotEmpty())
                    self->welcomeText += "  |  " + self->meEmail;

                self->updateRankFromRating();
                self->ratingTable.updateContent();
                self->ratingTable.repaint();
                self->repaint();
            });
    }

    void loadSummary()
    {
        const int requestId = ++summaryRequestId;
        beginRequest();
        juce::Component::SafePointer<AccountWindow> safeThis(this);

        api.getJson("/games/best-scores", token, 5000,
            [safeThis, requestId](ApiClient::Response response)
            {
                auto* self = safeThis.getComponent();
                if (self == nullptr)
                    return;

                self->endRequest();
                if (requestId != self->summaryRequestId)
                    return;

                if (self->handleAuthFailure(response))
                    return;

                if (!response.transportOk || !response.json.isObject())
                {
                    self->refreshHadError = true;
                    self->showAlert(AccountWindowDetail::apiErrorMessage(response),
                                    UiColors::red);
                    return;
                }

                self->ratingValue = AccountWindowDetail::formatCount(
                    static_cast<int>(response.json.getProperty("rating_points", 0)));
                self->songsValue = AccountWindowDetail::formatCount(
                    static_cast<int>(response.json.getProperty("songs_played", 0)));
                self->attemptsValue = AccountWindowDetail::formatCount(
                    static_cast<int>(response.json.getProperty("games_played", 0)));
                self->repaint();
            });
    }

    void loadRating()
    {
        const int requestId = ++ratingRequestId;
        beginRequest();
        juce::Component::SafePointer<AccountWindow> safeThis(this);

        api.getJson("/leaderboard/global?limit=50", token, 5000,
            [safeThis, requestId](ApiClient::Response response)
            {
                auto* self = safeThis.getComponent();
                if (self == nullptr)
                    return;

                self->endRequest();
                if (requestId != self->ratingRequestId)
                    return;

                if (self->handleAuthFailure(response))
                    return;

                if (!response.transportOk || !response.json.isObject())
                {
                    self->refreshHadError = true;
                    self->showAlert(AccountWindowDetail::apiErrorMessage(response),
                                    UiColors::red);
                    return;
                }

                const auto itemsVar = response.json.getProperty("items", juce::var());
                self->ratingRows.clear();

                if (const auto* items = itemsVar.getArray())
                {
                    for (const auto& item : *items)
                    {
                        RatingRow row;
                        row.rank = static_cast<int>(item.getProperty("rank", 0));
                        row.username = item.getProperty("username", juce::var()).toString();
                        row.displayName =
                            item.getProperty("display_name", juce::var()).toString();
                        row.ratingPoints =
                            static_cast<int>(item.getProperty("rating_points", 0));
                        row.gamesPlayed =
                            static_cast<int>(item.getProperty("games_played", 0));
                        row.bestAccuracy =
                            static_cast<double>(item.getProperty("best_accuracy", 0.0));
                        self->ratingRows.push_back(row);
                    }
                }

                self->ratingEmptyLabel.setVisible(self->ratingRows.empty());
                self->updateRankFromRating();
                self->ratingTable.updateContent();
                self->ratingTable.repaint();
                self->repaint();
            });
    }

    void loadHistory(bool reset)
    {
        if (reset)
        {
            historyRows.clear();
            historyOffset = 0;
            historyHasMore = false;
            historyEmptyLabel.setVisible(false);
            showMoreButton.setVisible(false);
            historyTable.updateContent();
            historyTable.repaint();
        }

        const int requestId = ++historyRequestId;
        const int offset = historyOffset;
        beginRequest();
        showMoreButton.setEnabled(false);
        juce::Component::SafePointer<AccountWindow> safeThis(this);

        api.getJson("/games/history?limit=20&offset=" + juce::String(offset), token, 5000,
            [safeThis, requestId, reset](ApiClient::Response response)
            {
                auto* self = safeThis.getComponent();
                if (self == nullptr)
                    return;

                self->endRequest();
                if (requestId != self->historyRequestId)
                    return;

                self->showMoreButton.setEnabled(true);
                if (self->handleAuthFailure(response))
                    return;

                if (!response.transportOk || !response.json.isArray())
                {
                    self->refreshHadError = true;
                    self->showAlert(AccountWindowDetail::apiErrorMessage(response),
                                    UiColors::red);
                    return;
                }

                if (reset)
                    self->historyRows.clear();

                int received = 0;
                if (const auto* items = response.json.getArray())
                {
                    received = items->size();
                    for (const auto& item : *items)
                    {
                        HistoryRow row;
                        row.songTitle =
                            item.getProperty("song_title", juce::var()).toString();
                        row.startedAt = AccountWindowDetail::formatDate(
                            item.getProperty("started_at", juce::var()).toString());
                        row.status = item.getProperty("status", juce::var()).toString();

                        const auto score = item.getProperty("score_points", juce::var());
                        row.hasScore = !score.isVoid();
                        if (row.hasScore)
                            row.scorePoints = static_cast<int>(score);

                        const auto accuracy =
                            item.getProperty("accuracy_percent", juce::var());
                        row.hasAccuracy = !accuracy.isVoid();
                        if (row.hasAccuracy)
                            row.accuracy = static_cast<double>(accuracy);

                        const auto combo = item.getProperty("max_combo", juce::var());
                        row.hasCombo = !combo.isVoid();
                        if (row.hasCombo)
                            row.maxCombo = static_cast<int>(combo);

                        self->historyRows.push_back(row);
                    }
                }

                self->historyOffset += received;
                self->historyHasMore = received >= 20;
                self->historyEmptyLabel.setVisible(self->historyRows.empty());
                self->showMoreButton.setVisible(self->historyHasMore);
                self->historyTable.updateContent();
                self->historyTable.repaint();
                self->resized();
                self->repaint();
            });
    }

    void saveProfile()
    {
        if (sessionExpired)
            return;

        const auto username = usernameEditor.getText().trim();
        const auto displayName = displayNameEditor.getText().trim();

        if (username.length() < 3 || username.length() > 32)
        {
            showAlert("Username must be 3-32 characters.", UiColors::red);
            return;
        }

        if (!username.containsOnly(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-"))
        {
            showAlert("Username may contain only letters, digits, dot, dash and underscore.",
                      UiColors::red);
            return;
        }

        if (displayName.isEmpty())
        {
            showAlert("Display name cannot be blank.", UiColors::red);
            return;
        }

        if (displayName.length() > 64)
        {
            showAlert("Display name must be at most 64 characters.", UiColors::red);
            return;
        }

        juce::DynamicObject::Ptr payload = new juce::DynamicObject();
        bool hasChanges = false;

        if (username != meUsername)
        {
            payload->setProperty("username", username);
            hasChanges = true;
        }

        if (displayName != meDisplayName)
        {
            payload->setProperty("display_name", displayName);
            hasChanges = true;
        }

        if (!hasChanges)
        {
            showAlert("No changes to save.", UiColors::muted);
            return;
        }

        saveProfileButton.setEnabled(false);
        // A profile GET may still be in flight from a refresh; it must not
        // overwrite the values returned by this PATCH when it completes later.
        ++meRequestId;
        beginRequest();
        const int requestId = ++profileSaveRequestId;
        juce::Component::SafePointer<AccountWindow> safeThis(this);

        api.patchJson("/auth/me", juce::var(payload.get()), token, 5000,
            [safeThis, requestId](ApiClient::Response response)
            {
                auto* self = safeThis.getComponent();
                if (self == nullptr)
                    return;

                self->endRequest();
                if (requestId != self->profileSaveRequestId)
                    return;

                if (self->handleAuthFailure(response))
                    return;

                if (!response.transportOk || response.statusCode >= 400)
                {
                    self->showAlert(AccountWindowDetail::apiErrorMessage(response),
                                    UiColors::red);
                    return;
                }

                if (response.json.isObject())
                {
                    self->meUsername =
                        response.json.getProperty("username", juce::var()).toString();
                    self->meDisplayName =
                        response.json.getProperty("display_name", juce::var()).toString();
                    self->meEmail =
                        response.json.getProperty("email", juce::var()).toString();
                    self->meCreatedAt =
                        response.json.getProperty("created_at", juce::var()).toString();
                }

                self->profileDirty = false;
                self->usernameEditor.setText(self->meUsername, false);
                self->displayNameEditor.setText(self->meDisplayName, false);
                self->welcomeText = "Signed in as " + self->meUsername;
                if (self->meEmail.isNotEmpty())
                    self->welcomeText += "  |  " + self->meEmail;

                if (self->callbacks.onProfileUpdated)
                    self->callbacks.onProfileUpdated(self->meUsername,
                                                     self->meDisplayName);

                self->showAlert("Profile updated.", UiColors::green);
                self->loadRating();
                self->repaint();
            });
    }

    void changePassword()
    {
        if (sessionExpired)
            return;

        const auto currentPassword = currentPasswordEditor.getText();
        const auto newPassword = newPasswordEditor.getText();

        if (currentPassword.isEmpty())
        {
            showAlert("Enter the current password.", UiColors::red);
            return;
        }

        if (newPassword.length() < 6)
        {
            showAlert("The new password must be at least 6 characters.", UiColors::red);
            return;
        }

        if (newPassword == currentPassword)
        {
            showAlert("The new password must differ from the current one.", UiColors::red);
            return;
        }

        juce::DynamicObject::Ptr payload = new juce::DynamicObject();
        payload->setProperty("current_password", currentPassword);
        payload->setProperty("new_password", newPassword);

        changePasswordButton.setEnabled(false);
        beginRequest();
        juce::Component::SafePointer<AccountWindow> safeThis(this);

        api.postJson("/auth/change-password", juce::var(payload.get()), token, 5000,
            [safeThis](ApiClient::Response response)
            {
                auto* self = safeThis.getComponent();
                if (self == nullptr)
                    return;

                self->endRequest();

                if (self->handleAuthFailure(response))
                    return;

                if (!self->responseIsSuccess(response))
                {
                    self->showAlert(AccountWindowDetail::apiErrorMessage(response),
                                    UiColors::red);
                    return;
                }

                self->currentPasswordEditor.clear();
                self->newPasswordEditor.clear();
                self->showAlert("Password changed. All sessions are signed out.",
                                UiColors::green);

                juce::Component::SafePointer<AccountWindow> safeForAlert(self);
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::NoIcon,
                    "Password changed",
                    "All sessions have been signed out. Please sign in again.",
                    "OK",
                    self,
                    juce::ModalCallbackFunction::create(
                        [safeForAlert](int)
                        {
                            if (auto* window = safeForAlert.getComponent())
                            {
                                if (window->callbacks.onLogout)
                                    window->callbacks.onLogout();
                                window->closeDialog();
                            }
                        }));
            });
    }

    void logout()
    {
        if (callbacks.onLogout)
            callbacks.onLogout();
        closeDialog();
    }

    bool responseIsSuccess(const ApiClient::Response& response) const
    {
        return response.transportOk && response.statusCode >= 200
            && response.statusCode < 300;
    }

    bool handleAuthFailure(const ApiClient::Response& response)
    {
        if (response.statusCode != 401)
            return false;

        if (!sessionExpired)
        {
            sessionExpired = true;
            showAlert("Session expired. Please sign in again.", UiColors::red);
            if (callbacks.onSessionExpired)
                callbacks.onSessionExpired();

            juce::Component::SafePointer<AccountWindow> safeThis(this);
            juce::MessageManager::callAsync([safeThis]
            {
                if (auto* self = safeThis.getComponent())
                    self->closeDialog();
            });
        }

        return true;
    }

    void closeDialog()
    {
        if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
            dialog->exitModalState(0);
    }

    void showAlert(const juce::String& text, juce::Colour colour)
    {
        alertLabel.setText(text, juce::dontSendNotification);
        alertLabel.setColour(juce::Label::textColourId, colour);
        alertLabel.setVisible(text.isNotEmpty());
    }

    void beginRequest()
    {
        ++pendingRequests;
        refreshButton.setEnabled(false);
        // Do not start a destructive action while another request is in flight.
        saveProfileButton.setEnabled(false);
        changePasswordButton.setEnabled(false);
    }

    void endRequest()
    {
        pendingRequests = juce::jmax(0, pendingRequests - 1);
        if (pendingRequests == 0)
        {
            refreshButton.setEnabled(true);
            saveProfileButton.setEnabled(true);
            changePasswordButton.setEnabled(true);
            if (!refreshHadError && alertLabel.getText() == "Loading account...")
                showAlert(juce::String(), UiColors::muted);
        }
    }

    void updateRankFromRating()
    {
        int rank = 0;
        for (const auto& row : ratingRows)
        {
            if (row.username == meUsername)
            {
                rank = row.rank;
                break;
            }
        }

        if (rank > 0)
        {
            rankValue = "#" + juce::String(rank);
            rankNote = "in the global top 50";
        }
        else
        {
            rankValue = "-";
            rankNote = "not in the top 50";
        }

        repaint();
    }

    // The main window owns the client and outlives this dialog; sharing it
    // avoids destroying a ThreadPool (and waiting for in-flight requests) on
    // the message thread when the window closes.
    ApiClient& api;
    juce::String token;
    Callbacks callbacks;

    juce::String meUsername;
    juce::String meDisplayName;
    juce::String meEmail;
    juce::String meCreatedAt;
    juce::String welcomeText = "Loading profile...";
    juce::String ratingValue = "-";
    juce::String rankValue = "-";
    juce::String rankNote = "not in the top 50";
    juce::String songsValue = "-";
    juce::String attemptsValue = "-";
    juce::String levelValue = "-";
    juce::String xpValue = "-";
    juce::String xpNote = "total experience";
    juce::String accuracyValue = "-";
    juce::String timingValue = "-";
    std::vector<AccountWindowDetail::AchievementCard> achievementCards;
    std::map<juce::String, juce::Image> achievementIcons;
    AccountWindowDetail::AchievementListModel achievementModel;
    juce::ListBox achievementList { "Achievements", &achievementModel };
    int achievementsUnlockedCount = 0;
    int achievementsTotalCount = 0;
    int achievementsRequestId = 0;
    int progressionRequestId = 0;

    std::vector<RatingRow> ratingRows;
    std::vector<HistoryRow> historyRows;
    int historyOffset = 0;
    bool historyHasMore = false;
    bool profileDirty = false;
    bool sessionExpired = false;
    bool refreshHadError = false;
    int pendingRequests = 0;
    int meRequestId = 0;
    int summaryRequestId = 0;
    int ratingRequestId = 0;
    int historyRequestId = 0;
    int profileSaveRequestId = 0;

    RatingModel ratingModel;
    HistoryModel historyModel;
    juce::TableListBox ratingTable { "Global rating", &ratingModel };
    juce::TableListBox historyTable { "Attempt history", &historyModel };

    juce::TextEditor usernameEditor;
    juce::TextEditor displayNameEditor;
    juce::TextEditor currentPasswordEditor;
    juce::TextEditor newPasswordEditor;

    juce::TextButton saveProfileButton { "SAVE PROFILE" };
    juce::TextButton changePasswordButton { "CHANGE PASSWORD" };
    juce::TextButton showMoreButton { "SHOW MORE" };
    juce::TextButton refreshButton { "REFRESH" };
    juce::TextButton logoutButton { "LOG OUT" };
    juce::Label alertLabel;
    juce::Label ratingEmptyLabel;
    juce::Label historyEmptyLabel;

    juce::Rectangle<int> statsArea;
    juce::Rectangle<int> ratingCard;
    juce::Rectangle<int> historyCard;
    juce::Rectangle<int> profileCard;
    juce::Rectangle<int> passwordCard;
    juce::Rectangle<int> accountCard;
    juce::Rectangle<int> usernameEditorArea;
    juce::Rectangle<int> displayNameEditorArea;
    juce::Rectangle<int> saveProfileButtonArea;
    juce::Rectangle<int> currentPasswordEditorArea;
    juce::Rectangle<int> newPasswordEditorArea;
    juce::Rectangle<int> changePasswordButtonArea;
    juce::Rectangle<int> showMoreButtonArea;
};

void MainComponent::openAccountWindow()
{
    if (onlineAccessToken.isEmpty() || currentUsername.isEmpty())
        return;

    if (accountDialog != nullptr)
    {
        if (accountDialog->isVisible())
        {
            accountDialog->toFront(true);
            return;
        }

        accountDialog = nullptr;
    }

    AccountWindow::Callbacks callbacks;
    callbacks.onProfileUpdated =
        [safeThis = juce::Component::SafePointer<MainComponent>(this)]
        (const juce::String& username, const juce::String&)
        {
            if (auto* self = safeThis.getComponent())
            {
                self->currentUsername = username;
                self->currentUserLabel.setText(username, juce::dontSendNotification);
                self->resized();
                self->repaint();
            }
        };
    callbacks.onLogout =
        [safeThis = juce::Component::SafePointer<MainComponent>(this)]
        {
            if (auto* self = safeThis.getComponent())
                self->logoutCurrentUser();
        };
    callbacks.onSessionExpired =
        [safeThis = juce::Component::SafePointer<MainComponent>(this)]
        {
            if (auto* self = safeThis.getComponent())
                self->logoutCurrentUser();
        };

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(
        new AccountWindow(apiClient, onlineAccessToken, std::move(callbacks)));
    options.dialogTitle = "Personal account - FretFight";
    options.dialogBackgroundColour = UiColors::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.componentToCentreAround = this;

    if (auto* dialog = options.launchAsync())
    {
        dialog->setResizeLimits(1000, 860, 1800, 1200);
        accountDialog = dialog;
    }
}

//==============================================================================



void MainComponent::rebuildGuitarInputChoices()
{
    if (inputMonitor == nullptr)
        return;

    inputMonitor->refreshDevices(
        preferredInputChannel.load(std::memory_order_relaxed));
}

void MainComponent::refreshGuitarInputCombo()
{
    rebuildGuitarInputChoices();
}

namespace
{
    struct TuningPreset
    {
        const char* name;
        std::array<int, 6> notes;
    };

    constexpr int customTuningItemId = 8;
    constexpr int tuningPresetCount = 7;

    //: ComboBox item id for "EMPTY". MIDI notes start at 0 and the tuner only
    //: offers 24..96, so 1 cannot collide with a real open note.
    constexpr int emptyComboItemId = 1;

    bool comboHasItemId(juce::ComboBox& combo, int itemId)
    {
        for (int index = 0; index < combo.getNumItems(); ++index)
            if (combo.getItemId(index) == itemId)
                return true;

        return false;
    }

    const TuningPreset& tuningPreset(int index)
    {
        static const TuningPreset presets[] =
        {
            { "STANDARD", { 40, 45, 50, 55, 59, 64 } },
            { "DROP D",   { 38, 45, 50, 55, 59, 64 } },
            { "EB",       { 39, 44, 49, 54, 58, 63 } },
            { "DADGAD",   { 38, 45, 50, 55, 57, 62 } },
            { "OPEN G",   { 38, 43, 50, 55, 59, 62 } },
            { "OPEN D",   { 38, 45, 50, 54, 57, 62 } },
            { "DROP C",   { 36, 43, 48, 53, 57, 62 } },
        };

        return presets[juce::jlimit(0, tuningPresetCount - 1, index)];
    }
}

GuitarTuning::Tuning MainComponent::presetNotes(int presetIndex)
{
    // Presets describe a six-string guitar; the slot count is part of the
    // tuning, so selecting one resets an extended-range instrument to six.
    GuitarTuning::Tuning tuning = GuitarTuning::standardTuning();
    const auto& preset = tuningPreset(presetIndex);

    for (int slot = 0; slot < 6; ++slot)
        tuning.notes[static_cast<size_t>(slot)] = preset.notes[static_cast<size_t>(slot)];

    return tuning;
}

const char* MainComponent::presetName(int presetIndex)
{
    return tuningPreset(presetIndex).name;
}

int MainComponent::presetItemIdForTuning(const GuitarTuning::Tuning& tuning)
{
    for (int i = 0; i < tuningPresetCount; ++i)
        if (GuitarTuning::sameStrings(presetNotes(i), tuning))
            return i + 1;

    return customTuningItemId;
}

void MainComponent::initialiseTuningSelector()
{
    tuningPresetCombo.clear(juce::dontSendNotification);

    for (int i = 0; i < tuningPresetCount; ++i)
        tuningPresetCombo.addItem(presetName(i), i + 1);

    tuningPresetCombo.addItem("Custom", customTuningItemId);
    tuningPresetCombo.setTooltip(
        "Open-string tuning. The tablature is rebuilt for the selected tuning.");
    tuningPresetCombo.setColour(juce::ComboBox::backgroundColourId, UiColors::surface2);
    tuningPresetCombo.setColour(juce::ComboBox::textColourId, UiColors::text);
    tuningPresetCombo.setColour(juce::ComboBox::outlineColourId, UiColors::border);
    tuningPresetCombo.setColour(juce::ComboBox::arrowColourId, UiColors::accent);
    tuningPresetCombo.onChange = [this]
    {
        applyPresetTuning(tuningPresetCombo.getSelectedId());
    };
    addAndMakeVisible(tuningPresetCombo);

    // The instrument may have more or fewer than six strings. The list length
    // is the slot count, so "7 STRINGS" really means seven lines in the tab.
    stringCountCombo.clear(juce::dontSendNotification);

    for (int count = GuitarTuning::minStringCount;
         count <= GuitarTuning::maxStrings; ++count)
    {
        stringCountCombo.addItem(juce::String(count) + " STRINGS", count);
    }

    stringCountCombo.setTooltip(
        "Number of strings on the instrument. Extra strings start as the low B "
        "and low F# of an extended-range guitar; lower the count to drop the "
        "lowest strings.");
    stringCountCombo.setColour(juce::ComboBox::backgroundColourId, UiColors::surface2);
    stringCountCombo.setColour(juce::ComboBox::textColourId, UiColors::text);
    stringCountCombo.setColour(juce::ComboBox::outlineColourId, UiColors::border);
    stringCountCombo.setColour(juce::ComboBox::arrowColourId, UiColors::accent);
    stringCountCombo.onChange = [this]
    {
        applyStringCountFromUi();
    };
    addAndMakeVisible(stringCountCombo);

    // Frets on the neck. A note that would need a higher fret is moved by
    // whole octaves so the melody stays playable on a short neck.
    fretCountCombo.clear(juce::dontSendNotification);

    for (int frets = GuitarTuning::minFretCount;
         frets <= GuitarTuning::maxFretCount; ++frets)
    {
        fretCountCombo.addItem(juce::String(frets) + " FRETS", frets);
    }

    fretCountCombo.setTooltip(
        "How many frets the instrument has. Notes that only fit above this fret "
        "are transposed by an octave so the melody stays playable.");
    fretCountCombo.setColour(juce::ComboBox::backgroundColourId, UiColors::surface2);
    fretCountCombo.setColour(juce::ComboBox::textColourId, UiColors::text);
    fretCountCombo.setColour(juce::ComboBox::outlineColourId, UiColors::border);
    fretCountCombo.setColour(juce::ComboBox::arrowColourId, UiColors::accent);
    fretCountCombo.onChange = [this]
    {
        applyFretCountFromUi();
    };
    addAndMakeVisible(fretCountCombo);

    for (int slot = 0; slot < GuitarTuning::maxStrings; ++slot)
    {
        auto& combo = stringTuningCombos[static_cast<size_t>(slot)];
        combo.clear(juce::dontSendNotification);

        // An empty slot exists on the instrument but is not tuned and never
        // receives a note from the MIDI chart.
        combo.addItem("EMPTY", emptyComboItemId);

        // Offer a practical range around the standard open note: enough for
        // drop tunings and open tunings without a 100-item popup. Slots above
        // six continue downwards in perfect fourths (B1, then F#1), which is
        // what a seven- or eight-string guitar adds.
        const int reference = slot < static_cast<int>(GuitarTuning::standard.size())
            ? GuitarTuning::standard[static_cast<size_t>(slot)]
            : juce::jmax(24, GuitarTuning::standard[0] - 5 * (slot - 5));

        for (int note = juce::jmax(24, reference - 12);
             note <= juce::jmin(96, reference + 12); ++note)
        {
            combo.addItem(GuitarTuning::noteName(note, true), note);
        }

        // A stored custom note may sit outside the practical range: keep it
        // selectable instead of leaving the combo blank.
        if (slot < currentTuning.count)
        {
            const int note = currentTuning.notes[static_cast<size_t>(slot)];

            if (note > 0 && ! comboHasItemId(combo, note))
                combo.addItem(GuitarTuning::noteName(note, true), note);
        }

        combo.setColour(juce::ComboBox::backgroundColourId, UiColors::surface2);
        combo.setColour(juce::ComboBox::textColourId, UiColors::accent);
        combo.setColour(juce::ComboBox::outlineColourId, UiColors::borderStrong);
        combo.setColour(juce::ComboBox::arrowColourId, UiColors::accent);
        combo.onChange = [this] { applyCustomTuningFromUi(); };
        addAndMakeVisible(combo);
    }

    // A stored tuning that matches a preset should show that preset, even if
    // the previous session ended on the Custom item. Anything else (a custom
    // open tuning, or an instrument that is not a six-string guitar) is custom.
    const int presetId = presetItemIdForTuning(currentTuning);
    currentTuningName = presetId != customTuningItemId ? presetName(presetId - 1)
                                                       : juce::String("CUSTOM");

    setCurrentTuning(currentTuning, currentTuningName, false);
    syncTuningUi();
}

void MainComponent::syncTuningUi()
{
    tuningUiUpdating = true;

    tuningPresetCombo.setSelectedId(presetItemIdForTuning(currentTuning),
                                    juce::dontSendNotification);
    stringCountCombo.setSelectedId(currentTuning.count,
                                   juce::dontSendNotification);
    fretCountCombo.setSelectedId(currentTuning.fretCount,
                                 juce::dontSendNotification);

    const bool tunerScreen = gameState == GameState::Tuner;

    for (int slot = 0; slot < GuitarTuning::maxStrings; ++slot)
    {
        auto& combo = stringTuningCombos[static_cast<size_t>(slot)];
        const bool exists = slot < currentTuning.count;
        const int note = exists
            ? currentTuning.notes[static_cast<size_t>(slot)]
            : GuitarTuning::emptyNote;

        // A note restored from settings may sit outside the offered range.
        if (note > 0 && ! comboHasItemId(combo, note))
            combo.addItem(GuitarTuning::noteName(note, true), note);

        combo.setSelectedId(note == GuitarTuning::emptyNote ? emptyComboItemId : note,
                            juce::dontSendNotification);
        combo.setTooltip("String "
                         + juce::String(GuitarTuning::stringNumber(currentTuning, slot))
                         + ": open note, or EMPTY so the MIDI chart never uses it");
        combo.setVisible(tunerScreen && exists);
    }

    tuningUiUpdating = false;

    // The tile grid switches between three and four columns, so the layout has
    // to follow the new slot count.
    resized();
}

void MainComponent::applyPresetTuning(int presetItemId)
{
    if (tuningUiUpdating)
        return;

    if (presetItemId == customTuningItemId)
    {
        // Keep the current strings; the player edits the per-string combos.
        setCurrentTuning(currentTuning, "CUSTOM", true);
        syncTuningUi();
        return;
    }

    const int index = presetItemId - 1;
    if (index < 0 || index >= tuningPresetCount)
        return;

    // A preset describes a six-string guitar, so it also resets an
    // extended-range instrument back to six slots. The neck length is a
    // property of the player's instrument, not of the tuning, so it stays.
    auto preset = presetNotes(index);
    preset.fretCount = currentTuning.fretCount;

    setCurrentTuning(preset, presetName(index), true);
    syncTuningUi();
}

void MainComponent::applyStringCountFromUi()
{
    if (tuningUiUpdating)
        return;

    const int requested = stringCountCombo.getSelectedId();

    if (requested < GuitarTuning::minStringCount
        || requested > GuitarTuning::maxStrings)
        return;

    GuitarTuning::Tuning next = currentTuning;
    next.count = juce::jlimit(1, GuitarTuning::maxStrings, next.count);

    // Growing adds a lower string in perfect fourths below the current lowest
    // one, which is exactly what a seven-string (B1) and an eight-string
    // (F#1) guitar add. Shrinking drops the lowest strings first.
    while (next.count < requested)
    {
        int lowest = 96;

        for (int slot = 0; slot < next.count; ++slot)
            if (GuitarTuning::isActive(next, slot))
                lowest = juce::jmin(lowest, next.notes[static_cast<size_t>(slot)]);

        const int added = lowest >= 96 ? 40 : juce::jmax(24, lowest - 5);

        for (int slot = next.count; slot > 0; --slot)
            next.notes[static_cast<size_t>(slot)] =
                next.notes[static_cast<size_t>(slot - 1)];

        next.notes[0] = added;
        ++next.count;
    }

    while (next.count > requested)
    {
        for (int slot = 1; slot < next.count; ++slot)
            next.notes[static_cast<size_t>(slot - 1)] =
                next.notes[static_cast<size_t>(slot)];

        --next.count;
        next.notes[static_cast<size_t>(next.count)] = GuitarTuning::emptyNote;
    }

    setCurrentTuning(next, "CUSTOM", true);
    syncTuningUi();
}

void MainComponent::applyFretCountFromUi()
{
    if (tuningUiUpdating)
        return;

    const int requested = fretCountCombo.getSelectedId();

    if (requested < GuitarTuning::minFretCount
        || requested > GuitarTuning::maxFretCount)
        return;

    GuitarTuning::Tuning next = currentTuning;
    next.fretCount = requested;

    // Only the neck length changes: the open strings, the preset name and the
    // instrument stay exactly as they are.
    setCurrentTuning(next, currentTuningName, true);
    syncTuningUi();
}

void MainComponent::applyCustomTuningFromUi()
{
    if (tuningUiUpdating)
        return;

    GuitarTuning::Tuning next = currentTuning;
    next.count = juce::jlimit(1, GuitarTuning::maxStrings, next.count);

    for (int slot = 0; slot < next.count; ++slot)
    {
        const int selected = stringTuningCombos[static_cast<size_t>(slot)].getSelectedId();

        next.notes[static_cast<size_t>(slot)] =
            (selected <= 0 || selected == emptyComboItemId)
                ? GuitarTuning::emptyNote
                : selected;
    }

    // The last playable string must stay playable: an all-empty tuning would
    // silently produce a chart with no notes. Put the combo back where it was.
    if (GuitarTuning::activeCount(next) == 0)
    {
        syncTuningUi();
        return;
    }

    setCurrentTuning(next, "CUSTOM", true);
    syncTuningUi();
}

void MainComponent::initialiseUi()
{
    //==========================================================================
    // Tuner UI

    tunerTitleLabel.setText(
        "LIVE INPUT",
        juce::dontSendNotification);

    tunerTitleLabel.setFont(
        GameUi::eyebrow(11.0f));

    tunerTitleLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    tunerTitleLabel.setJustificationType(
        juce::Justification::centredLeft);

    tunerNoteLabel.setText(
        "---",
        juce::dontSendNotification);

    tunerNoteLabel.setFont(
        GameUi::heading(72.0f));

    tunerNoteLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    tunerNoteLabel.setJustificationType(
        juce::Justification::centred);

    tunerFrequencyLabel.setText(
        "--- Hz",
        juce::dontSendNotification);

    tunerFrequencyLabel.setFont(
        GameUi::font(18.0f, true));

    tunerFrequencyLabel.setColour(
        juce::Label::textColourId,
        UiColors::muted);

    tunerFrequencyLabel.setJustificationType(
        juce::Justification::centred);

    tunerStringLabel.setText(
        "---",
        juce::dontSendNotification);

    tunerStringLabel.setFont(
        GameUi::font(19.0f, true));

    tunerStringLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    tunerStringLabel.setJustificationType(
        juce::Justification::centred);

    tunerCentsLabel.setText(
        "--- cents",
        juce::dontSendNotification);

    tunerCentsLabel.setFont(
        GameUi::font(19.0f, true));

    tunerCentsLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    tunerCentsLabel.setJustificationType(
        juce::Justification::centred);

    tunerStatusLabel.setText(
        "PLAY A STRING",
        juce::dontSendNotification);

    tunerStatusLabel.setFont(
        GameUi::font(13.5f, true));

    tunerStatusLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    tunerStatusLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(tunerTitleLabel);
    addAndMakeVisible(tunerNoteLabel);
    addAndMakeVisible(tunerFrequencyLabel);
    addAndMakeVisible(tunerStringLabel);
    addAndMakeVisible(tunerCentsLabel);
    addAndMakeVisible(tunerStatusLabel);

    // Tuning selector: presets plus a per-string note (or EMPTY). It lives on the
    // tuner screen, before the player starts the song.
    initialiseTuningSelector();

    previewButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    previewButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    previewButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accentStrong);
    previewButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    previewButton.setTooltip("Play or stop the full-song preview for the selected track");
    previewButton.onClick = [this] { togglePreviewPlayback(); };
    addAndMakeVisible(previewButton);

    // Training run: no score, no rating, no server session.
    practiceModeButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    practiceModeButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    practiceModeButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accentStrong);
    practiceModeButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    practiceModeButton.setTooltip("Practice the song note by note. No score, no rating, no XP.");
    practiceModeButton.onClick = [this] { setPracticeMode(! practiceMode); };
    addAndMakeVisible(practiceModeButton);
    setPracticeMode(false);

    // VST monitor plugin: sits in the header next to SETTINGS / AUDIO
    // SETTINGS and opens the plugin chooser. The plugin only processes the
    // monitored guitar; note detection reads the dry input.
    vstButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    vstButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    vstButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accentStrong);
    vstButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    vstButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    vstButton.setTooltip("Load a VST3 effect for the monitored guitar. Note detection stays dry.");
    vstButton.onClick = [this] { openVstPanel(); };
    addAndMakeVisible(vstButton);
    updateVstButton();

    // Compact input selector shown on the tuner screen. The guitar may be
    // connected to any interface input, so the player can switch the capture
    // channel without entering a song and opening the full audio settings.
    // The selector itself now lives inside the shared GuitarInputMonitor,
    // which is visible on the song screen, in the tuner and during gameplay.
    addAndMakeVisible(*inputMonitor);

    tunerStartButton.setEnabled(false);
    tunerStartButton.setColour(juce::TextButton::buttonColourId, UiColors::accent);
    tunerStartButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accentStrong);
    tunerStartButton.setColour(juce::TextButton::textColourOffId, UiColors::onAccent);
    tunerStartButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);

    addAndMakeVisible(tunerStartButton);

    tunerSkipButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    tunerSkipButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
    tunerSkipButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    tunerSkipButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    addAndMakeVisible(tunerSkipButton);
    tunerSkipButton.onClick = [this]
    {
        // Treat setup as complete and reuse the normal game-start path.
        tuner.markAllStringsTuned();
        tunerStartButton.onClick();
    };

    readyStartButton.setColour(juce::TextButton::buttonColourId, UiColors::accent);
    readyStartButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accentStrong);
    readyStartButton.setColour(juce::TextButton::textColourOffId, UiColors::onAccent);
    readyStartButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    addAndMakeVisible(readyStartButton);
    readyStartButton.onClick = [this]
    {
        beginGameplayCountIn();
    };

    resumeOverlayButton.setColour(juce::TextButton::buttonColourId, UiColors::accent);
    resumeOverlayButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accentStrong);
    resumeOverlayButton.setColour(juce::TextButton::textColourOffId, UiColors::onAccent);
    resumeOverlayButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    addAndMakeVisible(resumeOverlayButton);
    resumeOverlayButton.onClick = [this]
    {
        resumeGame();
    };

    tunerStartButton.onClick =
        [this]
        {
            if (!tuner.areAllStringsTuned())
            {
                return;
            }

            if (gameSession.getMidiNotes().empty())
            {
                DBG("START BLOCKED: MIDI is empty");
                return;
            }

            // Open a server session for online songs when the player is signed in.
            // If the server is unavailable, local gameplay remains available.
            // The server session and the launch counter start only when the
            // player confirms readiness on the Ready screen.

            //==============================================================
 // Reset game session

            gameSession.resetGameplay();
            // resetGameplay() resets its own tempo defaults, so restore the
            // selected song's original BPM and the currently selected speed.
            const double selectedBpm = baseBpm * speedPercent / 100.0;
            gameSession.setBaseBpm(baseBpm);
            gameSession.setSpeedPercent(speedPercent);
            gameSession.setBpm(selectedBpm);
            bpm.store(selectedBpm, std::memory_order_relaxed);
            audioPlaybackRate.store(speedPercent / 100.0, std::memory_order_relaxed);
            metronome.setBpm(selectedBpm);
            tablatureComponent.setBpm(selectedBpm);
            bpmLabel.setText(
                juce::String(static_cast<int>(std::round(selectedBpm))),
                juce::dontSendNotification);
            gameplayStats.reset();

            gameSessionLabel.setText(
                "0",
                juce::dontSendNotification);

            hitLabel.setText(
                "READY",
                juce::dontSendNotification);

            //==============================================================
            // Reset gameplay state

            tunerActive.store(
                false,
                std::memory_order_relaxed);

            gameSession.setTargetHit(false);
            gameSession.resetResolvedTarget();
            gameSession.setLastHitSample(0);

            lastOnsetSample.store(
                0,
                std::memory_order_relaxed);

            lastNoteOnSample.store(
                0,
                std::memory_order_relaxed);


            // Reset onset detection for the first gameplay note.
            noteOn.store(
                false,
                std::memory_order_relaxed);

            previousRmsValue = 0.0;
            previousRmsValue2 = 0.0;

            pausedSamples.store(
                0,
                std::memory_order_relaxed);

            pauseStartSample.store(
                0,
                std::memory_order_relaxed);

            //==============================================================
            // The song is prepared, but the timeline and the count-in have
            // not started yet. Nothing is audible and no notes are processed
            // until the player presses START on the Ready screen.
            countInActive.store(false, std::memory_order_release);
            countInRemaining.store(0, std::memory_order_relaxed);
            gameplayStartSample.store(0, std::memory_order_relaxed);
            currentPlayheadBeat.store(0.0, std::memory_order_relaxed);
            tablatureComponent.setPlayheadBeat(0.0);
            tablatureComponent.setPlayheadRunning(false);
            metronome.pauseScheduler();
            metronomeSamplesRemaining = 0;
            metronomePlaybackPosition = 0;

            gameState = GameState::Ready;
            gameStartButton.setButtonText("START");
            menuButton.setButtonText("BACK");
            tunerStartButton.setEnabled(false);
            hitLabel.setText("READY", juce::dontSendNotification);

            resized();
            repaint();
        };


    //==========================================================================
    // Game buttons

    gameStartButton.setColour(
        juce::TextButton::buttonColourId,
        UiColors::accent);
    gameStartButton.setColour(
        juce::TextButton::buttonOnColourId,
        UiColors::accentStrong);
    gameStartButton.setColour(
        juce::TextButton::textColourOffId,
        UiColors::onAccent);
    gameStartButton.setColour(
        juce::TextButton::textColourOnId,
        UiColors::onAccent);
    gameStartButton.setLookAndFeel(&getLookAndFeel());

    pauseButton.setColour(
        juce::TextButton::buttonColourId,
        UiColors::surface2);
    pauseButton.setColour(
        juce::TextButton::buttonOnColourId,
        UiColors::accent);
    pauseButton.setColour(
        juce::TextButton::textColourOffId,
        UiColors::text);
    pauseButton.setColour(
        juce::TextButton::textColourOnId,
        UiColors::onAccent);

    menuButton.setColour(
        juce::TextButton::buttonColourId,
        juce::Colours::transparentBlack);
    menuButton.setColour(
        juce::TextButton::buttonOnColourId,
        UiColors::surface2);
    menuButton.setColour(
        juce::TextButton::textColourOffId,
        UiColors::muted);
    menuButton.setColour(
        juce::TextButton::textColourOnId,
        UiColors::text);

    // Consistent typography across the primary gameplay controls.
    for (auto* button : { &gameStartButton, &pauseButton, &menuButton,
                          &speed50Button, &speed75Button, &speed100Button,
                          &speed120Button,
                          &audioSettingsButton, &tunerStartButton, &tunerSkipButton })
        button->setLookAndFeel(&getLookAndFeel());

    addAndMakeVisible(gameStartButton);
    addAndMakeVisible(pauseButton);
    addAndMakeVisible(menuButton);

    // Compact stompbox-style distortion pedal controls.
    distortionButton.setClickingTogglesState(false);
    distortionButton.setColour(juce::TextButton::buttonColourId,
                               UiColors::red.darker(0.58f));
    distortionButton.setColour(juce::TextButton::buttonOnColourId,
                               UiColors::red);
    distortionButton.setColour(juce::TextButton::textColourOffId,
                               UiColors::text);
    distortionButton.setColour(juce::TextButton::textColourOnId,
                               UiColors::background);
    distortionButton.setTooltip("Switch the distortion pedal on or off.");
    addAndMakeVisible(distortionButton);
    midiBackingButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    midiBackingButton.setColour(juce::TextButton::buttonOnColourId,
                                UiColors::surface2.interpolatedWith(UiColors::green, 0.20f));
    midiBackingButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    midiBackingButton.setColour(juce::TextButton::textColourOnId, UiColors::background);
    const bool midiGuitarInitiallyEnabled =
        midiBackingEnabled.load(std::memory_order_relaxed);
    midiBackingButton.setToggleState(midiGuitarInitiallyEnabled,
                                     juce::dontSendNotification);
    midiBackingButton.setButtonText(midiGuitarInitiallyEnabled
                                        ? "SOLO GTR ON" : "SOLO GTR OFF");
    midiBackingButton.setTooltip("Toggle the MIDI solo-guitar part generated by FretFight.");
    addAndMakeVisible(midiBackingButton);
    midiBackingButton.onClick = [this]
    {
        const bool enabled = !midiBackingEnabled.load(std::memory_order_relaxed);
        midiBackingEnabled.store(enabled, std::memory_order_relaxed);
        midiBackingButton.setButtonText(enabled ? "SOLO GTR ON" : "SOLO GTR OFF");
        midiBackingButton.setToggleState(enabled, juce::dontSendNotification);

        if (auto* settings = appProperties.getUserSettings())
        {
            settings->setValue("MidiGuitarEnabled", enabled);
            settings->saveIfNeeded();
        }
    };
    midiBackingVolumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    midiBackingVolumeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    midiBackingVolumeSlider.setRange(0.0, 0.8, 0.01);
    midiBackingVolumeSlider.setValue(0.30, juce::dontSendNotification);
    midiBackingVolumeSlider.setTooltip("Solo guitar MIDI volume");
    midiBackingVolumeSlider.onValueChange = [this]
    {
        midiBackingVolume.store(static_cast<float>(midiBackingVolumeSlider.getValue()),
                                std::memory_order_relaxed);
    };
    // The slider is the single source of truth for the initial level: without
    // this the audio thread kept the header default (0.22) while the UI showed
    // 0.30 until the first drag.
    midiBackingVolume.store(static_cast<float>(midiBackingVolumeSlider.getValue()),
                            std::memory_order_relaxed);
    addAndMakeVisible(midiBackingVolumeSlider);
    midiBackingVolumeLabel.setText("SOLO VOL", juce::dontSendNotification);
    midiBackingVolumeLabel.setJustificationType(juce::Justification::centredLeft);
    midiBackingVolumeLabel.setColour(juce::Label::textColourId, UiColors::text);
    midiBackingVolumeLabel.setFont(GameUi::font(12.0f, true));
    addAndMakeVisible(midiBackingVolumeLabel);

    midiRhythmGuitarButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    midiRhythmGuitarButton.setColour(juce::TextButton::buttonOnColourId,
                                     UiColors::surface2.interpolatedWith(UiColors::green, 0.20f));
    midiRhythmGuitarButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    midiRhythmGuitarButton.setColour(juce::TextButton::textColourOnId, UiColors::background);
    midiRhythmGuitarButton.setToggleState(true, juce::dontSendNotification);
    midiRhythmGuitarButton.setTooltip("Toggle the rhythm-guitar WAV stem.");
    addAndMakeVisible(midiRhythmGuitarButton);
    midiRhythmGuitarButton.onClick = [this]
    {
        const bool enabled = !midiRhythmGuitarEnabled.load(std::memory_order_relaxed);
        midiRhythmGuitarEnabled.store(enabled, std::memory_order_relaxed);
        midiRhythmGuitarButton.setButtonText(enabled ? "RHYTHM GTR ON" : "RHYTHM GTR OFF");
        midiRhythmGuitarButton.setToggleState(enabled, juce::dontSendNotification);
    };
    midiRhythmGuitarVolumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    midiRhythmGuitarVolumeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    midiRhythmGuitarVolumeSlider.setRange(0.0, 0.8, 0.01);
    midiRhythmGuitarVolumeSlider.setValue(0.20, juce::dontSendNotification);
    midiRhythmGuitarVolumeSlider.setTooltip("Rhythm-guitar stem volume");
    midiRhythmGuitarVolumeSlider.onValueChange = [this]
    { midiRhythmGuitarVolume.store(static_cast<float>(midiRhythmGuitarVolumeSlider.getValue()), std::memory_order_relaxed); };
    addAndMakeVisible(midiRhythmGuitarVolumeSlider);
    midiRhythmGuitarVolumeLabel.setText("RHYTHM VOL", juce::dontSendNotification);
    midiRhythmGuitarVolumeLabel.setJustificationType(juce::Justification::centredLeft);
    midiRhythmGuitarVolumeLabel.setColour(juce::Label::textColourId, UiColors::text);
    midiRhythmGuitarVolumeLabel.setFont(GameUi::font(10.5f, true));
    addAndMakeVisible(midiRhythmGuitarVolumeLabel);

    auto setupBackingChannel = [this](juce::TextButton& button, juce::Slider& slider,
                                      juce::Label& label, std::atomic<bool>& enabled,
                                      std::atomic<float>& volume, const juce::String& name,
                                      float initialVolume, double maximum = 0.8,
                                      juce::String settingsKey = {})
    {
        button.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
        button.setColour(juce::TextButton::buttonOnColourId,
                         UiColors::surface2.interpolatedWith(UiColors::green, 0.20f));
        button.setColour(juce::TextButton::textColourOffId, UiColors::text);
        button.setColour(juce::TextButton::textColourOnId, UiColors::background);
        const bool initiallyEnabled = enabled.load(std::memory_order_relaxed);
        button.setToggleState(initiallyEnabled, juce::dontSendNotification);
        button.setButtonText(name + (initiallyEnabled ? " ON" : " OFF"));
        button.setTooltip("Toggle the " + name + " audio channel.");
        addAndMakeVisible(button);
        auto* buttonPtr = &button;
        auto* enabledPtr = &enabled;
        button.onClick = [this, buttonPtr, enabledPtr, name, settingsKey]()
        {
            const bool next = !enabledPtr->load(std::memory_order_relaxed);
            enabledPtr->store(next, std::memory_order_relaxed);
            buttonPtr->setButtonText(name + (next ? " ON" : " OFF"));
            buttonPtr->setToggleState(next, juce::dontSendNotification);

            if (settingsKey.isNotEmpty())
            {
                if (auto* settings = appProperties.getUserSettings())
                {
                    settings->setValue(settingsKey, next);
                    settings->saveIfNeeded();
                }
            }
        };
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange(0.0, maximum, 0.01);
        slider.setValue(initialVolume, juce::dontSendNotification);
        auto* sliderPtr = &slider;
        auto* volumePtr = &volume;
        slider.onValueChange = [sliderPtr, volumePtr]()
        { volumePtr->store(static_cast<float>(sliderPtr->getValue()), std::memory_order_relaxed); };
        addAndMakeVisible(slider);
        label.setText(name + " VOL", juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centredLeft);
        label.setColour(juce::Label::textColourId, UiColors::text);
        label.setFont(GameUi::font(10.5f, true));
        addAndMakeVisible(label);
    };
    setupBackingChannel(liveGuitarButton, liveGuitarVolumeSlider, liveGuitarVolumeLabel,
                        liveGuitarEnabled, liveGuitarVolume, "LIVE GTR", 0.5f, 1.0);
    liveGuitarVolumeSlider.setTooltip("Volume of the guitar you are playing. "
                                      "Note detection always uses the dry input.");
    setupBackingChannel(midiBassButton, midiBassVolumeSlider, midiBassVolumeLabel,
                        midiBassEnabled, midiBassVolume, "BASS", 0.45f);
    setupBackingChannel(midiDrumsButton, midiDrumsVolumeSlider, midiDrumsVolumeLabel,
                        midiDrumsEnabled, midiDrumsVolume, "DRUMS", 0.45f);
    setupBackingChannel(midiPianoButton, midiPianoVolumeSlider, midiPianoVolumeLabel,
                        midiPianoEnabled, midiPianoVolume, "OTHERS", 0.30f);
    setupBackingChannel(metronomeMixerButton, metronomeMixerVolumeSlider,
                        metronomeMixerVolumeLabel, metronomeMixerEnabled,
                        metronomeMixerVolume, "METRONOME", 0.70f, 1.0,
                        "MetronomeEnabled");
    metronomeMixerVolumeSlider.setTooltip("Metronome click volume");

    distortionButton.onClick = [this]
    {
        const bool enabled = !distortionEnabled.load(std::memory_order_relaxed);
        distortionEnabled.store(enabled, std::memory_order_relaxed);
        distortionButton.setButtonText(enabled ? "DISTORTION  ON" : "DISTORTION  OFF");
        distortionButton.setToggleState(enabled, juce::dontSendNotification);
    };

    auto setupPedalKnob = [this](juce::Slider& knob, juce::Label& label,
                                  const juce::String& name, double minimum,
                                  double maximum, double initial)
    {
        knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 48, 15);
        knob.setRange(minimum, maximum, 0.01);
        knob.setValue(initial, juce::dontSendNotification);
        knob.setColour(juce::Slider::rotarySliderFillColourId, UiColors::red);
        knob.setColour(juce::Slider::rotarySliderOutlineColourId,
                       UiColors::red.darker(0.72f));
        knob.setColour(juce::Slider::thumbColourId, UiColors::text);
        knob.setColour(juce::Slider::textBoxTextColourId, UiColors::text);
        knob.setColour(juce::Slider::textBoxBackgroundColourId, UiColors::surface2);
        knob.setColour(juce::Slider::textBoxOutlineColourId, UiColors::border);
        addAndMakeVisible(knob);
        label.setText(name, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centred);
        label.setColour(juce::Label::textColourId, UiColors::text);
        label.setFont(juce::FontOptions().withHeight(12.0f).withStyle("Bold"));
        addAndMakeVisible(label);
    };
    setupPedalKnob(distortionDriveSlider, distortionDriveLabel, "DRIVE", 1.0, 8.0, 2.5);
    setupPedalKnob(distortionToneSlider, distortionToneLabel, "TONE", 0.0, 1.0, 0.65);
    distortionDriveSlider.onValueChange = [this]
    { distortionDrive.store((float) distortionDriveSlider.getValue(), std::memory_order_relaxed); };
    distortionToneSlider.onValueChange = [this]
    { distortionTone.store((float) distortionToneSlider.getValue(), std::memory_order_relaxed); };

    gameStartButton.onClick =
        [this]
        {
            // START always begins a fresh session from MIDI note zero.
            // Reuse the same reset path as the tuner-complete start button
            // so scoring, onset detection, timeline and metronome are reset
            // consistently.
            if (gameState == GameState::Playing
                || gameState == GameState::Paused
                || gameState == GameState::Finished)
            {
                if (tuner.areAllStringsTuned())
                    tunerStartButton.onClick();
            }
        };

    pauseButton.onClick =
        [this]
        {
            if (gameState == GameState::Playing)
            {
                pauseGame();
            }
            else if (gameState == GameState::Paused)
            {
                resumeGame();
            }
        };

    menuButton.onClick =
        [this]
        {
            returnToSongSelect();
        };

    //==========================================================================
    // Register WAV/MP3 decoders before online song assets are loaded.
    audioFormatManager.registerBasicFormats();

    // The catalogue is server-only. Local Songs/ folders are intentionally
    // ignored; published songs arrive asynchronously from refreshOnlineCatalog().
    songPridumayButton.setButtonText("START SONG");

    // Daily challenge: the song is chosen by the server for today's weekday.
    dailyChallengeButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    dailyChallengeButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
    dailyChallengeButton.setColour(juce::TextButton::textColourOffId, UiColors::muted);
    dailyChallengeButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    dailyChallengeButton.onClick = [this] { onDailyChallengeClicked(); };
    addAndMakeVisible(dailyChallengeButton);

    dailyChallengeStatusLabel.setText("Loading daily challenge...",
                                      juce::dontSendNotification);
    dailyChallengeStatusLabel.setFont(GameUi::font(9.0f));
    dailyChallengeStatusLabel.setColour(juce::Label::textColourId, UiColors::muted);
    dailyChallengeStatusLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(dailyChallengeStatusLabel);
    rebuildCatalogRows(juce::var());

    //==========================================================================
    // Pitch / combo HUD labels

    pitchLabel.setText(
        "---",
        juce::dontSendNotification);

    pitchLabel.setFont(
        GameUi::font(20.0f, true));

    pitchLabel.setColour(
        juce::Label::textColourId,
        UiColors::green);

    pitchLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        pitchLabel);

    comboLabel.setText(
        "x0",
        juce::dontSendNotification);

    comboLabel.setFont(
        GameUi::heading(30.0f));

    comboLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    comboLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        comboLabel);

    //==========================================================================
    // Beat label

    beatLabel.setText(
        "0",
        juce::dontSendNotification);

    beatLabel.setFont(
        GameUi::font(14.0f, true));

    beatLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    beatLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        beatLabel);

    //==========================================================================
    // Bar beat label

    barBeatLabel.setText(
        "0",
        juce::dontSendNotification);

    barBeatLabel.setFont(
        GameUi::font(14.0f, true));

    barBeatLabel.setColour(
        juce::Label::textColourId,
        UiColors::text);

    barBeatLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        barBeatLabel);

    //==========================================================================
    // BPM

    bpmLabel.setText(
        juce::String(static_cast<int>(std::round(baseBpm * speedPercent / 100.0))),
        juce::dontSendNotification);

    bpmLabel.setFont(
        GameUi::font(13.0f, true));

    bpmLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    bpmLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        bpmLabel);

    //==========================================================================
    // Speed buttons
    // Explicitly add all tempo buttons to the component hierarchy.
    // Without addAndMakeVisible(), setVisible(true) in resized() is not enough
    // to make these buttons appear.

    addAndMakeVisible(speed50Button);
    addAndMakeVisible(speed75Button);
    addAndMakeVisible(speed100Button);
    addAndMakeVisible(speed120Button);

    speed50Button.setClickingTogglesState(true);
    speed75Button.setClickingTogglesState(true);
    speed100Button.setClickingTogglesState(true);
    speed120Button.setClickingTogglesState(true);

    speed50Button.setRadioGroupId(2001);
    speed75Button.setRadioGroupId(2001);
    speed100Button.setRadioGroupId(2001);
    speed120Button.setRadioGroupId(2001);

    speed50Button.setToggleState(
        false,
        juce::dontSendNotification);

    speed75Button.setToggleState(
        false,
        juce::dontSendNotification);

    speed100Button.setToggleState(
        true,
        juce::dontSendNotification);

    speed120Button.setToggleState(
        false,
        juce::dontSendNotification);

    auto setSpeed =
        [this](double percent)
        {
            // Applies immediately before the song starts, or enters the
            // managed preparation state during play. The BPM label and the
            // tablature tempo are updated when the change actually lands.
            applyTempoPercent(percent);
        };

    speed50Button.onClick =
        [this, setSpeed]
        {
            setSpeed(50.0);
        };

    speed75Button.onClick =
        [this, setSpeed]
        {
            setSpeed(75.0);
        };

    speed100Button.onClick =
        [this, setSpeed]
        {
            setSpeed(100.0);
        };

    speed120Button.onClick =
        [this, setSpeed]
        {
            setSpeed(120.0);
        };

    for (auto* button :
        { &speed50Button,
          &speed75Button,
          &speed100Button,
          &speed120Button })
    {
        button->setColour(
            juce::TextButton::buttonColourId,
            UiColors::surface2);

        button->setColour(
            juce::TextButton::buttonOnColourId,
            UiColors::accent);

        button->setColour(
            juce::TextButton::textColourOffId,
            UiColors::muted);

        button->setColour(
            juce::TextButton::textColourOnId,
            UiColors::onAccent);
    }

    //==========================================================================
    // Song select

    songTitleLabel.setText("Choose your next challenge", juce::dontSendNotification);
    songTitleLabel.setFont(GameUi::heading(32.0f));
    songTitleLabel.setColour(juce::Label::textColourId, UiColors::text);
    songTitleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(songTitleLabel);

    // Current song title shown in the gameplay header.
    gameSongTitleLabel.setText("FretFight", juce::dontSendNotification);
    gameSongTitleLabel.setFont(GameUi::heading(23.0f));
    gameSongTitleLabel.setColour(juce::Label::textColourId, UiColors::text);
    gameSongTitleLabel.setJustificationType(juce::Justification::centredLeft);
    gameSongTitleLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(gameSongTitleLabel);

    songPridumayButton.setColour(juce::TextButton::buttonColourId, UiColors::accent);
    songPridumayButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accentStrong);
    songPridumayButton.setColour(juce::TextButton::textColourOffId, UiColors::onAccent);
    songPridumayButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    addAndMakeVisible(songPridumayButton);
    onlineCatalogButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    onlineCatalogButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
    onlineCatalogButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    onlineCatalogButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    addAndMakeVisible(onlineCatalogButton);
    onlineCatalogStatusLabel.setFont(GameUi::font(12.0f));
    onlineCatalogStatusLabel.setColour(juce::Label::textColourId, UiColors::muted);
    onlineCatalogStatusLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(onlineCatalogStatusLabel);

    // Thin download bar under the status label. It is hidden until START SONG
    // actually has to fetch assets, so the normal screen is unchanged.
    songDownloadProgressBar.setColour(juce::ProgressBar::backgroundColourId, UiColors::surface2);
    songDownloadProgressBar.setColour(juce::ProgressBar::foregroundColourId, UiColors::accent);
    // The bar is only 8 px tall: the default percentage text would be clipped.
    // The status label above it already says which song is downloading.
    songDownloadProgressBar.setTextToDisplay(" ");
    songDownloadProgressBar.setVisible(false);
    addChildComponent(songDownloadProgressBar);

    // Catalogue list: one row per playable song with the player's best score.
    // Rows are painted by SongListModel in the shared navy/gold style; the card
    // behind the list already provides the rounded surface.
    songListBox.setRowHeight(56);
    songListBox.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    songListBox.setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    songListBox.setOutlineThickness(0);
    songListBox.setModel(&songListModel);
    addAndMakeVisible(songListBox);

    leaderboardTitleLabel.setText("GLOBAL RATING", juce::dontSendNotification);
    leaderboardTitleLabel.setFont(GameUi::heading(19.0f));
    leaderboardTitleLabel.setColour(juce::Label::textColourId, UiColors::text);
    leaderboardTitleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(leaderboardTitleLabel);

    leaderboardLabel.setText({}, juce::dontSendNotification);
    leaderboardLabel.setFont(GameUi::font(11.0f));
    leaderboardLabel.setColour(juce::Label::textColourId, UiColors::muted);
    leaderboardLabel.setJustificationType(juce::Justification::topLeft);
    leaderboardLabel.setMinimumHorizontalScale(0.8f);
    addAndMakeVisible(leaderboardLabel);

    // Global rating table: columns are rank, player, level and rating points.
    // Clicking a row opens that player's public card. Styling matches the
    // account window's GLOBAL RATING card: same header, row striping, rank
    // colours and point formatting.
    leaderboardModel = std::make_unique<MainLeaderboardModel>();
    leaderboardTable.setModel(leaderboardModel.get());
    leaderboardTable.setColour(juce::ListBox::backgroundColourId,
                               juce::Colours::transparentBlack);
    leaderboardTable.setColour(juce::ListBox::outlineColourId,
                               juce::Colours::transparentBlack);
    leaderboardTable.setOutlineThickness(0);
    leaderboardTable.setRowHeight(32);
    leaderboardTable.setHeaderHeight(28);
    leaderboardTable.getViewport()->setScrollBarsShown(true, false);

    auto& leaderboardHeader = leaderboardTable.getHeader();
    leaderboardHeader.setColour(juce::TableHeaderComponent::backgroundColourId,
                                UiColors::surface2);
    leaderboardHeader.setColour(juce::TableHeaderComponent::textColourId,
                                UiColors::muted);
    leaderboardHeader.setColour(juce::TableHeaderComponent::outlineColourId,
                                UiColors::border);
    leaderboardHeader.setColour(juce::TableHeaderComponent::highlightColourId,
                                UiColors::accent.withAlpha(0.25f));
    leaderboardHeader.setStretchToFitActive(true);

    leaderboardTable.getHeader().addColumn(
        "#", 1, 46, 40, 70, juce::TableHeaderComponent::notSortable);
    leaderboardTable.getHeader().addColumn(
        "PLAYER", 2, 220, 120, -1, juce::TableHeaderComponent::notSortable);
    leaderboardTable.getHeader().addColumn(
        "LEVEL", 3, 84, 70, 120, juce::TableHeaderComponent::notSortable);
    leaderboardTable.getHeader().addColumn(
        "RATING", 4, 110, 80, 160, juce::TableHeaderComponent::notSortable);

    leaderboardModel->onRowClicked = [this](const juce::String& userId)
    {
        openPublicProfile(userId);
    };
    addAndMakeVisible(leaderboardTable);

    progressionSummaryLabel.setText("", juce::dontSendNotification);
    progressionSummaryLabel.setFont(GameUi::font(12.0f, true));
    progressionSummaryLabel.setColour(juce::Label::textColourId, UiColors::accent);
    progressionSummaryLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(progressionSummaryLabel);
    progressionSummaryLabel.setVisible(false);

    refreshLeaderboardButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    refreshLeaderboardButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
    refreshLeaderboardButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    refreshLeaderboardButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    refreshLeaderboardButton.onClick = [this] { refreshGlobalLeaderboard(); };
    refreshLeaderboardButton.setButtonText("REFRESH");
    addAndMakeVisible(refreshLeaderboardButton);

    accountStatusLabel.setText({}, juce::dontSendNotification);
    accountStatusLabel.setFont(GameUi::font(12.0f));
    accountStatusLabel.setColour(juce::Label::textColourId, UiColors::muted);
    accountStatusLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(accountStatusLabel);

    loginScreenTitleLabel.setText("WELCOME BACK", juce::dontSendNotification);
    loginScreenTitleLabel.setFont(GameUi::heading(36.0f));
    loginScreenTitleLabel.setColour(juce::Label::textColourId, UiColors::text);
    loginScreenTitleLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(loginScreenTitleLabel);

    currentUserLabel.setFont(GameUi::font(13.0f, true));
    currentUserLabel.setColour(juce::Label::textColourId, UiColors::text);
    currentUserLabel.setJustificationType(juce::Justification::centredLeft);
    currentUserLabel.setMinimumHorizontalScale(0.8f);
    addAndMakeVisible(currentUserLabel);

    // LOG OUT uses exactly the same surface/ink treatment as SETTINGS and
    // AUDIO SETTINGS; the three buttons form one unified account row.
    logoutButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    logoutButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
    logoutButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    logoutButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    logoutButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    logoutButton.setTooltip("Sign out of your account");
    logoutButton.onClick = [this] { logoutCurrentUser(); };
    addAndMakeVisible(logoutButton);

    // Quit button on the main screen. systemRequestedQuit() runs the normal
    // shutdown path, so the audio session and telemetry are closed cleanly.
    quitButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    quitButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
    quitButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    quitButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    quitButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    quitButton.setTooltip("Close GuitarBattle");
    quitButton.onClick = []
    {
        if (auto* app = juce::JUCEApplication::getInstance())
            app->systemRequestedQuit();
    };
    addAndMakeVisible(quitButton);

    // The username is a plain label. This visible button opens the personal
    // account/profile window without covering the name.
    accountSettingsButton.setButtonText("PROFILE");
    accountSettingsButton.setColour(juce::TextButton::buttonColourId,
                                    UiColors::surface2);
    accountSettingsButton.setColour(juce::TextButton::buttonOnColourId,
                                    UiColors::accent);
    accountSettingsButton.setColour(juce::TextButton::textColourOffId,
                                    UiColors::text);
    accountSettingsButton.setColour(juce::TextButton::textColourOnId,
                                    UiColors::onAccent);
    accountSettingsButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    accountSettingsButton.setTooltip("Open your profile");
    accountSettingsButton.onClick = [this] { openAccountWindow(); };
    addAndMakeVisible(accountSettingsButton);

    // Admin-only draft preview. The toggle is visible only when /auth/me
    // reported is_admin; its state is remembered in the user settings so it
    // survives a restart, and switching it reloads the catalogue with drafts.
    adminPreviewButton.setButtonText("ADMIN");
    adminPreviewButton.setClickingTogglesState(true);
    adminPreviewButton.setColour(juce::TextButton::buttonColourId,
                                 UiColors::surface2);
    adminPreviewButton.setColour(juce::TextButton::buttonOnColourId,
                                 UiColors::accent);
    adminPreviewButton.setColour(juce::TextButton::textColourOffId,
                                 UiColors::text);
    adminPreviewButton.setColour(juce::TextButton::textColourOnId,
                                 UiColors::onAccent);
    adminPreviewButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    adminPreviewButton.setTooltip("Show drafts and play them as preview "
                                  "(results are not ranked)");
    adminPreviewButton.setToggleState(adminPreviewMode,
                                      juce::dontSendNotification);
    adminPreviewButton.onClick = [this]
    {
        setAdminPreviewMode(adminPreviewButton.getToggleState());
    };
    adminPreviewButton.setVisible(false);
    addAndMakeVisible(adminPreviewButton);

    // Support: one click opens the public support page in the default browser.
    // Same accent fill as START SONG so it reads as a primary action in the
    // header instead of a faded secondary button.
    supportButton.setButtonText("SUPPORT");
    supportButton.setColour(juce::TextButton::buttonColourId,
                            UiColors::accent);
    supportButton.setColour(juce::TextButton::buttonOnColourId,
                            UiColors::accentStrong);
    supportButton.setColour(juce::TextButton::textColourOffId,
                            UiColors::onAccent);
    supportButton.setColour(juce::TextButton::textColourOnId,
                            UiColors::onAccent);
    supportButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    supportButton.setTooltip("Open the Fretfight support page in your browser");
    supportButton.onClick = []
    {
        // Same origin as every other link in the client, so a future domain
        // change only has to touch ApiClient::serverOrigin.
        juce::URL(juce::String(ApiClient::serverOrigin) + "/support")
            .launchInDefaultBrowser();
    };
    addAndMakeVisible(supportButton);

    retryConnectionButton.setColour(juce::TextButton::buttonColourId,
                                    UiColors::surface2);
    retryConnectionButton.setColour(juce::TextButton::textColourOffId,
                                    UiColors::text);
    retryConnectionButton.setColour(juce::TextButton::buttonOnColourId,
                                    UiColors::accent);
    retryConnectionButton.onClick = [this] { tryRestoreSavedSession(); };
    addAndMakeVisible(retryConnectionButton);

    accountLoginButton.setColour(juce::TextButton::buttonColourId, UiColors::accent);
    accountLoginButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accentStrong);
    accountLoginButton.setColour(juce::TextButton::textColourOffId, UiColors::onAccent);
    accountLoginButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    accountLoginButton.setButtonText("SIGN IN");
    accountRegisterButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    accountRegisterButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
    accountRegisterButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    accountRegisterButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    accountRegisterButton.setButtonText("CREATE ACCOUNT");

    for (auto* button : { &verifyEmailButton, &forgotPasswordButton })
    {
        button->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        button->setColour(juce::TextButton::buttonOnColourId, UiColors::surface2);
        button->setColour(juce::TextButton::textColourOffId, UiColors::muted);
        button->setColour(juce::TextButton::textColourOnId, UiColors::text);
    }

    for (auto* button : { &accountLoginButton, &accountRegisterButton,
                          &verifyEmailButton, &forgotPasswordButton })
        addAndMakeVisible(*button);

    accountLoginButton.onClick = [this] { authenticateOnlineAccount(false); };
    accountRegisterButton.onClick = [this] { authenticateOnlineAccount(true); };
    verifyEmailButton.onClick = [this]
    {
        showEmailVerificationDialog({}, {}, false);
    };
    forgotPasswordButton.onClick = [this] { recoverOnlineAccountPassword(); };

    addAndMakeVisible(songMistButton);
    addAndMakeVisible(songChordsButton);
    addAndMakeVisible(aiSoloButton);
    addAndMakeVisible(oldGeneratorArchiveButton);
    // For now the generator uses one carefully controlled style. Keep the ID at 2
    // so it maps to MidiMelodyGenerator::Style::PowerMetal internally.
    melodyStyleCombo.addItem("Power Rock", 2);
    melodyStyleCombo.setSelectedId(2, juce::dontSendNotification);
    melodyStyleCombo.onChange = [this]
    {
        // Rebuild today's deterministic challenge with the selected style.
        // NEW GENERATION still creates a fresh random seed on demand.
        generateDailyChallengeMidi(false);
    };
    melodyStyleCombo.setColour(juce::ComboBox::backgroundColourId, UiColors::card);
    melodyStyleCombo.setColour(juce::ComboBox::textColourId, UiColors::text);
    melodyStyleCombo.setColour(juce::ComboBox::outlineColourId, UiColors::blueDark);
    addAndMakeVisible(melodyStyleCombo);
    addAndMakeVisible(generateMelodyButton);
    addAndMakeVisible(newGenerationButton);
    for (auto* button : { &songMistButton, &songChordsButton, &aiSoloButton, &oldGeneratorArchiveButton, &generateMelodyButton, &newGenerationButton })
    {
        button->setColour(juce::TextButton::buttonColourId, UiColors::card);
        button->setColour(juce::TextButton::textColourOffId, UiColors::text);
        button->setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
        button->setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    }
    addAndMakeVisible(songPridumayBestLabel);
    addAndMakeVisible(songMistBestLabel);
    addAndMakeVisible(songChordsBestLabel);
    addAndMakeVisible(dailyChallengeBestLabel);

    auto configureBestLabel = [](juce::Label& label)
    {
        label.setFont(GameUi::eyebrow(10.0f));
        label.setColour(juce::Label::textColourId, UiColors::muted);
        label.setJustificationType(juce::Justification::centredLeft);
        label.setInterceptsMouseClicks(false, false);
    };
    configureBestLabel(songPridumayBestLabel);
    configureBestLabel(songMistBestLabel);
    configureBestLabel(songChordsBestLabel);
    configureBestLabel(dailyChallengeBestLabel);
    songPridumayBestLabel.setColour(juce::Label::textColourId, UiColors::accent);
    updateSongBestScoreLabels();

    songPridumayButton.onClick = [this]
    {
        // Loads the highlighted catalogue row; online assets are downloaded on
        // demand by downloadOnlineSong() on an ApiClient worker thread.
        startSelectedSong();
    };

    onlineCatalogButton.onClick = [this]
    {
        // Re-reads the published catalogue (and the personal best scores when
        // signed in). The request runs on an ApiClient worker thread; this
        // handler never blocks the message thread.
        refreshOnlineCatalog();
    };

    // Populate the server catalogue as soon as the client starts. There is no
    // local fallback: if the request fails the list stays empty until REFRESH.
    refreshOnlineCatalog();

    songMistButton.onClick = [] {};
    songChordsButton.onClick = [] {};
    aiSoloButton.onClick = [] {};

    oldGeneratorArchiveButton.onClick = [this]
    {
        oldGeneratorArchiveOpen = !oldGeneratorArchiveOpen;
        oldGeneratorArchiveButton.setButtonText(oldGeneratorArchiveOpen
            ? "CLOSE OLD GENERATOR" : "OLD GENERATOR (ARCHIVE)");
        resized();
        repaint();
    };

    generateMelodyButton.onClick = [this]
    {
        if (loadGeneratedMelody())
            beginTuningForSelectedSong();
    };

    // Test-only control: create a fresh random seed, rebuild guitar and backing
    // MIDI together, then open the new challenge so its tablature can be tested.
    newGenerationButton.onClick = [this]
    {
        if (generateDailyChallengeMidi(true)
            && loadGeneratedMelody())
        {
            beginTuningForSelectedSong();
        }
    };

    //==========================================================================
    // Score display (gameSessionLabel is the visible score label)

    gameSessionLabel.setText(
        "0",
        juce::dontSendNotification);

    gameSessionLabel.setFont(
        GameUi::heading(46.0f));

    gameSessionLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    gameSessionLabel.setJustificationType(
        juce::Justification::centredRight);

    addAndMakeVisible(
        gameSessionLabel);

    //==========================================================================
    // Hit status

    hitLabel.setText(
        "READY",
        juce::dontSendNotification);

    hitLabel.setFont(
        GameUi::heading(27.0f));

    hitLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    hitLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        hitLabel);

    //==========================================================================
    // Target

    targetNoteLabel.setText(
        gameSession.getTargetNote(),
        juce::dontSendNotification);

    targetNoteLabel.setFont(
        GameUi::heading(34.0f));

    targetNoteLabel.setColour(
        juce::Label::textColourId,
        UiColors::accent);

    targetNoteLabel.setJustificationType(
        juce::Justification::centred);

    addAndMakeVisible(
        targetNoteLabel);

    //==========================================================================
    // Audio settings

    audioSettingsButton.setButtonText(
        "AUDIO");

    audioSettingsButton.setColour(
        juce::TextButton::buttonColourId,
        UiColors::surface2);

    audioSettingsButton.setColour(
        juce::TextButton::buttonOnColourId,
        UiColors::accent);

    audioSettingsButton.setColour(
        juce::TextButton::textColourOffId,
        UiColors::text);

    audioSettingsButton.setColour(
        juce::TextButton::textColourOnId,
        UiColors::onAccent);

    addAndMakeVisible(
        audioSettingsButton);

    audioSettingsButton.onClick =
        [this]
        {
            juce::DialogWindow::LaunchOptions options;

            options.content.setOwned(
                new AudioSettingsPanel(
                    deviceManager,
                    preferredInputChannel.load(std::memory_order_relaxed),
                    [this](int channel)
                    {
                        setPreferredInputChannel(channel);
                    },
                    guitarInputOnly.load(std::memory_order_relaxed),
                    [this](bool only)
                    {
                        setGuitarInputOnly(only);
                    },
                    backgroundMusicVolume.load(std::memory_order_relaxed),
                    backgroundMusicEnabled.load(std::memory_order_relaxed),
                    [this](float volume)
                    {
                        setBackgroundMusicVolume(volume);
                    },
                    [this](bool enabled)
                    {
                        setBackgroundMusicEnabled(enabled);
                    }));

            options.dialogTitle =
                "Audio Settings";

            options.dialogBackgroundColour =
                UiColors::background;

            options.escapeKeyTriggersCloseButton =
                true;

            options.useNativeTitleBar =
                true;

            options.resizable =
                true;

            options.launchAsync();
        };

    //==========================================================================
    // Difficulty

    easyButton.setClickingTogglesState(true);
    normalButton.setClickingTogglesState(true);
    hardButton.setClickingTogglesState(true);

    easyButton.setRadioGroupId(1001);
    normalButton.setRadioGroupId(1001);
    hardButton.setRadioGroupId(1001);

    easyButton.setToggleState(
        false,
        juce::dontSendNotification);

    normalButton.setToggleState(
        true,
        juce::dontSendNotification);

    hardButton.setToggleState(
        false,
        juce::dontSendNotification);

    easyButton.onClick =
        [this]
        {
            difficulty =
                Difficulty::Easy;
        };

    normalButton.onClick =
        [this]
        {
            difficulty =
                Difficulty::Normal;
        };

    hardButton.onClick =
        [this]
        {
            difficulty =
                Difficulty::Hard;
        };
}

//==============================================================================
// VST monitor plugin
//==============================================================================

void MainComponent::openVstPanel()
{
    if (vstDialogWindow != nullptr)
    {
        vstDialogWindow->setVisible(true);
        vstDialogWindow->toFront(true);
        return;
    }

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new VstPanel(vstHost));
    options.dialogTitle = "VST Plugin";
    options.dialogBackgroundColour = UiColors::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.componentToCentreAround = this;

    // create() instead of launchAsync(): the chooser must not be modal, because
    // the plugin editor is a separate top-level window that has to stay
    // interactive while the chooser is open. The window is owned here and its
    // close button only hides it, so reopening is instant.
    vstDialogWindow.reset(options.create());

    if (vstDialogWindow != nullptr)
    {
        vstDialogWindow->setVisible(true);
        vstDialogWindow->toFront(true);
    }
}

void MainComponent::updateVstButton()
{
    const auto name = vstHost.getLoadedPluginName();

    // The header slot is narrow, so the button only carries the state and the
    // loaded plugin name lives in the tooltip and in the VST window.
    if (name.isEmpty())
    {
        vstButton.setButtonText("VST: OFF");
        vstButton.setToggleState(false, juce::dontSendNotification);
        vstButton.setTooltip("Load a VST3 effect for the monitored guitar. "
                             "Note detection stays dry.");
        return;
    }

    if (vstHost.isBypassed())
    {
        vstButton.setButtonText("VST: BYPASS");
        vstButton.setToggleState(false, juce::dontSendNotification);
        vstButton.setTooltip("Bypassed: " + name + ". Click to choose another plugin.");
        return;
    }

    vstButton.setButtonText("VST: ON");
    vstButton.setToggleState(true, juce::dontSendNotification);
    vstButton.setTooltip("Active: " + name + ". Click to choose another plugin.");
}
