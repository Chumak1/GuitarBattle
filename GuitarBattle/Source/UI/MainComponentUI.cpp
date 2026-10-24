#include "../MainComponent.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include "GameStyle.h"

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
    repaint();
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
        hitLabel.setFont(juce::FontOptions().withHeight(feedbackBaseFontSize).withStyle("Bold"));
        hitLabel.setColour(juce::Label::textColourId, UiColors::blue);
        repaint();
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
    hitLabel.setFont(juce::FontOptions().withHeight(fontSize).withStyle("Bold"));
    hitLabel.setColour(juce::Label::textColourId, feedbackColour.withAlpha(alpha));
    repaint();
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
            + " and choose a new password of at least 10 characters.",
        juce::AlertWindow::NoIcon);

    prompt->addTextEditor("code", "", "6-digit reset code");
    prompt->addTextEditor("new_password", "", "New password (10+ characters)", true);
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

        if (newPassword.length() < 10)
        {
            showError("Reset failed", "The new password must be at least 10 characters.");
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
    const auto row = juce::Rectangle<int>(0, 0, width, height).reduced(2, 3);

    g.setColour(rowIsSelected ? UiColors::accent.withAlpha(0.16f)
                              : UiColors::surface2.withAlpha(0.55f));
    g.fillRoundedRectangle(row.toFloat(), 12.0f);
    g.setColour(rowIsSelected ? UiColors::accent.withAlpha(0.55f) : UiColors::border);
    g.drawRoundedRectangle(row.toFloat().reduced(0.5f), 12.0f, 1.0f);

    if (rowIsSelected)
    {
        const juce::Rectangle<int> selectionBar(row.getX() + 2, row.getY() + 11,
                                                4, juce::jmax(4, row.getHeight() - 22));
        g.setColour(UiColors::accent);
        g.fillRoundedRectangle(selectionBar.toFloat(), 2.0f);
    }

    // The best score sits on the right; the metadata column shrinks with the card.
    const int bestWidth = juce::jlimit(84, 152, width / 3);
    const auto bestArea = row.withTrimmedRight(14).removeFromRight(bestWidth);
    auto textArea = row.withTrimmedLeft(16).withTrimmedRight(bestWidth + 22);

    juce::StringArray meta;
    if (song.artist.isNotEmpty())
        meta.add(song.artist);
    meta.add(juce::String(juce::roundToInt(song.bpm)) + " BPM");
    meta.add(song.downloaded ? "ONLINE / CACHED" : "ONLINE");

    g.setColour(UiColors::text);
    g.setFont(GameUi::font(14.0f, true));
    g.drawFittedText(song.title,
                     textArea.removeFromTop(juce::jmax(18, height * 3 / 5)).toNearestInt(),
                     juce::Justification::centredLeft, 1, 0.8f);

    g.setColour(UiColors::muted);
    g.setFont(GameUi::font(10.5f));
    g.drawFittedText(meta.joinIntoString("   |   "),
                     textArea.toNearestInt(),
                     juce::Justification::centredLeft, 1, 0.7f);

    const int best = owner.getBestScoreForSong(song.slug, song.title);

    // Fitted text with a horizontal-scale floor: never truncate the digits.
    g.setColour(best > 0 ? UiColors::accent : UiColors::muted.withAlpha(0.55f));
    g.setFont(GameUi::font(10.0f, true, 0.55f));
    g.drawFittedText(best > 0 ? "BEST: " + juce::String(best) : juce::String("NO SCORE"),
                     bestArea, juce::Justification::centredRight, 1, 0.65f);
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

void MainComponent::paint(juce::Graphics& g)
{
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

    auto drawBrand = [&g](float x, float y, bool withBadge)
    {
        g.setColour(UiColors::accent);
        g.fillRoundedRectangle(x, y, 32.0f, 32.0f, 10.0f);
        g.setColour(UiColors::onAccent);
        g.setFont(GameUi::heading(14.0f));
        g.drawText("FF", juce::Rectangle<float>(x, y, 32.0f, 32.0f),
                   juce::Justification::centred, false);

        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(16.0f));
        g.drawText("FRETFIGHT",
                   juce::Rectangle<float>(x + 44.0f, y + 1.0f, 190.0f, 30.0f),
                   juce::Justification::centredLeft, false);

        if (withBadge)
            GameUi::drawPill(g,
                             juce::Rectangle<float>(x + 184.0f, y + 7.0f, 82.0f, 18.0f),
                             "PLAYER",
                             UiColors::accent,
                             false);
    };

    //==========================================================================
    // Authentication / landing screen
    //==========================================================================

    if (gameState == GameState::SongSelect && showAuthScreen)
    {
        drawBrand(32.0f, 26.0f, true);

        GameUi::drawPill(g,
                         juce::Rectangle<float>(width - 194.0f, 31.0f, 162.0f, 22.0f),
                         "DESKTOP CLIENT",
                         UiColors::muted,
                         false);

        const float cardWidth = juce::jmin(560.0f, width - 48.0f);
        const float cardHeight = 400.0f;
        const float cardX = (width - cardWidth) * 0.5f;
        const float cardY = juce::jmax(150.0f, (height - cardHeight) * 0.5f);
        const juce::Rectangle<float> card(cardX, cardY, cardWidth, cardHeight);

        GameUi::drawCard(g, card, 24.0f, true);
        g.setColour(UiColors::accent);
        g.fillRoundedRectangle(cardX + 24.0f, cardY, 78.0f, 3.0f, 1.5f);

        const float iconSize = 58.0f;
        const juce::Rectangle<float> icon(cardX + (cardWidth - iconSize) * 0.5f,
                                          cardY + 34.0f, iconSize, iconSize);
        g.setColour(UiColors::accent.withAlpha(0.08f));
        g.fillEllipse(icon.expanded(8.0f));
        g.setColour(UiColors::surface2);
        g.fillEllipse(icon);
        g.setColour(UiColors::accent.withAlpha(0.55f));
        g.drawEllipse(icon, 1.2f);
        g.setColour(UiColors::accent);
        g.setFont(GameUi::heading(20.0f));
        g.drawText("FF", icon, juce::Justification::centred, false);

        g.setColour(UiColors::accent);
        g.setFont(GameUi::eyebrow(10.0f));
        g.drawText("PLAYER ACCESS",
                   juce::Rectangle<float>(cardX, cardY + 101.0f, cardWidth, 17.0f),
                   juce::Justification::centred, false);

        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(13.0f));
        g.drawText("Sign in to sync your scores, unlock online songs and track your global rating.",
                   juce::Rectangle<float>(cardX + 44.0f, cardY + 168.0f,
                                          cardWidth - 88.0f, 36.0f),
                   juce::Justification::centred, true);

        g.setColour(UiColors::surface2.withAlpha(0.55f));
        g.fillRoundedRectangle(cardX + 42.0f, cardY + 202.0f,
                               cardWidth - 84.0f, 40.0f, 12.0f);
        g.setColour(UiColors::border);
        g.drawRoundedRectangle(cardX + 42.0f, cardY + 202.0f,
                               cardWidth - 84.0f, 40.0f, 12.0f, 1.0f);

        GameUi::drawDivider(g, cardX + 42.0f, cardX + cardWidth - 42.0f,
                            cardY + 258.0f);

        const float pillY = cardY + cardHeight + 20.0f;
        if (pillY + 26.0f < height - 20.0f)
        {
            const float total = 160.0f + 132.0f + 152.0f + 20.0f;
            float x = (width - total) * 0.5f;
            GameUi::drawPill(g, { x, pillY, 160.0f, 26.0f },
                             "REAL-TIME PITCH", UiColors::green, false);
            x += 170.0f;
            GameUi::drawPill(g, { x, pillY, 132.0f, 26.0f },
                             "TABLATURE", UiColors::accent, false);
            x += 142.0f;
            GameUi::drawPill(g, { x, pillY, 152.0f, 26.0f },
                             "GLOBAL RATING", UiColors::blue, false);
        }

        return;
    }

    //==========================================================================
    // Song select
    //==========================================================================

    if (gameState == GameState::SongSelect)
    {
        const juce::Rectangle<float> nav(24.0f, 20.0f, width - 48.0f, 78.0f);
        GameUi::drawCard(g, nav, 18.0f);
        drawBrand(44.0f, 40.0f, true);

        if (currentUsername.isNotEmpty())
        {
            const float avatarSize = 30.0f;
            const juce::Rectangle<float> avatar(width - 404.0f, 42.0f,
                                                avatarSize, avatarSize);
            g.setColour(UiColors::accent.withAlpha(0.16f));
            g.fillEllipse(avatar);
            g.setColour(UiColors::accent.withAlpha(0.55f));
            g.drawEllipse(avatar, 1.0f);
            g.setColour(UiColors::accent);
            g.setFont(GameUi::heading(13.0f));
            g.drawText(currentUsername.substring(0, 1).toUpperCase(),
                       avatar, juce::Justification::centred, false);

            g.setColour(UiColors::muted);
            g.setFont(GameUi::eyebrow(8.0f));
            g.drawText("SIGNED IN AS",
                       juce::Rectangle<float>(width - 362.0f, 35.0f, 160.0f, 13.0f),
                       juce::Justification::centredLeft, false);
        }

        const float contentY = 228.0f;
        const float availableH = juce::jmax(320.0f, height - 68.0f - 20.0f - contentY);
        const float contentH = juce::jmin(560.0f, availableH);
        const float footerY = contentY + contentH + 20.0f;
        const float contentW = width - 96.0f;
        const float gap = 20.0f;
        const float leftW = contentW * 0.56f;
        const float rightX = 48.0f + leftW + gap;
        const float rightW = contentW - leftW - gap;

        GameUi::drawEyebrow(g, "SONG LIBRARY",
                            juce::Rectangle<float>(48.0f, 121.0f, 360.0f, 16.0f));
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(13.0f));
        g.drawText("Pick a track, tune up and make every note count.",
                   juce::Rectangle<float>(48.0f, 188.0f, width - 96.0f, 24.0f),
                   juce::Justification::centredLeft, false);

        const juce::Rectangle<float> leftCard(48.0f, contentY, leftW, contentH);
        const juce::Rectangle<float> rightCard(rightX, contentY, rightW, contentH);
        GameUi::drawCard(g, leftCard, 20.0f, true);
        GameUi::drawCard(g, rightCard, 20.0f);

        // Album artwork placeholder, made from the same brand mark as the nav.
        const juce::Rectangle<float> art(leftCard.getX() + 24.0f,
                                         leftCard.getY() + 24.0f,
                                         118.0f, 118.0f);
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

        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(8.5f, true, 0.9f));
        g.drawText(headerKind + "  /  " + juce::String(juce::roundToInt(headerBpm)) + " BPM",
                   juce::Rectangle<float>(art.getRight() + 20.0f,
                                          leftCard.getY() + 28.0f,
                                          leftW - 170.0f, 16.0f),
                   juce::Justification::centredLeft, false);
        g.setColour(UiColors::text);
        g.setFont(GameUi::heading(26.0f));
        g.drawText(headerTitle,
                   juce::Rectangle<float>(art.getRight() + 20.0f,
                                          leftCard.getY() + 44.0f,
                                          leftW - 170.0f, 34.0f),
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
        drawBrand(42.0f, 24.0f, true);
        const int tunedMaskTop = tuner.getTunedMask();
        int tunedCountTop = 0;
        for (int i = 0; i < 6; ++i)
            tunedCountTop += (tunedMaskTop & (1 << i)) != 0 ? 1 : 0;
        const bool allTunedTop = tunedCountTop == 6;

        GameUi::drawPill(g,
                         juce::Rectangle<float>(width - 178.0f, 29.0f, 136.0f, 22.0f),
                         allTunedTop ? juce::String("READY")
                                     : "TUNING " + juce::String(tunedCountTop) + "/6",
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
        g.drawText("Standard tuning E A D G B E. Play each string and wait for the green light.",
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
        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(11.0f));
        g.drawText("STANDARD TUNING  /  E  A  D  G  B  E",
                   juce::Rectangle<float>(leftPanel.getX() + 24.0f,
                                          leftPanel.getY() + 44.0f,
                                          leftW - 48.0f, 18.0f),
                   juce::Justification::centredLeft, false);

        const char* stringNames[6] = { "E", "A", "D", "G", "B", "E" };
        const char* stringNumbers[6] = { "6", "5", "4", "3", "2", "1" };
        const int tunedMask = tuner.getTunedMask();
        const float tileGap = 12.0f;
        const float tileW = (leftW - 48.0f - tileGap * 2.0f) / 3.0f;
        const float tileH = 112.0f;

        for (int i = 0; i < 6; ++i)
        {
            const int column = i % 3;
            const int row = i / 3;
            const float x = leftPanel.getX() + 24.0f
                + static_cast<float>(column) * (tileW + tileGap);
            const float y = leftPanel.getY() + 82.0f
                + static_cast<float>(row) * (tileH + tileGap);
            const bool tuned = (tunedMask & (1 << i)) != 0;
            const auto colour = tuned ? UiColors::green : UiColors::accent;

            g.setColour(tuned ? UiColors::green.withAlpha(0.10f)
                              : UiColors::surface2.withAlpha(0.72f));
            g.fillRoundedRectangle(x, y, tileW, tileH, 14.0f);
            g.setColour(colour.withAlpha(tuned ? 0.46f : 0.22f));
            g.drawRoundedRectangle(x, y, tileW, tileH, 14.0f, 1.0f);

            g.setColour(colour);
            g.setFont(GameUi::heading(34.0f));
            g.drawText(stringNames[i],
                       juce::Rectangle<float>(x, y + 10.0f, tileW, 40.0f),
                       juce::Justification::centred, false);
            g.setColour(UiColors::muted);
            g.setFont(GameUi::eyebrow(8.5f));
            g.drawText("STRING " + juce::String(stringNumbers[i]),
                       juce::Rectangle<float>(x, y + 52.0f, tileW, 16.0f),
                       juce::Justification::centred, false);
            GameUi::drawPill(g,
                             juce::Rectangle<float>(x + 10.0f, y + 77.0f,
                                                    tileW - 20.0f, 20.0f),
                             tuned ? "IN TUNE" : "WAITING",
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

        if (panelH > 470.0f)
        {
            GameUi::drawDivider(g, leftPanel.getX() + 24.0f,
                                leftPanel.getRight() - 24.0f,
                                leftPanel.getY() + 352.0f);
            GameUi::drawEyebrow(g, "REFERENCE PITCH",
                                juce::Rectangle<float>(leftPanel.getX() + 24.0f,
                                                       leftPanel.getY() + 368.0f,
                                                       leftW - 48.0f, 14.0f),
                                UiColors::muted);

            const char* references[6]
            {
                "6   E2   82.4 Hz", "5   A2  110.0 Hz", "4   D3  146.8 Hz",
                "3   G3  196.0 Hz", "2   B3  246.9 Hz", "1   E4  329.6 Hz"
            };
            const float refGap = 10.0f;
            const float refW = (leftW - 48.0f - refGap * 2.0f) / 3.0f;

            for (int i = 0; i < 6; ++i)
            {
                const int column = i % 3;
                const int row = i / 3;
                const float x = leftPanel.getX() + 24.0f
                    + static_cast<float>(column) * (refW + refGap);
                const float y = leftPanel.getY() + 396.0f
                    + static_cast<float>(row) * 24.0f;
                g.setColour(UiColors::accent.withAlpha(0.72f));
                g.fillEllipse(x, y + 5.0f, 4.0f, 4.0f);
                g.setColour(UiColors::muted);
                g.setFont(GameUi::font(10.5f));
                g.drawText(references[i],
                           juce::Rectangle<float>(x + 10.0f, y, refW - 10.0f, 16.0f),
                           juce::Justification::centredLeft, false);
            }
        }

        if (panelH > 520.0f)
        {
            const bool allReady = tuner.areAllStringsTuned();
            int tunedCount = 0;
            for (int i = 0; i < 6; ++i)
                tunedCount += (tunedMask & (1 << i)) != 0 ? 1 : 0;

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
                                : juce::String(tunedCount) + " / 6 STRINGS READY",
                       juce::Rectangle<float>(readyCard.getX() + 58.0f,
                                              readyCard.getY() + 14.0f,
                                              readyCard.getWidth() - 72.0f, 18.0f),
                       juce::Justification::centredLeft, false);
            g.setColour(UiColors::muted);
            g.setFont(GameUi::font(10.5f));
            g.drawText(allReady ? "All six strings are in tune."
                                : "Keep tuning the remaining strings.",
                       juce::Rectangle<float>(readyCard.getX() + 58.0f,
                                              readyCard.getY() + 38.0f,
                                              readyCard.getWidth() - 72.0f, 16.0f),
                       juce::Justification::centredLeft, false);
        }

        // Large input dial. The note itself is a real Label above this paint.
        const float dialRadius = juce::jmin(88.0f, rightW * 0.21f);
        const float dialX = rightPanel.getCentreX();
        const float dialY = rightPanel.getY() + 196.0f;
        g.setColour(UiColors::accent.withAlpha(0.035f));
        g.fillEllipse(dialX - dialRadius - 16.0f, dialY - dialRadius - 16.0f,
                      (dialRadius + 16.0f) * 2.0f, (dialRadius + 16.0f) * 2.0f);
        g.setColour(UiColors::surface2.withAlpha(0.78f));
        g.fillEllipse(dialX - dialRadius, dialY - dialRadius,
                      dialRadius * 2.0f, dialRadius * 2.0f);
        g.setColour(UiColors::borderStrong);
        g.drawEllipse(dialX - dialRadius, dialY - dialRadius,
                      dialRadius * 2.0f, dialRadius * 2.0f, 1.0f);
        g.setColour(UiColors::accent.withAlpha(0.22f));
        g.drawEllipse(dialX - dialRadius + 12.0f, dialY - dialRadius + 12.0f,
                      (dialRadius - 12.0f) * 2.0f, (dialRadius - 12.0f) * 2.0f, 1.0f);

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
        drawBrand(48.0f, 32.0f, true);
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
    const float studioH = juce::jlimit(176.0f, 218.0f, height / 4.0f);
    const float studioY = bottomY - 14.0f - studioH;
    const float tabY = hudY + hudH + 12.0f;
    const float tabH = juce::jmax(150.0f, studioY - 12.0f - tabY);

    const juce::Rectangle<float> topBar(24.0f, topY, width - 48.0f, topH);
    GameUi::drawCard(g, topBar, 18.0f);
    drawBrand(44.0f, 32.0f, false);

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

    GameUi::drawEyebrow(g, "LIVE PERFORMANCE",
                        juce::Rectangle<float>(statsCard.getX() + 18.0f,
                                               statsCard.getY() + 9.0f,
                                               statsCard.getWidth() - 36.0f, 14.0f),
                        UiColors::muted);

    {
        const float cellWidth = statsCard.getWidth() / 4.0f;
        const std::array<juce::String, 4> values
        {
            juce::String(gameplayStats.getMissCount()),
            juce::String(gameplayStats.getPerfectCount()),
            juce::String(gameplayStats.getGreatCount()),
            juce::String(gameplayStats.getGoodCount())
        };
        const std::array<juce::String, 4> captions { "MISS", "PERFECT", "GREAT", "GOOD" };
        const std::array<juce::Colour, 4> colours
        {
            UiColors::red, UiColors::accent, UiColors::green, UiColors::info
        };

        for (int i = 0; i < 4; ++i)
        {
            if (i > 0)
            {
                g.setColour(UiColors::border);
                g.fillRect(statsCard.getX() + cellWidth * static_cast<float>(i),
                           statsCard.getY() + 28.0f, 1.0f, 52.0f);
            }
            g.setColour(colours[static_cast<size_t>(i)]);
            g.setFont(GameUi::heading(24.0f));
            g.drawText(values[static_cast<size_t>(i)],
                       juce::Rectangle<float>(statsCard.getX() + cellWidth * static_cast<float>(i),
                                              statsCard.getY() + 28.0f,
                                              cellWidth, 34.0f),
                       juce::Justification::centred, false);
            g.setColour(UiColors::muted);
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

    // Guitar-effect pedals are intentionally not shown yet; the backing-track
    // mixer uses the full width until the effects UI is implemented.
    const juce::Rectangle<float> mixer(24.0f, studioY,
                                       width - 48.0f, studioH);

    GameUi::drawCard(g, mixer, 18.0f);
    GameUi::drawEyebrow(g, "MIXER",
                        juce::Rectangle<float>(mixer.getX() + 22.0f,
                                               mixer.getY() + 16.0f,
                                               180.0f, 14.0f));
    g.setColour(UiColors::text);
    g.setFont(GameUi::heading(17.0f));
    g.drawText("BACKING TRACK",
               juce::Rectangle<float>(mixer.getX() + 22.0f,
                                      mixer.getY() + 31.0f,
                                      mixer.getWidth() - 190.0f, 22.0f),
               juce::Justification::centredLeft, false);
    GameUi::drawPill(g,
                     juce::Rectangle<float>(mixer.getRight() - 122.0f,
                                            mixer.getY() + 18.0f,
                                            100.0f, 20.0f),
                     "6 CHANNELS",
                     UiColors::green,
                     false);
    GameUi::drawDivider(g, mixer.getX() + 22.0f,
                        mixer.getRight() - 22.0f,
                        mixer.getY() + 58.0f);

    {
        const float cellWidth = (mixer.getWidth() - 28.0f - 24.0f) / 3.0f;
        for (int i = 1; i < 3; ++i)
        {
            const float dividerX = mixer.getX() + 14.0f
                + static_cast<float>(i) * (cellWidth + 12.0f) - 6.0f;
            g.setColour(UiColors::border);
            g.fillRect(dividerX, mixer.getY() + 68.0f, 1.0f,
                       mixer.getHeight() - 78.0f);
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

    // Count-in card.
    if (gameState == GameState::Playing
        && countInActive.load(std::memory_order_acquire))
    {
        const float cardWidth = 280.0f;
        const float cardHeight = 220.0f;
        const juce::Rectangle<float> countCard(
            (width - cardWidth) * 0.5f,
            (height - cardHeight) * 0.5f,
            cardWidth, cardHeight);

        g.setColour(UiColors::background.withAlpha(0.32f));
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

        const int count = juce::jmax(1, countInRemaining.load(std::memory_order_relaxed));
        g.setColour(count <= 1 ? UiColors::accentStrong : UiColors::accent);
        g.setFont(GameUi::heading(92.0f));
        g.drawText(juce::String(count),
                   juce::Rectangle<float>(countCard.getX(),
                                          countCard.getY() + 48.0f,
                                          cardWidth, 110.0f),
                   juce::Justification::centred, false);

        for (int i = 0; i < 4; ++i)
        {
            const float dotX = countCard.getCentreX() - 34.0f + static_cast<float>(i) * 22.0f;
            g.setColour(i < count ? UiColors::accent : UiColors::borderStrong);
            g.fillEllipse(dotX, countCard.getBottom() - 42.0f, 6.0f, 6.0f);
        }
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
    const bool showGame = gameState == GameState::Playing
        || gameState == GameState::Paused;

    //==========================================================================
    // Visibility
    //==========================================================================

    songTitleLabel.setVisible(songSelect);
    loginScreenTitleLabel.setVisible(authScreen);
    currentUserLabel.setVisible(songSelect && currentUsername.isNotEmpty());
    logoutButton.setVisible(songSelect && currentUsername.isNotEmpty());
    gameSongTitleLabel.setVisible(showGame);

    songPridumayButton.setVisible(songSelect);
    onlineCatalogButton.setVisible(songSelect);
    onlineCatalogStatusLabel.setVisible(songSelect);
    leaderboardTitleLabel.setVisible(songSelect);
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
    guitarInputLabel.setVisible(tunerScreen);
    guitarInputCombo.setVisible(tunerScreen);

    pitchLabel.setVisible(showGame);
    comboLabel.setVisible(showGame);
    beatLabel.setVisible(showGame);
    barBeatLabel.setVisible(showGame);
    bpmLabel.setVisible(showGame);
    gameSessionLabel.setVisible(showGame);
    hitLabel.setVisible(showGame);
    targetNoteLabel.setVisible(showGame);

    gameStartButton.setVisible(showGame || finishedScreen);
    pauseButton.setVisible(showGame);
    menuButton.setVisible(showGame || finishedScreen);
    distortionButton.setVisible(false);
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
    speed100Button.setVisible(showGame);
    speed120Button.setVisible(showGame);
    audioSettingsButton.setVisible(showGame);
    easyButton.setVisible(false);
    normalButton.setVisible(false);
    hardButton.setVisible(false);
    tablatureComponent.setVisible(showGame);

    //==========================================================================
    // Authentication
    //==========================================================================

    if (authScreen)
    {
        const int cardWidth = juce::jmin(560, width - 48);
        const int cardHeight = 400;
        const int cardX = (width - cardWidth) / 2;
        const int cardY = juce::jmax(150, (height - cardHeight) / 2);

        loginScreenTitleLabel.setBounds(cardX + 40, cardY + 114, cardWidth - 80, 52);
        accountStatusLabel.setJustificationType(juce::Justification::centred);
        accountStatusLabel.setBounds(cardX + 50, cardY + 198, cardWidth - 100, 38);

        const int buttonWidth = (cardWidth - 92) / 2;
        accountLoginButton.setBounds(cardX + 40, cardY + 276, buttonWidth, 44);
        accountRegisterButton.setBounds(cardX + 52 + buttonWidth, cardY + 276,
                                        buttonWidth, 44);

        verifyEmailButton.setBounds(cardX + 40, cardY + 338, buttonWidth, 28);
        forgotPasswordButton.setBounds(cardX + 52 + buttonWidth, cardY + 338,
                                       buttonWidth, 28);
    }

    //==========================================================================
    // Song select
    //==========================================================================

    if (songSelect)
    {
        accountStatusLabel.setJustificationType(juce::Justification::centredLeft);
        currentUserLabel.setBounds(width - 362, 48, 220, 22);
        currentUserLabel.setJustificationType(juce::Justification::centredLeft);
        logoutButton.setBounds(width - 122, 42, 94, 32);
        songTitleLabel.setBounds(48, 140, width - 96, 44);

        const int contentY = 228;
        const int availableH = juce::jmax(320, height - 68 - 20 - contentY);
        const int contentH = juce::jmin(560, availableH);
        const int footerY = contentY + contentH + 20;
        const int contentW = width - 96;
        const int gap = 20;
        const int leftW = contentW * 56 / 100;
        const int rightX = 48 + leftW + gap;
        const int rightW = contentW - leftW - gap;

        songPridumayBestLabel.setBounds(48 + 24 + 138, contentY + 78,
                                        leftW - 24 - 138 - 24, 20);

        const int buttonY = contentY + contentH - 64;
        const int available = leftW - 48;
        const int startW = juce::jmin(180, (available - 12) / 2);
        const int onlineW = juce::jmin(240, available - startW - 12);
        songPridumayButton.setBounds(48 + 24, buttonY, startW, 42);
        onlineCatalogButton.setBounds(48 + 24 + startW + 12, buttonY, onlineW, 42);
        onlineCatalogStatusLabel.setBounds(48 + 24, buttonY - 30, leftW - 48, 22);

        // Catalogue list between the header block and the action buttons.
        const int listTop = contentY + 186;
        const int listBottom = buttonY - 38;
        songListBox.setBounds(48 + 18, listTop, leftW - 36,
                              juce::jmax(56, listBottom - listTop));

        leaderboardTitleLabel.setBounds(rightX + 24, contentY + 38, rightW - 48, 30);
        refreshLeaderboardButton.setBounds(rightX + rightW - 142, contentY + 68,
                                           118, 32);
        leaderboardLabel.setBounds(rightX + 24, contentY + 118,
                                   rightW - 48, contentH - 142);

        accountStatusLabel.setBounds(24 + 34, footerY + 7, width - 48 - 58, 30);
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
        guitarInputLabel.setBounds(rightX + 24, panelY + 58, 100, 22);
        guitarInputCombo.setBounds(rightX + 126, panelY + 54,
                                   juce::jmax(120, rightW - 150), 30);
        tunerNoteLabel.setBounds(rightX + 24, panelY + 142, rightW - 48, 96);
        tunerFrequencyLabel.setBounds(rightX + 24, panelY + 242, rightW - 48, 26);
        tunerStringLabel.setBounds(rightX + rightW - 180, panelY + 278, 160, 30);
        tunerCentsLabel.setBounds(rightX + rightW - 180, panelY + 310, 160, 30);
        tunerStringLabel.setJustificationType(juce::Justification::centredRight);
        tunerCentsLabel.setJustificationType(juce::Justification::centredRight);
        tunerStatusLabel.setBounds(rightX + 40, panelY + 368, rightW - 80, 40);
        tunerStatusLabel.setJustificationType(juce::Justification::centred);

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
        const int hudY = 120;
        const int hudH = 96;
        const int bottomH = juce::jlimit(124, 160, height / 6);
        const int bottomY = height - 24 - bottomH;
        const int studioH = juce::jlimit(176, 218, height / 4);
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

        speed50Button.setBounds(310, 82, 44, 24);
        speed100Button.setBounds(360, 82, 48, 24);
        speed120Button.setBounds(414, 82, 48, 24);

        const int hudGap = 12;
        const int comboW = 124;
        const int pitchW = 214;
        const int targetW = 190;
        const int comboX = 24;
        const int pitchX = comboX + comboW + hudGap;
        const int targetX = width - 24 - targetW;
        const int statsX = pitchX + pitchW + hudGap;
        const int statsW = juce::jmax(180, targetX - hudGap - statsX);
        juce::ignoreUnused(statsW);

        comboLabel.setBounds(comboX + 8, hudY + 32, comboW - 16, 46);
        pitchLabel.setBounds(pitchX + 10, hudY + 32, pitchW - 20, 46);
        targetNoteLabel.setBounds(targetX + 10, hudY + 32, targetW - 20, 46);

        tablatureComponent.setBounds(24, tabY, width - 48, tabH);

        const int mixerX = 24;
        const int mixerW = width - 48;
        const int channelX = mixerX + 14;
        const int channelTop = studioY + 66;
        const int cellGap = 12;
        const int cellW = (mixerW - 28 - cellGap * 2) / 3;
        const int rowH = (studioH - 66 - 10) / 2;

        auto layoutMixerChannel = [&](juce::TextButton& button, juce::Label& label,
                                      juce::Slider& slider, int column, int row)
        {
            const int x = channelX + column * (cellW + cellGap);
            const int y = channelTop + row * rowH;
            button.setBounds(x, y, cellW, 22);
            slider.setBounds(x, y + 27, cellW, 18);
            label.setBounds(x, y + 48, cellW, 12);
        };

        layoutMixerChannel(midiBackingButton, midiBackingVolumeLabel,
                           midiBackingVolumeSlider, 0, 0);
        layoutMixerChannel(midiDrumsButton, midiDrumsVolumeLabel,
                           midiDrumsVolumeSlider, 1, 0);
        layoutMixerChannel(midiBassButton, midiBassVolumeLabel,
                           midiBassVolumeSlider, 2, 0);
        layoutMixerChannel(midiRhythmGuitarButton, midiRhythmGuitarVolumeLabel,
                           midiRhythmGuitarVolumeSlider, 0, 1);
        layoutMixerChannel(midiPianoButton, midiPianoVolumeLabel,
                           midiPianoVolumeSlider, 1, 1);
        layoutMixerChannel(metronomeMixerButton, metronomeMixerVolumeLabel,
                           metronomeMixerVolumeSlider, 2, 1);

        const int bottomW = width - 48;
        const int judgementW = bottomW * 40 / 100;
        const int scoreX = 24 + bottomW * 66 / 100;
        hitLabel.setBounds(24 + 24, bottomY + 34, judgementW - 48, bottomH - 44);
        gameSessionLabel.setBounds(scoreX + 16, bottomY + 24,
                                   width - 24 - scoreX - 32, bottomH - 30);
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
                       std::function<void(int)> onInputChannelChanged)
        : selector(manager, 0, 32, 0, 2, true, true, true, false),
          deviceManager(manager),
          inputChannelChanged(onInputChannelChanged)
    {
        setSize(760, 600);
        addAndMakeVisible(selector);
        addAndMakeVisible(inputChannelLabel);
        addAndMakeVisible(inputChannelBox);

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
        g.drawText("Choose your guitar input and configure audio performance.",
                   48, 59, getWidth() - 80, 24, juce::Justification::centredLeft);

        GameUi::drawPill(g,
                         juce::Rectangle<float>(getWidth() - 134.0f, 31.0f, 96.0f, 22.0f),
                         "AUDIO I/O",
                         UiColors::green,
                         true);

        g.setColour(UiColors::border);
        g.drawLine(30.0f, 96.0f, static_cast<float>(getWidth() - 30), 96.0f, 1.0f);
        g.drawLine(30.0f, static_cast<float>(getHeight() - 92),
                   static_cast<float>(getWidth() - 30),
                   static_cast<float>(getHeight() - 92), 1.0f);

        g.setColour(UiColors::muted);
        g.setFont(GameUi::font(10.5f));
        g.drawText("Auto follows the loudest active input. Enable extra inputs above for other channels.",
                   30, getHeight() - 44, getWidth() - 60, 20,
                   juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        selector.setBounds(30, 108, getWidth() - 60,
                           juce::jmax(80, getHeight() - 204));
        inputChannelLabel.setBounds(30, getHeight() - 76, 130, 24);
        inputChannelBox.setBounds(168, getHeight() - 78, 320, 28);
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        rebuildInputChannelChoices(-1);
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
    juce::Label inputChannelLabel;
    juce::ComboBox inputChannelBox;
};

//==============================================================================



void MainComponent::rebuildGuitarInputChoices()
{
    populateInputChannelCombo(
        guitarInputCombo,
        deviceManager,
        preferredInputChannel.load(std::memory_order_relaxed));
}

void MainComponent::refreshGuitarInputCombo()
{
    rebuildGuitarInputChoices();
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
        GameUi::font(13.5f, true, 0.8f));

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

    // Compact input selector available on the tuner screen. The guitar may be
    // connected to any interface input, so the player can switch the capture
    // channel without entering a song and opening the full audio settings.
    guitarInputLabel.setText(
        "GUITAR INPUT",
        juce::dontSendNotification);

    guitarInputLabel.setFont(
        GameUi::eyebrow(10.0f));

    guitarInputLabel.setColour(
        juce::Label::textColourId,
        UiColors::muted);

    guitarInputLabel.setJustificationType(
        juce::Justification::centredLeft);

    guitarInputCombo.setColour(
        juce::ComboBox::backgroundColourId,
        UiColors::surface2);

    guitarInputCombo.setColour(
        juce::ComboBox::textColourId,
        UiColors::text);

    guitarInputCombo.setColour(
        juce::ComboBox::outlineColourId,
        UiColors::borderStrong);

    guitarInputCombo.setColour(
        juce::ComboBox::arrowColourId,
        UiColors::accent);

    guitarInputCombo.onChange =
        [this]
        {
            const int selectedId =
                guitarInputCombo.getSelectedId();

            if (selectedId > 0)
                setPreferredInputChannel(selectedId - 2);
        };

    addAndMakeVisible(guitarInputLabel);
    addAndMakeVisible(guitarInputCombo);

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
            startOnlineGameSession();

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
            // REAL START SAMPLE
            //
            // No artificial MIDI shift.
            // MIDI startBeat remains unchanged.

            const auto currentSample =
                audioSampleCounter.load(
                    std::memory_order_relaxed);

            const double beatLengthSamples =
                metronome.getBeatLengthSamples();

            // Give the player a full bar of four audible metronome clicks.
            // The song timeline starts on the beat immediately after click 4.
            // Use the fractional beat length so the count-in and the song start
            // stay locked to the same timeline as the MIDI backing.
            countInRemaining.store(4, std::memory_order_relaxed);
            countInActive.store(true, std::memory_order_release);
            tablatureComponent.setPlayheadRunning(false);

            const auto countInSamples =
                static_cast<uint64_t>(
                    std::llround(beatLengthSamples * 4.0));

            gameplayStartSample.store(
                currentSample + countInSamples,
                std::memory_order_relaxed);

            currentPlayheadBeat.store(
                0.0,
                std::memory_order_relaxed);
            tablatureComponent.setPlayheadBeat(0.0);

            //==============================================================
 // Reset metronome timeline
 //
 // First metronome beat happens at START.

            metronome.resetScheduler(
                currentSample);

            metronomeSamplesRemaining = 0;
            metronomePlaybackPosition = 0;

            //==============================================================
            // First MIDI target

            gameSession.setTargetNoteIndex(0);

            setTargetFromMidiIndex(0);

            gameState =
                GameState::Playing;

            gameStartButton.setButtonText("START");
            menuButton.setButtonText("MAIN MENU");

            DBG("START -> PLAYING");
            DBG("GAMEPLAY START SAMPLE = "
                + juce::String(
                    static_cast<juce::int64>(
                        currentSample)));

            tunerStartButton.setEnabled(false);

            pauseButton.setButtonText(
                "PAUSE");

            hitLabel.setText(
                "COUNT-IN 4",
                juce::dontSendNotification);

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
                          &speed50Button, &speed100Button, &speed120Button,
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
    midiBackingButton.setToggleState(true, juce::dontSendNotification);
    midiBackingButton.setButtonText("SOLO GUITAR ON");
    midiBackingButton.setTooltip("Toggle the MIDI solo-guitar part generated by FretFight.");
    addAndMakeVisible(midiBackingButton);
    midiBackingButton.onClick = [this]
    {
        const bool enabled = !midiBackingEnabled.load(std::memory_order_relaxed);
        midiBackingEnabled.store(enabled, std::memory_order_relaxed);
        midiBackingButton.setButtonText(enabled ? "SOLO GUITAR ON" : "SOLO GUITAR OFF");
        midiBackingButton.setToggleState(enabled, juce::dontSendNotification);
    };
    midiBackingVolumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    midiBackingVolumeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 20);
    midiBackingVolumeSlider.setColour(juce::Slider::textBoxTextColourId, UiColors::text);
    midiBackingVolumeSlider.setColour(juce::Slider::textBoxBackgroundColourId, UiColors::card);
    midiBackingVolumeSlider.setColour(juce::Slider::textBoxOutlineColourId, UiColors::border);
    midiBackingVolumeSlider.setRange(0.0, 0.8, 0.01);
    midiBackingVolumeSlider.setValue(0.30, juce::dontSendNotification);
    midiBackingVolumeSlider.setTooltip("Solo guitar MIDI volume");
    midiBackingVolumeSlider.onValueChange = [this]
    {
        midiBackingVolume.store(static_cast<float>(midiBackingVolumeSlider.getValue()),
                                std::memory_order_relaxed);
    };
    addAndMakeVisible(midiBackingVolumeSlider);
    midiBackingVolumeLabel.setText("SOLO VOL", juce::dontSendNotification);
    midiBackingVolumeLabel.setJustificationType(juce::Justification::centredLeft);
    midiBackingVolumeLabel.setColour(juce::Label::textColourId, UiColors::text);
    midiBackingVolumeLabel.setFont(juce::FontOptions().withHeight(12.0f).withStyle("Bold"));
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
        midiRhythmGuitarButton.setButtonText(enabled ? "RHYTHM GUITAR ON" : "RHYTHM GUITAR OFF");
        midiRhythmGuitarButton.setToggleState(enabled, juce::dontSendNotification);
    };
    midiRhythmGuitarVolumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    midiRhythmGuitarVolumeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 42, 18);
    midiRhythmGuitarVolumeSlider.setColour(juce::Slider::textBoxTextColourId, UiColors::text);
    midiRhythmGuitarVolumeSlider.setColour(juce::Slider::textBoxBackgroundColourId, UiColors::card);
    midiRhythmGuitarVolumeSlider.setColour(juce::Slider::textBoxOutlineColourId, UiColors::border);
    midiRhythmGuitarVolumeSlider.setRange(0.0, 0.8, 0.01);
    midiRhythmGuitarVolumeSlider.setValue(0.20, juce::dontSendNotification);
    midiRhythmGuitarVolumeSlider.setTooltip("Guitar stem volume");
    midiRhythmGuitarVolumeSlider.onValueChange = [this]
    { midiRhythmGuitarVolume.store(static_cast<float>(midiRhythmGuitarVolumeSlider.getValue()), std::memory_order_relaxed); };
    addAndMakeVisible(midiRhythmGuitarVolumeSlider);
    midiRhythmGuitarVolumeLabel.setText("GUITAR VOL", juce::dontSendNotification);
    midiRhythmGuitarVolumeLabel.setJustificationType(juce::Justification::centredLeft);
    midiRhythmGuitarVolumeLabel.setColour(juce::Label::textColourId, UiColors::text);
    midiRhythmGuitarVolumeLabel.setFont(juce::FontOptions().withHeight(10.5f).withStyle("Bold"));
    addAndMakeVisible(midiRhythmGuitarVolumeLabel);

    auto setupBackingChannel = [this](juce::TextButton& button, juce::Slider& slider,
                                      juce::Label& label, std::atomic<bool>& enabled,
                                      std::atomic<float>& volume, const juce::String& name,
                                      float initialVolume)
    {
        button.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
        button.setColour(juce::TextButton::buttonOnColourId,
                         UiColors::surface2.interpolatedWith(UiColors::green, 0.20f));
        button.setColour(juce::TextButton::textColourOffId, UiColors::text);
        button.setColour(juce::TextButton::textColourOnId, UiColors::background);
        button.setToggleState(true, juce::dontSendNotification);
        button.setTooltip("Toggle the " + name + " audio channel.");
        addAndMakeVisible(button);
        auto* buttonPtr = &button;
        auto* enabledPtr = &enabled;
        button.onClick = [buttonPtr, enabledPtr, name]()
        {
            const bool next = !enabledPtr->load(std::memory_order_relaxed);
            enabledPtr->store(next, std::memory_order_relaxed);
            buttonPtr->setButtonText(name + (next ? " ON" : " OFF"));
            buttonPtr->setToggleState(next, juce::dontSendNotification);
        };
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 42, 18);
        slider.setColour(juce::Slider::textBoxTextColourId, UiColors::text);
        slider.setColour(juce::Slider::textBoxBackgroundColourId, UiColors::card);
        slider.setColour(juce::Slider::textBoxOutlineColourId, UiColors::border);
        slider.setRange(0.0, 0.8, 0.01);
        slider.setValue(initialVolume, juce::dontSendNotification);
        auto* sliderPtr = &slider;
        auto* volumePtr = &volume;
        slider.onValueChange = [sliderPtr, volumePtr]()
        { volumePtr->store(static_cast<float>(sliderPtr->getValue()), std::memory_order_relaxed); };
        addAndMakeVisible(slider);
        label.setText(name + " VOL", juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centredLeft);
        label.setColour(juce::Label::textColourId, UiColors::text);
        label.setFont(juce::FontOptions().withHeight(10.5f).withStyle("Bold"));
        addAndMakeVisible(label);
    };
    setupBackingChannel(midiBassButton, midiBassVolumeSlider, midiBassVolumeLabel,
                        midiBassEnabled, midiBassVolume, "BASS", 0.45f);
    setupBackingChannel(midiDrumsButton, midiDrumsVolumeSlider, midiDrumsVolumeLabel,
                        midiDrumsEnabled, midiDrumsVolume, "DRUMS", 0.45f);
    setupBackingChannel(midiPianoButton, midiPianoVolumeSlider, midiPianoVolumeLabel,
                        midiPianoEnabled, midiPianoVolume, "OTHERS", 0.30f);
    setupBackingChannel(metronomeMixerButton, metronomeMixerVolumeSlider,
                        metronomeMixerVolumeLabel, metronomeMixerEnabled,
                        metronomeMixerVolume, "METRONOME", 0.70f);
    metronomeMixerVolumeSlider.setRange(0.0, 1.0, 0.01);
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
    addAndMakeVisible(speed100Button);
    addAndMakeVisible(speed120Button);

    speed50Button.setClickingTogglesState(true);
    speed100Button.setClickingTogglesState(true);
    speed120Button.setClickingTogglesState(true);

    speed50Button.setRadioGroupId(2001);
    speed100Button.setRadioGroupId(2001);
    speed120Button.setRadioGroupId(2001);

    speed50Button.setToggleState(
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
            speedPercent = percent;

            const double actualBpm =
                baseBpm
                * speedPercent
                / 100.0;

            bpm.store(
                actualBpm,
                std::memory_order_relaxed);
            audioPlaybackRate.store(speedPercent / 100.0, std::memory_order_relaxed);
            prepareExternalAudioForSpeed(speedPercent);

            metronome.setBpm(
                actualBpm);
            gameSession.setBaseBpm(baseBpm);
            gameSession.setSpeedPercent(speedPercent);
            gameSession.setBpm(actualBpm);

            bpmLabel.setText(
                juce::String(
                    static_cast<int>(
                        std::round(
                            actualBpm))),
                juce::dontSendNotification);

            tablatureComponent.setBpm(
                actualBpm);
        };

    speed50Button.onClick =
        [this, setSpeed]
        {
            setSpeed(50.0);
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
    leaderboardLabel.setFont(GameUi::font(13.0f));
    leaderboardLabel.setColour(juce::Label::textColourId, UiColors::muted);
    leaderboardLabel.setJustificationType(juce::Justification::topLeft);
    leaderboardLabel.setMinimumHorizontalScale(0.8f);
    addAndMakeVisible(leaderboardLabel);

    refreshLeaderboardButton.setColour(juce::TextButton::buttonColourId, UiColors::surface2);
    refreshLeaderboardButton.setColour(juce::TextButton::buttonOnColourId, UiColors::accent);
    refreshLeaderboardButton.setColour(juce::TextButton::textColourOffId, UiColors::text);
    refreshLeaderboardButton.setColour(juce::TextButton::textColourOnId, UiColors::onAccent);
    refreshLeaderboardButton.onClick = [this] { refreshGlobalLeaderboard(); };
    refreshLeaderboardButton.setButtonText("REFRESH");
    addAndMakeVisible(refreshLeaderboardButton);

    accountStatusLabel.setText("Sign in or create an account. Email confirmation is required.",
                               juce::dontSendNotification);
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
    currentUserLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(currentUserLabel);

    logoutButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    logoutButton.setColour(juce::TextButton::buttonOnColourId, UiColors::surface2);
    logoutButton.setColour(juce::TextButton::textColourOffId, UiColors::muted);
    logoutButton.setColour(juce::TextButton::textColourOnId, UiColors::text);
    logoutButton.onClick = [this]
    {
        // Invalidate a sign-in chain that may still be waiting for the server.
        ++authRequestId;
        // Server-reported best scores belong to the signed-in account; the
        // locally saved values stay available after signing out.
        ++bestScoresRequestId;
        remoteBestScores.clear();
        updateCatalogRows();
        onlineAccessToken.clear();
        onlineGameSessionUuid.clear();
        currentUsername.clear();
        currentUserLabel.setText("", juce::dontSendNotification);
        accountStatusLabel.setText("Please sign in to continue.", juce::dontSendNotification);
        showAuthScreen = true;
        gameState = GameState::SongSelect;
        resized();
        repaint();
    };
    addAndMakeVisible(logoutButton);

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
