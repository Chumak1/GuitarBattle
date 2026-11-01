#include "../MainComponent.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <vector>

// Network-only implementation for MainComponent: authentication, catalogue,
// leaderboard, song downloads and online game sessions.

void MainComponent::authenticateOnlineAccount(bool createAccount)
{
    // JUCE 9 no longer supports AlertWindow::runModalLoop(). The dialog stays
    // open while credentials are checked; only CANCEL (or a successful sign-in)
    // dismisses it.
    auto* prompt = new juce::AlertWindow(
        createAccount ? "Create FretFight account" : "Sign in to FretFight",
        createAccount
            ? "Username, email and password are required. We will send a 6-digit confirmation code to your email."
            : "Enter your username/email and password.",
        juce::AlertWindow::NoIcon);

    juce::Component::SafePointer<juce::ToggleButton> safeConsent;

    // Keeps the consent row alive while the dialog is open: AlertWindow does
    // not own components added with addCustomComponent().
    std::shared_ptr<juce::Component> consentRow;

    if (createAccount)
    {
        prompt->addTextEditor("username", "", "Username");
        prompt->addTextEditor("email", "", "Email");
        prompt->addTextEditor("display_name", "", "Display name (optional)");
        prompt->addTextEditor("password", "", "Password (6+ characters)", true);

        // Consent is required only for a new account, never for a sign-in.
        // The policy is a page on the site, so the dialog carries a real link:
        // a toggle label cannot open a browser.
        consentRow = std::make_shared<juce::Component>();
        consentRow->setSize(420, 52);

        auto* consent = new juce::ToggleButton("I agree to the Privacy Policy");
        consent->setBounds(0, 0, 420, 24);
        consentRow->addAndMakeVisible(consent);
        safeConsent = consent;

        auto* policyLink = new juce::HyperlinkButton(
            "Open the Privacy Policy",
            juce::URL(juce::String(ApiClient::serverOrigin) + "/policy"));
        policyLink->setBounds(18, 26, 300, 22);
        policyLink->setFont(GameUi::font(13.0f), false);
        policyLink->setColour(juce::HyperlinkButton::textColourId, UiColors::accent);
        policyLink->setJustificationType(juce::Justification::centredLeft);
        policyLink->setTooltip("Opens the Privacy Policy on fretfight.online in your browser.");
        consentRow->addAndMakeVisible(policyLink);

        prompt->addCustomComponent(consentRow.get());

        prompt->addButton("CREATE", 1);
    }
    else
    {
        prompt->addTextEditor("identifier", "", "Username or email");
        prompt->addTextEditor("password", "", "Password", true);
        prompt->addButton("SIGN IN", 1);
    }
    prompt->addButton("CANCEL", 0);

    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::Component::SafePointer<juce::AlertWindow> safePrompt(prompt);
    const bool createNewAccount = createAccount;

    // Invalidate an attempt left over from a previous credentials dialog.
    ++authRequestId;

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

        // Defer the popup so it is added on top of the modal credentials window.
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

    auto signIn = [safeThis, safePrompt, createNewAccount, showError, setSubmitEnabled]
                  (int attemptId, juce::String usernameOrEmail, juce::String password)
    {
        auto* self = safeThis.getComponent();
        if (self == nullptr || attemptId != self->authRequestId)
            return;

        juce::DynamicObject::Ptr login = new juce::DynamicObject();
        login->setProperty("identifier", usernameOrEmail);
        login->setProperty("password", password);

        self->apiClient.postJson("/auth/login", juce::var(login.get()), {}, 5000,
            [safeThis, safePrompt, attemptId, createNewAccount, usernameOrEmail, showError, setSubmitEnabled]
            (ApiClient::Response response)
            {
                auto* component = safeThis.getComponent();
                if (component == nullptr || attemptId != component->authRequestId)
                    return;

                const auto accessToken =
                    response.json.getProperty("access_token", juce::var()).toString();
                if (accessToken.isEmpty())
                {
                    setSubmitEnabled(true);

                    if (response.statusCode == 403)
                    {
                        component->accountStatusLabel.setText(
                            "Email is not verified. Enter the code or resend it.",
                            juce::dontSendNotification);
                        showError("Email not verified",
                                  "Confirm your email before signing in. "
                                  "Use VERIFY EMAIL on the welcome screen to enter or resend the code.");
                    }
                    else if (response.statusCode == 401 || response.statusCode == 422)
                    {
                        component->accountStatusLabel.setText(
                            "Incorrect email/username or password",
                            juce::dontSendNotification);
                        showError("Sign-in failed", "Incorrect email/username or password");
                    }
                    else
                    {
                        component->accountStatusLabel.setText(
                            "Sign-in failed. Check server connection.",
                            juce::dontSendNotification);
                        showError("Sign-in failed",
                                  "Could not sign in. Check the server connection and try again.");
                    }
                    return;
                }

                component->onlineAccessToken = accessToken;

                // The refresh token is the only long-lived secret; it goes
                // straight into the Windows Credential Manager.
                component->storeRefreshToken(
                    response.json.getProperty("refresh_token", juce::var()).toString());

                if (auto* settings = component->appProperties.getUserSettings())
                {
                    settings->setValue("SavedUsername", usernameOrEmail);
                    settings->saveIfNeeded();
                }

                // The dialog closes only after a successful sign-in.
                if (auto* dialog = safePrompt.getComponent())
                    dialog->exitModalState(1);

                component->finishOnlineSignIn(createNewAccount, usernameOrEmail,
                                              accessToken, attemptId);
            });
    };

    if (auto* submit = prompt->getButton(0))
    {
        submit->onClick = [safeThis, safePrompt, createNewAccount, signIn, showError, setSubmitEnabled, safeConsent, consentRow]()
        {
            auto* self = safeThis.getComponent();
            auto* dialog = safePrompt.getComponent();
            if (self == nullptr || dialog == nullptr)
                return;

            juce::String usernameOrEmail;
            juce::String email;
            juce::String password;
            juce::String displayName;

            if (createNewAccount)
            {
                usernameOrEmail = dialog->getTextEditorContents("username").trim();
                email = dialog->getTextEditorContents("email").trim();
                displayName = dialog->getTextEditorContents("display_name").trim();
                password = dialog->getTextEditorContents("password");

                if (usernameOrEmail.isEmpty() || email.isEmpty() || password.length() < 6)
                {
                    showError("Registration failed",
                              "Fill in username, email and a password of at least 6 characters.");
                    return;
                }

                if (! email.containsChar('@') || ! email.containsChar('.'))
                {
                    showError("Registration failed", "Enter a valid email address.");
                    return;
                }

                if (safeConsent.getComponent() == nullptr
                    || ! safeConsent->getToggleState())
                {
                    showError("Registration failed",
                              "Please accept the Privacy Policy to create an account.");
                    return;
                }
            }
            else
            {
                usernameOrEmail = dialog->getTextEditorContents("identifier").trim();
                password = dialog->getTextEditorContents("password");

                if (usernameOrEmail.isEmpty() || password.isEmpty())
                {
                    self->accountStatusLabel.setText(
                        "Incorrect email/username or password",
                        juce::dontSendNotification);
                    showError("Sign-in failed", "Incorrect email/username or password");
                    return;
                }
            }

            const int attemptId = ++self->authRequestId;
            setSubmitEnabled(false);

            if (! createNewAccount)
            {
                signIn(attemptId, usernameOrEmail, password);
                return;
            }

            juce::DynamicObject::Ptr registration = new juce::DynamicObject();
            registration->setProperty("username", usernameOrEmail);
            registration->setProperty("email", email);
            registration->setProperty("password", password);
            registration->setProperty("privacy_consent", true);
            if (displayName.isNotEmpty())
                registration->setProperty("display_name", displayName);

            self->apiClient.postJson("/auth/register", juce::var(registration.get()), {}, 5000,
                [safeThis, safePrompt, attemptId, usernameOrEmail, email,
                 showError, setSubmitEnabled](ApiClient::Response response)
                {
                    auto* component = safeThis.getComponent();
                    if (component == nullptr || attemptId != component->authRequestId)
                        return;

                    if (response.statusCode >= 200 && response.statusCode < 300)
                    {
                        component->accountStatusLabel.setText(
                            "Account created. Enter the code sent to " + email + ".",
                            juce::dontSendNotification);

                        if (auto* dialog = safePrompt.getComponent())
                            dialog->exitModalState(1);

                        // The modal window must be fully dismissed before the
                        // verification dialog is opened on top of it.
                        juce::MessageManager::callAsync([safeThis, email, usernameOrEmail]()
                        {
                            if (auto* self = safeThis.getComponent())
                                self->showEmailVerificationDialog(email, usernameOrEmail, true);
                        });
                        return;
                    }

                    setSubmitEnabled(true);

                    if (response.statusCode == 409)
                    {
                        component->accountStatusLabel.setText(
                            "Username or email is already registered.",
                            juce::dontSendNotification);
                        showError("Registration failed",
                                  "Username or email is already registered. "
                                  "If you created this account earlier, use VERIFY EMAIL to confirm it.");
                    }
                    else if (response.statusCode == 503)
                    {
                        component->accountStatusLabel.setText(
                            "Could not send the verification email. Try again later.",
                            juce::dontSendNotification);
                        showError("Registration failed",
                                  "The account was not created because the verification email "
                                  "could not be sent. Try again later.");
                    }
                    else if (response.statusCode == 422)
                    {
                        component->accountStatusLabel.setText(
                            "Registration details were rejected.",
                            juce::dontSendNotification);
                        showError("Registration failed",
                                  "Check the email address, username, and password "
                                  "(at least 6 characters).");
                    }
                    else
                    {
                        component->accountStatusLabel.setText(
                            "Registration failed. Check server and account details.",
                            juce::dontSendNotification);
                        showError("Registration failed",
                                  "Registration failed. Check the server connection and account details.");
                    }
                });
        };
    }

    prompt->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis, consentRow](int result)
        {
            if (result != 1)
                if (auto* self = safeThis.getComponent())
                    ++self->authRequestId;
        }), true);
}

void MainComponent::recoverOnlineAccountPassword()
{
    auto* prompt = new juce::AlertWindow(
        "Reset your password",
        "Enter the email address linked to your account. "
        "We will send a 6-digit password reset code.",
        juce::AlertWindow::NoIcon);

    prompt->addTextEditor("email", "", "Email");
    prompt->addButton("SEND CODE", 1);
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

    if (auto* submit = prompt->getButton(0))
    {
        submit->onClick = [safeThis, safePrompt, attemptId, setSubmitEnabled, showError]()
        {
            auto* self = safeThis.getComponent();
            auto* dialog = safePrompt.getComponent();
            if (self == nullptr || dialog == nullptr || attemptId != self->authRequestId)
                return;

            const auto emailAddress = dialog->getTextEditorContents("email").trim().toLowerCase();
            if (emailAddress.isEmpty() || ! emailAddress.containsChar('@'))
            {
                showError("Reset failed", "Enter the email address linked to your account.");
                return;
            }

            juce::DynamicObject::Ptr payload = new juce::DynamicObject();
            payload->setProperty("email", emailAddress);

            setSubmitEnabled(false);

            self->apiClient.postJson("/auth/forgot-password", juce::var(payload.get()), {}, 5000,
                [safeThis, safePrompt, attemptId, emailAddress,
                 setSubmitEnabled, showError](ApiClient::Response response)
                {
                    auto* component = safeThis.getComponent();
                    if (component == nullptr || attemptId != component->authRequestId)
                        return;

                    if (response.statusCode >= 200 && response.statusCode < 300)
                    {
                        component->accountStatusLabel.setText(
                            "If the account exists, a reset code was sent to " + emailAddress + ".",
                            juce::dontSendNotification);

                        if (auto* dialog = safePrompt.getComponent())
                            dialog->exitModalState(1);

                        juce::MessageManager::callAsync([safeThis, emailAddress]()
                        {
                            if (auto* self = safeThis.getComponent())
                                self->showPasswordResetDialog(emailAddress);
                        });
                    }
                    else
                    {
                        setSubmitEnabled(true);
                        showError("Reset failed",
                                  "Could not request a reset code. Check the server connection and try again.");
                    }
                });
        };
    }

    prompt->enterModalState(true,
        juce::ModalCallbackFunction::create([safeThis](int result)
        {
            if (result != 1)
                if (auto* self = safeThis.getComponent())
                    ++self->authRequestId;
        }), true);
}

void MainComponent::finishOnlineSignIn(bool createAccount,
                                       const juce::String& fallbackName,
                                       const juce::String& accessToken,
                                       int requestId,
                                       bool silent)
{
    juce::Component::SafePointer<MainComponent> safeThis(this);

    apiClient.getJson("/auth/me", accessToken, 4000,
        [safeThis, requestId, createAccount, fallbackName, silent](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || requestId != self->authRequestId)
                return;

            juce::String display = fallbackName;
            juce::String username = fallbackName;
            bool isAdmin = false;
            if (response.transportOk)
            {
                const auto profile = response.json;
                const auto serverUsername = profile.getProperty("username", juce::var()).toString();
                const auto fromServer = profile.getProperty("display_name", juce::var()).toString();
                if (serverUsername.isNotEmpty())
                    username = serverUsername;
                if (fromServer.isNotEmpty())
                    display = fromServer;
                else if (serverUsername.isNotEmpty())
                    display = serverUsername;

                // Additive field: an older server never sends it, so the admin
                // tools simply stay hidden.
                isAdmin = static_cast<bool>(
                    profile.getProperty("is_admin", juce::var(false)));
            }

            // The ADMIN toggle is restored only for an administrator. The
            // saved preference is kept so it survives a restart and a
            // non-admin session never re-enables it.
            self->onlineIsAdmin = isAdmin;
            bool savedPreviewMode = false;
            if (auto* settings = self->appProperties.getUserSettings())
                savedPreviewMode = settings->getBoolValue("AdminPreviewMode", false);
            self->adminPreviewMode = isAdmin && savedPreviewMode;
            self->adminPreviewButton.setToggleState(self->adminPreviewMode,
                                                    juce::dontSendNotification);

            self->currentUsername = username;
            self->currentUserLabel.setText(self->currentUsername, juce::dontSendNotification);
            self->showAuthScreen = false;
            self->resized();
            self->repaint();
            const auto successMessage = createAccount
                ? "Account created and signed in as " + display + ". Online scores are enabled."
                : "Signed in as " + display + ". Online scores are enabled.";
            self->accountStatusLabel.setText(successMessage, juce::dontSendNotification);

            if (! silent)
            {
                // Defer the confirmation until the credentials dialog has fully closed.
                // Opening a second async dialog inside the first dialog's modal callback
                // can leave the confirmation hidden behind the active modal window.
                const auto title = createAccount ? juce::String("Registration successful")
                                                 : juce::String("Sign-in successful");
                juce::MessageManager::callAsync([safeThis, title, successMessage]()
                {
                    if (auto* component = safeThis.getComponent())
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::MessageBoxIconType::NoIcon,
                            title,
                            successMessage,
                            "OK",
                            component);
                });
            }
            self->refreshGlobalLeaderboard();
            // Personal best scores require the freshly obtained token.
            self->refreshOnlineBestScores();
            self->refreshDailyChallenge();
            // The catalogue was loaded anonymously at startup; an admin with the
            // preview toggle remembered needs one authorised refresh to see the
            // drafts in the list.
            if (self->isAdminPreviewActive())
                self->refreshOnlineCatalog();
            // Start a new activity session for this sign-in and report it.
            self->telemetryClientSessionId = juce::Uuid().toString();
            self->telemetryTickCounter = 0;
            self->sendActivityHeartbeat(false);
        });
}

void MainComponent::storeRefreshToken(const juce::String& token)
{
    onlineRefreshToken = token;

    if (token.isNotEmpty())
        SessionStore::saveRefreshToken(token);
}

void MainComponent::clearSavedSession()
{
    onlineRefreshToken.clear();
    SessionStore::clearRefreshToken();
}

void MainComponent::revokeSavedSession()
{
    const auto token = onlineRefreshToken.isNotEmpty()
        ? onlineRefreshToken
        : SessionStore::loadRefreshToken();

    if (token.isEmpty())
        return;

    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty("refresh_token", token);

    // Fire-and-forget: the user is already signed out locally, and a failed
    // revoke must not keep the interface waiting.
    apiClient.postJson("/auth/logout", juce::var(body.get()), {}, 4000,
                       [](ApiClient::Response) {});
}

void MainComponent::tryRestoreSavedSession()
{
    if (sessionRestoreInFlight)
        return;

    const auto savedToken = SessionStore::loadRefreshToken();

    if (savedToken.isEmpty())
    {
        sessionOfflineRetryAvailable = false;
        return;
    }

    sessionRestoreInFlight = true;
    showAuthScreen = true;
    accountStatusLabel.setText("Restoring your session...", juce::dontSendNotification);
    sessionOfflineRetryAvailable = false;
    resized();
    repaint();

    juce::Component::SafePointer<MainComponent> safeThis(this);
    const int requestId = ++authRequestId;

    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty("refresh_token", savedToken);

    apiClient.postJson("/auth/refresh", juce::var(body.get()), {}, 6000,
        [safeThis, requestId, savedToken](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->sessionRestoreInFlight = false;

            if (requestId != self->authRequestId)
                return;

            const auto accessToken =
                response.json.getProperty("access_token", juce::var()).toString();

            if (accessToken.isNotEmpty())
            {
                const auto rotated =
                    response.json.getProperty("refresh_token", juce::var()).toString();

                self->storeRefreshToken(rotated.isNotEmpty() ? rotated : savedToken);
                self->onlineAccessToken = accessToken;

                juce::String hint = "player";

                if (auto* settings = self->appProperties.getUserSettings())
                    hint = settings->getValue("SavedUsername", "player");

                self->finishOnlineSignIn(false, hint, accessToken, requestId, true);
                return;
            }

            const bool rejected = response.statusCode == 400
                || response.statusCode == 401
                || response.statusCode == 403
                || response.statusCode == 404
                || response.statusCode == 422;

            if (rejected)
            {
                // The server proved the session is gone: drop the secret and
                // show the normal sign-in screen.
                self->clearSavedSession();
                self->accountStatusLabel.setText(
                    "Session expired. Sign in again.",
                    juce::dontSendNotification);
                self->sessionOfflineRetryAvailable = false;
            }
            else
            {
                // Transport error or server outage: keep the session and let
                // the player retry instead of forcing a new password.
                self->accountStatusLabel.setText(
                    "Cannot reach the server. Your saved session is kept.",
                    juce::dontSendNotification);
                self->sessionOfflineRetryAvailable = true;
            }

            self->resized();
            self->repaint();
        });
}

void MainComponent::logoutCurrentUser()
{
    // Invalidate a sign-in chain that may still be waiting for the server.
    ++authRequestId;

    // Revoke the refresh session on the server (best effort) and forget the
    // local secret. The access token is dropped regardless of the response.
    revokeSavedSession();
    clearSavedSession();
    // Server-reported best scores belong to the signed-in account; the
    // locally saved values stay available after signing out.
    ++bestScoresRequestId;
    remoteBestScores.clear();
    updateCatalogRows();
    // Close the activity session while the token is still valid.
    sendActivityHeartbeat(true);
    telemetryClientSessionId.clear();
    onlineAccessToken.clear();
    onlineGameSessionUuid.clear();
    currentSessionIsDaily = false;
    // The admin tools belong to the account, not to the machine: hide them and
    // drop the preview mode. The saved preference stays in the settings so the
    // next admin sign-in restores it.
    onlineIsAdmin = false;
    adminPreviewMode = false;
    currentSessionIsPreview = false;
    adminPreviewButton.setToggleState(false, juce::dontSendNotification);
    dailyChallengeSongSlug.clear();
    dailyChallengeSongId.clear();
    dailyOfficialUsed = false;
    dailyChallengeButton.setButtonText("DAILY CHALLENGE");
    dailyChallengeStatusLabel.setText({}, juce::dontSendNotification);
    currentUsername.clear();
    currentUserLabel.setText(juce::String(), juce::dontSendNotification);
    accountStatusLabel.setText(juce::String(), juce::dontSendNotification);
    showAuthScreen = true;
    gameState = GameState::SongSelect;
    resized();
    repaint();
}

void MainComponent::sendActivityHeartbeat(bool finalEvent)
{
    if (onlineAccessToken.isEmpty() || telemetryClientSessionId.isEmpty())
        return;

    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("client_session_id", telemetryClientSessionId);
    payload->setProperty("app_version", juce::String(ProjectInfo::versionString));
    payload->setProperty("platform", juce::SystemStats::getOperatingSystemName());
    payload->setProperty("final", finalEvent);

    // Fire-and-forget: runs on a worker thread, the result is ignored.
    // Telemetry must never affect gameplay or UI responsiveness.
    apiClient.postJson("/activity/heartbeat",
                       juce::var(payload.get()),
                       onlineAccessToken,
                       3000,
                       nullptr);
}

void MainComponent::sendGameLaunchTelemetry()
{
    if (onlineAccessToken.isEmpty() || telemetryClientSessionId.isEmpty())
        return;

    // A stable, human-readable key for every kind of game start, including
    // locally generated melodies that have no server song id.
    juce::String gameKey;

    if (selectedSongSlug.isNotEmpty())
        gameKey = "song:" + selectedSongSlug;
    else if (selectedSongId.isNotEmpty())
        gameKey = "local:" + selectedSongId;
    else
        gameKey = "practice";

    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("client_session_id", telemetryClientSessionId);
    payload->setProperty("game_key", gameKey);

    if (onlineSongUuid.isNotEmpty())
        payload->setProperty("song_id", onlineSongUuid);

    apiClient.postJson("/activity/game-launch",
                       juce::var(payload.get()),
                       onlineAccessToken,
                       3000,
                       nullptr);
}

void MainComponent::startOnlineGameSession()
{
    onlineGameSessionUuid.clear();

    // Count the launch even when the game stays local (no server song id).
    sendGameLaunchTelemetry();

    // A response from an older start request must not resurrect a session.
    const int requestId = ++onlineSessionRequestId;

    if (onlineSongUuid.isEmpty() || onlineAccessToken.isEmpty())
        return;

    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("song_id", onlineSongUuid);
    payload->setProperty("speed_percent",
                         juce::jlimit(50, 120,
                                      juce::roundToInt(
                                          static_cast<double>(speedPercent))));
    payload->setProperty("daily", currentSessionIsDaily);

    juce::Component::SafePointer<MainComponent> safeThis(this);
    apiClient.postJson("/games/sessions", juce::var(payload.get()), onlineAccessToken, 4000,
        [safeThis, requestId](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || requestId != self->onlineSessionRequestId)
                return;

            if (!response.transportOk)
            {
                self->accountStatusLabel.setText("Could not start online session; score will stay local.",
                                                 juce::dontSendNotification);
                return;
            }

            if (response.statusCode == 409)
            {
                const auto detail =
                    response.json.getProperty("detail", juce::var()).toString();

                if (detail.isNotEmpty())
                {
                    if (self->currentSessionIsDaily)
                        self->dailyChallengeStatusLabel.setText(
                            detail, juce::dontSendNotification);
                    else
                        self->accountStatusLabel.setText(
                            detail, juce::dontSendNotification);
                }

                self->currentSessionIsDaily = false;
                return;
            }

            self->onlineGameSessionUuid = response.json.getProperty("id", juce::var()).toString();

            // The server is authoritative for the session kind: a draft attempt
            // comes back as "preview" (nothing is ranked, no XP is awarded).
            // Older servers only know "normal"/"daily", so the flag simply
            // stays false for them.
            const auto sessionMode =
                response.json.getProperty("mode", juce::var()).toString();
            if (sessionMode.isNotEmpty())
                self->currentSessionIsPreview = sessionMode == "preview";

            // The daily flag applies to exactly this session; a replay starts
            // a normal session unless the daily button is used again.
            self->currentSessionIsDaily = false;

            if (self->onlineGameSessionUuid.isEmpty())
                self->accountStatusLabel.setText("Server did not create a session; score will stay local.",
                                                 juce::dontSendNotification);
        });
}

void MainComponent::submitOnlineGameResult()
{
    if (onlineGameSessionUuid.isEmpty() || onlineAccessToken.isEmpty())
        return;

    const int hits = gameplayStats.getHitCount();
    const int misses = gameplayStats.getMissCount();
    const int total = hits + misses;
    const double accuracy = total > 0 ? 100.0 * static_cast<double>(hits) / total : 0.0;

    // Completion is measured against the actual number of target notes in the
    // MIDI chart, not against the score. On a natural finish this is ~100%;
    // the server still clamps it and rejects attempts below 50%.
    const int notesTotal = static_cast<int>(gameSession.getMidiNotes().size());
    const int notesPlayed = notesTotal > 0
        ? juce::jmin(notesTotal, total)
        : total;
    const double completion = notesTotal > 0
        ? juce::jlimit(0.0, 100.0,
                       100.0 * static_cast<double>(notesPlayed)
                           / static_cast<double>(notesTotal))
        : 100.0;

    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty("score_points", juce::jmax(0, gameplayStats.getScore()));
    payload->setProperty("accuracy_percent", juce::jlimit(0.0, 100.0, accuracy));
    payload->setProperty("max_combo", juce::jmax(0, gameplayStats.getMaxCombo()));
    payload->setProperty("perfect_count", juce::jmax(0, gameplayStats.getPerfectCount()));
    payload->setProperty("good_count", juce::jmax(0, gameplayStats.getGreatCount() + gameplayStats.getGoodCount()));
    payload->setProperty("miss_count", juce::jmax(0, misses));
    payload->setProperty("completion_percent", completion);
    payload->setProperty("notes_total", juce::jmax(0, notesTotal));
    payload->setProperty("notes_played", juce::jmax(0, notesPlayed));

    const double averageTiming = gameplayStats.getAverageTimingErrorMs();

    if (averageTiming >= 0.0)
        payload->setProperty("avg_timing_error_ms", averageTiming);

    const auto sessionUuid = onlineGameSessionUuid;
    onlineGameSessionUuid.clear();
    // The finished game cannot adopt a session that is still being created.
    ++onlineSessionRequestId;

    juce::Component::SafePointer<MainComponent> safeThis(this);
    apiClient.postJson("/games/sessions/" + sessionUuid + "/finish",
                       juce::var(payload.get()), onlineAccessToken, 5000,
        [safeThis](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr)
                return;

            if (!response.transportOk)
            {
                self->accountStatusLabel.setText("Score saved locally; server submission failed.",
                                                 juce::dontSendNotification);
                return;
            }

            if (response.json.getProperty("id", juce::var()).toString().isNotEmpty())
            {
                self->accountStatusLabel.setText(
                    self->currentSessionIsPreview
                        ? "Preview attempt saved. This result is not ranked."
                        : "Online score submitted. Refreshing global rating...",
                    juce::dontSendNotification);

                // Progression returned by the server: XP, level and any
                // achievements unlocked by this attempt. A preview attempt of
                // an unpublished draft never awards any, so the results card
                // keeps its progression block hidden.
                const auto& json = response.json;

                if (json.hasProperty("xp_earned") && ! self->currentSessionIsPreview)
                {
                    self->lastProgressionValid = true;
                    self->lastXpEarned =
                        static_cast<int>(json.getProperty("xp_earned", 0));
                    self->lastXpTotal =
                        static_cast<int>(json.getProperty("xp_total", 0));
                    self->lastLevel =
                        static_cast<int>(json.getProperty("level", 1));
                    self->lastXpForNext =
                        static_cast<int>(json.getProperty(
                            "xp_to_next",
                            json.getProperty("xp_for_next", 0)));

                    // A finished attempt can raise the player level, which may
                    // unlock the next difficulty tier: refresh the catalogue
                    // state without waiting for the next progression poll.
                    if (self->lastLevel > 0)
                    {
                        self->progressionLevel = self->lastLevel;
                        self->songListBox.repaint();
                        self->refreshSongUnlockState();
                    }

                    juce::String achievements;

                    if (auto* awarded = json.getProperty("achievements_awarded", juce::var())
                                            .getArray())
                    {
                        for (const auto& item : *awarded)
                        {
                            const auto title =
                                item.getProperty("title", juce::var()).toString();

                            if (title.isNotEmpty())
                                achievements << (achievements.isEmpty() ? "" : "  |  ")
                                             << title;
                        }
                    }

                    self->lastAchievementsText = achievements;
                }

                self->refreshGlobalLeaderboard();
                // Pick up the new personal best for this song right away.
                self->refreshOnlineBestScores();
                // The official daily attempt is now used; update the button.
                self->refreshDailyChallenge();
            }
            else
            {
                self->accountStatusLabel.setText("Score saved locally; server rejected online result.",
                                                 juce::dontSendNotification);
            }
        });
}

void MainComponent::refreshGlobalLeaderboard()
{
    leaderboardLabel.setText("Loading global rating...", juce::dontSendNotification);

    const int requestId = ++leaderboardRequestId;
    juce::Component::SafePointer<MainComponent> safeThis(this);

    apiClient.getJson("/leaderboard/global?limit=10", {}, 4000,
        [safeThis, requestId](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || requestId != self->leaderboardRequestId)
                return;

            if (!response.transportOk)
            {
                self->leaderboardLabel.setText("Server unavailable. Start the FretFight server and try again.",
                                               juce::dontSendNotification);
                return;
            }

            const auto parsed = response.json;
            if (!parsed.isObject())
            {
                self->leaderboardLabel.setText("Could not read rating response from server.",
                                               juce::dontSendNotification);
                return;
            }

            const auto itemsValue = parsed.getProperty("items", juce::var());
            const auto* items = itemsValue.getArray();
            if (items == nullptr || items->isEmpty())
            {
                self->leaderboardLabel.setText(
                    "No player results yet. Play a song and submit a score to appear here.",
                    juce::dontSendNotification);
                return;
            }

            std::vector<MainLeaderboardModel::Row> rows;
            rows.reserve(static_cast<size_t>(items->size()));

            for (const auto& item : *items)
            {
                MainLeaderboardModel::Row row;
                row.userId = item.getProperty("user_id", juce::var()).toString();
                row.username = item.getProperty("username", juce::var()).toString();
                row.displayName =
                    item.getProperty("display_name", juce::var()).toString();
                row.rank = static_cast<int>(item.getProperty("rank", 0));
                row.level = static_cast<int>(item.getProperty("level", 1));
                row.rating =
                    static_cast<int>(item.getProperty("rating_points", 0));
                rows.push_back(row);
            }

            if (self->leaderboardModel != nullptr)
            {
                // Highlight the signed-in player the same way the account
                // window does. Set on every refresh so a login or rename is
                // reflected without recreating the table.
                self->leaderboardModel->setCurrentUsername(self->currentUsername);
                self->leaderboardModel->setRows(std::move(rows));
            }

            self->leaderboardTable.updateContent();
            self->leaderboardTable.repaint();
            self->leaderboardLabel.setText(
                "Click a player to open their card.",
                juce::dontSendNotification);
        });
}

namespace
{
    // Collected by the background thread of downloadOnlineSong() and applied to
    // the song library on the message thread.
    struct OnlineCatalogDownload
    {
        bool ok = false;
        juce::String title;
        juce::String slug;
        juce::String artist;
        juce::String songId;
        double bpm = 120.0;
        int version = 0;
        bool usedCache = false;
        juce::String assetState;
        juce::String missingRoles;
        juce::File songFolder;
        juce::File midiFile;
        juce::File drumsFile;
        juce::File bassFile;
        juce::File guitarFile;
        juce::File othersFile;
    };

    juce::File onlineSongsCacheRoot()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("GuitarBattle")
            .getChildFile("OnlineSongs");
    }
}

void MainComponent::rebuildCatalogRows(const juce::var& serverItems)
{
    // Remember the highlighted row so a catalogue refresh keeps the selection.
    juce::String previousSlug;
    juce::String previousTitle;
    if (juce::isPositiveAndBelow(selectedCatalogIndex,
                                 static_cast<int>(catalogSongs.size())))
    {
        previousSlug = catalogSongs[static_cast<size_t>(selectedCatalogIndex)].slug;
        previousTitle = catalogSongs[static_cast<size_t>(selectedCatalogIndex)].title;
    }

    std::vector<CatalogSong> rows;

    if (const auto* items = serverItems.getArray())
    {
        const auto cacheRoot = onlineSongsCacheRoot();

        for (const auto& item : *items)
        {
            const auto slug = item.getProperty("slug", juce::var()).toString();
            if (slug.isEmpty())
                continue;

            CatalogSong row;
            row.slug = slug;
            row.serverId = item.getProperty("id", juce::var()).toString();
            row.title = item.getProperty("title", juce::var()).toString();
            if (row.title.isEmpty())
                row.title = slug;
            row.artist = item.getProperty("artist", juce::var()).toString();
            row.genre = item.getProperty("genre", juce::var()).toString();
            row.previewUrl = item.getProperty("preview_url", juce::var()).toString();
            row.bpm = static_cast<double>(item.getProperty("bpm", 120));
            row.difficulty = static_cast<int>(item.getProperty("difficulty", 1));
            row.version = static_cast<int>(item.getProperty("version", 0));
            // Additive: an older server does not send the field, and every row
            // it returns is a published song.
            row.published = static_cast<bool>(
                item.getProperty("is_published", juce::var(true)));
            row.online = true;
            row.downloaded = findOnlineLibraryEntry(slug) != nullptr
                || cacheRoot.getChildFile(slug).getChildFile("version.txt").existsAsFile();
            rows.push_back(row);
        }
    }

    // The catalogue is ordered by the published difficulty, then by title, so
    // the list reads as a progression instead of an alphabetical dump. The
    // selection is restored by slug below, so sorting does not move the cursor.
    std::stable_sort(rows.begin(), rows.end(),
                     [](const CatalogSong& a, const CatalogSong& b)
                     {
                         if (a.difficulty != b.difficulty)
                             return a.difficulty < b.difficulty;

                         return a.title.compareIgnoreCase(b.title) < 0;
                     });

    catalogSongs = std::move(rows);

    int newIndex = 0;
    for (int i = 0; i < static_cast<int>(catalogSongs.size()); ++i)
    {
        const auto& row = catalogSongs[static_cast<size_t>(i)];
        if ((previousSlug.isNotEmpty() && row.slug == previousSlug)
            || (previousSlug.isEmpty() && previousTitle.isNotEmpty()
                && row.title == previousTitle))
        {
            newIndex = i;
            break;
        }
    }

    selectedCatalogIndex = catalogSongs.empty()
        ? 0 : juce::jlimit(0, static_cast<int>(catalogSongs.size()) - 1, newIndex);

    updateCatalogRows();

    if (!catalogSongs.empty())
        selectCatalogSong(selectedCatalogIndex);
}

void MainComponent::setAdminPreviewMode(bool enabled)
{
    // Only an administrator with a valid token may ask the server for drafts;
    // the server ignores the flag for everybody else anyway.
    adminPreviewMode = enabled && onlineIsAdmin;

    adminPreviewButton.setToggleState(adminPreviewMode, juce::dontSendNotification);

    if (auto* settings = appProperties.getUserSettings())
    {
        settings->setValue("AdminPreviewMode", adminPreviewMode);
        settings->saveIfNeeded();
    }

    // Switching the toggle on/off changes the contents of the catalogue, so it
    // is reloaded with the matching request. The request runs on a worker
    // thread; only the callback touches the UI.
    refreshOnlineCatalog();
    repaint();
}

void MainComponent::refreshOnlineCatalog()
{
    // The catalogue refresh is also a natural retry point when the initial
    // sound-pack download failed (for example while the network was offline).
    if (! clientAssetsReady && ! clientAssetsRefreshInFlight)
        startClientAssetRefresh();

    onlineCatalogButton.setEnabled(false);

    // An admin in preview mode asks for drafts and authenticates the request;
    // everybody else (and an older server without the parameter) keeps the
    // original anonymous published-only request.
    const bool withDrafts = isAdminPreviewActive();
    const auto cataloguePath = withDrafts
        ? juce::String("/songs?limit=100&offset=0&include_drafts=1")
        : juce::String("/songs?limit=100&offset=0");
    const auto catalogueToken = withDrafts ? onlineAccessToken : juce::String();

    onlineCatalogStatusLabel.setText(withDrafts
                                         ? "Loading song catalogue (drafts included)..."
                                         : "Loading song catalogue...",
                                     juce::dontSendNotification);

    // The request runs on an ApiClient worker thread; only this callback touches
    // components. A failure keeps the last server catalogue, if one was loaded.
    juce::Component::SafePointer<MainComponent> safeThis(this);
    apiClient.getJson(cataloguePath, catalogueToken, 6000,
        [safeThis](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr)
                return;

            self->onlineCatalogButton.setEnabled(true);

            if (!response.transportOk || !response.json.isObject())
            {
                self->onlineCatalogStatusLabel.setText(
                    "Server unavailable. Press REFRESH CATALOG to retry.",
                    juce::dontSendNotification);
                return;
            }

            const auto itemsValue = response.json.getProperty("items", juce::var());
            const auto* items = itemsValue.getArray();
            const int onlineCount = items != nullptr ? items->size() : 0;

            self->rebuildCatalogRows(itemsValue);

            // Count the drafts so the admin knows the toggle did something.
            int draftCount = 0;
            if (items != nullptr)
                for (const auto& item : *items)
                    if (! static_cast<bool>(
                            item.getProperty("is_published", juce::var(true))))
                        ++draftCount;

            if (onlineCount <= 0)
            {
                self->onlineCatalogStatusLabel.setText(
                    "No published songs yet. Publish a song, then press REFRESH CATALOG.",
                    juce::dontSendNotification);
            }
            else
            {
                juce::String status;
                status << onlineCount << " online song"
                       << (onlineCount == 1 ? "" : "s")
                       << " in the catalogue";
                if (draftCount > 0)
                    status << " (" << draftCount << " draft"
                           << (draftCount == 1 ? "" : "s") << ")";
                status << ". Select a track and press START SONG.";
                self->onlineCatalogStatusLabel.setText(status,
                                                       juce::dontSendNotification);
            }

            // Personal bests need the token, so they are only fetched when the
            // player is signed in.
            self->refreshOnlineBestScores();

            self->resized();
            self->repaint();
        });
}

void MainComponent::refreshOnlineBestScores()
{
    if (onlineAccessToken.isEmpty())
        return;

    const int requestId = ++bestScoresRequestId;
    juce::Component::SafePointer<MainComponent> safeThis(this);

    apiClient.getJson("/games/best-scores", onlineAccessToken, 6000,
        [safeThis, requestId](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || requestId != self->bestScoresRequestId)
                return;

            if (!response.transportOk || !response.json.isObject())
                return;

            self->ratingPoints = static_cast<int>(
                response.json.getProperty("rating_points", 0));

            const auto itemsValue = response.json.getProperty("items", juce::var());
            const auto* items = itemsValue.getArray();
            if (items == nullptr)
                return;

            for (const auto& item : *items)
            {
                const auto slug = item.getProperty("song_slug", juce::var()).toString();
                const auto score = static_cast<int>(item.getProperty("best_score", 0));
                if (slug.isEmpty() || score <= 0)
                    continue;

                auto& stored = self->remoteBestScores[slug];
                stored = juce::jmax(stored, score);
            }

            self->updateCatalogRows();
            self->repaint();
            self->refreshProgressionSummary();
        });
}

void MainComponent::refreshProgressionSummary()
{
    if (onlineAccessToken.isEmpty())
    {
        progressionSummaryLabel.setText({}, juce::dontSendNotification);
        return;
    }

    juce::Component::SafePointer<MainComponent> safeThis(this);

    apiClient.getJson("/profile/progression", onlineAccessToken, 5000,
        [safeThis](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || !response.transportOk
                || !response.json.isObject())
                return;

            const auto& json = response.json;
            const int level = static_cast<int>(json.getProperty("level", 1));
            const int xp = static_cast<int>(json.getProperty("xp_total", 0));
            const int xpForNext =
                static_cast<int>(json.getProperty("xp_for_next", 0));
            const int unlocked =
                static_cast<int>(json.getProperty("achievements_unlocked", 0));
            const int total =
                static_cast<int>(json.getProperty("achievements_total", 0));

            self->progressionLevel = level;
            self->progressionXp = xp;
            self->progressionAchievements = unlocked;
            self->progressionAchievementsTotal = total;
            self->progressionXpForNext = xpForNext;
            self->progressionXpRemaining =
                static_cast<int>(json.getProperty("xp_remaining", 0));

            // The catalogue unlocks by level, so a level-up must re-evaluate the
            // rows and the START button immediately.
            self->songListBox.repaint();
            self->refreshSongUnlockState();

            juce::String text;
            text << "LEVEL " << level
                 << "   |   " << xp << " XP"
                 << "   |   " << xpForNext << " XP TO NEXT"
                 << "   |   " << unlocked << "/" << total << " ACHIEVEMENTS"
                 << "   |   RATING " << self->ratingPoints;

            self->progressionSummaryLabel.setText(text,
                                                  juce::dontSendNotification);
            self->overlayComponent.repaint();
            self->repaint();
        });
}

void MainComponent::refreshDailyChallenge()
{
    if (onlineAccessToken.isEmpty())
    {
        dailyChallengeSongSlug.clear();
        dailyChallengeSongId.clear();
        dailyChallengeStatusLabel.setText({}, juce::dontSendNotification);
        return;
    }

    juce::Component::SafePointer<MainComponent> safeThis(this);

    apiClient.getJson("/daily/today", onlineAccessToken, 5000,
        [safeThis](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr)
                return;

            if (! response.transportOk || ! response.json.isObject())
            {
                self->dailyChallengeStatusLabel.setText(
                    "Daily challenge is unavailable right now.",
                    juce::dontSendNotification);
                return;
            }

            const auto& json = response.json;
            const int weekday =
                static_cast<int>(json.getProperty("weekday", 0));

            static const char* dayNames[] =
            {
                "Monday", "Tuesday", "Wednesday", "Thursday",
                "Friday", "Saturday", "Sunday"
            };

            const auto dayName = juce::String(
                dayNames[juce::jlimit(0, 6, weekday)]);

            const auto song = json.getProperty("song", juce::var());

            if (! song.isObject())
            {
                self->dailyChallengeSongSlug.clear();
                self->dailyChallengeSongId.clear();
                self->dailyOfficialUsed = false;
                self->dailyChallengeButton.setButtonText("DAILY CHALLENGE");
                self->dailyChallengeButton.setColour(
                    juce::TextButton::buttonColourId, UiColors::surface2);
                self->dailyChallengeButton.setColour(
                    juce::TextButton::textColourOffId, UiColors::muted);
                self->dailyChallengeStatusLabel.setText(
                    "No challenge scheduled for " + dayName + " (UTC).",
                    juce::dontSendNotification);
                self->repaint();
                return;
            }

            self->dailyChallengeSongSlug =
                song.getProperty("slug", juce::var()).toString();
            self->dailyChallengeSongId =
                song.getProperty("id", juce::var()).toString();
            self->dailyOfficialUsed =
                static_cast<int>(
                    json.getProperty("official_attempt_used", 0)) != 0;

            const auto title =
                song.getProperty("title", juce::var()).toString();

            self->dailyChallengeButton.setButtonText(
                "DAILY  |  " + (title.isNotEmpty() ? title : dayName));

            self->dailyChallengeButton.setColour(
                juce::TextButton::buttonColourId, UiColors::accent);
            self->dailyChallengeButton.setColour(
                juce::TextButton::textColourOffId, UiColors::onAccent);

            if (self->dailyOfficialUsed)
            {
                const auto result =
                    json.getProperty("official_result", juce::var());

                juce::String status = "Official attempt used.";

                if (result.isObject())
                {
                    status << " Score "
                           << static_cast<int>(result.getProperty("score_points", 0));

                    const auto rank = result.getProperty("rank", juce::var());

                    if (! rank.isVoid())
                        status << "  |  rank #" << static_cast<int>(rank);

                    status << "  |  replays are practice only.";
                }
                else
                {
                    status << " Replays are practice only.";
                }

                self->dailyChallengeStatusLabel.setText(
                    status, juce::dontSendNotification);
            }
            else
            {
                self->dailyChallengeStatusLabel.setText(
                    dayName + " (UTC) challenge - one official attempt.",
                    juce::dontSendNotification);
            }
        });
}

void MainComponent::onDailyChallengeClicked()
{
    if (onlineAccessToken.isEmpty())
        return;

    if (dailyChallengeSongSlug.isEmpty())
    {
        dailyChallengeStatusLabel.setText(
            "No challenge is scheduled for today.", juce::dontSendNotification);
        return;
    }

    // One official attempt per day; after it is used the same song can still
    // be played as a normal practice session.
    currentSessionIsDaily = ! dailyOfficialUsed;
    // The daily song is always published, so its attempt is never a preview.
    currentSessionIsPreview = false;

    dailyChallengeStatusLabel.setText(
        currentSessionIsDaily
            ? "Loading the official daily challenge..."
            : "Loading today's challenge in practice mode...",
        juce::dontSendNotification);

    downloadOnlineSong(dailyChallengeSongSlug, dailyChallengeSongId, true);
}

void MainComponent::openPublicProfile(const juce::String& userId)
{
    if (userId.isEmpty() || onlineAccessToken.isEmpty())
        return;

    juce::Component::SafePointer<MainComponent> safeThis(this);

    apiClient.getJson("/profile/users/" + userId, onlineAccessToken, 5000,
        [safeThis](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr)
                return;

            if (! response.transportOk)
            {
                self->accountStatusLabel.setText(
                    "Could not load the player card.", juce::dontSendNotification);
                return;
            }

            const auto& json = response.json;
            auto name = json.getProperty("display_name", juce::var()).toString();

            if (name.isEmpty())
                name = json.getProperty("username", juce::var()).toString();

            juce::String text;
            text << "Level: " << static_cast<int>(json.getProperty("level", 1))
                 << "\nXP: " << static_cast<int>(json.getProperty("xp_total", 0))
                 << "\nSongs mastered: "
                 << static_cast<int>(json.getProperty("songs_mastered", 0))
                 << "\n\n";

            const auto* achievements =
                json.getProperty("achievements", juce::var()).getArray();
            const int unlocked = achievements != nullptr
                ? achievements->size()
                : static_cast<int>(json.getProperty("achievements_unlocked", 0));

            text << "Achievements (" << unlocked << "):\n";

            if (achievements != nullptr)
            {
                for (const auto& item : *achievements)
                    text << "* "
                         << item.getProperty("title", juce::var()).toString()
                         << "  (+"
                         << static_cast<int>(item.getProperty("xp_bonus", 0))
                         << " XP)\n";
            }

            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::NoIcon,
                name.isNotEmpty() ? name : juce::String("Player"),
                text, "OK", self);
        });
}

void MainComponent::downloadOnlineSong(const juce::String& slug,
                                       const juce::String& serverId,
                                       bool startAfterDownload)
{
    juce::Component::SafePointer<MainComponent> safeThis(this);
    auto download = std::make_shared<OnlineCatalogDownload>();

    // A draft is only reachable with the admin token: the detail lookup and
    // every file download carry it in preview mode. For a published song (or a
    // non-admin) the token stays empty and the requests are anonymous exactly
    // as before.
    const auto songToken = isAdminPreviewActive() ? onlineAccessToken : juce::String();

    // Detail lookup and file downloads run on the ApiClient worker threads.
    // Only the message-thread callback below touches components, the song library
    // or the gameplay state.
    auto backgroundWork = [download, slug, serverId, songToken]()
    {
        if (slug.isEmpty())
            return;

        const auto detailResponse = ApiClient::get("/songs/" + slug, songToken, 5000);
        if (!detailResponse.transportOk)
            return;

        const auto detail = detailResponse.json;
        if (!detail.isObject())
            return;

        // The server describes asset completeness in the same song object as the
        // metadata.  Older servers do not send it; the client then behaves as before.
        const auto readinessValue = detail.getProperty("readiness", juce::var());
        if (readinessValue.isObject())
        {
            download->assetState = readinessValue.getProperty("state", juce::var()).toString();
            if (const auto* missing = readinessValue.getProperty("missing_roles", juce::var()).getArray())
            {
                juce::StringArray roles;
                for (const auto& role : *missing)
                {
                    const auto name = role.toString();
                    if (name.isNotEmpty())
                        roles.add(name);
                }
                download->missingRoles = roles.joinIntoString(", ");
            }
        }

        const auto filesValue = detail.getProperty("files", juce::var());
        const auto* files = filesValue.getArray();
        if (files == nullptr)
            return;

        const auto cacheRoot = juce::File::getSpecialLocation(
            juce::File::userApplicationDataDirectory)
            .getChildFile("GuitarBattle")
            .getChildFile("OnlineSongs");
        const auto songFolder = cacheRoot.getChildFile(slug);
        if (!songFolder.createDirectory())
            return;

        const auto midiFile = songFolder.getChildFile(slug + ".mid");
        const auto versionFile = songFolder.getChildFile("version.txt");

        // Resolve every downloadable asset of the song detail to its cache path.
        struct PendingAsset
        {
            juce::String role;
            juce::String downloadUrl;
            juce::File targetFile;
            juce::int64 expectedSize = 0;
            bool isMidi = false;
        };

        std::vector<PendingAsset> assets;
        bool assetsComplete = true;
        bool hasMidi = false;

        for (const auto& fileInfo : *files)
        {
            const auto role = fileInfo.getProperty("role", juce::var()).toString().toLowerCase();
            const auto fileUrl = fileInfo.getProperty("download_url", juce::var()).toString();
            if (fileUrl.isEmpty())
                continue;

            PendingAsset asset;
            asset.role = role;
            asset.downloadUrl = fileUrl;
            asset.expectedSize = static_cast<juce::int64>(fileInfo.getProperty("size_bytes", 0));
            asset.isMidi = role == "midi";

            if (asset.isMidi)
                asset.targetFile = midiFile;
            else if (role == "drums")
                asset.targetFile = songFolder.getChildFile("drums.mp3");
            else if (role == "bass")
                asset.targetFile = songFolder.getChildFile("bass.mp3");
            else if (role == "guitar")
                asset.targetFile = songFolder.getChildFile("guitar.mp3");
            else if (role == "others")
                asset.targetFile = songFolder.getChildFile("others.mp3");
            else
                continue;

            // Without a trustworthy size the cached files cannot be validated.
            if (asset.expectedSize <= 0)
                assetsComplete = false;

            hasMidi = hasMidi || asset.isMidi;
            assets.push_back(std::move(asset));
        }

        if (!hasMidi || !assetsComplete)
            return;

        const auto serverVersion = static_cast<int>(detail.getProperty("version", 0));
        const auto cachedVersion = versionFile.existsAsFile()
            ? versionFile.loadFileAsString().trim().getIntValue()
            : 0;

        auto hasMidiHeader = [](const juce::File& file)
        {
            juce::FileInputStream stream(file);
            if (!stream.openedOk())
                return false;

            char header[4] {};
            return stream.read(header, 4) == 4
                && header[0] == 'M' && header[1] == 'T'
                && header[2] == 'h' && header[3] == 'd';
        };

        // The cache is reused only when the server content version matches and
        // every published file is present with the advertised size.
        auto usedCache = serverVersion > 0 && cachedVersion == serverVersion;
        if (usedCache)
            for (const auto& asset : assets)
                if (!asset.targetFile.existsAsFile()
                    || asset.targetFile.getSize() != asset.expectedSize
                    || (asset.isMidi && !hasMidiHeader(asset.targetFile)))
                {
                    usedCache = false;
                    break;
                }

        if (usedCache)
        {
            DBG("ONLINE CATALOG: using cached assets for version " + juce::String(serverVersion));
        }
        else
        {
            // A version mismatch forces every stem to be downloaded again, even
            // if the previously cached file happens to have the same size.
            auto allAssetsDownloaded = true;

            for (const auto& asset : assets)
            {
                const auto writtenBytes = ApiClient::downloadToFile(asset.downloadUrl,
                                                                    asset.targetFile,
                                                                    songToken,
                                                                    5000);
                const auto sizeOk = writtenBytes > 0 && writtenBytes == asset.expectedSize;
                const auto headerOk = !asset.isMidi || (sizeOk && hasMidiHeader(asset.targetFile));

                if (!sizeOk || !headerOk)
                {
                    DBG("ONLINE CATALOG: could not download " + asset.role
                        + " from " + asset.downloadUrl);
                    asset.targetFile.deleteFile();

                    if (asset.isMidi)
                        return;

                    allAssetsDownloaded = false;
                    continue;
                }

                DBG("ONLINE CATALOG: downloaded " + asset.role + " ("
                    + juce::String(writtenBytes) + " bytes)");
            }

            // Only a complete asset set may be marked as this content version.
            if (allAssetsDownloaded && serverVersion > 0
                && !versionFile.replaceWithText(juce::String(serverVersion)))
                DBG("ONLINE CATALOG: could not write " + versionFile.getFullPathName());
        }

        juce::File drumsFile, bassFile, guitarFile, othersFile;
        for (const auto& asset : assets)
        {
            if (!asset.targetFile.existsAsFile())
                continue;

            if (asset.role == "drums")         drumsFile = asset.targetFile;
            else if (asset.role == "bass")     bassFile = asset.targetFile;
            else if (asset.role == "guitar")   guitarFile = asset.targetFile;
            else if (asset.role == "others")   othersFile = asset.targetFile;
        }

        const auto title = detail.getProperty("title", juce::var()).toString();
        if (title.isEmpty())
            return;

        download->title = title;
        download->slug = slug;
        download->artist = detail.getProperty("artist", juce::var()).toString();
        download->songId = detail.getProperty("id", juce::var()).toString();
        if (download->songId.isEmpty())
            download->songId = serverId;
        download->bpm = static_cast<double>(detail.getProperty("bpm", 120));
        download->version = serverVersion;
        download->usedCache = usedCache;
        download->songFolder = songFolder;
        download->midiFile = midiFile;
        download->drumsFile = drumsFile;
        download->bassFile = bassFile;
        download->guitarFile = guitarFile;
        download->othersFile = othersFile;
        download->ok = true;
        DBG("ONLINE CATALOG: loaded " + title + " (version " + juce::String(serverVersion) + ")");
    };

    apiClient.runAsync(std::move(backgroundWork), [safeThis, download, startAfterDownload]()
    {
        auto* self = safeThis.getComponent();
        if (self == nullptr)
            return;

        self->songPridumayButton.setEnabled(true);

        bool loaded = false;

        if (download->ok)
        {
            // Reuse the cached library entry created by an earlier download.
            auto* found = self->findOnlineLibraryEntry(download->slug);

            if (found == nullptr)
            {
                SongEntry entry;
                entry.title = download->title;
                entry.slug = download->slug;
                entry.artist = download->artist;
                entry.folder = download->songFolder;
                entry.soloMidi = download->midiFile;
                entry.drums = download->drumsFile;
                entry.bass = download->bassFile;
                entry.rhythmGuitar = download->guitarFile;
                entry.others = download->othersFile;
                entry.bpm = download->bpm;
                self->songLibrary.push_back(entry);
            }
            else
            {
                found->title = download->title;
                found->artist = download->artist;
                found->folder = download->songFolder;
                found->soloMidi = download->midiFile;
                found->drums = download->drumsFile;
                found->bass = download->bassFile;
                found->rhythmGuitar = download->guitarFile;
                found->others = download->othersFile;
                found->bpm = download->bpm;
            }

            // Mark the catalogue row as cached so the list reflects the state.
            for (auto& row : self->catalogSongs)
            {
                if (! row.online || row.slug != download->slug)
                    continue;

                row.serverId = download->songId;
                row.title = download->title;
                row.bpm = download->bpm;
                row.version = download->version;
                row.downloaded = true;
            }

            self->updateCatalogRows();

            if (startAfterDownload
                && self->gameState == MainComponent::GameState::SongSelect)
            {
                // Open the online session for exactly this song.
                self->onlineSongUuid = download->songId;
                self->onlineGameSessionUuid.clear();
                loaded = self->loadSongMidi(download->title);

                // Keep the highlight on the song that was actually loaded, even
                // if the player clicked another row while the download ran.
                if (loaded)
                {
                    for (int i = 0; i < static_cast<int>(self->catalogSongs.size()); ++i)
                        if (self->catalogSongs[static_cast<size_t>(i)].slug == download->slug)
                        {
                            self->selectCatalogSong(i);
                            break;
                        }
                }
            }
        }

        juce::String statusText = download->ok
            ? "Downloaded: " + download->title
            : juce::String("Could not download the selected song. Check the connection and try again.");
        if (loaded)
        {
            statusText = "Online song loaded";
            if (download->version > 0)
                statusText << " (v" << download->version
                           << (download->usedCache ? ", cached)" : ")");
            if (download->assetState == "midi_only")
                statusText << " [MIDI only]";
            else if (download->missingRoles.isNotEmpty())
                statusText << " [missing: " << download->missingRoles << "]";
            statusText << ". Select another track or press START SONG to replay.";
        }

        self->onlineCatalogStatusLabel.setText(statusText, juce::dontSendNotification);
        self->resized();
        self->repaint();
        if (loaded)
            self->beginTuningForSelectedSong();
    });
}


//==============================================================================
// Self-update
//==============================================================================

namespace
{
    /** "1.2.10" > "1.2.9". Missing components count as 0 ("1.2" == "1.2.0")
        and a leading "v" is ignored, so the server may use either form.
    */
    int compareVersionStrings (const juce::String& left, const juce::String& right)
    {
        auto normalise = [] (juce::String value)
        {
            value = value.trim();

            if (value.startsWithIgnoreCase ("v"))
                value = value.substring (1);

            return value;
        };

        const auto leftParts = juce::StringArray::fromTokens (normalise (left), ".", "");
        const auto rightParts = juce::StringArray::fromTokens (normalise (right), ".", "");
        const int numParts = juce::jmax (leftParts.size(), rightParts.size());

        for (int i = 0; i < numParts; ++i)
        {
            const int a = i < leftParts.size() ? leftParts[i].getIntValue() : 0;
            const int b = i < rightParts.size() ? rightParts[i].getIntValue() : 0;

            if (a != b)
                return a < b ? -1 : 1;
        }

        return 0;
    }

    /** Turns the API path ("/api/v1/downloads/game") or an absolute URL into
        a URL the download thread can open. The origin comes from ApiClient so
        a domain change only has to touch one constant.
    */
    juce::String resolveUpdateUrl (const juce::String& urlOrPath)
    {
        if (urlOrPath.startsWithIgnoreCase ("http://")
            || urlOrPath.startsWithIgnoreCase ("https://"))
            return urlOrPath;

        return juce::String (ApiClient::serverOrigin)
            + (urlOrPath.startsWithChar ('/') ? urlOrPath : "/" + urlOrPath);
    }

    juce::String skippedUpdateVersion (juce::ApplicationProperties& properties)
    {
        if (auto* settings = properties.getUserSettings())
            return settings->getValue ("SkippedUpdateVersion").trim();

        return {};
    }

    /** Never trust a server-supplied name as a path: keep it a plain file name. */
    juce::String safeInstallerFileName (const juce::String& serverName,
                                        const juce::String& version)
    {
        auto name = juce::File::createLegalFileName (serverName.trim());

        if (name.isEmpty() || name.startsWithChar ('.'))
            name = "FretFightSetup-" + version + ".exe";

        return name;
    }

    /** Downloads the installer on a worker thread and reports progress in a
        modal progress window. The window owns itself and deletes itself when
        the thread finishes, following the JUCE ThreadWithProgressWindow
        pattern. Nothing here touches the audio thread.
    */
    class UpdateDownloadTask final : public juce::ThreadWithProgressWindow
    {
    public:
        UpdateDownloadTask (juce::File target,
                            juce::String sourceUrl,
                            juce::String versionToInstall,
                            juce::int64 expectedBytes,
                            std::function<void (bool, juce::File)> onFinished)
            : juce::ThreadWithProgressWindow ("FretFight update", true, true,
                                              3000, "CANCEL"),
              targetFile (std::move (target)),
              url (std::move (sourceUrl)),
              version (std::move (versionToInstall)),
              expectedSize (expectedBytes),
              completion (std::move (onFinished))
        {
            setStatusMessage ("Downloading FretFight " + version + "...");
        }

        void run() override
        {
            juce::int64 totalBytes = 0;

            {
                int statusCode = 0;

                auto stream = juce::URL (url).createInputStream (
                    juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                        .withConnectionTimeoutMs (15000)
                        .withStatusCode (&statusCode));

                // A 404/500 body must never be saved as an installer.
                if (stream == nullptr
                    || (statusCode != 0 && statusCode != 200))
                {
                    failed = true;
                    return;
                }

                targetFile.deleteFile();

                juce::FileOutputStream output (targetFile);

                if (! output.openedOk())
                {
                    failed = true;
                    return;
                }

                constexpr int bufferSize = 64 * 1024;
                juce::HeapBlock<char> buffer (bufferSize);

                for (;;)
                {
                    if (threadShouldExit())
                    {
                        cancelled = true;
                        break;
                    }

                    const int numRead = stream->read (buffer.getData(), bufferSize);

                    if (numRead <= 0)
                        break;

                    if (! output.write (buffer.getData(),
                                        static_cast<size_t> (numRead)))
                    {
                        failed = true;
                        break;
                    }

                    totalBytes += numRead;

                    if (expectedSize > 0)
                        setProgress (juce::jlimit (0.0, 1.0,
                                                   static_cast<double> (totalBytes)
                                                       / static_cast<double> (expectedSize)));

                    juce::String status = "Downloading FretFight " + version + ": "
                        + juce::File::descriptionOfSizeInBytes (totalBytes);

                    if (expectedSize > 0)
                        status << " of " << juce::File::descriptionOfSizeInBytes (expectedSize);

                    setStatusMessage (status);
                }

                if (! output.getStatus().wasOk())
                    failed = true;
            }

            // A connection dropped mid-stream produces a short file: never hand
            // a truncated installer to the shell.
            if (expectedSize > 0 && totalBytes != expectedSize)
                failed = true;

            if (totalBytes <= 0)
                failed = true;

            if (cancelled || failed)
                targetFile.deleteFile();
        }

        void threadComplete (bool userPressedCancel) override
        {
            if (userPressedCancel)
            {
                cancelled = true;
                targetFile.deleteFile();
            }

            if (completion != nullptr)
                completion (! cancelled && ! failed, targetFile);

            delete this;
        }

    private:
        juce::File targetFile;
        juce::String url;
        juce::String version;
        juce::int64 expectedSize = 0;
        std::function<void (bool, juce::File)> completion;
        bool failed = false;
        bool cancelled = false;
    };
}

void MainComponent::checkForUpdatesOnLaunch()
{
    if (updateCheckStarted)
        return;

    updateCheckStarted = true;

    const auto localVersion = juce::String (ProjectInfo::versionString).trim();

    if (localVersion.isEmpty())
        return;

    juce::Component::SafePointer<MainComponent> safeThis (this);

    // Runs on ApiClient's worker pool; the callback is delivered on the
    // message thread. Being offline is a normal state and stays silent.
    apiClient.getJson ("/landing", {}, 6000,
        [safeThis, localVersion] (ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr || ! response.isJsonObject())
                return;

            const auto download = response.json.getProperty ("download", {});

            if (! static_cast<bool> (download.getProperty ("available", false)))
                return;

            const auto remoteVersion = download.getProperty ("version", {}).toString().trim();
            const auto downloadUrl = download.getProperty ("url", {}).toString().trim();

            if (remoteVersion.isEmpty() || downloadUrl.isEmpty())
                return;

            if (compareVersionStrings (remoteVersion, localVersion) <= 0)
                return;

            if (remoteVersion == skippedUpdateVersion (self->appProperties))
                return;

            // Optional "what's new" text edited in the admin panel. Older
            // servers do not send the field; the dialog falls back to the
            // plain version prompt when it is empty.
            const auto releaseNotes = download.getProperty ("release_notes", {}).toString().trim();

            self->promptForUpdate (remoteVersion,
                                   downloadUrl,
                                   download.getProperty ("filename", {}).toString(),
                                   static_cast<juce::int64> (download.getProperty ("size_bytes", 0)),
                                   releaseNotes);
        });
}

void MainComponent::promptForUpdate (const juce::String& version,
                                     const juce::String& downloadUrl,
                                     const juce::String& filename,
                                     juce::int64 sizeBytes,
                                     const juce::String& releaseNotes)
{
    const auto notes = releaseNotes.replace ("\r\n", "\n")
                                   .replace ("\r", "\n")
                                   .trim();

    juce::String message;
    message << "FretFight " << version << " is available. You are running "
            << ProjectInfo::versionString << ".";

    if (sizeBytes > 0)
        message << "\n\nDownload size: "
                << juce::File::descriptionOfSizeInBytes (sizeBytes) << ".";

    if (notes.isEmpty())
        message << "\n\nDownload and install the update now?";

    auto* prompt = new juce::AlertWindow ("Update available", message,
                                          juce::AlertWindow::InfoIcon);

    // The release notes are authored in the admin panel and are read-only in
    // the dialog: the player can scroll and copy them, but the text is never
    // editable on the client. Capturing the editor in the modal callback keeps
    // it alive for as long as the alert window needs it.
    std::shared_ptr<juce::TextEditor> notesBox;

    if (notes.isNotEmpty())
    {
        notesBox = std::make_shared<juce::TextEditor>();
        // AlertWindow paints a non-empty component name as a caption above it.
        notesBox->setName ("What's new in " + version);
        notesBox->setMultiLine (true, true);
        notesBox->setReadOnly (true);
        notesBox->setScrollbarsShown (true);
        notesBox->setCaretVisible (false);
        notesBox->setPopupMenuEnabled (true);
        notesBox->setEscapeAndReturnKeysConsumed (false);
        notesBox->setFont (GameUi::font (14.0f));
        notesBox->setColour (juce::TextEditor::backgroundColourId, UiColors::surface2);
        notesBox->setColour (juce::TextEditor::textColourId, UiColors::text);
        notesBox->setColour (juce::TextEditor::outlineColourId, UiColors::border);
        notesBox->setColour (juce::TextEditor::focusedOutlineColourId, UiColors::borderStrong);
        notesBox->setColour (juce::TextEditor::highlightColourId, UiColors::accent.withAlpha (0.28f));
        notesBox->setColour (juce::TextEditor::highlightedTextColourId, UiColors::text);
        notesBox->setIndents (10, 8);
        notesBox->setText (notes, false);
        // AlertWindow derives its own width from this size (at 80%) and places
        // the component as-is, so the height also caps the scroll area.
        notesBox->setSize (420, 150);
        prompt->addCustomComponent (notesBox.get());
        prompt->addTextBlock ("Download and install the update now?");
    }

    prompt->addButton ("UPDATE NOW", 1);
    prompt->addButton ("LATER", 0);
    prompt->addButton ("SKIP VERSION", 2);

    juce::Component::SafePointer<MainComponent> safeThis (this);

    prompt->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [safeThis, version, downloadUrl, filename, sizeBytes, notesBox] (int result)
            {
                auto* self = safeThis.getComponent();

                if (self == nullptr)
                    return;

                if (result == 1)
                {
                    self->startUpdateDownload (version, downloadUrl,
                                               filename, sizeBytes);
                }
                else if (result == 2)
                {
                    if (auto* settings = self->appProperties.getUserSettings())
                        settings->setValue ("SkippedUpdateVersion", version);
                }
            }),
        true);
}

void MainComponent::startUpdateDownload (const juce::String& version,
                                         const juce::String& downloadUrl,
                                         const juce::String& filename,
                                         juce::int64 sizeBytes)
{
   #if JUCE_MAC
    juce::ignoreUnused (version, downloadUrl, filename, sizeBytes);

    // The build published on the site is the Windows installer. A Mac needs the
    // .dmg from the download page, so the update opens that page instead of
    // downloading a file the system cannot run.
    juce::URL (juce::String (ApiClient::serverOrigin) + "/").launchInDefaultBrowser();
   #else
    auto updateDirectory = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("FretFightUpdate");
    updateDirectory.deleteRecursively();
    updateDirectory.createDirectory();

    const auto targetFile = updateDirectory.getChildFile (
        safeInstallerFileName (filename, version));

    juce::Component::SafePointer<MainComponent> safeThis (this);

    auto* task = new UpdateDownloadTask (
        targetFile,
        resolveUpdateUrl (downloadUrl),
        version,
        sizeBytes,
        [safeThis] (bool success, juce::File installerFile)
        {
            // The progress window has already closed; defer the follow-up
            // dialog so it is not created from inside the window's teardown.
            juce::MessageManager::callAsync ([safeThis, success, installerFile]
            {
                auto* self = safeThis.getComponent();

                if (self == nullptr)
                    return;

                if (! success)
                {
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "Update download failed",
                        "FretFight could not download the update. "
                        "Check the connection and try again later.",
                        "OK");
                    return;
                }

                self->installDownloadedUpdate (installerFile);
            });
        });

    task->launchThread();
   #endif
}

void MainComponent::installDownloadedUpdate (const juce::File& installerFile)
{
    if (! installerFile.existsAsFile())
        return;

    auto* prompt = new juce::AlertWindow (
        "Update downloaded",
        "FretFight will close, install the update and start again.",
        juce::AlertWindow::InfoIcon);
    prompt->addButton ("INSTALL NOW", 1);
    prompt->addButton ("LATER", 0);

    juce::Component::SafePointer<MainComponent> safeThis (this);

    prompt->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [safeThis, installerFile] (int result)
            {
                if (result != 1)
                    return;

                // Silent in-place upgrade. The installer keeps the same AppId,
                // the same folder and the settings in %APPDATA%, so the wizard
                // would only add clicks; it relaunches the game when it is done
                // (see the [Run] section of docs/FretFight.iss).
                const juce::String updateArguments (
                    "/SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /SP-");

                if (! installerFile.startAsProcess (updateArguments))
                {
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "Could not start the installer",
                        "The installer was downloaded to:\n"
                            + installerFile.getFullPathName()
                            + "\n\nStart it manually to finish the update.",
                        "OK");
                    return;
                }

                // Quit after the modal callback returns: the installer replaces
                // the running executable and needs this process gone.
                juce::MessageManager::callAsync ([]()
                {
                    if (auto* app = juce::JUCEApplication::getInstance())
                        app->systemRequestedQuit();
                });
            }),
        true);
}

