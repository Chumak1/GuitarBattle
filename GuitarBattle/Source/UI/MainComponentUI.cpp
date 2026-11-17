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
#include "FriendsPanel.h"
#include "MessagesPanel.h"

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

    // Normal catalogue rows use the same surface tile as the rest of the lists;
    // the selected row is only slightly lighter, with the accent bar on the
    // left as the selection marker (no gold wash on the row itself).
    if (rowIsSelected)
    {
        g.setColour(UiColors::surface.withAlpha(0.70f));
    }
    else
    {
        g.setColour(UiColors::surface.withAlpha(0.38f));
    }

    g.fillRoundedRectangle(row.toFloat(), 12.0f);

    g.setColour(UiColors::border.withAlpha(rowIsSelected ? 0.70f : 0.45f));
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

    // The title is always readable: no row is locked by player level.
    g.setColour(UiColors::text);
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

    // The difficulty tag is always gold: the gold in the list means "playable
    // now", and every row is.
    g.setColour(UiColors::accent.withAlpha(rowIsSelected ? 0.22f : 0.14f));
    g.fillRoundedRectangle(levelPill, 6.0f);
    g.setColour(UiColors::accent);
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

    updateStartButtonState();
}

void MainComponent::updateStartButtonState()
{
    // Access never depends on the player level: the button is enabled as soon
    // as the catalogue has a selected row.
    songPridumayButton.setEnabled(juce::isPositiveAndBelow(
        selectedCatalogIndex, static_cast<int>(catalogSongs.size())));
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

MainComponent::TunerGeometry MainComponent::computeTunerGeometry() const
{
    TunerGeometry geo;

    const float width = static_cast<float> (getWidth());
    const float height = static_cast<float> (getHeight());
    const float sideW = static_cast<float> (sectionSidebarWidth());
    const bool hubTuner = tunerStandalone;
    const float topOffset = hubTuner ? 36.0f : 0.0f;
    // Both edges sit on the same inset as the section header. The sidebar
    // occupies the LEFT side only, so it must not be subtracted from the right
    // edge as well: that used to leave the blocks ~half a sidebar short.
    const float panelLeft = 42.0f + sideW;
    const float panelRight = width - 42.0f;
    const float gap = 16.0f;
    const float panelY = 190.0f + topOffset;
    const float panelW = juce::jmax (320.0f, panelRight - panelLeft);
    // The in-game tuner keeps its SKIP / START row below the blocks.
    const float buttonsH = hubTuner ? 0.0f : 78.0f;
    const float available = juce::jmax (360.0f,
                                        height - panelY - 24.0f - buttonsH);

    // STRINGS on top, LIVE INPUT below; both span the whole section width and
    // the split follows the window height.
    float h1 = juce::jlimit (200.0f, 320.0f, available * 0.44f);
    float h2 = juce::jmax (170.0f, available - h1 - gap);

    if (h1 + h2 + gap > available)
    {
        const float scale = available / (h1 + h2 + gap);
        h1 *= scale;
        h2 *= scale;
    }

    geo.stringsPanel = { panelLeft, panelY, panelW, h1 };
    geo.inputPanel = { panelLeft, panelY + h1 + gap, panelW, h2 };

    geo.stringSlots = juce::jlimit (1, GuitarTuning::maxStrings,
                                    currentTuning.count);

    const float innerW = geo.stringsPanel.getWidth() - 48.0f;
    geo.tileGap = 12.0f;
    geo.columns = geo.stringSlots;

    // As many tiles per row as fit comfortably: the usual 4-8 strings fit in a
    // single full-width row, two rows only on a narrow window.
    while (geo.columns > 1
           && (innerW - geo.tileGap * static_cast<float> (geo.columns - 1))
                  / static_cast<float> (geo.columns) < 88.0f)
        --geo.columns;

    geo.rows = (geo.stringSlots + geo.columns - 1) / geo.columns;
    geo.tileH = geo.rows == 1 ? 104.0f : 96.0f;
    geo.tileW = (innerW - geo.tileGap * static_cast<float> (geo.columns - 1))
                / static_cast<float> (geo.columns);
    geo.tileTop = geo.stringsPanel.getY() + 56.0f;

    geo.rightColW = juce::jlimit (200.0f, 340.0f,
                                  geo.inputPanel.getWidth() * 0.26f);
    geo.rightColX = geo.inputPanel.getRight() - 24.0f - geo.rightColW;

    geo.dialX = geo.inputPanel.getCentreX();
    geo.dialY = geo.inputPanel.getY() + geo.inputPanel.getHeight() * 0.46f;
    geo.dialRadius = juce::jmin (88.0f,
                                 juce::jmin (geo.inputPanel.getHeight() * 0.32f,
                                             geo.inputPanel.getWidth() * 0.08f));
    return geo;
}

void MainComponent::paint(juce::Graphics& g){    const auto area = getLocalBounds().toFloat();
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
        const int sideW = sectionSidebarWidth();
        const bool homeSection = currentSection == AppSection::Home;
        const bool songsSection = currentSection == AppSection::Songs;
        const bool songChrome = songsSection;

        const juce::Rectangle<float> nav(24.0f + sideW, 20.0f,
                                         width - 48.0f - sideW, 78.0f);
        GameUi::drawCard(g, nav, 18.0f);

        // The sidebar already carries the wordmark; the top bar shows the
        // active destination and leaves the top-right corner to EXIT.
        if (sideW == 0)
            drawBrand(44.0f, 40.0f);

        {
            juce::String sectionTitle = "HOME";

            switch (currentSection)
            {
                case AppSection::Profile:    sectionTitle = "PROFILE"; break;
                case AppSection::Campaign:   sectionTitle = "BATTLE"; break;
                case AppSection::Songs:      sectionTitle = "PRACTICE"; break;
                case AppSection::OnlineDuel: sectionTitle = "ONLINE"; break;
                case AppSection::Friends:    sectionTitle = "FRIENDS"; break;
                case AppSection::Messages:   sectionTitle = "MESSAGES"; break;
                case AppSection::Tuner:      sectionTitle = "TUNER"; break;
                case AppSection::Vst:        sectionTitle = "VST"; break;
                case AppSection::Settings:   sectionTitle = "SETTINGS"; break;
                case AppSection::Home:       break;
            }

            g.setColour (UiColors::muted);
            g.setFont (GameUi::eyebrow (8.0f));
            g.drawText ("FRETFIGHT",
                        juce::Rectangle<float> (nav.getX() + 24.0f,
                                                nav.getY() + 14.0f,
                                                260.0f, 12.0f),
                        juce::Justification::centredLeft, false);

            g.setColour (UiColors::text);
            g.setFont (GameUi::heading (22.0f));
            g.drawText (sectionTitle,
                        juce::Rectangle<float> (nav.getX() + 24.0f,
                                                nav.getY() + 28.0f,
                                                420.0f, 30.0f),
                        juce::Justification::centredLeft, false);
        }

        // Profile/Campaign/VST/Settings are painted by their own embedded
        // components below the header.
        if (! songChrome && ! homeSection)
            return;

        if (homeSection)
        {
            // Account summary moved here from the personal account page: the
            // identity line plus the eight stat cards, without the REFRESH /
            // LOG OUT buttons (they stay on the account page).
            const float stripX = 48.0f + sideW;
            const float stripW = width - 96.0f - sideW;
            const float stripY = 110.0f;

            g.setColour (UiColors::accent);
            g.setFont (GameUi::eyebrow (10.0f));
            g.drawText ("PERSONAL ACCOUNT",
                        juce::Rectangle<float> (stripX + 24.0f, stripY,
                                                360.0f, 14.0f),
                        juce::Justification::centredLeft, false);

            juce::String accountLine = "Signed in as ";
            accountLine << (currentUsername.isNotEmpty() ? currentUsername
                                                         : juce::String ("guest"));

            if (accountEmail.isNotEmpty())
                accountLine << "   |   " << accountEmail;

            g.setColour (UiColors::muted);
            g.setFont (GameUi::font (11.5f));
            g.drawText (accountLine,
                        juce::Rectangle<float> (stripX + 24.0f, stripY + 17.0f,
                                                stripW - 48.0f, 15.0f),
                        juce::Justification::centredLeft, true);

            const juce::String statLabels[] = { "SCORES", "RANK", "SONGS",
                                                "ATTEMPTS", "LEVEL", "XP",
                                                "ACCURACY", "TIMING" };
            const juce::String statValues[] = { accountScoresText, accountRankText,
                                                accountSongsText, accountAttemptsText,
                                                accountLevelText, accountXpText,
                                                accountAccuracyText, accountTimingText };
            const juce::String statNotes[] = { "best score per song", accountRankNote,
                                               "songs with an attempt", "total attempts",
                                               accountXpNote, "total experience",
                                               "last 100 attempts", "avg timing error" };

            const float cardGap = 10.0f;
            const float statW = (stripW - 48.0f - cardGap * 7.0f) / 8.0f;
            const float statY = stripY + 40.0f;
            const float statH = 70.0f;

            for (int i = 0; i < 8; ++i)
            {
                const juce::Rectangle<float> statCard(
                    stripX + 24.0f + static_cast<float>(i) * (statW + cardGap),
                    statY, statW, statH);
                GameUi::drawCard (g, statCard, 14.0f);
                GameUi::drawEyebrow (g, statLabels[i],
                                     statCard.withTrimmedLeft (14.0f)
                                         .withTrimmedRight (10.0f)
                                         .withTop (statCard.getY() + 10.0f)
                                         .withHeight (13.0f),
                                     UiColors::accent);

                g.setColour (UiColors::text);
                g.setFont (GameUi::heading (22.0f));
                g.drawText (statValues[i],
                            statCard.withTrimmedLeft (14.0f)
                                .withTrimmedRight (10.0f)
                                .withTop (statCard.getY() + 25.0f)
                                .withHeight (28.0f),
                            juce::Justification::centredLeft, false);

                g.setColour (UiColors::muted);
                g.setFont (GameUi::font (9.5f));
                g.drawText (statNotes[i],
                            statCard.withTrimmedLeft (14.0f)
                                .withTrimmedRight (10.0f)
                                .withTop (statCard.getBottom() - 22.0f)
                                .withHeight (16.0f),
                            juce::Justification::centredLeft, true);
            }

            const float contentY = 234.0f;
            const float contentW2 = width - 96.0f - sideW;
            const float contentH2 = juce::jmax(320.0f, height - 68.0f - 20.0f - contentY);
            const juce::Rectangle<float> card(48.0f + sideW, contentY,
                                              contentW2, contentH2);
            GameUi::drawCard(g, card, 20.0f, true);

            g.setColour (UiColors::accent);
            g.setFont (GameUi::eyebrow (10.0f));
            g.drawText ("GLOBAL LEADERBOARD",
                        juce::Rectangle<float> (card.getX() + 24.0f,
                                                card.getY() + 24.0f,
                                                card.getWidth() - 48.0f, 16.0f),
                        juce::Justification::centredLeft, false);

            // Podium: the top three players as medal cards above the table.
            if (leaderboardModel != nullptr)
            {
                const auto& top = leaderboardModel->getRows();
                const int topCount = juce::jmin (3, static_cast<int> (top.size()));

                if (topCount > 0)
                {
                    auto spaced = [] (int value)
                    {
                        auto text = juce::String (value);

                        for (int i = text.length() - 3; i > 0; i -= 3)
                            text = text.substring (0, i) + " "
                                   + text.substring (i);

                        return text;
                    };

                    const float gap = 14.0f;
                    const float innerX = card.getX() + 24.0f;
                    const float innerW = card.getWidth() - 48.0f;
                    const float slotW = (innerW - gap * 2.0f) / 3.0f;
                    const float baseY = card.getY() + 96.0f;
                    const float baseH = 150.0f;
                    const int order[3] = { 1, 0, 2 }; // silver, gold, bronze

                    for (int slot = 0; slot < 3; ++slot)
                    {
                        const int index = order[slot];

                        if (index >= topCount)
                            continue;

                        const auto& row = top[static_cast<size_t> (index)];
                        const bool isFirst = index == 0;
                        const float tileH = isFirst ? baseH : baseH - 22.0f;
                        const juce::Rectangle<float> tile (
                            innerX + static_cast<float> (slot) * (slotW + gap),
                            baseY + baseH - tileH, slotW, tileH);

                        GameUi::drawCard (g, tile, 16.0f, isFirst);

                        // Click target for the card, used by mouseUp().
                        podiumHitBounds[static_cast<size_t> (slot)] =
                            tile.toNearestInt();
                        podiumUserIds[static_cast<size_t> (slot)] = row.userId;

                        const juce::Colour medal = isFirst
                            ? UiColors::gold
                            : (index == 1 ? juce::Colour (0xFFC0C6CF)
                                          : juce::Colour (0xFFC58B5A));

                        const auto badge = juce::Rectangle<float> (
                            tile.getX() + 16.0f, tile.getY() + 14.0f, 26.0f, 26.0f);
                        g.setColour (medal.withAlpha (0.18f));
                        g.fillEllipse (badge);
                        g.setColour (medal);
                        g.setFont (GameUi::heading (13.0f));
                        g.drawText (juce::String (row.rank > 0 ? row.rank : index + 1),
                                    badge.toNearestInt(),
                                    juce::Justification::centred, false);

                        const auto avatar = juce::Rectangle<float> (
                            tile.getRight() - 60.0f, tile.getY() + 14.0f,
                            44.0f, 44.0f);
                        const auto name = row.displayName.isNotEmpty()
                                              ? row.displayName
                                              : row.username;

                        g.setColour (medal.withAlpha (0.14f));
                        g.fillEllipse (avatar);
                        g.setColour (medal);
                        g.setFont (GameUi::heading (18.0f));
                        g.drawText (name.isEmpty()
                                        ? juce::String ("?")
                                        : name.substring (0, 1).toUpperCase(),
                                    avatar.toNearestInt(),
                                    juce::Justification::centred, false);

                        g.setColour (UiColors::text);
                        g.setFont (GameUi::heading (isFirst ? 16.0f : 14.5f));
                        g.drawFittedText (
                            name,
                            juce::Rectangle<float> (tile.getX() + 16.0f,
                                                    tile.getY() + 48.0f,
                                                    tile.getWidth() - 100.0f, 22.0f)
                                .toNearestInt(),
                            juce::Justification::centredLeft, 1, 1.0f);

                        g.setColour (UiColors::muted);
                        g.setFont (GameUi::eyebrow (8.0f));
                        g.drawText ("LVL " + juce::String (row.level),
                                    juce::Rectangle<float> (tile.getX() + 16.0f,
                                                            tile.getY() + 72.0f,
                                                            tile.getWidth() - 32.0f,
                                                            12.0f),
                                    juce::Justification::centredLeft, false);

                        g.setColour (medal);
                        g.setFont (GameUi::heading (isFirst ? 24.0f : 20.0f));
                        const auto skillsText = row.guitarSkills >= 0.0
                                                    ? juce::String (row.guitarSkills, 1)
                                                    : spaced (row.rating);
                        g.drawText (skillsText,
                                    juce::Rectangle<float> (tile.getX() + 16.0f,
                                                            tile.getBottom() - 40.0f,
                                                            tile.getWidth() - 32.0f,
                                                            28.0f),
                                    juce::Justification::centredLeft, false);

                        // Rank movement since the previous calculation.
                        if (row.hasPositionChange && row.positionChange != 0)
                        {
                            const bool movedUp = row.positionChange > 0;
                            g.setColour (movedUp ? UiColors::green : UiColors::red);
                            g.setFont (GameUi::font (11.0f, true));
                            g.drawText ((movedUp ? "+" : "")
                                            + juce::String (row.positionChange),
                                        juce::Rectangle<float> (tile.getRight() - 66.0f,
                                                                tile.getBottom() - 36.0f,
                                                                50.0f, 18.0f),
                                        juce::Justification::centredRight, false);
                        }
                    }
                }
            }

            GameUi::drawDivider (g, card.getX() + 24.0f,
                                 card.getRight() - 24.0f,
                                 card.getY() + 262.0f);

            if (leaderboardLabel.getText().isEmpty())
            {
                const float centreX = card.getCentreX();
                const float iconY = card.getCentreY() - 20.0f;

                g.setColour (UiColors::accent.withAlpha (0.08f));
                g.fillEllipse (centreX - 44.0f, iconY - 44.0f, 88.0f, 88.0f);
                g.setColour (UiColors::accent.withAlpha (0.32f));
                g.drawEllipse (centreX - 44.0f, iconY - 44.0f, 88.0f, 88.0f, 1.0f);

                g.setColour (UiColors::text);
                g.setFont (GameUi::font (13.0f, true));
                g.drawText ("NO SCORES YET",
                            juce::Rectangle<float> (centreX - 160.0f, iconY + 62.0f,
                                                    320.0f, 20.0f),
                            juce::Justification::centred, false);
                g.setColour (UiColors::muted);
                g.setFont (GameUi::font (11.0f));
                g.drawText ("Press REFRESH to load the global rating.",
                            juce::Rectangle<float> (centreX - 180.0f, iconY + 84.0f,
                                                    360.0f, 18.0f),
                            juce::Justification::centred, false);
            }

            return;
        }

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

        const float contentY = homeSection ? 250.0f : 124.0f;
        const float availableH = juce::jmax(320.0f, height - 68.0f - 20.0f - contentY);
        // Fill the window instead of capping the content height: on large
        // displays the old 560 px cap left the lower third of the screen empty.
        const float contentH = availableH;
        const float footerY = contentY + contentH + 20.0f;
        const float contentW = width - 96.0f - sideW;
        const float gap = 20.0f;
        const float leftW = songsSection ? contentW : contentW * 0.56f;
        const float rightX = 48.0f + sideW + leftW + gap;
        const float rightW = juce::jmax(0.0f, contentW - leftW - gap);

        // Level panel: current level, XP earned inside the level, the cost of
        // the next level, what is left and an animated progress bar. Every
        // number comes from the server-side progression payload; the bar is a
        // clamped view of the same ratio, so text and fill never disagree.
        if (homeSection)
        {
            const juce::Rectangle<float> panel(
                48.0f + sideW, 108.0f,
                width - 96.0f - sideW - inputMonitorCardWidth(juce::roundToInt(width)) - 14.0f,
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

        const juce::Rectangle<float> leftCard(48.0f + sideW, contentY, leftW, contentH);
        const juce::Rectangle<float> rightCard(rightX, contentY, rightW, contentH);
        GameUi::drawCard(g, leftCard, 20.0f, true);

        if (homeSection)
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

        if (homeSection)
        {
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
        }

        // Footer status strip.
        const juce::Rectangle<float> footer(24.0f + sideW, footerY,
                                            width - 48.0f - sideW, 44.0f);
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
        const float sideWf = static_cast<float> (sectionSidebarWidth());
        // The hub tuner is a normal sidebar section; the in-game tuner keeps the
        // full-window layout without the sidebar.
        const bool hubTuner = tunerStandalone;

        if (hubTuner)
        {
            const juce::Rectangle<float> nav(24.0f + sideWf, 20.0f,
                                             width - 48.0f - sideWf, 78.0f);
            GameUi::drawCard(g, nav, 18.0f);

            g.setColour(UiColors::muted);
            g.setFont(GameUi::eyebrow(8.0f));
            g.drawText("FRETFIGHT",
                       juce::Rectangle<float>(nav.getX() + 24.0f,
                                              nav.getY() + 14.0f, 260.0f, 12.0f),
                       juce::Justification::centredLeft, false);

            g.setColour(UiColors::text);
            g.setFont(GameUi::heading(22.0f));
            g.drawText("TUNER",
                       juce::Rectangle<float>(nav.getX() + 24.0f,
                                              nav.getY() + 28.0f, 420.0f, 30.0f),
                       juce::Justification::centredLeft, false);
        }

        if (! tunerStandalone && sideWf <= 0.0f)
            drawBrand(42.0f, 24.0f);

        // The hub section header takes the same 118 px band as other sections,
        // so the tuner body starts slightly lower.
        const float topOffset = hubTuner ? 36.0f : 0.0f;

        const int tunedMaskTop = tuner.getTunedMask();
        const int activeStrings = juce::jmax(1, tuner.getActiveStringCount());
        int tunedCountTop = 0;
        for (int slot = 0; slot < currentTuning.count; ++slot)
            tunedCountTop += (tunedMaskTop & (1 << slot)) != 0 ? 1 : 0;
        const bool allTunedTop = tunedCountTop >= activeStrings;

        // The global EXIT / DONATION / FEEDBACK buttons occupy the top-right
        // corner on tuner screens; keep the tuning pill to their left.
        GameUi::drawPill(g,
                         juce::Rectangle<float>(width - 524.0f, 42.0f,
                                                136.0f, 22.0f),
                         allTunedTop ? juce::String("READY")
                                     : "TUNING " + juce::String(tunedCountTop)
                                           + "/" + juce::String(activeStrings),
                         allTunedTop ? UiColors::green : UiColors::accent,
                         allTunedTop);

        GameUi::drawEyebrow(g, "PRE-FLIGHT CHECK",
                            juce::Rectangle<float>(42.0f + sideWf,
                                                   82.0f + topOffset, 360.0f, 16.0f));
        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(34.0f));
        g.drawText("Tune your instrument",
                   juce::Rectangle<float>(42.0f + sideWf, 100.0f + topOffset,
                                          width - 84.0f - sideWf, 44.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(13.0f));
        const juce::String tuningHint =
            currentTuningName + " TUNING  /  "
            + GuitarTuning::label(currentTuning, false)
            + ". Play each string and wait for the green light.";
        g.drawText(tuningHint,
                   juce::Rectangle<float>(42.0f + sideWf, 148.0f + topOffset,
                                          width - 84.0f - sideWf, 24.0f),
                   juce::Justification::centredLeft, false);

        const auto geo = computeTunerGeometry();
        const juce::Rectangle<float> stringsPanel = geo.stringsPanel;
        const juce::Rectangle<float> inputPanel = geo.inputPanel;
        GameUi::drawCard(g, stringsPanel, 20.0f);
        GameUi::drawCard(g, inputPanel, 20.0f, true);

        g.setColour(UiColors::accent);
        g.setFont(GameUi::eyebrow(10.0f));
        g.drawText("STRINGS",
                   juce::Rectangle<float>(stringsPanel.getX() + 24.0f,
                                          stringsPanel.getY() + 20.0f,
                                          300.0f, 16.0f),
                   juce::Justification::centredLeft, false);

        const int stringSlots = geo.stringSlots;
        const float innerX = stringsPanel.getX() + 24.0f;
        const int tunedMask = tuner.getTunedMask();

        for (int slot = 0; slot < stringSlots; ++slot)
        {
            const int column = slot % geo.columns;
            const int row = slot / geo.columns;
            const float x = innerX
                + static_cast<float>(column) * (geo.tileW + geo.tileGap);
            const float y = geo.tileTop
                + static_cast<float>(row) * (geo.tileH + geo.tileGap);
            const bool active = GuitarTuning::isActive(currentTuning, slot);
            const bool tuned = active && (tunedMask & (1 << slot)) != 0;
            const auto colour = tuned ? UiColors::green
                                      : (active ? UiColors::accent : UiColors::muted);

            g.setColour(tuned ? UiColors::green.withAlpha(0.10f)
                              : UiColors::surface2.withAlpha(active ? 0.72f : 0.34f));
            g.fillRoundedRectangle(x, y, geo.tileW, geo.tileH, 14.0f);
            g.setColour(colour.withAlpha(tuned ? 0.46f : 0.22f));
            g.drawRoundedRectangle(x, y, geo.tileW, geo.tileH, 14.0f, 1.0f);

            // The note itself is a real ComboBox placed over the tile by
            // resized(); the paint layer only provides the card and status.
            g.setColour(UiColors::muted);
            g.setFont(GameUi::eyebrow(8.5f));
            g.drawText("STRING " + juce::String(GuitarTuning::stringNumber(currentTuning, slot)),
                       juce::Rectangle<float>(x, y + geo.tileH - 44.0f,
                                              geo.tileW, 14.0f),
                       juce::Justification::centred, false);
            GameUi::drawPill(g,
                             juce::Rectangle<float>(x + 10.0f,
                                                    y + geo.tileH - 28.0f,
                                                    geo.tileW - 20.0f, 20.0f),
                             active ? (tuned ? juce::String("IN TUNE")
                                             : juce::String("WAITING"))
                                    : juce::String("EMPTY"),
                             colour,
                             tuned);
        }

        // Tuner arc: a shallow gauge from flat (b) to sharp (#). The triangle
        // marks the target note at the top; the gold/green bar shows how far
        // the detected pitch is from it. The note itself is a real Label.
        const float dialX = geo.dialX;
        const float dialY = geo.dialY;
        const float dialRadius = geo.dialRadius;
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

        // Detected string and offset: caption above the value, both in the
        // right column of the LIVE INPUT block.
        g.setColour(UiColors::muted);
        g.setFont(GameUi::eyebrow(8.5f));
        g.drawText("DETECTED STRING",
                   juce::Rectangle<float>(geo.rightColX,
                                          inputPanel.getY() + 24.0f,
                                          geo.rightColW, 14.0f),
                   juce::Justification::centredRight, false);
        g.drawText("OFFSET",
                   juce::Rectangle<float>(geo.rightColX,
                                          inputPanel.getY() + 62.0f,
                                          geo.rightColW, 14.0f),
                   juce::Justification::centredRight, false);

        // Status pill across the bottom of the block.
        const auto statusColour = tuner.areAllStringsTuned()
            ? UiColors::green : UiColors::accent;
        const juce::Rectangle<float> statusPill(
            inputPanel.getX() + 24.0f, inputPanel.getBottom() - 58.0f,
            inputPanel.getWidth() - 48.0f, 38.0f);
        g.setColour(statusColour.withAlpha(0.10f));
        g.fillRoundedRectangle(statusPill, 19.0f);
        g.setColour(statusColour.withAlpha(0.38f));
        g.drawRoundedRectangle(statusPill, 19.0f, 1.0f);

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

        const juce::Rectangle<float> card(
            48.0f, 82.0f, width - 96.0f,
            juce::jmax(520.0f, height - 170.0f));

        // A finished bot duel is rendered inside this card: the header becomes
        // VICTORY / DEFEAT, both players' results follow, then the score strip
        // and the buttons. No second card is drawn on top.
        bool duelFinished = false;

        {
            const juce::SpinLock::ScopedLockType lock (duelLock);
            duelFinished = ! duelOnlineActive.load (std::memory_order_relaxed)
                           && duelMatch != nullptr
                           && duelMatch->isFinished();
        }

        if (duelFinished)
        {
            GameUi::drawCard(g, card, 24.0f, true);
            paintDuelResultInSessionCard (g, card);
            duelResultCardInSession = true;
            paintDuelOverlay (g);

            // Campaign: the next opponent may need its stems/chart downloaded.
            // Keep the victory card visible and show the content loading
            // progress on top instead of dropping to the generic results.
            if (campaignFightActive && songDownloadProgressState != nullptr)
            {
                const float w = 560.0f;
                const float h = 190.0f;
                const juce::Rectangle<float> loading (
                    (width - w) * 0.5f, (height - h) * 0.5f, w, h);

                g.setColour (UiColors::background.withAlpha (0.72f));
                g.fillRect (getLocalBounds());
                GameUi::drawCard (g, loading, 22.0f, true);

                g.setColour (UiColors::accent);
                g.setFont (GameUi::eyebrow (10.0f));
                g.drawText ("NEXT OPPONENT",
                            loading.withTrimmedTop (22.0f).withHeight (16.0f),
                            juce::Justification::centred, false);

                g.setColour (UiColors::text);
                g.setFont (GameUi::heading (28.0f));
                g.drawText ("LOADING CONTENT",
                            loading.withTrimmedTop (44.0f).withHeight (36.0f),
                            juce::Justification::centred, false);

                juce::String nextSong = "Next stage";

                if (juce::isPositiveAndBelow (
                        selectedCatalogIndex,
                        static_cast<int> (catalogSongs.size())))
                    nextSong = catalogSongs[
                        static_cast<size_t> (selectedCatalogIndex)].title;

                g.setColour (UiColors::muted);
                g.setFont (GameUi::font (12.5f));
                g.drawFittedText (
                    nextSong + " - downloading stems and chart",
                    loading.withTrimmedTop (84.0f).withHeight (20.0f)
                           .reduced (24.0f, 0.0f).toNearestInt(),
                    juce::Justification::centred, 1);

                const float progress = static_cast<float> (
                    juce::jlimit (0.0, 1.0, songDownloadProgressValue));
                const juce::Rectangle<float> bar (
                    loading.getX() + 32.0f, loading.getBottom() - 52.0f,
                    loading.getWidth() - 64.0f, 12.0f);

                g.setColour (UiColors::surface2);
                g.fillRoundedRectangle (bar, 6.0f);

                if (progress > 0.0f)
                {
                    g.setColour (UiColors::accent);
                    g.fillRoundedRectangle (
                        bar.withWidth (bar.getWidth() * progress), 6.0f);
                }

                g.setColour (UiColors::text);
                g.setFont (GameUi::font (12.0f, true));
                g.drawText (juce::String (juce::roundToInt (progress * 100.0f)) + "%",
                            juce::Rectangle<float> (loading.getRight() - 96.0f,
                                                    loading.getBottom() - 80.0f,
                                                    64.0f, 18.0f),
                            juce::Justification::centredRight, false);
            }

            return;
        }

        // A finished online match uses the same campaign-style card, filled
        // from the server's authoritative participant snapshot.
        if (duelOnlinePhase == DuelOnlinePhase::Finished
            && duelOnlineMatch.valid
            && duelOnlineMatch.status == "finished")
        {
            GameUi::drawCard(g, card, 24.0f, true);
            paintOnlineDuelResultInSessionCard (g, card);
            duelResultCardInSession = true;
            paintDuelOverlay (g);
            return;
        }

        const int score = gameplayStats.getScore();
        const int hits = gameplayStats.getHitCount();
        const int misses = gameplayStats.getMissCount();
        const int total = hits + misses;
        const double accuracy = total > 0
            ? 100.0 * static_cast<double>(hits) / static_cast<double>(total)
            : 0.0;

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

        if (currentSessionIsPreview || currentSessionIsPractice)
        {
            // The results screen repeats the notice: a draft preview and a
            // Practice-section run are saved as history only and never become
            // a best score or rating row.
            GameUi::drawPill(g,
                             juce::Rectangle<float>(card.getRight() - 232.0f,
                                                    card.getY() + 26.0f,
                                                    200.0f, 24.0f),
                             currentSessionIsPractice
                                 ? "PRACTICE - NOT RANKED"
                                 : "PREVIEW - NOT RANKED",
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

        // A finished duel draws its victory/defeat card on top of the session
        // summary; this branch returns before the gameplay overlay call.
        paintDuelOverlay (g);

        return;
    }

    //==========================================================================
    // Gameplay
    //==========================================================================

    const float topY = 12.0f;
    const float topH = 68.0f;
    const float hudY = 88.0f;
    const float hudH = 64.0f;
    // The old results strip at the bottom is gone: the mixer hugs the bottom
    // edge and the tablature takes the freed height.
    const float studioH = 86.0f;
    const float studioY = height - 16.0f - studioH;
    const float duelStripH = isDuelStageActive() ? 54.0f : 0.0f;
    const float tabY = hudY + hudH + 8.0f + duelStripH;
    float tabTop = tabY;
    float tabH = juce::jmax(150.0f, studioY - 12.0f - tabY);

    // Duel: the stage sits above the full-width tablature (same split as
    // resized()). The overlay only paints the tab card's shadow; the stage
    // itself is the DuelStageComponent child behind the overlay.
    if (isDuelStageActive())
    {
        const float gap = 10.0f;
        const float minTabs = juce::jmax(120.0f, tabH * 0.50f);
        float stageH = juce::jlimit(90.0f, 320.0f, tabH * 0.45f);

        if (tabH - stageH - gap < minTabs)
            stageH = juce::jmax(70.0f, tabH - gap - minTabs);

        tabTop = tabY + stageH + gap;
        tabH = juce::jmax(80.0f, tabH - stageH - gap);
    }

    const juce::Rectangle<float> topBar(24.0f, topY, width - 48.0f, topH);
    GameUi::drawGlassCard(g, topBar, 18.0f);
    drawBrand(44.0f, 20.0f);

    g.setColour(UiColors::muted);
    g.setFont(GameUi::eyebrow(8.0f));
    g.drawText("NOW PLAYING",
               juce::Rectangle<float>(210.0f, 20.0f, 260.0f, 12.0f),
               juce::Justification::centredLeft, false);

    constexpr float hudGap = 12.0f;
    const float comboW = 120.0f;
    const float accuracyW = 170.0f;
    const float scoreW = 190.0f;
    const float pitchW = 300.0f;
    const float statsW = 520.0f;
    const float targetW = 190.0f;

    // Every gameplay value lives in this single row: COMBO / ACCURACY / SCORE /
    // PITCH / LIVE PERFORMANCE / NEXT NOTE.
    const juce::Rectangle<float> comboCard(24.0f, hudY, comboW, hudH);
    const juce::Rectangle<float> accuracyCard(comboCard.getRight() + hudGap,
                                              hudY, accuracyW, hudH);
    const juce::Rectangle<float> scoreCard(accuracyCard.getRight() + hudGap,
                                           hudY, scoreW, hudH);
    const juce::Rectangle<float> pitchCard(scoreCard.getRight() + hudGap,
                                           hudY, pitchW, hudH);
    const juce::Rectangle<float> statsCard(pitchCard.getRight() + hudGap,
                                           hudY, statsW, hudH);
    const juce::Rectangle<float> targetCard(width - 24.0f - targetW, hudY,
                                            targetW, hudH);

    GameUi::drawGlassCard(g, comboCard, 16.0f);
    GameUi::drawGlassCard(g, accuracyCard, 16.0f);
    GameUi::drawGlassCard(g, scoreCard, 16.0f);
    GameUi::drawGlassCard(g, pitchCard, 16.0f);
    GameUi::drawGlassCard(g, statsCard, 16.0f);
    GameUi::drawGlassCard(g, targetCard, 16.0f);

    GameUi::drawEyebrow(g, "COMBO",
                        juce::Rectangle<float>(comboCard.getX() + 14.0f,
                                               comboCard.getY() + 10.0f,
                                               comboW - 28.0f, 15.0f));

    GameUi::drawEyebrow(g, "ACCURACY",
                        juce::Rectangle<float>(accuracyCard.getX() + 14.0f,
                                               accuracyCard.getY() + 10.0f,
                                               accuracyW - 28.0f, 15.0f),
                        UiColors::muted);

    GameUi::drawEyebrow(g, "SCORE",
                        juce::Rectangle<float>(scoreCard.getX() + 14.0f,
                                               scoreCard.getY() + 10.0f,
                                               scoreW - 28.0f, 15.0f));

    GameUi::drawEyebrow(g, "PITCH",
                        juce::Rectangle<float>(pitchCard.getX() + 16.0f,
                                               pitchCard.getY() + 10.0f,
                                               pitchW - 32.0f, 15.0f),
                        UiColors::green);

    g.setColour(UiColors::accent);
    g.setFont(GameUi::eyebrow(9.0f));
    g.drawText("NEXT NOTE",
               juce::Rectangle<float>(targetCard.getX() + 18.0f,
                                      targetCard.getY() + 10.0f,
                                      targetW - 30.0f, 15.0f),
               juce::Justification::centredLeft, false);

    // Accuracy is painted here; the score itself is gameSessionLabel.
    {
        const int hits = gameplayStats.getHitCount();
        const int misses = gameplayStats.getMissCount();
        const int total = hits + misses;
        const double accuracy = total > 0
            ? 100.0 * static_cast<double>(hits) / static_cast<double>(total)
            : 0.0;

        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(21.0f));
        g.drawText(juce::String(juce::roundToInt(accuracy)) + "%",
                   juce::Rectangle<float>(accuracyCard.getX() + 14.0f,
                                          accuracyCard.getY() + 26.0f,
                                          100.0f, 28.0f),
                   juce::Justification::centredLeft, false);

        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(8.5f, true));
        g.drawText(juce::String(hits) + "/" + juce::String(total) + " NOTES",
                   juce::Rectangle<float>(accuracyCard.getX() + 74.0f,
                                          accuracyCard.getY() + 34.0f,
                                          accuracyW - 86.0f, 16.0f),
                   juce::Justification::centredRight, false);
    }

    // LIVE PERFORMANCE: the four judgement counters, same row as PITCH.
    if (practiceActive.load(std::memory_order_relaxed))
    {
        GameUi::drawPill(g,
                         juce::Rectangle<float>(statsCard.getX() + 14.0f,
                                                statsCard.getY() + 20.0f,
                                                200.0f, 24.0f),
                         "STUDY - NO SCORE",
                         UiColors::accent,
                         false);
    }
    else
    {
        const auto performanceHeader =
            currentSessionIsPreview ? "PREVIEW - NOT RANKED"
            : currentSessionIsPractice ? "PRACTICE - NOT RANKED"
                                       : "LIVE PERFORMANCE";

        GameUi::drawEyebrow(g,
                            performanceHeader,
                            juce::Rectangle<float>(statsCard.getX() + 14.0f,
                                                   statsCard.getY() + 6.0f,
                                                   statsCard.getWidth() - 28.0f,
                                                   12.0f),
                            (currentSessionIsPreview || currentSessionIsPractice)
                                ? UiColors::red
                                : UiColors::accent);

        const std::array<juce::String, 4> values
        {
            juce::String(gameplayStats.getMissCount()),
            juce::String(gameplayStats.getPerfectCount()),
            juce::String(gameplayStats.getGreatCount()),
            juce::String(gameplayStats.getGoodCount())
        };
        const std::array<juce::String, 4> captions
        {
            "MISS", "PERFECT", "GREAT", "GOOD"
        };

        const float cellWidth = statsCard.getWidth() / 4.0f;

        for (int i = 0; i < 4; ++i)
        {
            if (i > 0)
            {
                g.setColour(UiColors::border.withAlpha(0.55f));
                g.fillRect(statsCard.getX() + cellWidth * static_cast<float>(i),
                           statsCard.getY() + 24.0f, 1.0f, 28.0f);
            }

            const juce::Rectangle<float> cell(statsCard.getX()
                                                  + cellWidth * static_cast<float>(i),
                                              statsCard.getY(), cellWidth,
                                              statsCard.getHeight());

            g.setColour(UiColors::accent);
            g.setFont(GameUi::heading(18.0f));
            g.drawText(values[static_cast<size_t>(i)],
                       cell.withY(cell.getY() + 20.0f).withHeight(24.0f),
                       juce::Justification::centred, false);

            g.setFont(GameUi::eyebrow(7.0f));
            g.drawText(captions[static_cast<size_t>(i)],
                       cell.withY(cell.getY() + 46.0f).withHeight(10.0f),
                       juce::Justification::centred, false);
        }
    }

    // The tablature card is transparent so the stage artwork shows through;
    // no shadow plate is painted behind it either. The per-hit judgement text
    // (hitLabel) is positioned just above the tabs by resized().

    // Guitar-effect pedals are intentionally not shown yet; the track mixer
    // uses the full width until the effects UI is implemented.
    const juce::Rectangle<float> mixer(24.0f, studioY,
                                       width - 48.0f, studioH);

    GameUi::drawGlassCard(g, mixer, 18.0f);
    GameUi::drawEyebrow(g, "MIXER",
                        juce::Rectangle<float>(mixer.getX() + 22.0f,
                                               mixer.getY() + 8.0f,
                                               180.0f, 14.0f));
    GameUi::drawPill(g,
                     juce::Rectangle<float>(mixer.getRight() - 122.0f,
                                            mixer.getY() + 8.0f,
                                            100.0f, 20.0f),
                     "7 CHANNELS",
                     UiColors::green,
                     false);

    paintDuelOverlay(g);
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

            if (currentSessionIsPreview || currentSessionIsPractice)
            {
                // The player must know before pressing START that a draft
                // preview or a Practice run is not ranked. Sits between the
                // hint text and the START button (card bottom - 74).
                GameUi::drawPill(g,
                                 juce::Rectangle<float>(countCard.getX() + 40.0f,
                                                        countCard.getY() + 140.0f,
                                                        cardWidth - 80.0f, 22.0f),
                                 currentSessionIsPractice
                                     ? "PRACTICE - NOT RANKED"
                                     : "PREVIEW - NOT RANKED",
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

    const int shellSideW = sectionSidebarWidth();
    if (sectionSidebar != nullptr)
        sectionSidebar->setBounds (0, 0, shellSideW, height);

    // Pixel-art duel backdrop. It only appears while a duel is actually being
    // played (or its ready/result screen is shown); normal games keep the
    // previous full-width tablature.
    const bool duelStageActive = isDuelStageActive();
    duelStage.setActive (duelStageActive);

    // Duel field: the stage (background + guitarists) sits above the full-width
    // tablature. When no duel is active the tabs use the whole play area.
    auto duelFieldLayout = [width, duelStageActive] (int top, int available)
    {
        struct Field
        {
            juce::Rectangle<int> stage;
            juce::Rectangle<int> tabs;
        };

        const int gap = 10;
        int stageH = 0;
        int tabTop = top;
        int tabH = available;

        if (duelStageActive)
        {
            const int minTabs = juce::jmax (120, available * 50 / 100);
            stageH = juce::jlimit (90, 320, available * 45 / 100);

            if (available - stageH - gap < minTabs)
                stageH = juce::jmax (70, available - gap - minTabs);

            tabTop = top + stageH + gap;
            tabH = juce::jmax (80, available - stageH - gap);
        }

        return Field { { 24, top, width - 48, stageH },
                       { 24, tabTop, width - 48, tabH } };
    };

    const bool authScreen = showAuthScreen;
    const bool songSelect = gameState == GameState::SongSelect && !authScreen;
    const bool tunerScreen = gameState == GameState::Tuner;
    const bool finishedScreen = gameState == GameState::Finished;
    const bool preparingScreen = gameState == GameState::Preparing;
    const bool showGame = gameState == GameState::Playing
        || gameState == GameState::Paused
        || preparingScreen;
    const bool readyScreen = gameState == GameState::Ready;

    // Campaign: when the next opponent's stems/chart still have to be
    // downloaded, the result card is covered by the loading overlay, so its
    // buttons must not stay clickable underneath it.
    const bool campaignContentLoading =
        finishedScreen && campaignFightActive
        && songDownloadProgressState != nullptr;

    //==========================================================================
    // Visibility
    //==========================================================================

    const bool hub = songSelect;
    const bool homeSection = hub && currentSection == AppSection::Home;
    const bool songsSection = hub && currentSection == AppSection::Songs;
    const bool onlineSection = hub && currentSection == AppSection::OnlineDuel;
    const bool profileSection = hub && currentSection == AppSection::Profile;
    const bool campaignSection = hub && currentSection == AppSection::Campaign;
    const bool vstSection = hub && currentSection == AppSection::Vst;
    const bool settingsSection = hub && currentSection == AppSection::Settings;
    const bool songChrome = songsSection;
    const bool shellVisible = (hub || (tunerScreen && tunerStandalone)) && !authScreen;

    //==========================================================================
    // Hub chrome / section panels
    //==========================================================================

    if (sectionSidebar != nullptr)
    {
        sectionSidebar->setVisible (shellVisible);
        sectionSidebar->setActiveSection (currentSection);
        sectionSidebar->setUser (currentUsername, progressionLevel);
    }

    if (campaignPanel != nullptr)
        campaignPanel->setVisible (campaignSection);

    if (vstSectionPanel != nullptr)
        vstSectionPanel->setVisible (vstSection);

    if (profileViewport != nullptr)
        profileViewport->setVisible (profileSection);

    if (settingsViewport != nullptr)
        settingsViewport->setVisible (settingsSection);

    if (friendsViewport != nullptr)
        friendsViewport->setVisible (hub && currentSection == AppSection::Friends);

    if (messagesViewport != nullptr)
        messagesViewport->setVisible (hub && currentSection == AppSection::Messages);

    // The header only keeps EXIT. Profile/VST/Settings controls live in their
    // sections now.
    vstButton.setVisible (false);
    accountSettingsButton.setVisible (false);
    // DONATION and FEEDBACK follow EXIT on every hub page and on the tuner.
    supportButton.setVisible ((hub || tunerScreen) && !authScreen);
    feedbackButton.setVisible ((hub || tunerScreen) && !authScreen);
    adminPreviewButton.setVisible (profileSection && onlineIsAdmin
                                   && onlineAccessToken.isNotEmpty());
    currentUserLabel.setVisible (false);
    quitButton.setVisible ((hub || tunerScreen) && !authScreen);
    fullScreenButton.setVisible (settingsSection);

    retryConnectionButton.setVisible(sessionOfflineRetryAvailable && authScreen);
    loginScreenTitleLabel.setVisible(authScreen);
    logoutButton.setVisible(false);
    accountStatusLabel.setVisible(authScreen || songChrome);
    accountLoginButton.setVisible(authScreen);
    accountRegisterButton.setVisible(authScreen);
    verifyEmailButton.setVisible(authScreen);
    forgotPasswordButton.setVisible(authScreen);
    gameSongTitleLabel.setVisible(showGame || readyScreen);

    // Song catalogue and song controls: Home, Songs and Online Duel.
    songTitleLabel.setVisible(false);
    previewButton.setVisible(songChrome);
    practiceModeButton.setVisible(songsSection);
    duelModeButton.setVisible(false);
    songPridumayButton.setVisible(songChrome);
    onlineCatalogButton.setVisible(songChrome);
    onlineCatalogStatusLabel.setVisible(songChrome);
    songPridumayBestLabel.setVisible(songChrome);
    songListBox.setVisible(songChrome);

    // Daily/generator cards remain hidden as before; the catalogue itself is
    // the Songs section.
    dailyChallengeButton.setVisible(false);
    dailyChallengeStatusLabel.setVisible(false);
    songMistButton.setVisible(false);
    songChordsButton.setVisible(false);
    aiSoloButton.setVisible(false);
    oldGeneratorArchiveButton.setVisible(false);
    melodyStyleCombo.setVisible(false);
    generateMelodyButton.setVisible(false);
    newGenerationButton.setVisible(false);
    songMistBestLabel.setVisible(false);
    songChordsBestLabel.setVisible(false);
    dailyChallengeBestLabel.setVisible(false);

    // Ranking is a Home-only widget, never part of a sub-section.
    leaderboardTitleLabel.setVisible(homeSection);
    leaderboardTable.setVisible(homeSection);
    leaderboardLabel.setVisible(homeSection);
    refreshLeaderboardButton.setVisible(homeSection);
    progressionSummaryLabel.setVisible(false);

    if (duelLobby != nullptr)
    {
        duelLobby->setVisible (onlineSection);
        duelLobby->setSignedIn (onlineAccessToken.isNotEmpty());
    }

    tunerTitleLabel.setVisible(tunerScreen);
    tunerNoteLabel.setVisible(tunerScreen);
    tunerFrequencyLabel.setVisible(tunerScreen);
    tunerStringLabel.setVisible(tunerScreen);
    tunerCentsLabel.setVisible(tunerScreen);
    tunerStatusLabel.setVisible(tunerScreen);
    tunerStartButton.setVisible(tunerScreen && ! tunerStandalone);
    tunerSkipButton.setVisible(tunerScreen && ! tunerStandalone);
    tunerReturnButton.setVisible(tunerScreen
                                 && (tunerReturnToLobby
                                     || (onlineDuelRoomDialog != nullptr
                                         && ! onlineDuelRoomDialog->isVisible())));
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
        inputMonitor->setVisible(tunerScreen);
        inputMonitor->setCompact(false);
    }

    pitchLabel.setVisible(showGame);
    comboLabel.setVisible(showGame);
    beatLabel.setVisible(false);
    barBeatLabel.setVisible(false);
    bpmLabel.setVisible(false);
    gameSessionLabel.setVisible(showGame);
    const bool duelHudVisible = duelActive.load (std::memory_order_relaxed)
                                || duelOnlineActive.load (std::memory_order_relaxed)
                                || duelOnlinePhase == DuelOnlinePhase::Playing
                                || duelOnlinePhase == DuelOnlinePhase::Finishing;
    hitLabel.setVisible (showGame && ! duelHudVisible);
    targetNoteLabel.setVisible(showGame);

    gameStartButton.setVisible(((showGame && !preparingScreen) || finishedScreen)
                               && ! campaignContentLoading);
    readyStartButton.setVisible(readyScreen);
    resumeOverlayButton.setVisible(gameState == GameState::Paused);
    pauseButton.setVisible(gameState == GameState::Playing
                           || gameState == GameState::Paused);
    menuButton.setVisible(((showGame && !preparingScreen)
                           || finishedScreen || readyScreen)
                          && ! campaignContentLoading);
    distortionButton.setVisible(false);
    liveGuitarButton.setVisible(showGame || readyScreen);
    liveGuitarVolumeSlider.setVisible(showGame || readyScreen);
    liveGuitarVolumeLabel.setVisible(false);
    midiBackingButton.setVisible(showGame || readyScreen);
    midiBackingVolumeSlider.setVisible(showGame || readyScreen);
    midiBackingVolumeLabel.setVisible(false);
    midiRhythmGuitarButton.setVisible(showGame || readyScreen);
    midiRhythmGuitarVolumeSlider.setVisible(showGame || readyScreen);
    midiRhythmGuitarVolumeLabel.setVisible(false);
    midiBassButton.setVisible(showGame || readyScreen);
    midiBassVolumeSlider.setVisible(showGame || readyScreen);
    midiBassVolumeLabel.setVisible(false);
    midiDrumsButton.setVisible(showGame || readyScreen);
    midiDrumsVolumeSlider.setVisible(showGame || readyScreen);
    midiDrumsVolumeLabel.setVisible(false);
    midiPianoButton.setVisible(showGame || readyScreen);
    midiPianoVolumeSlider.setVisible(showGame || readyScreen);
    midiPianoVolumeLabel.setVisible(false);
    metronomeMixerButton.setVisible(showGame || readyScreen);
    metronomeMixerVolumeSlider.setVisible(showGame || readyScreen);
    metronomeMixerVolumeLabel.setVisible(false);
    distortionDriveSlider.setVisible(false);
    distortionToneSlider.setVisible(false);
    distortionDriveLabel.setVisible(false);
    distortionToneLabel.setVisible(false);
    speed50Button.setVisible(songsSection);
    speed75Button.setVisible(songsSection);
    speed100Button.setVisible(songsSection);
    speed120Button.setVisible(songsSection);
    tempoLabel.setVisible(songsSection);
    // The bar only has song-list bounds; keep it hidden on every other screen
    // so a campaign download cannot paint it over the result card.
    songDownloadProgressBar.setVisible(songDownloadProgressState != nullptr
                                       && songsSection);
    audioSettingsButton.setVisible(false);
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

        const int sideW = sectionSidebarWidth();
        const int contentX = 48 + sideW;
        const int contentW = juce::jmax(420, width - 96 - sideW);

        // The hub header keeps EXIT; DONATION and FEEDBACK sit next to it on
        // every section.
        quitButton.setBounds(width - 44 - 100, 42, 100, 32);
        supportButton.setBounds (width - 44 - 100 - 8 - 110, 42, 110, 32);
        feedbackButton.setBounds (width - 44 - 100 - 8 - 110 - 8 - 110, 42,
                                  110, 32);
        songPridumayButton.setButtonText(currentSection == AppSection::OnlineDuel
                                             ? "CREATE LOBBY"
                                             : "START SONG");

        const int panelY = 118;
        const int panelH = juce::jmax(180, height - panelY - 24);

        if (profileViewport != nullptr)
        {
            const int profileH = juce::jmax (140, panelH - 48);
            profileViewport->setBounds (contentX, panelY, contentW, profileH);

            if (profileContent != nullptr)
            {
                // The account window scrolls its own page: give it exactly the
                // visible area so the section viewport never adds a second
                // scrollbar next to it.
                profileContent->setSize (
                    juce::jmax (900, profileViewport->getMaximumVisibleWidth()),
                    juce::jmax (560, profileViewport->getMaximumVisibleHeight()));
            }
        }

        if (settingsViewport != nullptr)
        {
            settingsViewport->setBounds(contentX, panelY, contentW, panelH);
            if (settingsContent != nullptr)
                settingsContent->setSize(contentW, panelH);

            // Left of FEEDBACK, so EXIT / DONATION / FEEDBACK stay in place.
            fullScreenButton.setBounds (width - 44 - 100 - 8 - 110 - 8 - 110 - 8 - 140,
                                        42, 140, 32);
        }

        if (friendsViewport != nullptr)
        {
            friendsViewport->setBounds (contentX, panelY, contentW, panelH);

            if (friendsContent != nullptr)
                friendsContent->setSize (contentW, panelH);
        }

        if (messagesViewport != nullptr)
        {
            messagesViewport->setBounds (contentX, panelY, contentW, panelH);

            if (messagesContent != nullptr)
                messagesContent->setSize (contentW, panelH);
        }

        if (vstSectionPanel != nullptr)
            vstSectionPanel->setBounds(contentX, panelY, contentW, panelH);
        if (campaignPanel != nullptr)
            campaignPanel->setBounds(contentX, panelY, contentW, panelH);

        // Public profile overlay covers the same area as a section page. It is
        // wrapped in a viewport: the profile has a fixed minimum height, so a
        // short 16:9 window scrolls instead of clipping the action buttons.
        if (publicProfileViewport != nullptr)
        {
            publicProfileViewport->setBounds(contentX, panelY, contentW, panelH);

            if (publicProfilePanel != nullptr)
            {
                const int profileWidth =
                    juce::jmax(320, publicProfileViewport->getMaximumVisibleWidth());
                publicProfilePanel->setSize(profileWidth,
                                            juce::jmax(panelH, 660));
            }
        }

        if (profileSection)
        {
            const int buttonH = 32;
            const int adminW = adminPreviewButton.isVisible() ? 100 : 0;
            const int footerY = panelY + panelH - 36;

            if (adminW > 0)
                adminPreviewButton.setBounds (contentX + 24, footerY, adminW, buttonH);
        }

        if (homeSection)
        {
            const int contentY = 234;
            const int contentH = juce::jmax(320, height - 68 - 20 - contentY);
            leaderboardTitleLabel.setBounds(contentX + 24, contentY + 38,
                                            contentW - 48, 30);
            refreshLeaderboardButton.setBounds(contentX + contentW - 142,
                                               contentY + 62, 118, 32);
            leaderboardLabel.setBounds(contentX + 24, contentY + 68,
                                       contentW - 180, 20);
            leaderboardTable.setBounds(contentX + 24, contentY + 272,
                                       contentW - 48,
                                       juce::jmax(100, contentH - 296));
        }
        else if (songChrome)
        {
            const int contentY = homeSection ? 250 : 124;
            const int availableH = juce::jmax(320, height - 68 - 20 - contentY);
            const int contentH = availableH;
            const int footerY = contentY + contentH + 20;
            const int gap = 20;
            const bool fullWidth = songsSection;
            const int leftW = fullWidth ? contentW : contentW * 56 / 100;
            const int rightX = contentX + leftW + gap;
            const int rightW = juce::jmax(0, contentW - leftW - gap);

            songTitleLabel.setBounds(contentX, 140, contentW, 44);

            songPridumayBestLabel.setBounds(contentX + 24 + 138, contentY + 108,
                                            juce::jmax(80, leftW - 360), 20);

            const int buttonY = contentY + contentH - 64;
            const int available = juce::jmax(120, leftW - 48);
            const int startW = juce::jmin(220, (available - 12) / 2);
            const int onlineW = juce::jmin(240, available - startW - 12);
            songPridumayButton.setBounds(contentX + 24, buttonY, startW, 42);
            onlineCatalogButton.setBounds(contentX + 24 + startW + 12, buttonY,
                                          onlineW, 42);

            // Tempo buttons on the Songs screen: the same controls the pause
            // panel uses, so the next start runs at the chosen speed.
            {
                const int tempoStart = contentX + 24 + startW + 12 + onlineW + 28;
                tempoLabel.setBounds (tempoStart, buttonY - 22, 200, 16);

                int tempoX = tempoStart;
                constexpr int tempoW = 66;

                for (auto* button : { &speed50Button, &speed75Button,
                                      &speed100Button, &speed120Button })
                {
                    button->setBounds (tempoX, buttonY, tempoW, 42);
                    tempoX += tempoW + 8;
                }
            }
            onlineCatalogStatusLabel.setBounds(contentX + 24, buttonY - 34,
                                               leftW - 48, 20);
            songDownloadProgressBar.setBounds(contentX + 24, buttonY - 12,
                                               leftW - 48, 8);

            dailyChallengeButton.setBounds(contentX + 24, contentY + 186,
                                           leftW - 48, 44);
            dailyChallengeStatusLabel.setBounds(contentX + 24, contentY + 232,
                                                leftW - 48, 14);

            const int listTop = contentY + 190;
            const int listBottom = buttonY - 38;
            songListBox.setBounds(contentX + 18, listTop, leftW - 36,
                                  juce::jmax(56, listBottom - listTop));

            if (homeSection)
            {
                leaderboardTitleLabel.setBounds(rightX + 24, contentY + 38,
                                                rightW - 48, 30);
                refreshLeaderboardButton.setBounds(rightX + rightW - 142,
                                                   contentY + 68, 118, 32);
                leaderboardLabel.setBounds(rightX + 24, contentY + 106,
                                           rightW - 48, 20);
                leaderboardTable.setBounds(rightX + 24, contentY + 130,
                                           rightW - 48,
                                           juce::jmax(100, contentH - 154));
            }

            progressionSummaryLabel.setBounds(contentX, 78, contentW, 22);
            accountStatusLabel.setBounds(contentX + 8, footerY + 7,
                                         juce::jmax(120, contentW - 16), 30);

            if (inputMonitor != nullptr)
            {
                const int monitorW = inputMonitorCardWidth(width);
                inputMonitor->setBounds(width - 48 - monitorW, 108,
                                        monitorW, 124);
            }

            // The song controls sit on the right edge of the full-width card.
            // (The input monitor is only shown in the tuner, so its reserved
            // width must not push these buttons into the middle of the page.)
            const int modeRight = contentX + leftW - 24;

            previewButton.setBounds(modeRight - 170, contentY + 56, 170, 34);
            practiceModeButton.setBounds(modeRight - 170, contentY + 96, 170, 34);
            duelModeButton.setBounds(modeRight - 170, contentY + 136, 170, 34);
        }
        else if (onlineSection && duelLobby != nullptr)
        {
            // Online duel: the lobby is the whole page. Its own card carries
            // the CREATE LOBBY and REFRESH buttons; the song catalogue is not
            // part of this section.
            const int contentY = 124;
            const int contentH = juce::jmax(320, height - 68 - 20 - contentY);
            duelLobby->setBounds(contentX, contentY, contentW, contentH);
        }
        else
        {
            accountStatusLabel.setBounds(contentX, height - 40, contentW, 24);
        }
    }

    //==========================================================================
    // Tuner
    //==========================================================================

    if (tunerScreen)
    {
        const bool hubTuner = tunerStandalone;
        const auto geo = computeTunerGeometry();
        const juce::Rectangle<float> stringsPanel = geo.stringsPanel;
        const juce::Rectangle<float> inputPanel = geo.inputPanel;

        quitButton.setBounds(width - 44 - 100, 42, 100, 32);
        supportButton.setBounds (width - 44 - 100 - 8 - 110, 42, 110, 32);
        feedbackButton.setBounds (width - 44 - 100 - 8 - 110 - 8 - 110, 42,
                                  110, 32);

        // The hub tuner navigates through the sidebar, so it has no BACK
        // button; the in-game tuner keeps the previous full-window layout.
        if (! hubTuner)
        {
            menuButton.setButtonText("BACK");
            menuButton.setBounds(24, 24, 100, 36);
        }

        // Tuning selectors: top-right of the STRINGS block, so the block title
        // keeps the left corner and the controls stay a normal size instead of
        // being stretched across the full width.
        const int presetWidth = 240;
        const int stringsWidth = 160;
        const int fretsWidth = 150;
        const int selectorGap = 8;
        const int selectorTotal = presetWidth + stringsWidth + fretsWidth
                                  + selectorGap * 2;
        const int selectorX = juce::roundToInt (stringsPanel.getRight() - 24.0f)
                              - selectorTotal;
        const int selectorY = juce::roundToInt (stringsPanel.getY()) + 14;

        tuningPresetCombo.setBounds(selectorX, selectorY, presetWidth, 28);
        stringCountCombo.setBounds(selectorX + presetWidth + selectorGap,
                                   selectorY, stringsWidth, 28);
        fretCountCombo.setBounds(selectorX + presetWidth + selectorGap
                                     + stringsWidth + selectorGap,
                                 selectorY, fretsWidth, 28);

        for (int slot = 0; slot < GuitarTuning::maxStrings; ++slot)
        {
            auto& combo = stringTuningCombos[static_cast<size_t>(slot)];

            if (slot >= geo.stringSlots)
            {
                combo.setVisible(false);
                continue;
            }

            const int column = slot % geo.columns;
            const int row = slot / geo.columns;
            const float x = stringsPanel.getX() + 24.0f
                + static_cast<float>(column) * (geo.tileW + geo.tileGap);
            const float y = geo.tileTop
                + static_cast<float>(row) * (geo.tileH + geo.tileGap);

            combo.setBounds(
                juce::Rectangle<float>(x + 8.0f, y + 8.0f,
                                       geo.tileW - 16.0f, 34.0f).toNearestInt());
        }

        // The input monitor fills the rest of the STRINGS block, full width.
        if (inputMonitor != nullptr)
        {
            const float monitorTop = geo.tileTop
                + static_cast<float>(geo.rows) * geo.tileH
                + static_cast<float>(geo.rows - 1) * geo.tileGap + 14.0f;
            const int monitorH = juce::jmax (
                56, juce::roundToInt (stringsPanel.getBottom() - 16.0f - monitorTop));

            inputMonitor->setBounds (juce::roundToInt (stringsPanel.getX() + 24.0f),
                                     juce::roundToInt (monitorTop),
                                     juce::roundToInt (stringsPanel.getWidth() - 48.0f),
                                     monitorH);
        }

        // LIVE INPUT: note + frequency on the left, the dial in the middle,
        // detected string/offset in the right column, status pill across the
        // bottom of the block.
        const int noteW = juce::jlimit (160, 300,
                                        juce::roundToInt (inputPanel.getWidth() * 0.22f));

        tunerTitleLabel.setBounds (juce::roundToInt (inputPanel.getX() + 24.0f),
                                   juce::roundToInt (inputPanel.getY() + 18.0f),
                                   300, 18);
        tunerNoteLabel.setBounds (juce::roundToInt (inputPanel.getX() + 24.0f),
                                  juce::roundToInt (inputPanel.getY() + 44.0f),
                                  noteW,
                                  juce::jmax (56, juce::roundToInt (inputPanel.getHeight()) - 140));
        tunerFrequencyLabel.setBounds (juce::roundToInt (inputPanel.getX() + 24.0f),
                                       juce::roundToInt (inputPanel.getBottom() - 92.0f),
                                       noteW, 26);
        tunerStringLabel.setBounds (juce::roundToInt (geo.rightColX),
                                    juce::roundToInt (inputPanel.getY() + 40.0f),
                                    juce::roundToInt (geo.rightColW), 24);
        tunerCentsLabel.setBounds (juce::roundToInt (geo.rightColX),
                                   juce::roundToInt (inputPanel.getY() + 78.0f),
                                   juce::roundToInt (geo.rightColW), 24);
        tunerStringLabel.setJustificationType(juce::Justification::centredRight);
        tunerCentsLabel.setJustificationType(juce::Justification::centredRight);
        tunerStatusLabel.setBounds (juce::roundToInt (inputPanel.getX() + 24.0f),
                                    juce::roundToInt (inputPanel.getBottom() - 58.0f),
                                    juce::roundToInt (inputPanel.getWidth() - 48.0f),
                                    38);
        tunerStatusLabel.setJustificationType(juce::Justification::centred);

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
        const int buttonY = juce::roundToInt (inputPanel.getBottom()) + 20;
        tunerSkipButton.setBounds(buttonsX, buttonY, buttonWidth, 46);
        tunerReturnButton.setBounds(width - 230, 118, 190, 30);
        tunerStartButton.setBounds(buttonsX + buttonWidth + buttonGap,
                                   buttonY, buttonWidth, 46);
    }

    //==========================================================================
    // Results
    //==========================================================================

    // Ready screen: the START button lives inside the same count-in card.
    if (readyScreen)
    {
        gameSongTitleLabel.setBounds(210, 40,
                                     juce::jmax(200, width - 640), 26);
        // Keep the top bar to one row: BACK sits with the other actions on the
        // right so it does not cover the brand on the left.
        menuButton.setBounds(width - 44 - 100, 22, 100, 34);

        const int cardW = 280;
        const int cardH = 240;
        const int cardX = (width - cardW) / 2;
        const int cardY = (height - cardH) / 2;
        readyStartButton.setBounds(cardX + 40, cardY + cardH - 74, 200, 44);
        // The tablature is visible before the start so the player can see the
        // first notes while getting ready.
        const int hudY = 88;
        const int hudH = 64;
        const int studioH = 86;
        const int studioY = height - 16 - studioH;
        const int duelStripH = isDuelStageActive() ? 54 : 0;
        const int tabY = hudY + hudH + 8 + duelStripH;
        const int available = juce::jmax(150, studioY - 12 - tabY);
        const auto field = duelFieldLayout (tabY, available);
        tablatureComponent.setBounds (field.tabs);

        // Mixer channels stay usable before the start, so the card is not blank.
        {
            const int mixerX = 24;
            const int mixerW = width - 48;
            const int cellGap = 8;
            const int cellW = (mixerW - 28 - cellGap * 6) / 7;
            const int channelTop = studioY + 30;

            auto placeChannel = [&](juce::TextButton& button, juce::Slider& slider,
                                    int column)
            {
                const int x = mixerX + 14 + column * (cellW + cellGap);
                button.setBounds(x, channelTop, cellW, 20);
                slider.setBounds(x, channelTop + 24, cellW, 16);
            };

            placeChannel(liveGuitarButton, liveGuitarVolumeSlider, 0);
            placeChannel(midiBackingButton, midiBackingVolumeSlider, 1);
            placeChannel(midiDrumsButton, midiDrumsVolumeSlider, 2);
            placeChannel(midiBassButton, midiBassVolumeSlider, 3);
            placeChannel(midiRhythmGuitarButton, midiRhythmGuitarVolumeSlider, 4);
            placeChannel(midiPianoButton, midiPianoVolumeSlider, 5);
            placeChannel(metronomeMixerButton, metronomeMixerVolumeSlider, 6);
        }

        if (isDuelStageActive())
        {
            duelStage.setBounds (getLocalBounds());
            duelStage.setGameplayBaseline (field.stage.getY(), field.stage.getBottom());
            duelStage.setPictureBottom (field.tabs.getY() + field.tabs.getHeight() / 2);
        }
        else
        {
            duelStage.setBounds (field.stage);
            duelStage.clearGameplayBaseline();
        }

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
        // The result card is painted by the overlay above the stage; give the
        // backdrop the whole window so a duel that ended before playback still
        // gets the stage behind its card.
        if (duelStageActive)
            duelStage.setBounds (getLocalBounds());

        const int buttonWidth = juce::jmin(180, (width - 100) / 2);
        const int gap = 20;
        const int totalWidth = buttonWidth * 2 + gap;
        const int left = (width - totalWidth) / 2;
        const int cardBottom = 82 + juce::jmax(520, height - 170);
        const int y = height >= 820 ? cardBottom - 56 : height - 76;
        gameStartButton.setBounds(left, y, buttonWidth, 44);
        menuButton.setBounds(left + buttonWidth + gap, y, buttonWidth, 44);
        if (campaignFightActive)
        {
            gameStartButton.setButtonText(
                campaignStageWon
                    ? (campaignStage >= campaignStageCount() ? "BATTLE"
                                                             : "NEXT OPPONENT")
                    : "RETRY");
            menuButton.setButtonText("BATTLE");
        }
        else if (duelOnlinePhase == DuelOnlinePhase::Finished
                 && duelOnlineMatchId.isNotEmpty())
        {
            // A finished online match offers exactly two actions: back to the
            // same room (chat and song preserved) or out of the room onto the
            // Online section. There is no rematch from the result screen.
            gameStartButton.setButtonText("GO TO LOBBY");
            menuButton.setButtonText("LEAVE LOBBY");
        }
        else
        {
            gameStartButton.setButtonText("PLAY AGAIN");
            menuButton.setButtonText("MAIN MENU");
        }
    }

    //==========================================================================
    // Gameplay
    //==========================================================================

    if (showGame)
    {
        menuButton.setButtonText(gameState == GameState::Paused ? "BACK" : "MENU");
        audioSettingsButton.setButtonText("AUDIO");
        const int hudY = 88;
        const int hudH = 64;
        const int studioH = 86;
        const int studioY = height - 16 - studioH;
        const int duelStripH = isDuelStageActive() ? 54 : 0;

        // Solo practice keeps the per-hit judgement above the tablature: the
        // label sits in its own band between the HUD and the tabs instead of
        // covering the notes. Duels hide that label and show the exchange
        // strip instead, so their layout is unchanged.
        const bool judgementAboveTabs = ! duelStageActive;
        constexpr int judgementBandH = 64;
        constexpr int judgementBandGap = 8;
        const int tabY = hudY + hudH + 8 + duelStripH
                         + (judgementAboveTabs ? judgementBandH + judgementBandGap : 0);
        const int tabH = juce::jmax(150, studioY - 12 - tabY);

        const int audioW = 112;
        const int menuW = 100;
        const int pauseW = 82;
        const int startW = 76;
        const int actionGap = 8;
        const int actionsW = audioW + menuW + pauseW + startW + actionGap * 3;
        const int actionY = 22;
        const int actionH = 34;
        int actionX = width - 24 - actionsW;
        gameStartButton.setBounds(actionX, actionY, startW, actionH);
        actionX += startW + actionGap;
        pauseButton.setBounds(actionX, actionY, pauseW, actionH);
        actionX += pauseW + actionGap;
        menuButton.setBounds(actionX, actionY, menuW, actionH);
        actionX += menuW + actionGap;
        audioSettingsButton.setBounds(actionX, actionY, audioW, actionH);

        gameSongTitleLabel.setBounds(210, 40, juce::jmax(200, width - 640), 26);

        beatLabel.setBounds(0, 0, 0, 0);
        barBeatLabel.setBounds(0, 0, 0, 0);
        bpmLabel.setBounds(0, 0, 0, 0);

        speed50Button.setBounds(0, 0, 0, 0);
        speed75Button.setBounds(0, 0, 0, 0);
        speed100Button.setBounds(0, 0, 0, 0);
        speed120Button.setBounds(0, 0, 0, 0);

        const int hudGap = 12;
        const int comboW = 120;
        const int accuracyW = 170;
        const int scoreW = 190;
        const int pitchW = 300;
        const int statsW = 520;
        const int targetW = 190;
        const int comboX = 24;
        const int accuracyX = comboX + comboW + hudGap;
        const int scoreX = accuracyX + accuracyW + hudGap;
        const int pitchX = scoreX + scoreW + hudGap;
        const int statsX = pitchX + pitchW + hudGap;
        const int targetX = width - 24 - targetW;

        comboLabel.setBounds(comboX + 10, hudY + 24, comboW - 20, 36);
        pitchLabel.setBounds(pitchX + 10, hudY + 24, pitchW - 20, 36);
        targetNoteLabel.setBounds(targetX + 10, hudY + 24, targetW - 20, 36);

        gameSessionLabel.setBounds(scoreX + 12, hudY + 24, scoreW - 24, 36);
        gameSessionLabel.setJustificationType(juce::Justification::centredLeft);

        const auto field = duelFieldLayout (tabY, tabH);
        tablatureComponent.setBounds (field.tabs);

        // The per-hit judgement text (PERFECT / GREAT / MISS) sits in the band
        // just above the tablature card in Practice; duels keep the label
        // hidden and use the exchange strip instead.
        const int judgementW = juce::jmin (760, width - 120);

        if (judgementAboveTabs)
        {
            hitLabel.setBounds ((width - judgementW) / 2,
                                tabY - judgementBandGap - judgementBandH,
                                judgementW, judgementBandH);
        }
        else
        {
            hitLabel.setBounds ((width - judgementW) / 2, height / 2 - 40,
                                judgementW, 80);
        }

        // The LIVE PERFORMANCE counters sit in the top row; remember that card
        // for targeted repaints.
        hudStatsBounds = { statsX, hudY, statsW, hudH };
        hudAccuracyBounds = { accuracyX, hudY, accuracyW, hudH };

        if (isDuelStageActive())
        {
            duelStage.setBounds (getLocalBounds());
            duelStage.setGameplayBaseline (field.stage.getY(), field.stage.getBottom());
            duelStage.setPictureBottom (field.tabs.getY() + field.tabs.getHeight() / 2);
        }
        else
        {
            duelStage.setBounds (field.stage);
            duelStage.clearGameplayBaseline();
        }

        const int mixerX = 24;
        const int mixerW = width - 48;
        const int channelX = mixerX + 14;
        const int channelTop = studioY + 30;
        const int cellGap = 8;
        const int cellW = (mixerW - 28 - cellGap * 6) / 7;

        auto layoutMixerChannel = [&](juce::TextButton& button, juce::Label& label,
                                      juce::Slider& slider, int column)
        {
            const int x = channelX + column * (cellW + cellGap);
            button.setBounds(x, channelTop, cellW, 20);
            slider.setBounds(x, channelTop + 24, cellW, 16);
            label.setBounds({});
            label.setVisible(false);
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
    }

    // Notifications: the corner stack floats above every section; the history
    // pop-up covers the window so an outside click can dismiss it.
    if (notificationToasts != nullptr)
    {
        const int toastW = juce::jmin (360, juce::jmax (220, width - 48));
        const int toastH = notificationToasts->getPreferredHeight();

        notificationToasts->setBounds (width - toastW - 24, height - toastH - 24,
                                       toastW, juce::jmax (1, toastH));
    }

    if (notificationPopup != nullptr)
        notificationPopup->setBounds (getLocalBounds());
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
                                 private juce::ChangeListener,
                                  private juce::Timer
{
public:
    void configureUiSounds (bool enabled, float volume, bool navigation,
                            bool actions, bool system,
                            std::function<void (bool, float, bool, bool, bool)> onChange)
    {
        uiSoundsToggle.setToggleState (enabled, juce::dontSendNotification);
        uiSoundsVolume.setValue (volume, juce::dontSendNotification);
        uiNavToggle.setToggleState (navigation, juce::dontSendNotification);
        uiActionToggle.setToggleState (actions, juce::dontSendNotification);
        uiSystemToggle.setToggleState (system, juce::dontSendNotification);
        uiSoundsChanged = std::move (onChange);
    }

    AudioSettingsPanel(juce::AudioDeviceManager& manager,
                       int initialInputChannel,
                       std::function<void(int)> onInputChannelChanged,
                       bool initialGuitarInputOnly,
                       std::function<void(bool)> onGuitarInputOnlyChanged,
                       float initialBackgroundVolume,
                       bool initialBackgroundEnabled,
                       std::function<void(float)> onBackgroundVolumeChanged,
                       std::function<void(bool)> onBackgroundEnabledChanged,
                       std::function<juce::String()> inputDiagnosticsProvider,
                       int initialMicChannel,
                       std::function<void(int)> onMicChanged,
                       std::function<float()> micLevelProvider,
                       int initialTimingOffsetMs,
                       std::function<void(int)> onTimingOffsetChanged,
                       std::function<juce::String()> latencyProvider)
        : selector(manager, 0, 32, 0, 2, false, false, true, false),
          deviceManager(manager),
          inputChannelChanged(onInputChannelChanged),
          guitarInputOnlyChanged(onGuitarInputOnlyChanged),
          backgroundVolumeChanged(onBackgroundVolumeChanged),
          backgroundEnabledChanged(onBackgroundEnabledChanged),
          diagnosticsProvider(std::move(inputDiagnosticsProvider)),
          microphoneChanged(std::move(onMicChanged)),
          selectedMicChannel(initialMicChannel),
          microphoneLevelProvider(std::move(micLevelProvider)),
          timingOffsetChanged(std::move(onTimingOffsetChanged)),
          latencyTextProvider(std::move(latencyProvider))
    {
        setSize(760, 680);

        // Live diagnostics for the guitar-only gate, the microphone level and
        // the reported device latency.
        if (diagnosticsProvider != nullptr || microphoneLevelProvider != nullptr
            || latencyTextProvider != nullptr)
            startTimerHz(10);
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

        // UI sound preferences: master switch, volume and the three
        // categories. They are separate from the game/guitar audio.
        uiSoundsLabel.setText("UI SOUNDS", juce::dontSendNotification);
        uiSoundsLabel.setJustificationType(juce::Justification::centredLeft);
        uiSoundsLabel.setColour(juce::Label::textColourId, UiColors::text);
        uiSoundsLabel.setFont(juce::FontOptions().withHeight(12.0f).withStyle("Bold"));
        addAndMakeVisible(uiSoundsLabel);

        uiSoundsToggle.setClickingTogglesState(true);
        uiSoundsToggle.setToggleState(true, juce::dontSendNotification);
        uiSoundsToggle.setTooltip("Interface sounds only; game audio and the guitar are unaffected.");
        addAndMakeVisible(uiSoundsToggle);

        uiSoundsVolume.setSliderStyle(juce::Slider::LinearHorizontal);
        uiSoundsVolume.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 22);
        uiSoundsVolume.setRange(0.0, 1.0, 0.01);
        uiSoundsVolume.setValue(0.8, juce::dontSendNotification);
        uiSoundsVolume.setColour(juce::Slider::trackColourId, UiColors::accent);
        addAndMakeVisible(uiSoundsVolume);

        for (auto* toggle : { &uiNavToggle, &uiActionToggle, &uiSystemToggle })
        {
            toggle->setClickingTogglesState(true);
            toggle->setToggleState(true, juce::dontSendNotification);
            addAndMakeVisible(*toggle);
        }

        uiNavToggle.setButtonText("NAVIGATION");
        uiActionToggle.setButtonText("ACTIONS");
        uiSystemToggle.setButtonText("NOTIFICATIONS");

        auto notifyUiSounds = [this]
        {
            if (uiSoundsChanged)
                uiSoundsChanged(uiSoundsToggle.getToggleState(),
                                static_cast<float>(uiSoundsVolume.getValue()),
                                uiNavToggle.getToggleState(),
                                uiActionToggle.getToggleState(),
                                uiSystemToggle.getToggleState());
        };

        uiSoundsToggle.onClick = notifyUiSounds;
        uiSoundsVolume.onValueChange = notifyUiSounds;
        uiNavToggle.onClick = notifyUiSounds;
        uiActionToggle.onClick = notifyUiSounds;
        uiSystemToggle.onClick = notifyUiSounds;

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

        inputChannelLabel.setText("GUITAR CHANNEL", juce::dontSendNotification);
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

        microphoneLabel.setText("MIC INPUT", juce::dontSendNotification);
        microphoneLabel.setJustificationType(juce::Justification::centredLeft);
        microphoneLabel.setColour(juce::Label::textColourId, UiColors::text);
        microphoneLabel.setFont(juce::FontOptions().withHeight(12.0f).withStyle("Bold"));
        microphoneBox.setColour(juce::ComboBox::textColourId, UiColors::text);
        microphoneBox.setColour(juce::ComboBox::backgroundColourId, UiColors::card);
        microphoneBox.setColour(juce::ComboBox::outlineColourId, UiColors::border);
        microphoneBox.setColour(juce::ComboBox::arrowColourId, UiColors::accent);
        microphoneBox.setTooltip("Voice chat uses this input channel of the "
                                 "audio device selected above. It may be the same "
                                 "channel as the guitar.");
        microphoneBox.onChange = [this]
        {
            const int id = microphoneBox.getSelectedId();
            selectedMicChannel = id <= 1 ? -1 : id - 2;

            if (microphoneChanged)
                microphoneChanged (selectedMicChannel);
        };
        addAndMakeVisible(microphoneLabel);
        addAndMakeVisible(microphoneBox);

        microphoneLevelLabel.setText("MIC -60.0 dB", juce::dontSendNotification);
        microphoneLevelLabel.setFont(juce::FontOptions().withHeight(10.5f));
        microphoneLevelLabel.setColour(juce::Label::textColourId, UiColors::muted);
        microphoneLevelLabel.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(microphoneLevelLabel);
        rebuildMicrophoneChoices();

        // Manual timing calibration. The device's reported output latency is
        // compensated automatically; this trims the remainder when a driver
        // hides a buffer (USB interfaces, Bluetooth) or over-reports it.
        timingOffsetLabel.setText("TIMING OFFSET", juce::dontSendNotification);
        timingOffsetLabel.setJustificationType(juce::Justification::centredLeft);
        timingOffsetLabel.setColour(juce::Label::textColourId, UiColors::text);
        timingOffsetLabel.setFont(juce::FontOptions().withHeight(12.0f).withStyle("Bold"));
        addAndMakeVisible(timingOffsetLabel);

        timingOffset.setSliderStyle(juce::Slider::LinearHorizontal);
        timingOffset.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
        timingOffset.setTextValueSuffix(" ms");
        timingOffset.setRange(-200.0, 200.0, 1.0);
        timingOffset.setNumDecimalPlacesToDisplay(0);
        timingOffset.setValue(initialTimingOffsetMs, juce::dontSendNotification);
        timingOffset.setTooltip(
            "Extra compensation added to the device's reported output latency. "
            "Raise it if notes still register late (hidden driver buffers, "
            "Bluetooth); lower it if they register early.");
        timingOffset.onValueChange = [this]
        {
            if (timingOffsetChanged)
                timingOffsetChanged(static_cast<int>(timingOffset.getValue()));
        };
        addAndMakeVisible(timingOffset);

        latencyStatusLabel.setColour(juce::Label::textColourId, UiColors::muted);
        latencyStatusLabel.setFont(juce::FontOptions().withHeight(11.0f));
        latencyStatusLabel.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(latencyStatusLabel);

        deviceManager.addChangeListener(this);
        rebuildInputChannelChoices(initialInputChannel);
    }

    ~AudioSettingsPanel() override
    {
        stopTimer();
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
        g.drawText("Choose your guitar input, voice microphone, input filter, buffer size, timing offset and menu music.",
                   48, 59, getWidth() - 80, 24, juce::Justification::centredLeft);

        g.setColour(UiColors::muted);
        g.setFont(GameUi::eyebrow(9.0f));
        g.drawText("v" + juce::String(ProjectInfo::versionString),
                   getWidth() - 180, 59, 142, 20,
                   juce::Justification::centredRight);

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

        auto* audioDevice = deviceManager.getCurrentAudioDevice();
        const bool noInputs = audioDevice == nullptr
                              || audioDevice->getInputChannelNames().isEmpty();

        g.setColour(noInputs ? UiColors::orange : UiColors::muted);
        g.setFont(GameUi::font(10.5f));
        g.drawText(noInputs
                       ? juce::String("No audio inputs on this device. Select a microphone/interface above.")
                       : juce::String("Auto follows the loudest active input. Enable extra inputs above for other channels."),
                   30, getHeight() - 34, getWidth() - 330, 20,
                   juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        backgroundLabel.setBounds(30, 112, 150, 24);
        backgroundToggle.setBounds(190, 110, 72, 28);
        backgroundVolume.setBounds(272, 110, 300, 28);
        backgroundStatusLabel.setBounds(582, 112, getWidth() - 612, 24);

        uiSoundsLabel.setBounds(30, 150, 150, 24);
        uiSoundsToggle.setBounds(190, 148, 72, 28);
        uiSoundsVolume.setBounds(272, 148, 300, 28);
        uiNavToggle.setBounds(30, 182, 118, 26);
        uiActionToggle.setBounds(156, 182, 100, 26);
        uiSystemToggle.setBounds(264, 182, 140, 26);

        selector.setBounds(30, 222, getWidth() - 60,
                           juce::jmax(80, getHeight() - 470));
        inputFilterLabel.setBounds(30, getHeight() - 218, 130, 24);
        inputFilterToggle.setBounds(168, getHeight() - 220, 132, 28);
        inputFilterStatusLabel.setBounds(310, getHeight() - 218,
                                         getWidth() - 340, 24);
        bufferLabel.setBounds(30, getHeight() - 178, 130, 24);
        bufferBox.setBounds(168, getHeight() - 180, 200, 28);
        bufferStatusLabel.setBounds(380, getHeight() - 180, getWidth() - 410, 28);
        timingOffsetLabel.setBounds(30, getHeight() - 144, 130, 24);
        timingOffset.setBounds(168, getHeight() - 146, 300, 28);
        latencyStatusLabel.setBounds(480, getHeight() - 144,
                                     getWidth() - 510, 24);
        inputChannelLabel.setBounds(30, getHeight() - 64, 120, 24);
        inputChannelBox.setBounds(156, getHeight() - 66, 260, 28);
        microphoneLabel.setBounds(430, getHeight() - 64, 90, 24);
        microphoneBox.setBounds(524, getHeight() - 66,
                                juce::jmax (120, getWidth() - 554), 28);
        microphoneLevelLabel.setBounds(getWidth() - 260, getHeight() - 34,
                                       230, 20);
    }

private:
    void timerCallback() override
    {
        if (diagnosticsProvider != nullptr)
        {
            const auto text = diagnosticsProvider();

            if (text != lastDiagnostics)
            {
                lastDiagnostics = text;
                inputFilterStatusLabel.setText(
                    text.isNotEmpty() ? text
                                      : juce::String("Rejects voice and other instruments."),
                    juce::dontSendNotification);

                if (text.containsIgnoreCase("PLUCK"))
                    inputFilterStatusLabel.setColour(juce::Label::textColourId,
                                                      UiColors::green);
                else if (text.containsIgnoreCase("REJECTED"))
                    inputFilterStatusLabel.setColour(juce::Label::textColourId,
                                                      UiColors::orange);
                else
                    inputFilterStatusLabel.setColour(juce::Label::textColourId,
                                                      UiColors::muted);
            }
        }

        if (microphoneLevelProvider != nullptr)
        {
            const float level = microphoneLevelProvider();
            const auto text = "MIC "
                + juce::String (juce::Decibels::gainToDecibels (level, -60.0f), 1)
                + " dB";

            if (text != microphoneLevelLabel.getText())
            {
                microphoneLevelLabel.setText (text, juce::dontSendNotification);
                microphoneLevelLabel.setColour (
                    juce::Label::textColourId,
                    level > 0.001f ? UiColors::green : UiColors::muted);
            }
        }

        if (latencyTextProvider != nullptr)
        {
            const auto text = latencyTextProvider();

            if (text != latencyStatusLabel.getText())
                latencyStatusLabel.setText (text, juce::dontSendNotification);
        }
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        rebuildInputChannelChoices(-1);
        rebuildMicrophoneChoices();
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

    void rebuildMicrophoneChoices()
    {
        microphoneBox.clear(juce::dontSendNotification);
        microphoneBox.addItem("Same as guitar input", 1);

        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            const auto names = device->getInputChannelNames();
            const auto active = device->getActiveInputChannels();
            const int maxChannels = juce::jmin (names.size(), 32);

            for (int hardware = 0; hardware < maxChannels; ++hardware)
            {
                auto name = names[hardware].trim();

                if (name.isEmpty())
                    name = "Input " + juce::String (hardware + 1);

                if (! active[hardware])
                    name << " (off)";

                microphoneBox.addItem (name, hardware + 2);
            }
        }

        int selected = selectedMicChannel < 0 ? 1 : selectedMicChannel + 2;

        if (microphoneBox.indexOfItemId (selected) < 0)
        {
            selected = 1;
            selectedMicChannel = -1;
        }

        microphoneBox.setSelectedId (selected, juce::dontSendNotification);
    }

    juce::AudioDeviceSelectorComponent selector;
    juce::AudioDeviceManager& deviceManager;
    std::function<void(int)> inputChannelChanged;
    std::function<void(bool)> guitarInputOnlyChanged;
    std::function<void(float)> backgroundVolumeChanged;
    std::function<void(bool)> backgroundEnabledChanged;
    std::function<juce::String()> diagnosticsProvider;
    juce::String lastDiagnostics;
    juce::Label inputChannelLabel;
    juce::ComboBox inputChannelBox;
    juce::Label microphoneLabel;
    juce::ComboBox microphoneBox;
    std::function<void (int)> microphoneChanged;
    int selectedMicChannel = -1;
    std::function<float()> microphoneLevelProvider;
    juce::Label microphoneLevelLabel;
    std::function<void (int)> timingOffsetChanged;
    std::function<juce::String()> latencyTextProvider;
    juce::Label timingOffsetLabel;
    juce::Slider timingOffset;
    juce::Label latencyStatusLabel;
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

    // UI sounds block.
    juce::Label uiSoundsLabel;
    juce::ToggleButton uiSoundsToggle;
    juce::Slider uiSoundsVolume;
    juce::ToggleButton uiNavToggle;
    juce::ToggleButton uiActionToggle;
    juce::ToggleButton uiSystemToggle;
    std::function<void(bool, float, bool, bool, bool)> uiSoundsChanged;

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
    // Scrollable page content: every block is full width and stacked, so the
    // window scrolls instead of squeezing the cards.
    class Content final : public juce::Component
    {
    public:
        explicit Content (AccountWindow& ownerToUse) : owner (ownerToUse) {}

        void paint (juce::Graphics& g) override { owner.paintContent (g); }
        void resized() override { owner.layoutContent(); }

    private:
        AccountWindow& owner;
    };

    struct Callbacks
    {
        std::function<void(const juce::String& username,
                           const juce::String& displayName)> onProfileUpdated;
        std::function<void()> onLogout;
        std::function<void()> onSessionExpired;
        // id, display name, server-relative spritesheet URL
        std::function<void(int, juce::String, juce::String)> onCharacterSelected;
    };

    AccountWindow(ApiClient& apiToUse, const juce::String& accessToken,
                  Callbacks callbacksToUse)
        : api(apiToUse),
          token(accessToken),
          callbacks(std::move(callbacksToUse)),
          ratingModel(*this),
          historyModel(*this)
    {
        setOpaque(true);

        contentHolder = std::make_unique<Content>(*this);
        viewport.setViewedComponent(contentHolder.get(), false);
        viewport.setScrollBarsShown(true, false);
        viewport.setScrollBarThickness(10);
        addAndMakeVisible(viewport);

        setSize(1180, 900);

        configureUi();
        resized();
        refreshAll();
    }

    // Re-reads GET /characters. MainComponent calls this after a tower unlocks a
    // character while the window is open; it is also part of REFRESH.
    void refreshCharacterCatalogue()
    {
        refreshCharacters();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(UiColors::background);
    }

    // Cards of the scrollable profile page: every block is full width and they
    // follow each other in the requested order.
    void paintContent(juce::Graphics& g)
    {
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

        // Caption of one full-width row: right-aligned in the fixed label column
        // that precedes the field.
        auto drawRowCaption = [&g](const juce::Rectangle<int>& field,
                                   const juce::String& caption)
        {
            g.setColour(UiColors::muted);
            g.setFont(GameUi::eyebrow(9.0f));
            g.drawText(caption,
                       juce::Rectangle<int>(field.getX() - 170, field.getY() + 7,
                                            160, 14).toFloat(),
                       juce::Justification::centredRight, false);
        };

        // --- ACCOUNT: username, display name, e-mail, member since ---
        GameUi::drawCard(g, accountCard.toFloat(), 16.0f);
        drawCardHeader(accountCard, "ACCOUNT", {});

        drawRowCaption(usernameEditorArea, "USERNAME");
        drawRowCaption(displayNameEditorArea, "DISPLAY NAME");
        drawRowCaption(emailArea, "E-MAIL");
        drawRowCaption(memberSinceArea, "MEMBER SINCE");

        g.setColour(UiColors::text);
        g.setFont(GameUi::font(12.5f));
        g.drawFittedText(meEmail.isNotEmpty() ? meEmail : juce::String("-"),
                         emailArea, juce::Justification::centredLeft, 1, 1.0f);
        g.drawFittedText(AccountWindowDetail::formatDate(meCreatedAt),
                         memberSinceArea, juce::Justification::centredLeft, 1, 1.0f);

        // --- CHARACTER: the same block as the public player card ---
        GameUi::drawCard(g, characterCard.toFloat(), 16.0f);

        g.setColour (UiColors::accent);
        g.setFont (GameUi::eyebrow (10.0f));
        g.drawText ("CHARACTER", characterCard.getX() + 20, characterCard.getY() + 16,
                    characterCard.getWidth() - 40, 14,
                    juce::Justification::centredLeft, false);

        drawCharacterFrames(g, characterPreviewArea);

        {
            const auto* current = currentCharacterOption();
            g.setColour(UiColors::text);
            g.setFont(GameUi::heading(22.0f));
            g.drawFittedText(current != nullptr ? current->name
                                                : juce::String("No character selected"),
                             characterNameArea, juce::Justification::centredLeft, 1, 1.0f);

            g.setColour(UiColors::muted);
            g.setFont(GameUi::font(11.0f));
            g.drawText("The guitarist that fights for you in duels and towers.",
                       characterHintArea, juce::Justification::centredLeft, true);
        }

        // --- OVERVIEW / GAME STATISTICS: metric tiles of the player card ---
        auto drawMetricCard = [&g](const juce::Rectangle<int>& card,
                                   const juce::String& title,
                                   std::initializer_list<std::pair<const char*, juce::String>> tiles,
                                   int tileH, float valueFont)
        {
            GameUi::drawCard(g, card.toFloat(), 16.0f);

            g.setColour (UiColors::accent);
            g.setFont (GameUi::eyebrow (10.0f));
            g.drawText (title, card.getX() + 20, card.getY() + 14,
                        card.getWidth() - 40, 14,
                        juce::Justification::centredLeft, false);

            const int tileW = (card.getWidth() - 40 - 3 * 10) / 4;
            int index = 0;

            for (const auto& tile : tiles)
            {
                const juce::Rectangle<int> r (card.getX() + 20
                                                  + (index % 4) * (tileW + 10),
                                              card.getY() + 38
                                                  + (index / 4) * (tileH + 8),
                                              tileW, tileH);

                g.setColour (UiColors::surface2.withAlpha (0.55f));
                g.fillRoundedRectangle (r.toFloat(), 10.0f);

                g.setColour (UiColors::muted);
                g.setFont (GameUi::eyebrow (9.0f));
                g.drawText (tile.first, r.getX() + 12, r.getY() + 8,
                            r.getWidth() - 24, 12,
                            juce::Justification::centredLeft, false);

                g.setColour (UiColors::text);
                g.setFont (GameUi::font (valueFont, true));
                g.drawText (tile.second, r.getX() + 12, r.getY() + 22,
                            r.getWidth() - 24, tileH - 26,
                            juce::Justification::centredLeft, false);

                ++index;
            }
        };

        drawMetricCard (overviewCard, "OVERVIEW",
                        { { "LEVEL", levelValue },
                          { "GUITAR SKILLS", guitarSkillsValue },
                          { "RATING", ratingValue },
                          { "TOTAL XP", xpValue } },
                        70, 20.0f);

        drawMetricCard (statsCard, "GAME STATISTICS",
                        { { "DUELS", duelsValue },
                          { "WINS", winsValue },
                          { "LOSSES", lossesValue },
                          { "WIN RATE", winRateValue },
                          { "BEST ACCURACY", bestAccuracyValue },
                          { "PEAK RATING", peakRatingValue },
                          { "SONGS MASTERED", songsMasteredValue } },
                        52, 16.0f);

        // --- PASSWORD ---
        GameUi::drawCard(g, passwordCard.toFloat(), 16.0f);
        drawCardHeader(passwordCard, "PASSWORD", {});
        drawRowCaption(currentPasswordEditorArea, "CURRENT PASSWORD");
        drawRowCaption(newPasswordEditorArea, "NEW PASSWORD");

        // --- ACHIEVEMENTS ---
        GameUi::drawCard(g, achievementsCard.toFloat(), 16.0f);
        drawCardHeader(achievementsCard, "ACHIEVEMENTS",
                       achievementsTotalCount > 0
                           ? juce::String(achievementsUnlockedCount) + " of "
                                 + juce::String(achievementsTotalCount) + " unlocked"
                           : juce::String("Your badges and the progress towards them."));

        // Every badge is drawn in the page flow: the block grows with the list
        // and has no inner scrolling.
        {
            const auto inner = achievementsCard.reduced (20);
            const int rowH = 88;
            int rowY = inner.getY() + 56;

            for (int i = 0; i < achievementModel.getNumRows(); ++i)
            {
                g.saveState();
                g.reduceClipRegion (inner.getX(), rowY, inner.getWidth(), rowH);
                g.addTransform (juce::AffineTransform::translation (
                    static_cast<float> (inner.getX()),
                    static_cast<float> (rowY)));
                achievementModel.paintListBoxItem (i, g, inner.getWidth(), rowH,
                                                   false);
                g.restoreState();
                rowY += rowH;
            }
        }

        // --- ATTEMPT HISTORY ---
        GameUi::drawCard(g, historyCard.toFloat(), 16.0f);
        drawCardHeader(historyCard, "ATTEMPT HISTORY",
                       "Your latest games: score, accuracy and max combo.");
    }
    void resized() override
    {
        if (contentHolder == nullptr)
            return;

        const int margin = 24;

        refreshButton.setBounds(getWidth() - margin - 104, 18, 104, 32);
        logoutButton.setBounds(getWidth() - margin - 104 - 12 - 104, 18, 104, 32);

        // Every block is full width and stacked; the page scrolls when the
        // window is shorter than the content.
        viewport.setBounds (0, 62, getWidth(),
                            juce::jmax (240, getHeight() - 62 - 44));
        contentHolder->setSize (juce::jmax (460, viewport.getMaximumVisibleWidth()),
                                contentHeight());

        alertLabel.setBounds(margin, getHeight() - 36,
                             getWidth() - margin * 2, 22);
    }

    int contentHeight() const
    {
        // The achievements block shows every badge: its height follows the
        // number of cards, and the page (not the block) scrolls.
        const int achievementRows = juce::jmax (1, (static_cast<int> (achievementCards.size()) + 1) / 2);
        const int achievementsH = 96 + achievementRows * 88;
        const int historyRowCount = juce::jmax (1, static_cast<int> (historyRows.size()));
        const int historyH = 150 + historyRowCount * 32;

        return 8 + 238 + 14 + 260 + 14 + 136 + 14 + 172 + 14 + 190 + 14
               + achievementsH + 14 + historyH + 8;
    }

    // Positions the widgets inside the scrollable page. Called from
    // Content::resized().
    void layoutContent()
    {
        const int margin = 24;
        const int gap = 14;
        const int width = contentHolder->getWidth();
        const int cardW = juce::jmax (360, width - margin * 2);

        int y = 8;
        accountCard = { margin, y, cardW, 238 };
        y += accountCard.getHeight() + gap;
        characterCard = { margin, y, cardW, 260 };
        y += characterCard.getHeight() + gap;
        overviewCard = { margin, y, cardW, 136 };
        y += overviewCard.getHeight() + gap;
        statsCard = { margin, y, cardW, 172 };
        y += statsCard.getHeight() + gap;
        passwordCard = { margin, y, cardW, 190 };
        y += passwordCard.getHeight() + gap;

        const int achievementRows = juce::jmax (1, (static_cast<int> (achievementCards.size()) + 1) / 2);
        const int achievementsH = 96 + achievementRows * 88;
        achievementsCard = { margin, y, cardW, achievementsH };
        y += achievementsCard.getHeight() + gap;

        const int historyRowCount = juce::jmax (1, static_cast<int> (historyRows.size()));
        historyCard = { margin, y, cardW, 150 + historyRowCount * 32 };

        ratingCard = {};

        // ACCOUNT: four full-width rows (username, display name, e-mail,
        // member since) plus the save button.
        {
            const auto inner = accountCard.reduced (20);
            const int labelW = 170;
            const int fieldX = inner.getX() + labelW;
            const int fieldW = juce::jmax (120, inner.getWidth() - labelW);

            usernameEditorArea = { fieldX, inner.getY() + 40, fieldW, 28 };
            displayNameEditorArea = { fieldX, inner.getY() + 80, fieldW, 28 };
            emailArea = { fieldX, inner.getY() + 120, fieldW, 28 };
            memberSinceArea = { fieldX, inner.getY() + 160, fieldW, 28 };
            saveProfileButtonArea = { inner.getRight() - 180,
                                      inner.getBottom() - 36, 180, 30 };

            usernameEditor.setBounds (usernameEditorArea);
            displayNameEditor.setBounds (displayNameEditorArea);
            saveProfileButton.setBounds (saveProfileButtonArea);
        }

        // CHARACTER: eyebrow, one framed sprite and the picker button.
        {
            const auto inner = characterCard.reduced (20);

            characterPreviewArea = { inner.getX(), inner.getY() + 30,
                                     244, inner.getHeight() - 40 };

            characterNameArea = { characterPreviewArea.getRight() + 24,
                                  inner.getY() + 44,
                                  juce::jmax (120, inner.getWidth()
                                                       - characterPreviewArea.getWidth()
                                                       - 24 - 236), 30 };
            characterHintArea = { characterNameArea.getX(),
                                  characterNameArea.getBottom() + 8,
                                  juce::jmax (120, characterNameArea.getWidth() + 230), 36 };
            chooseCharacterButtonArea = { inner.getRight() - 224,
                                          inner.getY() + 8, 224, 32 };

            chooseCharacterButton.setBounds (chooseCharacterButtonArea);
        }

        // PASSWORD.
        {
            const auto inner = passwordCard.reduced (20);
            const int labelW = 170;
            const int fieldX = inner.getX() + labelW;
            const int fieldW = juce::jmax (120, inner.getWidth() - labelW);

            currentPasswordEditorArea = { fieldX, inner.getY() + 40, fieldW, 28 };
            newPasswordEditorArea = { fieldX, inner.getY() + 80, fieldW, 28 };
            changePasswordButtonArea = { inner.getRight() - 200,
                                         inner.getBottom() - 34, 200, 30 };

            currentPasswordEditor.setBounds (currentPasswordEditorArea);
            newPasswordEditor.setBounds (newPasswordEditorArea);
            changePasswordButton.setBounds (changePasswordButtonArea);
        }

        // ACHIEVEMENTS: drawn by paintContent in the page flow, no inner list.
        achievementList.setVisible (false);

        // ATTEMPT HISTORY: every loaded attempt, no inner scrolling.
        {
            const auto inner = historyCard.reduced (20);
            constexpr int showMoreHeight = 28;
            const int tableTop = inner.getY() + 56;
            const int rows = juce::jmax (1, static_cast<int> (historyRows.size()));

            historyTable.setBounds (inner.getX(), tableTop, inner.getWidth(),
                                    28 + rows * 32);
            historyEmptyLabel.setBounds (historyTable.getBounds().withTrimmedTop (28));

            showMoreButtonArea = { inner.getRight() - 104,
                                   historyTable.getBottom() + 6,
                                   104, showMoreHeight };
            showMoreButton.setBounds (showMoreButtonArea);

            // The page scrolls; the table itself never does.
            if (auto* historyViewport = historyTable.getViewport())
                historyViewport->setScrollBarsShown (false, false);
        }

        ratingTable.setVisible (false);
        ratingEmptyLabel.setVisible (false);
    }

private:
    struct CharacterOption
    {
        int id = 0;
        juce::String name;
        juce::String sheetUrl;
        // Server ownership (GET /characters). Older servers do not send
        // ``unlocked`` at all, so a missing field keeps the character
        // selectable exactly like before.
        bool unlocked = true;
        juce::String unlockCondition;
        juce::String unlockCampaignName;
    };
    // --- Character preview and picker -----------------------------------------

    const CharacterOption* currentCharacterOption() const
    {
        const int id = selectedCharacterId();

        for (const auto& option : characterOptions)
            if (option.id == id)
                return &option;

        return nullptr;
    }

    void requestCharacterSheet (const juce::String& url)
    {
        if (url.isEmpty() || characterSheets.count (url) > 0
            || characterSheetsInFlight.count (url) > 0)
            return;

        characterSheetsInFlight[url] = true;

        const auto absolute = url.startsWith ("http")
                                  ? url
                                  : juce::String (ApiClient::serverOrigin) + url;
        auto data = std::make_shared<juce::MemoryBlock>();
        juce::Component::SafePointer<AccountWindow> safeThis (this);

        api.runAsync ([data, absolute]
                      {
                          *data = ApiClient::download (absolute, 15000);
                      },
                      [safeThis, url, data]
                      {
                          auto* self = safeThis.getComponent();

                          if (self == nullptr)
                              return;

                          self->characterSheetsInFlight.erase (url);

                          if (data->getSize() == 0)
                              return;

                          self->characterSheets[url] =
                              juce::ImageFileFormat::loadFrom (data->getData(),
                                                               data->getSize());

                          if (self->contentHolder != nullptr)
                              self->contentHolder->repaint();

                          if (auto* picker = self->characterPickerComponent.getComponent())
                              picker->repaint();
                      });
    }

    // The idle spritesheet: one large frame (the animation is shared, the
    // preview shows the first pose).
    void drawCharacterFrames (juce::Graphics& g, juce::Rectangle<int> area)
    {
        if (area.isEmpty())
            return;

        g.setColour (UiColors::surface2.withAlpha (0.55f));
        g.fillRoundedRectangle (area.toFloat(), 12.0f);

        const auto* current = currentCharacterOption();

        if (current == nullptr || current->sheetUrl.isEmpty())
            return;

        const auto found = characterSheets.find (current->sheetUrl);

        if (found == characterSheets.end())
        {
            requestCharacterSheet (current->sheetUrl);
            return;
        }

        const auto& sheet = found->second;

        if (! sheet.isValid())
            return;

        const int fw = sheet.getWidth() / 4;
        const int fh = sheet.getHeight() / 2;

        if (fw <= 0 || fh <= 0)
            return;

        // Keep the sprite proportions: the frame is 4x2 cells of 384x512, so
        // stretching it to an arbitrary box would fatten the character.
        const float scale = juce::jmin (static_cast<float> (area.getWidth())
                                            / static_cast<float> (fw),
                                        static_cast<float> (area.getHeight())
                                            / static_cast<float> (fh));
        const int w = juce::roundToInt (static_cast<float> (fw) * scale);
        const int h = juce::roundToInt (static_cast<float> (fh) * scale);

        g.drawImage (sheet, area.getCentreX() - w / 2, area.getCentreY() - h / 2,
                     w, h, 0, 0, fw, fh);
    }

    void openCharacterPicker()
    {
        if (characterOptions.empty())
        {
            showAlert ("No characters are available yet.", UiColors::muted);
            return;
        }

        // Start every spritesheet download; the grid repaints as they arrive.
        for (const auto& option : characterOptions)
            requestCharacterSheet (option.sheetUrl);

        auto* picker = new CharacterPicker (*this,
                                           [this] (int id) { pickCharacter (id); });

        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (picker);
        options.dialogTitle = "Choose a character";
        options.dialogBackgroundColour = UiColors::background;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = true;
        options.componentToCentreAround = this;

        const int count = static_cast<int> (characterOptions.size());
        const int columns = juce::jlimit (1, 4, (count + 2) / 3);
        const int rows = (count + columns - 1) / juce::jmax (1, columns);

        if (auto* dialog = options.launchAsync())
        {
            dialog->setResizeLimits (520, 300, 1600, 1200);
            dialog->setSize (juce::jlimit (520, 1100, columns * 214 + 36),
                             juce::jlimit (300, 860, rows * 224 + 36));
            characterPickerDialog = dialog;
        }

        characterPickerComponent = picker;
    }

    void pickCharacter (int id)
    {
        for (size_t i = 0; i < characterOptions.size(); ++i)
        {
            const auto& option = characterOptions[i];

            if (option.id != id || ! option.unlocked)
                continue;

            // The hidden combo is the single selection store used by
            // saveProfile(); setting it also applies the sprite locally.
            for (size_t comboIndex = 0; comboIndex < comboOptionIndices.size();
                 ++comboIndex)
            {
                if (comboOptionIndices[comboIndex] == i)
                {
                    characterCombo.setSelectedId (static_cast<int> (comboIndex) + 1,
                                                  juce::sendNotification);
                    break;
                }
            }

            profileDirty = true;

            if (contentHolder != nullptr)
                contentHolder->repaint();

            if (auto* dialog = characterPickerDialog.getComponent())
                dialog->exitModalState (0);

            return;
        }
    }

    // Grid of every character: name + the first two idle frames. Unlocked cards
    // are outlined in yellow, locked ones are grey with their unlock condition.
    class CharacterPicker final : public juce::Component,
                                  private juce::Timer
    {
    public:
        CharacterPicker (AccountWindow& ownerToUse,
                         std::function<void (int)> onPickToUse)
            : owner (ownerToUse), onPick (std::move (onPickToUse))
        {
            setOpaque (true);
            startTimerHz (6);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (UiColors::background);

            const int count = static_cast<int> (owner.characterOptions.size());

            for (int i = 0; i < count; ++i)
                paintCard (g, cardArea (i),
                           owner.characterOptions[static_cast<size_t> (i)]);
        }

        void mouseUp (const juce::MouseEvent& event) override
        {
            for (int i = 0; i < static_cast<int> (owner.characterOptions.size()); ++i)
            {
                const auto& option =
                    owner.characterOptions[static_cast<size_t> (i)];

                if (! option.unlocked || ! cardArea (i).contains (event.getPosition()))
                    continue;

                if (onPick != nullptr)
                    onPick (option.id);

                return;
            }
        }

    private:
        void timerCallback() override
        {
            // Sheets finish on worker threads; repaint until the grid is whole.
            bool missing = false;

            for (const auto& option : owner.characterOptions)
                if (option.sheetUrl.isNotEmpty()
                    && owner.characterSheets.count (option.sheetUrl) == 0)
                    missing = true;

            repaint();

            // A sheet that failed to download must not keep the timer alive.
            if (! missing || ++repaintTicks > 80)
                stopTimer();
        }

        int columns() const
        {
            return juce::jmax (2, (getWidth() - 36 + 14) / 214);
        }

        juce::Rectangle<int> cardArea (int index) const
        {
            constexpr int cardW = 200;
            constexpr int cardH = 210;
            constexpr int gap = 14;
            const int cols = columns();
            const int x = 18 + (index % cols) * (cardW + gap);
            const int y = 18 + (index / cols) * (cardH + gap);
            return { x, y, cardW, cardH };
        }

        void paintCard (juce::Graphics& g, juce::Rectangle<int> card,
                        const CharacterOption& option)
        {
            const bool selected = option.id == owner.selectedCharacterId();
            const bool unlocked = option.unlocked;

            GameUi::drawCard (g, card.toFloat(), 16.0f);

            g.setColour (unlocked ? UiColors::accent.withAlpha (selected ? 0.95f : 0.55f)
                                  : UiColors::border.withAlpha (0.6f));
            g.drawRoundedRectangle (card.toFloat().reduced (0.5f), 16.0f,
                                    selected ? 2.0f : 1.0f);

            // One large idle frame (the spritesheet's first pose).
            const juce::Rectangle<float> frame (
                static_cast<float> (card.getCentreX()) - 48.0f,
                static_cast<float> (card.getY()) + 14.0f, 96.0f, 128.0f);
            g.setColour (UiColors::surface2.withAlpha (0.55f));
            g.fillRoundedRectangle (frame, 10.0f);

            const auto found = owner.characterSheets.find (option.sheetUrl);

            if (found != owner.characterSheets.end() && found->second.isValid())
            {
                const auto& sheet = found->second;
                const int fw = sheet.getWidth() / 4;
                const int fh = sheet.getHeight() / 2;

                if (fw > 0 && fh > 0)
                {
                    if (! unlocked)
                        g.setOpacity (0.35f);

                    g.drawImage (sheet, (int) frame.getX(), (int) frame.getY(),
                                 (int) frame.getWidth(), (int) frame.getHeight(),
                                 0, 0, fw, fh);

                    if (! unlocked)
                        g.setOpacity (1.0f);
                }
            }

            // Name below the frame.
            g.setColour (unlocked ? UiColors::text : UiColors::muted);
            g.setFont (GameUi::heading (16.0f));
            g.drawFittedText (option.name,
                              juce::Rectangle<int> (card.getX() + 16,
                                                    card.getY() + 150,
                                                    card.getWidth() - 32, 24),
                              juce::Justification::centred, 1, 1.0f);

            if (selected)
            {
                g.setColour (UiColors::accent);
                g.setFont (GameUi::eyebrow (8.5f));
                g.drawText ("SELECTED",
                            juce::Rectangle<int> (card.getX() + 16,
                                                  card.getY() + 176,
                                                  card.getWidth() - 32, 12),
                            juce::Justification::centred, false);
            }
            else if (! unlocked)
            {
                juce::String condition = option.unlockCondition;

                if (condition.isEmpty() && option.unlockCampaignName.isNotEmpty())
                    condition = "Complete " + option.unlockCampaignName;

                g.setColour (UiColors::muted);
                g.setFont (GameUi::font (10.0f));
                g.drawFittedText (condition.isNotEmpty()
                                      ? juce::String ("Unavailable - ") + condition
                                      : juce::String ("Unavailable - not unlocked yet"),
                                  juce::Rectangle<int> (card.getX() + 14,
                                                        card.getY() + 174,
                                                        card.getWidth() - 28, 30),
                                  juce::Justification::topLeft, 2, 1.0f);
            }
            else
            {
                g.setColour (UiColors::muted);
                g.setFont (GameUi::font (10.0f));
                g.drawText ("Click to select",
                            juce::Rectangle<int> (card.getX() + 16,
                                                  card.getY() + 176,
                                                  card.getWidth() - 32, 16),
                            juce::Justification::centred, false);
            }
        }

        AccountWindow& owner;
        std::function<void (int)> onPick;
        int repaintTicks = 0;
    };

    struct RatingRow
    {
        int rank = 0;
        juce::String username;
        juce::String displayName;
        int ratingPoints = 0;
        int gamesPlayed = 0;
        double bestAccuracy = 0.0;

        // Guitar Skills is the primary metric; ratingPoints is kept for older
        // servers that do not send the new fields yet.
        double guitarSkills = -1.0;
        double accuracyPercent = 0.0;
        double duelWinRate = 0.0;
        int duelWins = 0;
        int duelLosses = 0;
        int duelDraws = 0;
        int level = 1;
        bool provisional = false;
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
                    text = row.guitarSkills >= 0.0
                               ? juce::String(row.guitarSkills, 1)
                               : AccountWindowDetail::formatCount(row.ratingPoints);
                    justification = juce::Justification::centredRight;
                    colour = isMe ? UiColors::accent : UiColors::gold;
                    break;
                case 4:
                    text = "LVL " + juce::String(row.level);
                    justification = juce::Justification::centred;
                    colour = UiColors::secondaryText;
                    break;
                case 5:
                    text = row.duelWins + row.duelLosses + row.duelDraws > 0
                               ? AccountWindowDetail::formatPercent(row.duelWinRate)
                                     + "  " + juce::String(row.duelWins)
                                     + "-" + juce::String(row.duelLosses)
                                     + (row.duelDraws > 0
                                            ? "-" + juce::String(row.duelDraws)
                                            : juce::String())
                               : juce::String("-");
                    justification = juce::Justification::centredRight;
                    colour = UiColors::secondaryText;
                    break;
                case 6:
                    // Only claim a rank status when the server actually sent
                    // guitar_skills; older payloads fall back to "-".
                    text = row.guitarSkills < 0.0
                               ? juce::String("-")
                               : (row.provisional ? juce::String("PROV")
                                                  : juce::String("RANKED"));
                    justification = juce::Justification::centred;
                    colour = row.guitarSkills < 0.0
                                 ? UiColors::muted
                                 : (row.provisional ? UiColors::muted
                                                    : UiColors::green);
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

        // The page scrolls; both blocks show their full content at once.
        if (auto* historyViewport = historyTable.getViewport())
            historyViewport->setScrollBarsShown (false, false);

        ratingTable.getHeader().addColumn(
            "#", 1, 40, 36, 70, juce::TableHeaderComponent::notSortable);
        ratingTable.getHeader().addColumn(
            "PLAYER", 2, 200, 120, 420, juce::TableHeaderComponent::notSortable);
        ratingTable.getHeader().addColumn(
            "SKILLS", 3, 82, 70, 130, juce::TableHeaderComponent::notSortable);
        ratingTable.getHeader().addColumn(
            "LEVEL", 4, 66, 58, 110, juce::TableHeaderComponent::notSortable);
        ratingTable.getHeader().addColumn(
            "DUELS", 5, 120, 96, 170, juce::TableHeaderComponent::notSortable);
        ratingTable.getHeader().addColumn(
            "STATUS", 6, 74, 64, 110, juce::TableHeaderComponent::notSortable);
        contentHolder->addAndMakeVisible(ratingTable);

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
        contentHolder->addAndMakeVisible(historyTable);

        for (auto* label : { &ratingEmptyLabel, &historyEmptyLabel })
        {
            label->setFont(GameUi::font(12.0f));
            label->setColour(juce::Label::textColourId, UiColors::muted);
            label->setJustificationType(juce::Justification::centred);
            label->setInterceptsMouseClicks(false, false);
            contentHolder->addAndMakeVisible(label);
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

        contentHolder->addAndMakeVisible(usernameEditor);
        contentHolder->addAndMakeVisible(displayNameEditor);
        contentHolder->addAndMakeVisible(currentPasswordEditor);
        contentHolder->addAndMakeVisible(newPasswordEditor);

        // Character picked from the admin-managed UI levels. The sheet is a
        // 4x2 PNG; the first two frames are the idle animation. The combo stays
        // as the internal selection store; the page shows a preview card and the
        // popup picker below.
        characterCombo.setTextWhenNothingSelected ("No characters uploaded");
        characterCombo.setColour (juce::ComboBox::backgroundColourId, UiColors::surface2);
        characterCombo.setColour (juce::ComboBox::outlineColourId, UiColors::border);
        characterCombo.setColour (juce::ComboBox::textColourId, UiColors::text);
        characterCombo.setColour (juce::ComboBox::arrowColourId, UiColors::accent);
        characterCombo.onChange = [this] { applyCharacterSelection(); };
        contentHolder->addAndMakeVisible (characterCombo);
        characterCombo.setVisible (false);

        AccountWindowDetail::styleButton (chooseCharacterButton, false);
        chooseCharacterButton.onClick = [this] { openCharacterPicker(); };
        contentHolder->addAndMakeVisible (chooseCharacterButton);

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

        contentHolder->addAndMakeVisible(saveProfileButton);
        contentHolder->addAndMakeVisible(changePasswordButton);
        addAndMakeVisible(refreshButton);
        addAndMakeVisible(logoutButton);
        contentHolder->addAndMakeVisible(showMoreButton);

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
        if (auto* listViewport = achievementList.getViewport())
            listViewport->setScrollBarsShown (false, false);
        contentHolder->addAndMakeVisible(achievementList);

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
        loadDuelSummary();
        loadAchievements();
        refreshCharacters();
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
                // The cards are painted by the scrollable page itself.
                self->achievementList.setVisible (false);
                self->achievementList.updateContent();

                if (auto* viewport = self->achievementList.getViewport())
                    viewport->setViewPosition(0, 0);

                // The achievements block grows with the number of cards: relayout
                // the page so the whole list is visible without inner scrolling.
                self->resized();
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
                self->xpValue = AccountWindowDetail::formatCount(xpTotal);
                self->xpNote = juce::String(xpForNext) + " to level "
                    + juce::String(level + 1);

                // Player-card tiles: mastered songs come from progression.
                const auto songsMastered =
                    json.getProperty("songs_mastered", juce::var());

                if (! songsMastered.isVoid())
                    self->songsMasteredValue =
                        juce::String(static_cast<int>(songsMastered));

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

    // Rating, peak rating and the online totals shown in the OVERVIEW and
    // GAME STATISTICS tiles come from the duel profile, the same source the
    // public player card uses.
    void loadDuelSummary()
    {
        const int requestId = ++duelSummaryRequestId;
        juce::Component::SafePointer<AccountWindow> safeThis(this);

        api.getJson("/duels/me", token, 6000,
            [safeThis, requestId](ApiClient::Response response)
            {
                auto* self = safeThis.getComponent();

                if (self == nullptr || requestId != self->duelSummaryRequestId)
                    return;

                if (! response.transportOk || ! response.json.isObject())
                    return;

                const auto& json = response.json;

                const int rating = static_cast<int>(json.getProperty("rating", 0));
                const int peak = static_cast<int>(json.getProperty("peak_rating", 0));

                if (rating > 0)
                    self->ratingValue = AccountWindowDetail::formatCount(rating);

                if (peak > 0)
                    self->peakRatingValue = AccountWindowDetail::formatCount(peak);

                const auto online = json.getProperty("online", juce::var());

                if (online.isObject())
                {
                    self->duelsValue = juce::String(
                        static_cast<int>(online.getProperty("duels", 0)));
                    self->winsValue = juce::String(
                        static_cast<int>(online.getProperty("wins", 0)));
                    self->lossesValue = juce::String(
                        static_cast<int>(online.getProperty("losses", 0)));
                    self->winRateValue = juce::String(
                        static_cast<double>(online.getProperty("win_rate", 0.0)), 1)
                        + "%";
                    self->bestAccuracyValue = juce::String(
                        static_cast<double>(online.getProperty("accuracy", 0.0)), 1)
                        + "%";
                }

                const auto userId = json.getProperty("user_id", juce::var()).toString();

                if (userId.isNotEmpty())
                    self->loadGuitarSkills(userId);

                self->repaint();
            });
    }

    // The player's own Guitar Skills, the same detail the public player card
    // shows. Read from the public leaderboard detail endpoint so the number
    // matches the Global Leaderboard exactly.
    void loadGuitarSkills(const juce::String& userId)
    {
        const int requestId = ++guitarSkillsRequestId;
        juce::Component::SafePointer<AccountWindow> safeThis(this);

        api.getJson("/leaderboard/global/" + userId, token, 6000,
            [safeThis, requestId](ApiClient::Response response)
            {
                auto* self = safeThis.getComponent();

                if (self == nullptr || requestId != self->guitarSkillsRequestId)
                    return;

                if (! response.transportOk || ! response.json.isObject())
                    return;

                if (response.json.getProperty("guitar_skills", juce::var()).isVoid())
                    return;

                self->guitarSkillsValue = juce::String(
                    static_cast<double>(response.json.getProperty("guitar_skills", 0.0)), 1);
                self->repaint();
            });
    }

    void refreshCharacters()
    {
        juce::Component::SafePointer<AccountWindow> safeThis (this);
        // The ownership flags are per account: without the bearer token the
        // server treats the request as anonymous and every non-starter
        // character would look locked.
        api.getJson ("/characters", token, 8000,
                     [safeThis] (ApiClient::Response reply)
                     {
                         auto* self = safeThis.getComponent();

                         if (self == nullptr)
                             return;

                         if (! reply.transportOk || reply.statusCode != 200
                             || ! reply.json.isArray())
                             return;

                         self->characterOptions.clear();
                         self->comboOptionIndices.clear();
                         self->characterComboUpdating = true;
                         self->characterCombo.clear (juce::dontSendNotification);
                         int comboId = 1;
                         int selectedComboId = 0;
                         juce::StringArray lockedLines;

                         // The picker grid is built from characterOptions, so
                         // the playable characters are collected first and the
                         // locked ones are appended after them.
                         std::vector<CharacterOption> unlockedOptions;
                         std::vector<CharacterOption> lockedOptions;

                         for (const auto& item : *reply.json.getArray())
                         {
                             if (! item.isObject())
                                 continue;

                             CharacterOption option;
                             option.id = static_cast<int> (item.getProperty ("id", 0));
                             option.name = item.getProperty ("name", "").toString();
                             option.sheetUrl = item.getProperty ("sheet_url", "").toString();

                             // Additive ownership fields. A missing ``unlocked``
                             // means an older server that knows no locks: every
                             // character stays selectable.
                             const auto unlockedVar =
                                 item.getProperty ("unlocked", juce::var());
                             option.unlocked = unlockedVar.isVoid()
                                 ? true
                                 : static_cast<bool> (unlockedVar);
                             option.unlockCondition = item.getProperty (
                                 "unlock_condition", juce::var()).toString();
                             option.unlockCampaignName = item.getProperty (
                                 "unlock_campaign_name", juce::var()).toString();

                             if (option.id <= 0 || option.name.isEmpty())
                                 continue;

                             if (option.unlocked)
                             {
                                 // The combo is indexed by the position inside
                                 // the unlocked block, which is also the
                                 // position in the final list.
                                 self->characterCombo.addItem (option.name, comboId);
                                 self->comboOptionIndices.push_back (
                                     static_cast<int> (unlockedOptions.size()));

                                 if (option.id == self->meCharacterId)
                                     selectedComboId = comboId;

                                 ++comboId;
                                 unlockedOptions.push_back (option);
                             }
                             else
                             {
                                 // The app has no icon font: a plain marker plus
                                 // the server's condition text is the lock hint.
                                 juce::String line = "[LOCKED] " + option.name;
                                 auto condition = option.unlockCondition;

                                 if (condition.isEmpty()
                                     && option.unlockCampaignName.isNotEmpty())
                                     condition = "Complete "
                                                 + option.unlockCampaignName;

                                 if (condition.isNotEmpty())
                                     line += " - " + condition;

                                 lockedLines.add (line);
                                 lockedOptions.push_back (option);
                             }
                         }

                         self->characterOptions = std::move (unlockedOptions);
                         self->characterOptions.insert (self->characterOptions.end(),
                                                        lockedOptions.begin(),
                                                        lockedOptions.end());

                         if (selectedComboId > 0)
                             self->characterCombo.setSelectedId (
                                 selectedComboId, juce::dontSendNotification);

                         self->characterComboUpdating = false;

                         // Keep the list bounded: six lines already make the
                         // PROFILE card tall, an arbitrary catalogue must not
                         // push SAVE off the card.
                         if (lockedLines.size() > 6)
                         {
                             const int extra = lockedLines.size() - 5;
                             lockedLines.removeRange (5, lockedLines.size() - 5);
                             lockedLines.add ("... and " + juce::String (extra)
                                              + " more");
                         }

                         self->lockedCharactersText = lockedLines.joinIntoString ("\n");

                         // "No characters uploaded" is only true when the
                         // admin list is empty; with entries the empty field
                         // must invite the player to pick one, otherwise it
                         // looks like the selector is unavailable.
                         self->characterCombo.setTextWhenNothingSelected (
                             self->characterOptions.empty()
                                 ? juce::String ("No characters uploaded")
                                 : self->comboOptionIndices.empty()
                                     ? juce::String ("No unlocked characters")
                                     : juce::String ("Choose a character"));

                         self->resized();
                         self->repaint();

                         if (self->characterCombo.getSelectedId() > 0)
                             self->applyCharacterSelection();
                     });
    }

    int selectedCharacterId() const
    {
        const int position = characterCombo.getSelectedId() - 1;

        if (position < 0
            || position >= static_cast<int> (comboOptionIndices.size()))
            return 0;

        const auto index = comboOptionIndices[static_cast<size_t> (position)];

        if (index >= characterOptions.size())
            return 0;

        return characterOptions[index].id;
    }

    void applyCharacterSelection()
    {
        if (characterComboUpdating)
            return;

        const int position = characterCombo.getSelectedId() - 1;

        if (position < 0
            || position >= static_cast<int> (comboOptionIndices.size()))
            return;

        const auto index = comboOptionIndices[static_cast<size_t> (position)];

        if (index >= characterOptions.size())
            return;

        const auto& option = characterOptions[index];

        if (callbacks.onCharacterSelected != nullptr)
            callbacks.onCharacterSelected (option.id, option.name, option.sheetUrl);
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
                self->meCharacterId = static_cast<int> (me.getProperty ("character_id", 0));

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

                // RATING in the player card is the duel rating, the same number
                // the public profile shows; it is loaded by loadDuelSummary().
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
                        row.accuracyPercent = static_cast<double>(
                            item.getProperty("accuracy_percent", row.bestAccuracy));
                        row.duelWinRate = static_cast<double>(
                            item.getProperty("duel_win_rate", 0.0));
                        row.duelWins = static_cast<int>(
                            item.getProperty("duel_wins", 0));
                        row.duelLosses = static_cast<int>(
                            item.getProperty("duel_losses", 0));
                        row.duelDraws = static_cast<int>(
                            item.getProperty("duel_draws", 0));
                        row.level = static_cast<int>(
                            item.getProperty("level", 1));
                        row.provisional = static_cast<bool>(
                            item.getProperty("provisional", false));

                        if (const auto* object = item.getDynamicObject())
                        {
                            if (object->hasProperty("guitar_skills"))
                                row.guitarSkills = static_cast<double>(
                                    object->getProperty("guitar_skills"));
                        }

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
            notifySaveResult("Username must be 3-32 characters.", UiColors::red,
                             juce::MessageBoxIconType::WarningIcon);
            return;
        }

        if (!username.containsOnly(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-"))
        {
            notifySaveResult("Username may contain only letters, digits, dot, dash and underscore.",
                             UiColors::red, juce::MessageBoxIconType::WarningIcon);
            return;
        }

        if (displayName.isEmpty())
        {
            notifySaveResult("Display name cannot be blank.", UiColors::red,
                             juce::MessageBoxIconType::WarningIcon);
            return;
        }

        if (displayName.length() > 64)
        {
            notifySaveResult("Display name must be at most 64 characters.",
                             UiColors::red, juce::MessageBoxIconType::WarningIcon);
            return;
        }

        juce::DynamicObject::Ptr payload = new juce::DynamicObject();
        const bool usernameChanged = username != meUsername;
        const bool displayNameChanged = displayName != meDisplayName;
        const int chosenCharacterId = selectedCharacterId();
        const bool characterChanged = chosenCharacterId > 0
                                      && chosenCharacterId != meCharacterId;
        const bool onlyCharacterChanged = characterChanged
                                          && ! usernameChanged
                                          && ! displayNameChanged;

        if (usernameChanged)
            payload->setProperty("username", username);

        if (displayNameChanged)
            payload->setProperty("display_name", displayName);

        // The character picked in the combo is part of the same save, so the
        // button reports exactly what happened to it.
        if (characterChanged)
            payload->setProperty("character_id", chosenCharacterId);

        if (! usernameChanged && ! displayNameChanged && ! characterChanged)
        {
            notifySaveResult("No changes to save.", UiColors::muted,
                             juce::MessageBoxIconType::InfoIcon);
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
            [safeThis, requestId, onlyCharacterChanged, chosenCharacterId](ApiClient::Response response)
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
                    // A stale catalogue (the tower was completed on another
                    // device) can offer a character the server now rejects.
                    if (response.statusCode == 422
                        && AccountWindowDetail::apiErrorMessage(response)
                               .containsIgnoreCase("locked"))
                    {
                        self->refreshCharacters();
                        self->notifySaveResult(
                            "This character is locked. Finish the tower that unlocks it.",
                            UiColors::red, juce::MessageBoxIconType::WarningIcon);
                        return;
                    }

                    self->notifySaveResult(
                        "Could not save profile: "
                            + AccountWindowDetail::apiErrorMessage(response),
                        UiColors::red, juce::MessageBoxIconType::WarningIcon);
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

                if (chosenCharacterId > 0)
                {
                    const auto savedCharacterId =
                        response.json.getProperty("character_id", juce::var());

                    self->meCharacterId = savedCharacterId.isVoid()
                        ? chosenCharacterId
                        : static_cast<int>(savedCharacterId);
                }

                self->welcomeText = "Signed in as " + self->meUsername;
                if (self->meEmail.isNotEmpty())
                    self->welcomeText += "  |  " + self->meEmail;

                if (self->callbacks.onProfileUpdated)
                    self->callbacks.onProfileUpdated(self->meUsername,
                                                     self->meDisplayName);

                self->notifySaveResult(
                    onlyCharacterChanged ? juce::String("Character saved.")
                                         : juce::String("Profile updated."),
                    UiColors::green, juce::MessageBoxIconType::InfoIcon);
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

    // The inline label sits at the bottom of the account window and is often
    // outside the embedded profile viewport, so a save result also gets a
    // small message box that is visible no matter where the page is scrolled.
    void notifySaveResult(const juce::String& text, juce::Colour colour,
                          juce::MessageBoxIconType icon)
    {
        showAlert(text, colour);
        juce::AlertWindow::showMessageBoxAsync(icon, "Profile", text, "OK");
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
    juce::String guitarSkillsValue = "-";
    juce::String rankValue = "-";
    juce::String rankNote = "not in the top 50";
    juce::String songsValue = "-";
    juce::String attemptsValue = "-";
    juce::String levelValue = "-";
    juce::String xpValue = "-";
    juce::String xpNote = "total experience";
    juce::String accuracyValue = "-";
    juce::String timingValue = "-";

    // Player card blocks: the same numbers the public profile shows.
    juce::String peakRatingValue = "-";
    juce::String duelsValue = "-";
    juce::String winsValue = "-";
    juce::String lossesValue = "-";
    juce::String winRateValue = "-";
    juce::String bestAccuracyValue = "-";
    juce::String songsMasteredValue = "-";
    int duelSummaryRequestId = 0;
    int guitarSkillsRequestId = 0;
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


    std::vector<CharacterOption> characterOptions;
    // Combo item position -> index in ``characterOptions``. The combo lists only
    // unlocked characters, so it cannot be indexed by its own selected id.
    std::vector<size_t> comboOptionIndices;
    juce::ComboBox characterCombo;
    bool characterComboUpdating = false;
    int meCharacterId = 0;
    // "[LOCKED] Jackson - Complete Jackson Tower" lines painted under the combo.
    juce::String lockedCharactersText;

    juce::TextButton saveProfileButton { "SAVE PROFILE" };
    juce::TextButton changePasswordButton { "CHANGE PASSWORD" };
    juce::TextButton showMoreButton { "SHOW MORE" };
    juce::TextButton refreshButton { "REFRESH" };
    juce::TextButton logoutButton { "LOG OUT" };
    juce::Label alertLabel;
    juce::Label ratingEmptyLabel;
    juce::Label historyEmptyLabel;

    juce::Viewport viewport;
    std::unique_ptr<Content> contentHolder;

    juce::TextButton chooseCharacterButton { "CHOOSE CHARACTER..." };

    juce::Rectangle<int> characterCard;
    juce::Rectangle<int> achievementsCard;
    juce::Rectangle<int> overviewCard;
    juce::Rectangle<int> statsCard;
    juce::Rectangle<int> characterHintArea;
    juce::Rectangle<int> emailArea;
    juce::Rectangle<int> memberSinceArea;
    juce::Rectangle<int> characterNameArea;
    juce::Rectangle<int> characterPreviewArea;
    juce::Rectangle<int> chooseCharacterButtonArea;

    // Spritesheets by server-relative URL, decoded once. Frames are the 4x2
    // grid the duel stage uses.
    std::map<juce::String, juce::Image> characterSheets;
    std::map<juce::String, bool> characterSheetsInFlight;
    juce::Component::SafePointer<juce::Component> characterPickerComponent;
    juce::Component::SafePointer<juce::DialogWindow> characterPickerDialog;

    juce::Rectangle<int> ratingCard;
    juce::Rectangle<int> historyCard;
    juce::Rectangle<int> profileCard;
    juce::Rectangle<int> passwordCard;
    juce::Rectangle<int> accountCard;
    juce::Rectangle<int> usernameEditorArea;
    juce::Rectangle<int> displayNameEditorArea;
    juce::Rectangle<int> characterComboArea;
    juce::Rectangle<int> lockedCharactersArea;
    juce::Rectangle<int> saveProfileButtonArea;
    juce::Rectangle<int> currentPasswordEditorArea;
    juce::Rectangle<int> newPasswordEditorArea;
    juce::Rectangle<int> changePasswordButtonArea;
    juce::Rectangle<int> showMoreButtonArea;
};

//==============================================================================
// Fullscreen
//
// The window is a normal 16:9 DocumentWindow. FULLSCREEN puts it into kiosk
// mode: the peer drops the native frame and the window covers the whole
// display, taskbar included. WINDOWED puts the remembered 16:9 geometry back.
void MainComponent::toggleFullScreen()
{
    auto& desktop = juce::Desktop::getInstance();
    auto* window = dynamic_cast<juce::ResizableWindow*> (getTopLevelComponent());

    if (window == nullptr)
        return;

    if (desktop.getKioskModeComponent() == window)
    {
        desktop.setKioskModeComponent (nullptr);

       #if ! JUCE_MAC
        // Kiosk mode restores the bounds it saved on the way in. Set them again
        // so a moved/resized window never comes back with the frame at the
        // fullscreen size.
        if (! windowedBoundsBeforeFullscreen.isEmpty())
            window->setBounds (windowedBoundsBeforeFullscreen);
       #endif

        updateFullScreenToggle();
        return;
    }

   #if JUCE_MAC
    // The green traffic-light button also puts the window into full screen.
    // The toggle mirrors that state instead of stacking another request.
    if (window->isFullScreen())
    {
        window->setFullScreen (false);
        updateFullScreenToggle();
        return;
    }
   #endif

    // On Windows a maximized window ignores the resize kiosk mode performs, so
    // the window would keep the taskbar-sized frame. Leave that state first:
    // JUCE restores the last normal (16:9) bounds, which are then remembered.
    if (window->isFullScreen())
        window->setFullScreen (false);

    windowedBoundsBeforeFullscreen = window->getBounds();

    desktop.setKioskModeComponent (window, false);

   #if ! JUCE_MAC
    // Cover the whole display (taskbar area included) even on window managers
    // that ignore the resize kiosk mode applies.
    if (const auto* display = desktop.getDisplays()
                                  .getDisplayForRect (window->getScreenBounds()))
        window->setBounds (display->logicalBounds.toNearestInt());
   #endif

    updateFullScreenToggle();
}

void MainComponent::updateFullScreenToggle()
{
    const bool fullscreen =
        juce::Desktop::getInstance().getKioskModeComponent() != nullptr;

    fullScreenButton.setButtonText (fullscreen ? "WINDOWED" : "FULLSCREEN");
}

//==============================================================================
void MainComponent::updateScreenSaverForSession()
{
    // While a song is loaded the player reads the tabs and plays the guitar, so
    // the OS sees no keyboard or mouse input and dims the display after the
    // configured idle time. Keep the screen awake for the session and hand the
    // normal idle behaviour back on the menus. JUCE only holds the assertion
    // while the app is the foreground process.
    const bool keepAwake = ! showAuthScreen && gameState != GameState::SongSelect;

    if (keepAwake == screenSaverSuppressed)
        return;

    screenSaverSuppressed = keepAwake;
    juce::Desktop::setScreenSaverEnabled (! keepAwake);
}

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
    callbacks.onCharacterSelected =
        [safeThis = juce::Component::SafePointer<MainComponent>(this)]
        (int id, juce::String name, juce::String sheetUrl)
        {
            if (auto* self = safeThis.getComponent())
                self->setSelectedCharacter (id, name, sheetUrl, false);
        };

    juce::DialogWindow::LaunchOptions options;
    auto* window = new AccountWindow(apiClient, onlineAccessToken, std::move(callbacks));
    options.content.setOwned(window);
    // A tower can unlock a character while this dialog is open; the callback
    // re-reads GET /characters so the locked list and the combo update without
    // closing the window. The SafePointer clears itself when the dialog dies.
    refreshOpenAccountCharacters =
        [safeWindow = juce::Component::SafePointer<AccountWindow>(window)]
        {
            if (auto* accountWindow = safeWindow.getComponent())
                accountWindow->refreshCharacterCatalogue();
        };
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
        //: Strings the preset describes. Guitars use six; the bass presets
        //: four or five, and the remaining slots stay empty.
        int count;
        std::array<int, 6> notes;
    };

    //: Item id of the "Custom" entry. It must not collide with the preset ids
    //: (1..tuningPresetCount), which are handed out by initialiseTuningSelector.
    constexpr int customTuningItemId = 100;
    constexpr int tuningPresetCount = 9;

    //: Index of "BASS EADG" in the preset list. Used by the octave migration
    //: in initialiseTuningSelector.
    constexpr int bassEadgPresetIndex = 7;

    //: ComboBox item id for "EMPTY". MIDI notes start at 0 and the tuner only
    //: offers 23..96, so 1 cannot collide with a real open note.
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
            { "STANDARD",   6, { 40, 45, 50, 55, 59, 64 } },
            { "DROP D",     6, { 38, 45, 50, 55, 59, 64 } },
            { "EB",         6, { 39, 44, 49, 54, 58, 63 } },
            { "DADGAD",     6, { 38, 45, 50, 55, 57, 62 } },
            { "OPEN G",     6, { 38, 43, 50, 55, 59, 62 } },
            { "OPEN D",     6, { 38, 45, 50, 54, 57, 62 } },
            { "DROP C",     6, { 36, 43, 48, 53, 57, 62 } },

            // Basses sound an octave below a guitar. The MIDI parser moves the
            // whole chart into the selected range, so a guitar chart becomes
            // playable on these strings instead of asking for notes above the
            // bass neck.
            { "BASS EADG",  4, { 28, 33, 38, 43,
                                 GuitarTuning::emptyNote, GuitarTuning::emptyNote } },
            { "BASS BEADG", 5, { 23, 28, 33, 38, 43,
                                 GuitarTuning::emptyNote } },
        };

        return presets[juce::jlimit(0, tuningPresetCount - 1, index)];
    }
}

GuitarTuning::Tuning MainComponent::presetNotes(int presetIndex)
{
    // A preset carries its own string count: the guitar presets describe a
    // six-string guitar, the bass presets four or five strings. Selecting one
    // resets an extended-range instrument to that count; unused slots stay
    // empty so neither the tuner nor the MIDI parser can use them.
    GuitarTuning::Tuning tuning = GuitarTuning::standardTuning();
    const auto& preset = tuningPreset(presetIndex);

    tuning.count = juce::jlimit(1, 6, preset.count);

    for (int slot = 0; slot < 6; ++slot)
        tuning.notes[static_cast<size_t>(slot)] = preset.notes[static_cast<size_t>(slot)];

    for (int slot = tuning.count; slot < GuitarTuning::maxStrings; ++slot)
        tuning.notes[static_cast<size_t>(slot)] = GuitarTuning::emptyNote;

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
    // A four-string E-A-D-G tuning stored two octaves above the bass range is
    // the same instrument with the wrong octave: the tuner looks for E3 while
    // the string sounds E1, and the chart keeps guitar pitches above the bass
    // neck. Snap it to the playable bass octave once, so the instrument works
    // without editing four note combos. A real four-string guitar is tuned
    // E2-A2-D3-G3 and is left alone.
    if (currentTuning.count == 4
        && currentTuning.notes[0] >= 52
        && currentTuning.notes[0] % 12 == 4    // E
        && currentTuning.notes[1] % 12 == 9    // A
        && currentTuning.notes[2] % 12 == 2    // D
        && currentTuning.notes[3] % 12 == 7)   // G
    {
        auto bass = presetNotes(bassEadgPresetIndex);
        bass.fretCount = currentTuning.fretCount;
        setCurrentTuning(bass, presetName(bassEadgPresetIndex), true);
    }

    tuningPresetCombo.clear(juce::dontSendNotification);

    for (int i = 0; i < tuningPresetCount; ++i)
        tuningPresetCombo.addItem(presetName(i), i + 1);

    tuningPresetCombo.addItem("Custom", customTuningItemId);
    tuningPresetCombo.setTooltip(
        "Open-string tuning. The chart and the tablature are moved into the "
        "selected range, so a guitar song stays playable on a bass "
        "(BASS EADG / BASS BEADG).");
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

    // A preset carries its own string count: a guitar preset resets an
    // extended-range instrument back to six slots, a bass preset sets four or
    // five. The neck length is a property of the player's instrument, not of
    // the tuning, so it stays.
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

//==============================================================================
// In-app feedback form.
//
// A small modal dialog with a multi-line message and an optional attachment.
// The reply address is taken from the signed-in profile on the server, so the
// form does not ask for it. Sending is asynchronous: the callback posts the
// JSON to /feedback and the reply is reported through a message box.
namespace
{
    class FeedbackWindow final : public juce::Component
    {
    public:
        using SendCallback = std::function<void (juce::String message,
                                                 juce::String attachmentName,
                                                 juce::String attachmentData)>;

        explicit FeedbackWindow (SendCallback sendCallback)
            : onSend (std::move (sendCallback))
        {
            setSize (520, 420);

            heading.setText ("FEEDBACK", juce::dontSendNotification);
            heading.setFont (GameUi::heading (24.0f));
            heading.setColour (juce::Label::textColourId, UiColors::text);
            addAndMakeVisible (heading);

            hint.setText ("Tell us what should be improved. The message goes "
                          "straight to the developers.",
                          juce::dontSendNotification);
            hint.setFont (GameUi::font (12.0f));
            hint.setColour (juce::Label::textColourId, UiColors::muted);
            addAndMakeVisible (hint);

            messageLabel.setText ("MESSAGE", juce::dontSendNotification);
            messageLabel.setFont (GameUi::eyebrow (9.0f));
            messageLabel.setColour (juce::Label::textColourId, UiColors::accent);
            addAndMakeVisible (messageLabel);

            messageEditor.setMultiLine (true, true);
            messageEditor.setReturnKeyStartsNewLine (true);
            messageEditor.setTextToShowWhenEmpty ("What happened? What should work better?",
                                                  UiColors::muted);
            messageEditor.setColour (juce::TextEditor::backgroundColourId,
                                     UiColors::surface2);
            messageEditor.setColour (juce::TextEditor::outlineColourId,
                                     UiColors::border);
            messageEditor.setColour (juce::TextEditor::textColourId,
                                     UiColors::text);
            addAndMakeVisible (messageEditor);

            attachmentLabel.setText ("ATTACHMENT (OPTIONAL)",
                                     juce::dontSendNotification);
            attachmentLabel.setFont (GameUi::eyebrow (9.0f));
            attachmentLabel.setColour (juce::Label::textColourId, UiColors::muted);
            addAndMakeVisible (attachmentLabel);

            attachmentButton.setButtonText ("ATTACH FILE");
            attachmentButton.setColour (juce::TextButton::buttonColourId,
                                        UiColors::surface2);
            attachmentButton.setColour (juce::TextButton::textColourOffId,
                                        UiColors::text);
            attachmentButton.setMouseCursor (juce::MouseCursor::PointingHandCursor);
            attachmentButton.onClick = [this] { chooseAttachment(); };
            addAndMakeVisible (attachmentButton);

            attachmentName.setText ("No file selected",
                                    juce::dontSendNotification);
            attachmentName.setFont (GameUi::font (11.0f));
            attachmentName.setColour (juce::Label::textColourId, UiColors::muted);
            addAndMakeVisible (attachmentName);

            status.setText ({}, juce::dontSendNotification);
            status.setFont (GameUi::font (11.0f, true));
            status.setColour (juce::Label::textColourId, UiColors::red);
            addAndMakeVisible (status);

            sendButton.setButtonText ("SEND");
            sendButton.setColour (juce::TextButton::buttonColourId,
                                  UiColors::accent);
            sendButton.setColour (juce::TextButton::textColourOffId,
                                  UiColors::onAccent);
            sendButton.onClick = [this] { submit(); };
            addAndMakeVisible (sendButton);

            cancelButton.setButtonText ("CANCEL");
            cancelButton.setColour (juce::TextButton::buttonColourId,
                                    UiColors::surface2);
            cancelButton.setColour (juce::TextButton::textColourOffId,
                                    UiColors::text);
            cancelButton.onClick = [this]
            {
                if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
                    dialog->exitModalState (0);
            };
            addAndMakeVisible (cancelButton);

            messageEditor.grabKeyboardFocus();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (UiColors::background);

            GameUi::drawCard (g, getLocalBounds().toFloat().reduced (10.0f), 18.0f);

            g.setColour (UiColors::accent);
            g.fillRoundedRectangle (20.0f, 12.0f, 76.0f, 3.0f, 1.5f);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (30, 24);

            heading.setBounds (area.removeFromTop (30));
            hint.setBounds (area.removeFromTop (34));
            area.removeFromTop (8);

            messageLabel.setBounds (area.removeFromTop (16));
            messageEditor.setBounds (area.removeFromTop (132));
            area.removeFromTop (10);

            attachmentLabel.setBounds (area.removeFromTop (16));
            attachmentButton.setBounds (area.removeFromTop (28).removeFromLeft (130));
            attachmentName.setBounds (area.getX() + 140, attachmentButton.getY(),
                                      area.getWidth() - 140, 28);
            area.removeFromTop (36);

            status.setBounds (area.removeFromTop (18));

            auto buttons = area.removeFromBottom (36);
            cancelButton.setBounds (buttons.removeFromRight (110));
            buttons.removeFromRight (10);
            sendButton.setBounds (buttons.removeFromRight (110));
        }

    private:
        void chooseAttachment()
        {
            // 5 MB is the server-side limit; check here too so the user gets an
            // instant answer instead of a failed send.
            chooser = std::make_unique<juce::FileChooser> ("Attach a file",
                                                           juce::File(),
                                                           "*");

            juce::Component::SafePointer<FeedbackWindow> safeThis (this);

            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectFiles,
                                  [safeThis] (const juce::FileChooser& fc)
                                  {
                                      if (safeThis == nullptr)
                                          return;

                                      const auto file = fc.getResult();

                                      if (! file.existsAsFile())
                                          return;

                                      if (file.getSize() > kMaxAttachmentBytes)
                                      {
                                          safeThis->status.setText (
                                              "The file is too large (5 MB max).",
                                              juce::dontSendNotification);
                                          return;
                                      }

                                      safeThis->attachment = file;
                                      safeThis->status.setText ({}, juce::dontSendNotification);
                                      safeThis->attachmentName.setText (
                                          file.getFileName() + "  ("
                                              + juce::File::descriptionOfSizeInBytes (
                                                    file.getSize())
                                              + ")",
                                          juce::dontSendNotification);
                                  });
        }

        void submit()
        {
            const auto message = messageEditor.getText().trim();

            if (message.isEmpty())
            {
                status.setText ("Write a message first.",
                                juce::dontSendNotification);
                return;
            }

            juce::String name;
            juce::String data;

            if (attachment.existsAsFile())
            {
                if (attachment.getSize() > kMaxAttachmentBytes)
                {
                    status.setText ("The file is too large (5 MB max).",
                                    juce::dontSendNotification);
                    return;
                }

                juce::MemoryBlock block;

                if (! attachment.loadFileAsData (block))
                {
                    status.setText ("Could not read the attached file.",
                                    juce::dontSendNotification);
                    return;
                }

                name = attachment.getFileName();
                // Plain RFC 4648 base64: MemoryBlock::toBase64Encoding() writes
                // a "<size>." prefix (JUCE's own format), which the feedback
                // API rejects as invalid base64.
                data = juce::Base64::toBase64 (block.getData(), block.getSize());
            }

            if (onSend != nullptr)
                onSend (message, name, data);

            if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
                dialog->exitModalState (1);
        }

        static constexpr juce::int64 kMaxAttachmentBytes = 5 * 1024 * 1024;

        SendCallback onSend;
        juce::File attachment;
        std::unique_ptr<juce::FileChooser> chooser;
        juce::Label heading, hint, messageLabel, attachmentLabel, attachmentName, status;
        juce::TextEditor messageEditor;
        juce::TextButton sendButton, cancelButton, attachmentButton;
    };
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
    practiceModeButton.setTooltip("Study the song note by note. No score, no rating, no XP.");
    practiceModeButton.onClick = [this] { setPracticeMode(! practiceMode); };
    addAndMakeVisible(practiceModeButton);

    // Guitar Duels: the entry point on the song screen. The menu picks one of
    // the three bots; online matchmaking plugs into the same flow once the
    // server rooms are available.
    duelModeButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    duelModeButton.setColour(juce::TextButton::textColourOffId, UiColors::accent);
    duelModeButton.setTooltip("Duel another player or one of the three bots on the selected song.");
    duelModeButton.onClick = [this] { showDuelModeMenu(); };
    addAndMakeVisible(duelModeButton);

    // Open online rooms under the rating. Refresh is manual (the button) and
    // also runs on a short timer while the song screen is visible.
    duelLobby = std::make_unique<DuelLobbyComponent>();
    duelLobby->onCreate = [this]
    {
        if (onlineAccessToken.isEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync (
                juce::MessageBoxIconType::InfoIcon, "Online",
                "Sign in to create an online room.", "OK");
            return;
        }

        openCreateDuelRoomDialog();
    };
    duelLobby->onRefresh = [this] { refreshDuelLobby(); };
    duelLobby->onJoin = [this] (const juce::String& lobbyId, bool hasPassword)
    {
        if (! hasPassword)
        {
            joinDuelLobbyRoom (lobbyId, {});
            return;
        }

        auto* prompt = new juce::AlertWindow ("Password protected",
                                              "Enter the room password:",
                                              juce::AlertWindow::NoIcon);
        prompt->addTextEditor ("password", juce::String(), "Password");
        prompt->getTextEditor ("password")->setPasswordCharacter (0x2022);
        prompt->addButton ("JOIN", 1, juce::KeyPress (juce::KeyPress::returnKey));
        prompt->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        juce::Component::SafePointer<juce::AlertWindow> safePrompt (prompt);
        juce::Component::SafePointer<MainComponent> safeThis (this);

        prompt->enterModalState (true, juce::ModalCallbackFunction::create (
            [safePrompt, safeThis, lobbyId] (int result)
            {
                if (result != 1 || safeThis == nullptr || safePrompt == nullptr)
                    return;

                safeThis->joinDuelLobbyRoom (
                    lobbyId, safePrompt->getTextEditorContents ("password"));
            }), true);
    };
    addAndMakeVisible(*duelLobby);
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

    tunerReturnButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    tunerReturnButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
    tunerReturnButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    tunerReturnButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    tunerReturnButton.onClick = [this] { closeLobbyTuner(); };
    addChildComponent(tunerReturnButton);

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
            audiblePlayheadBeat.store(0.0, std::memory_order_relaxed);
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
        // The slider is the single source of truth for the initial level.
        volume.store(static_cast<float>(slider.getValue()), std::memory_order_relaxed);
        label.setText(name + " VOL", juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centredLeft);
        label.setColour(juce::Label::textColourId, UiColors::text);
        label.setFont(GameUi::font(10.5f, true));
        addAndMakeVisible(label);
    };
    setupBackingChannel(liveGuitarButton, liveGuitarVolumeSlider, liveGuitarVolumeLabel,
                        liveGuitarEnabled, liveGuitarVolume, "LIVE GTR", 0.9f, 2.5);
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
            // The result screen has two actions for an online match (GO TO
            // LOBBY / LEAVE LOBBY) and the campaign/bot actions otherwise.
            // Without this the finished duel state stayed on screen over the
            // next run.
            if (gameState == GameState::Finished)
            {
                if (campaignFightActive)
                {
                    if (campaignStageWon && campaignStage < campaignStageCount())
                    {
                        // Victory: straight to the next opponent, landing on
                        // the Ready screen without the tuner step.
                        campaignStageWon = false;
                        startCampaignStage (true);
                    }
                    else if (campaignStageWon)
                    {
                        campaignFightActive = false;
                        returnToSongSelect();
                    }
                    else
                    {
                        // RETRY after a loss: the run was already reset to
                        // stage one, so restart there (tuner included).
                        campaignStageWon = false;
                        startCampaignStage (false);
                    }

                    return;
                }

                if (duelOnlinePhase != DuelOnlinePhase::None)
                {
                    // GO TO LOBBY: back to the same room, chat and song kept by
                    // the server. A rematch is never offered here: the host
                    // picks the next song in the room.
                    goToOnlineDuelLobby();
                    return;
                }

                if (duelMatch != nullptr)
                {
                    requestDuelWithBot (static_cast<int> (duelPendingLevel));
                    return;
                }
            }

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
            // LEAVE LOBBY on an online result drops the room and lands on the
            // Online section (the lobby list). Leaving the room cannot fail the
            // navigation: a room that is already gone answers 404/409.
            if (gameState == GameState::Finished
                && duelOnlinePhase == DuelOnlinePhase::Finished
                && duelOnlineMatchId.isNotEmpty())
            {
                leaveOnlineDuelLobby();
                return;
            }

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

    tempoLabel.setText("TEMPO", juce::dontSendNotification);
    tempoLabel.setFont(GameUi::eyebrow(9.0f));
    tempoLabel.setColour(juce::Label::textColourId, UiColors::muted);
    tempoLabel.setJustificationType(juce::Justification::centredLeft);
    tempoLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(tempoLabel);

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

    leaderboardTitleLabel.setText("GUITAR SKILLS", juce::dontSendNotification);
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
        "#", 1, 42, 36, 70, juce::TableHeaderComponent::notSortable);
    leaderboardTable.getHeader().addColumn(
        "PLAYER", 2, 220, 120, -1, juce::TableHeaderComponent::notSortable);
    leaderboardTable.getHeader().addColumn(
        "LEVEL", 3, 70, 60, 110, juce::TableHeaderComponent::notSortable);
    leaderboardTable.getHeader().addColumn(
        "SKILLS", 4, 84, 70, 130, juce::TableHeaderComponent::notSortable);
    leaderboardTable.getHeader().addColumn(
        "DUELS", 5, 118, 96, 170, juce::TableHeaderComponent::notSortable);
    leaderboardTable.getHeader().addColumn(
        "STATUS", 6, 78, 66, 110, juce::TableHeaderComponent::notSortable);

    leaderboardModel->onRowClicked = [this](const juce::String& userId)
    {
        openPublicProfile(userId);
    };
    // Row clicks arrive through a direct mouse listener (see mouseUp()).
    leaderboardTable.addMouseListener (this, true);
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

    // Fullscreen toggle lives in Settings; it controls the whole top-level
    // window without changing the 16:9 windowed default.
    fullScreenButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    fullScreenButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
    fullScreenButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    fullScreenButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    fullScreenButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    fullScreenButton.setTooltip("Switch between the 16:9 window and fullscreen.");
    fullScreenButton.onClick = [this] { toggleFullScreen(); };
    fullScreenButton.setVisible(false);
    addAndMakeVisible(fullScreenButton);

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

    // Donations: one click opens the public donation page in the default
    // browser. Same accent fill as START SONG so it reads as a primary action
    // in the header instead of a faded secondary button.
    supportButton.setButtonText("DONATION");
    supportButton.setColour(juce::TextButton::buttonColourId,
                            UiColors::accent);
    supportButton.setColour(juce::TextButton::buttonOnColourId,
                            UiColors::accentStrong);
    supportButton.setColour(juce::TextButton::textColourOffId,
                            UiColors::onAccent);
    supportButton.setColour(juce::TextButton::textColourOnId,
                            UiColors::onAccent);
    supportButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    supportButton.setTooltip("Open the Fretfight donation page in your browser");
    supportButton.onClick = []
    {
        // Same origin as every other link in the client, so a future domain
        // change only has to touch ApiClient::serverOrigin.
        juce::URL(juce::String(ApiClient::serverOrigin) + "/support")
            .launchInDefaultBrowser();
    };
    addAndMakeVisible(supportButton);

    // In-app feedback: the message is emailed to the developers through the
    // server (the client never talks SMTP).
    feedbackButton.setButtonText("FEEDBACK");
    feedbackButton.setColour(juce::TextButton::buttonColourId,
                             UiColors::surface2);
    feedbackButton.setColour(juce::TextButton::buttonOnColourId,
                             UiColors::surface2);
    feedbackButton.setColour(juce::TextButton::textColourOffId,
                             UiColors::text);
    feedbackButton.setColour(juce::TextButton::textColourOnId,
                             UiColors::text);
    feedbackButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    feedbackButton.setTooltip("Write a message to the FretFight team");
    feedbackButton.onClick = [this]
    {
        auto* content = new FeedbackWindow(
            [this] (juce::String message, juce::String attachmentName,
                    juce::String attachmentData)
            {
                juce::DynamicObject::Ptr payload = new juce::DynamicObject();
                payload->setProperty("message", message);
                payload->setProperty("username", currentUsername);
                payload->setProperty("app_version", ProjectInfo::versionString);

                if (attachmentName.isNotEmpty() && attachmentData.isNotEmpty())
                {
                    payload->setProperty("attachment_name", attachmentName);
                    payload->setProperty("attachment_data", attachmentData);
                }

                juce::Component::SafePointer<MainComponent> safeThis (this);

                apiClient.postJson(
                    "/feedback", juce::var (payload.get()), onlineAccessToken,
                    120000, [safeThis] (ApiClient::Response reply)
                    {
                        if (safeThis == nullptr)
                            return;

                        const bool sent = reply.transportOk
                                          && reply.statusCode >= 200
                                          && reply.statusCode < 300;

                        juce::String detail;

                        if (reply.isJsonObject())
                        {
                            const auto value = reply.json.getProperty ("detail", juce::var());

                            if (const auto* items = value.getArray())
                            {
                                // FastAPI validation errors arrive as a list of
                                // {loc, msg, type}; show the first readable msg
                                // instead of JUCE's "[Array]" placeholder.
                                if (! items->isEmpty())
                                    detail = items->getReference (0)
                                                 .getProperty ("msg", juce::var())
                                                 .toString();
                            }
                            else
                            {
                                detail = value.toString();
                            }
                        }

                        juce::AlertWindow::showMessageBoxAsync(
                            sent ? juce::MessageBoxIconType::InfoIcon
                                 : juce::MessageBoxIconType::WarningIcon,
                            sent ? "FEEDBACK SENT" : "FEEDBACK NOT SENT",
                            sent ? "Thanks! Your message has been delivered."
                                 : (reply.transportOk
                                        ? (detail.isNotEmpty()
                                               ? detail
                                               : juce::String ("The server could not "
                                                               "send the message. Please "
                                                               "try again later."))
                                        : "No connection to the server. Check your "
                                          "internet connection and try again."),
                            "OK");
                    });
            });

        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (content);
        options.dialogTitle = "Feedback";
        options.componentToCentreAround = this;
        options.dialogBackgroundColour = UiColors::background;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;
        options.launchAsync();
    };
    addAndMakeVisible(feedbackButton);

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
        matchOriginSection = currentSection;

        if (currentSection == AppSection::OnlineDuel)
        {
            campaignFightActive = false;

            if (onlineAccessToken.isEmpty())
            {
                juce::AlertWindow::showMessageBoxAsync (
                    juce::MessageBoxIconType::InfoIcon, "Online",
                    "Sign in to create an online room.", "OK");
            }
            else
            {
                openCreateDuelRoomDialog();
            }
        }
        else
        {
            campaignFightActive = false;
            startSelectedSong();
        }
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
        GameUi::heading(26.0f));

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
        GameUi::heading(30.0f));

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
                    },
                    [this]() -> juce::String
                    {
                        const float rms =
                            analysisRms.load(std::memory_order_relaxed);
                        const float pitch =
                            analysisPitch.load(std::memory_order_relaxed);
                        const auto level = juce::String("A ")
                            + juce::String(
                                  juce::Decibels::gainToDecibels(rms, -90.0f), 1)
                            + " dBFS";
                        const auto pitchText = pitch > 20.0f
                            ? juce::String(pitch, 1) + " Hz"
                            : juce::String("no pitch");

                        if (! guitarInputOnly.load(std::memory_order_relaxed))
                            return "Filter off - " + level + ", " + pitchText;

                        const int decision =
                            gateDecision.load(std::memory_order_relaxed);

                        if (decision == 0)
                            return level + ", " + pitchText + " - no attack";

                        const auto attack = juce::String(
                            gateAttackMs.load(std::memory_order_relaxed), 1);
                        const auto climb = juce::String(
                            gateClimbFraction.load(std::memory_order_relaxed), 2);
                        const auto rise = juce::String(
                            gateRiseDb.load(std::memory_order_relaxed), 1);

                        if (decision == 1)
                            return level + " - PLUCK attack " + attack
                                   + " ms, climb " + climb;

                        if (decision == 2)
                            return level + " - REJECTED attack " + attack
                                   + " ms, climb " + climb + ", rise " + rise
                                   + " dB";

                        return level + " - WAIT attack at the frame edge";
                    },
                    preferredMicChannel.load(std::memory_order_relaxed),
                    [this](int channel)
                    {
                        setPreferredMicChannel(channel);
                    },
                    [this]() -> float
                    {
                        return voiceChat != nullptr ? voiceChat->getInputLevel() : 0.0f;
                    },
                    getTimingOffsetMs(),
                    [this](int offset)
                    {
                        setTimingOffsetMs(offset);
                    },
                    [this]() -> juce::String
                    {
                        return getLatencyDiagnosticsText();
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

    // Persistent hub navigation. It is added last so it stays above the
    // content; the section components themselves sit behind it.
    sectionSidebar = std::make_unique<SectionSidebar>();
    sectionSidebar->onSectionSelected = [this] (AppSection section)
    {
        setSection (section);
    };
    sectionSidebar->onNotificationsSelected = [this] { openNotifications(); };
    sectionSidebar->setUser (currentUsername, progressionLevel);
    addAndMakeVisible (*sectionSidebar);
    sectionSidebar->toFront (false);

    resized();
}

void MainComponent::mouseUp (const juce::MouseEvent& event)
{
    if (showAuthScreen || currentSection != AppSection::Home)
        return;

    // Leaderboard rows: JUCE 9's TableListBox no longer forwards row clicks to
    // TableListBoxModel::cellClicked(), so the table is listened to directly.
    if (leaderboardTable.isVisible() && leaderboardModel != nullptr
        && event.eventComponent != nullptr
        && (event.eventComponent == &leaderboardTable
            || leaderboardTable.isParentOf (event.eventComponent)))
    {
        const auto local = leaderboardTable.getLocalPoint (
            nullptr, event.getScreenPosition()).roundToInt();

        if (leaderboardTable.getLocalBounds().contains (local)
            && local.y >= leaderboardTable.getHeaderHeight())
        {
            const int row = leaderboardTable.getRowContainingPosition (local.x, local.y);

            if (row >= 0)
                leaderboardModel->openRow (row);
        }
    }

    // Podium cards on HOME are painted, not components; paint() records their
    // bounds so a click opens the same profile page as a leaderboard row.
    for (size_t i = 0; i < podiumHitBounds.size(); ++i)
    {
        if (podiumUserIds[i].isNotEmpty()
            && podiumHitBounds[i].contains (event.getPosition()))
        {
            openPublicProfile (podiumUserIds[i]);
            return;
        }
    }
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

//==============================================================================
// Hub sections: embedded Profile and Settings
//==============================================================================

void MainComponent::ensureProfileSection()
{
    if (onlineAccessToken.isEmpty() || currentUsername.isEmpty())
    {
        currentSection = AppSection::Home;
        showAuthScreen = true;

        if (sectionSidebar != nullptr)
            sectionSidebar->setVisible (false);

        resized();
        repaint();
        overlayComponent.repaint();
        return;
    }

    if (profileContent != nullptr)
        return;

    AccountWindow::Callbacks callbacks;
    callbacks.onProfileUpdated =
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        (const juce::String& username, const juce::String&)
        {
            if (auto* self = safeThis.getComponent())
            {
                self->currentUsername = username;
                self->currentUserLabel.setText (username, juce::dontSendNotification);

                // Confirmation for the player: the profile save succeeded.
                self->uiSounds.play ("save.success");

                if (self->sectionSidebar != nullptr)
                    self->sectionSidebar->setUser (self->currentUsername,
                                                   self->progressionLevel);

                self->resized();
                self->repaint();
            }
        };
    callbacks.onLogout =
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* self = safeThis.getComponent())
                self->logoutCurrentUser();
        };
    callbacks.onSessionExpired =
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* self = safeThis.getComponent())
                self->logoutCurrentUser();
        };
    callbacks.onCharacterSelected =
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        (int id, juce::String name, juce::String sheetUrl)
        {
            if (auto* self = safeThis.getComponent())
                self->setSelectedCharacter (id, name, sheetUrl, false);
        };

    profileContent = std::make_unique<AccountWindow> (
        apiClient, onlineAccessToken, std::move (callbacks));
    profileContent->setSize (1000, 860);

    profileViewport = std::make_unique<juce::Viewport>();
    profileViewport->setViewedComponent (profileContent.get(), false);
    // The account window has its own page scrollbar; this wrapper only hosts
    // it, so it must not add another one.
    profileViewport->setScrollBarsShown (false, false);
    addChildComponent (*profileViewport);
}

void MainComponent::ensureFriendsSection()
{
    if (friendsContent == nullptr)
    {
        if (onlineAccessToken.isEmpty())
            return;

        auto panel = std::make_unique<FriendsPanel> (apiClient, onlineAccessToken);
        panel->onOpenProfile =
            [safeThis = juce::Component::SafePointer<MainComponent> (this)]
            (const juce::String& userId)
            {
                if (auto* self = safeThis.getComponent())
                    self->openPublicProfile (userId);
            };
        friendsContent = std::move (panel);

        friendsViewport = std::make_unique<juce::Viewport>();
        friendsViewport->setViewedComponent (friendsContent.get(), false);
        friendsViewport->setScrollBarsShown (false, false);
        addChildComponent (*friendsViewport);
    }

    if (auto* panel = dynamic_cast<FriendsPanel*> (friendsContent.get()))
        panel->refreshAll();
}

void MainComponent::ensureMessagesSection()
{
    if (messagesContent == nullptr)
    {
        if (onlineAccessToken.isEmpty())
            return;

        auto panel = std::make_unique<MessagesPanel> (apiClient, onlineAccessToken);
        panel->onOpenProfile =
            [safeThis = juce::Component::SafePointer<MainComponent> (this)]
            (const juce::String& userId)
            {
                if (auto* self = safeThis.getComponent())
                    self->openPublicProfile (userId);
            };
        panel->onUnreadChanged =
            [safeThis = juce::Component::SafePointer<MainComponent> (this)] (int unread)
            {
                if (auto* self = safeThis.getComponent())
                    if (self->sectionSidebar != nullptr)
                        self->sectionSidebar->setUnreadMessages (unread);
            };
        messagesContent = std::move (panel);

        messagesViewport = std::make_unique<juce::Viewport>();
        messagesViewport->setViewedComponent (messagesContent.get(), false);
        messagesViewport->setScrollBarsShown (false, false);
        addChildComponent (*messagesViewport);
    }

    if (auto* panel = dynamic_cast<MessagesPanel*> (messagesContent.get()))
        panel->refreshAll();
}

void MainComponent::openMessagesFor (const juce::String& userId,
                                     const juce::String& displayName)
{
    if (userId.isEmpty() || onlineAccessToken.isEmpty())
        return;

    closePublicProfile();
    setSection (AppSection::Messages);
    ensureMessagesSection();

    if (auto* panel = dynamic_cast<MessagesPanel*> (messagesContent.get()))
        panel->openConversation (userId, displayName);

    resized();
    repaint();
}

void MainComponent::ensureSettingsSection()
{    if (settingsContent != nullptr)
        return;

    auto panel = std::make_unique<AudioSettingsPanel> (
        deviceManager,
        preferredInputChannel.load (std::memory_order_relaxed),
        [this] (int channel) { setPreferredInputChannel (channel); },
        guitarInputOnly.load (std::memory_order_relaxed),
        [this] (bool only) { setGuitarInputOnly (only); },
        backgroundMusicVolume.load (std::memory_order_relaxed),
        backgroundMusicEnabled.load (std::memory_order_relaxed),
        [this] (float volume) { setBackgroundMusicVolume (volume); },
        [this] (bool enabled) { setBackgroundMusicEnabled (enabled); },
        [this]() -> juce::String
        {
            const float rms = analysisRms.load (std::memory_order_relaxed);
            const float pitch = analysisPitch.load (std::memory_order_relaxed);
            const auto level = juce::String ("A ")
                + juce::String (juce::Decibels::gainToDecibels (rms, -90.0f), 1)
                + " dBFS";
            const auto pitchText = pitch > 20.0f
                ? juce::String (pitch, 1) + " Hz"
                : juce::String ("no pitch");

            if (! guitarInputOnly.load (std::memory_order_relaxed))
                return "Filter off - " + level + ", " + pitchText;

            const int decision = gateDecision.load (std::memory_order_relaxed);

            if (decision == 0)
                return level + ", " + pitchText + " - no attack";

            const auto attack = juce::String (
                gateAttackMs.load (std::memory_order_relaxed), 1);
            const auto climb = juce::String (
                gateClimbFraction.load (std::memory_order_relaxed), 2);
            const auto rise = juce::String (
                gateRiseDb.load (std::memory_order_relaxed), 1);

            if (decision == 1)
                return level + " - PLUCK attack " + attack
                       + " ms, climb " + climb;

            if (decision == 2)
                return level + " - REJECTED attack " + attack
                       + " ms, climb " + climb + ", rise " + rise + " dB";

            return level + " - WAIT attack at the frame edge";
        },
        preferredMicChannel.load(std::memory_order_relaxed),
        [this](int channel)
        {
            setPreferredMicChannel(channel);
        },
        [this]() -> float
        {
            return voiceChat != nullptr ? voiceChat->getInputLevel() : 0.0f;
        },
        getTimingOffsetMs(),
        [this](int offset)
        {
            setTimingOffsetMs(offset);
        },
        [this]() -> juce::String
        {
            return getLatencyDiagnosticsText();
        });

    settingsContent = std::move (panel);
    settingsContent->setSize (760, 680);

    // UI sound preferences: the block lives in the audio settings panel but
    // the values are owned by MainComponent and persisted in app properties.
    if (auto* audioPanel = dynamic_cast<AudioSettingsPanel*> (settingsContent.get()))
    {
        auto* settings = appProperties.getUserSettings();
        const bool uiEnabled = settings != nullptr
                                   ? settings->getBoolValue ("UiSoundsEnabled", true)
                                   : true;
        const float uiVolume = settings != nullptr
                                   ? static_cast<float> (settings->getDoubleValue (
                                         "UiSoundsVolume", 0.8))
                                   : 0.8f;
        const bool uiNav = settings != nullptr
                               ? settings->getBoolValue ("UiSoundsNavigation", true)
                               : true;
        const bool uiActions = settings != nullptr
                                   ? settings->getBoolValue ("UiSoundsActions", true)
                                   : true;
        const bool uiSystem = settings != nullptr
                                  ? settings->getBoolValue ("UiSoundsSystem", true)
                                  : true;

        audioPanel->configureUiSounds (
            uiEnabled, uiVolume, uiNav, uiActions, uiSystem,
            [safeThis = juce::Component::SafePointer<MainComponent> (this)]
            (bool enabled, float volume, bool navigation, bool actions, bool system)
            {
                auto* self = safeThis.getComponent();

                if (self == nullptr)
                    return;

                if (auto* store = self->appProperties.getUserSettings())
                {
                    store->setValue ("UiSoundsEnabled", enabled);
                    store->setValue ("UiSoundsVolume", static_cast<double> (volume));
                    store->setValue ("UiSoundsNavigation", navigation);
                    store->setValue ("UiSoundsActions", actions);
                    store->setValue ("UiSoundsSystem", system);
                    store->saveIfNeeded();
                }

                self->uiSounds.setPreferences (enabled, volume, navigation,
                                               actions, system);
            });
    }

    settingsViewport = std::make_unique<juce::Viewport>();
    settingsViewport->setViewedComponent (settingsContent.get(), false);
    settingsViewport->setScrollBarsShown (true, true);
    addChildComponent (*settingsViewport);
}
