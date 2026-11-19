#include "../MainComponent.h"
#include "../UI/PlayerCard.h"
#include "../UI/GameStyle.h"
#include "../UI/FriendsPanel.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#if JUCE_WINDOWS
 #include <windows.h>
#elif JUCE_MAC
 #include <unistd.h>
 #include <crt_externs.h>
 #include <spawn.h>
#endif

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
            int characterId = 0;
            juce::String characterName;
            juce::String characterSheetUrl;
            juce::String email;
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

                characterId = static_cast<int>(
                    profile.getProperty("character_id", 0));
                characterName = profile.getProperty(
                    "character_name", juce::var()).toString();
                characterSheetUrl = profile.getProperty(
                    "character_sheet_url", juce::var()).toString();
                // Shown in the HOME account summary line.
                email = profile.getProperty("email", juce::var()).toString();
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
            self->accountEmail = email;            self->showAuthScreen = false;
            self->currentSection = AppSection::Home;
            self->matchOriginSection = AppSection::Home;

            // A new session starts with an empty notification memory: the first
            // poll primes it and only events that arrive later pop up.
            self->notificationsPrimed = false;
            self->seenNotificationIds.clear();
            self->refreshSocialNotifications();

            // Profile character: the account choice wins; a brand-new profile
            // starts with Ray selected. Existing profiles without a character
            // keep their local choice instead of being forced to a default.
            if (characterId > 0 && characterName.isNotEmpty()
                && characterSheetUrl.isNotEmpty())
            {
                self->applyCharacterFromServer (characterId, characterName,
                                                characterSheetUrl);
            }
            else if (createAccount)
            {
                self->applyDefaultCharacterIfNeeded (true);
            }
            else if (self->selectedCharacterId > 0
                     && self->selectedCharacterSheetUrl.isNotEmpty())
            {
                self->setSelectedCharacter (self->selectedCharacterId,
                                            self->selectedCharacterName,
                                            self->selectedCharacterSheetUrl);
            }

            if (self->sectionSidebar != nullptr)
                self->sectionSidebar->setUser (self->currentUsername,
                                               self->progressionLevel);

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
            // Merge server-side tower progress after a successful sign-in:
            // uploads local clears, then takes max(local, server) and handles
            // any newly unlocked character. Runs entirely on ApiClient's pool.
            self->syncServerCampaignProgress();
            // Warm the Battle list in the background, so opening the section
            // shows the towers immediately instead of waiting for a request.
            self->refreshServerCampaigns (true);
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
    currentSessionIsPractice = false;
    // Notifications belong to the account: drop the history pop-up and the
    // corner cards, and forget which events were already shown.
    closeNotificationPopup();
    notificationsPrimed = false;
    seenNotificationIds.clear();
    socialUnreadNotifications = 0;

    if (notificationToasts != nullptr)
        notificationToasts->dismissAll();

    if (sectionSidebar != nullptr)
        sectionSidebar->setUnreadNotifications (0);
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

    // The hub falls back to the signed-out landing screen. The embedded
    // profile is destroyed asynchronously: logout can be triggered from inside
    // the AccountWindow itself.
    currentSection = AppSection::Home;
    matchOriginSection = AppSection::Home;
    campaignFightActive = false;
    tunerStandalone = false;
    tunerActive.store (false, std::memory_order_relaxed);

    if (profileViewport != nullptr)
        profileViewport->setVisible (false);

    {
        juce::Component::SafePointer<MainComponent> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* self = safeThis.getComponent())
            {
                if (self->profileViewport != nullptr)
                {
                    self->removeChildComponent (self->profileViewport.get());
                    self->profileViewport.reset();
                    self->profileContent.reset();
                }
            }
        });
    }

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
    // A Practice-section run is stored by the server for audit only: no XP, no
    // rating, no personal best. Older servers ignore the unknown field.
    payload->setProperty("practice", currentSessionIsPractice);

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
            // comes back as "preview" and a Practice-section run as "practice"
            // (nothing is ranked, no XP is awarded). Older servers only know
            // "normal"/"daily", so the flags follow the local request.
            const auto sessionMode =
                response.json.getProperty("mode", juce::var()).toString();
            if (sessionMode.isNotEmpty())
            {
                self->currentSessionIsPreview = sessionMode == "preview";
                self->currentSessionIsPractice = sessionMode == "practice";
            }

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
    // An online duel is scored by the /duels endpoints; a Battle ("tower")
    // stage is scored by the campaign stage-result endpoint. Submitting the
    // same run as a regular game session would award progression twice.
    if (campaignFightActive)
        return;

    if (duelOnlinePhase != DuelOnlinePhase::None
        && duelOnlinePhase != DuelOnlinePhase::Finished)
        return;

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

    // Capture the attempt kind now: the player may leave the results screen
    // (which clears the flags) before the response arrives.
    const bool previewRun = currentSessionIsPreview;
    const bool practiceRun = currentSessionIsPractice;

    juce::Component::SafePointer<MainComponent> safeThis(this);
    apiClient.postJson("/games/sessions/" + sessionUuid + "/finish",
                       juce::var(payload.get()), onlineAccessToken, 5000,
        [safeThis, previewRun, practiceRun](ApiClient::Response response)
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
                const bool unranked = previewRun || practiceRun;

                if (unranked)
                {
                    // An unranked attempt never contributes to the results
                    // card's progression block.
                    self->lastProgressionValid = false;
                    self->lastAchievementsText.clear();
                }

                self->accountStatusLabel.setText(
                    practiceRun
                        ? "Practice attempt saved. This result is not ranked."
                        : previewRun
                            ? "Preview attempt saved. This result is not ranked."
                            : "Online score submitted. Refreshing global rating...",
                    juce::dontSendNotification);

                // Progression returned by the server: XP, level and any
                // achievements unlocked by this attempt. A preview attempt of
                // an unpublished draft and a Practice-section run never award
                // any, so the results card keeps its progression block hidden.
                const auto& json = response.json;

                if (json.hasProperty("xp_earned") && ! unranked)
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

                    // A finished attempt can raise the player level: repaint the
                    // catalogue without waiting for the next progression poll.
                    if (self->lastLevel > 0)
                    {
                        self->progressionLevel = self->lastLevel;
                        self->songListBox.repaint();

                        if (self->campaignPanel != nullptr)
                            self->campaignPanel->setPlayerLevel (self->lastLevel);
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

                // An unranked attempt changes neither the rating nor the
                // personal bests, so there is nothing to refresh.
                if (! unranked)
                {
                    self->refreshGlobalLeaderboard();
                    // Pick up the new personal best for this song right away.
                    self->refreshOnlineBestScores();
                    // The official daily attempt is now used; update the button.
                    self->refreshDailyChallenge();
                }
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
    leaderboardLabel.setText("Loading Guitar Skills...", juce::dontSendNotification);

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

                if (const auto* object = item.getDynamicObject())
                {
                    if (object->hasProperty("guitar_skills"))
                        row.guitarSkills = static_cast<double>(
                            object->getProperty("guitar_skills"));
                }

                // accuracy_percent is the new field; older servers only send
                // best_accuracy. Use it as the fallback so the column is not
                // stuck at 0.0% before the server rollout.
                const double bestAccuracy = static_cast<double>(
                    item.getProperty("best_accuracy", 0.0));
                row.accuracyPercent = static_cast<double>(
                    item.getProperty("accuracy_percent", bestAccuracy));
                row.duelWinRate = static_cast<double>(
                    item.getProperty("duel_win_rate", 0.0));
                row.duelWins = static_cast<int>(
                    item.getProperty("duel_wins", 0));
                row.duelLosses = static_cast<int>(
                    item.getProperty("duel_losses", 0));
                row.duelDraws = static_cast<int>(
                    item.getProperty("duel_draws", 0));
                row.xpTotal = static_cast<int>(
                    item.getProperty("xp_total", 0));
                row.provisional = static_cast<bool>(
                    item.getProperty("provisional", false));

                // position_change is null on the first calculation; only draw
                // the arrow when the server actually knows the previous rank.
                if (const auto* object = item.getDynamicObject())
                {
                    if (object->hasProperty("position_change")
                        && ! object->getProperty("position_change").isVoid())
                    {
                        row.positionChange = static_cast<int>(
                            object->getProperty("position_change"));
                        row.hasPositionChange = true;
                    }
                }

                rows.push_back(row);
            }

            if (self->leaderboardModel != nullptr)
            {
                // Highlight the signed-in player the same way the account
                // window does. Set on every refresh so a login or rename is
                // reflected without recreating the table.
                self->leaderboardModel->setCurrentUsername(self->currentUsername);
                self->leaderboardModel->setRows(rows);
                // The page shows the top three as a podium and the table below.
                self->leaderboardModel->setFirstRow(
                    juce::jmin(3, static_cast<int>(rows.size())));
            }

            self->leaderboardTable.updateContent();
            self->leaderboardTable.repaint();
            self->leaderboardLabel.setText(
                "Ranked by Guitar Skills. PROV = provisional. Click a player to open their card.",
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
        int timeSignatureNumerator = 0;
        int timeSignatureDenominator = 0;
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
            // Additive: an older server does not send the meter, and the
            // metronome then keeps its 4/4 default.
            row.timeSignatureNumerator = static_cast<int>(
                item.getProperty("time_signature_numerator", juce::var(0)));
            row.timeSignatureDenominator = static_cast<int>(
                item.getProperty("time_signature_denominator", juce::var(0)));
            row.difficulty = static_cast<int>(item.getProperty("difficulty", 1));
            row.version = static_cast<int>(item.getProperty("version", 0));
            // Additive: an older server does not send the field, and every row
            // it returns is a published song.
            row.published = static_cast<bool>(
                item.getProperty("is_published", juce::var(true)));
            // Visuals assigned in the admin panel: the stage backdrop and the
            // opponent character. Older servers omit them; the client then
            // keeps the neutral stage.
            row.characterName = item.getProperty("character_name", juce::var()).toString();
            row.characterSheetUrl = item.getProperty("character_sheet_url", juce::var()).toString();
            row.stageName = item.getProperty("stage_name", juce::var()).toString();
            row.stageBackgroundUrl = item.getProperty("stage_background_url", juce::var()).toString();
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

    // The Battle stage rows are resolved against this catalogue: an open
    // Battle section must pick up the songs that just arrived, otherwise its
    // FIGHT button keeps saying "not in the catalogue" until the player leaves
    // and re-enters the section.
    applyServerCampaigns();

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

    // Admin mode reloads the catalogue with drafts; the already loaded rows and
    // the START button must switch immediately instead of waiting for the next
    // selection or catalogue refresh.
    updateStartButtonState();
    songListBox.repaint();
    repaint();
}

void MainComponent::refreshOnlineCatalog()
{
    if (onlineCatalogInFlight)
        return;

    onlineCatalogInFlight = true;

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

            self->onlineCatalogInFlight = false;
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

            // HOME account summary: the same numbers the personal account page
            // used to show in its stat cards.
            self->accountScoresText = juce::String(self->ratingPoints);
            self->accountSongsText = juce::String(static_cast<int>(
                response.json.getProperty("songs_played", 0)));
            self->accountAttemptsText = juce::String(static_cast<int>(
                response.json.getProperty("games_played", 0)));
            self->refreshHomeAccountRank();

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

void MainComponent::refreshHomeAccountRank()
{
    if (onlineAccessToken.isEmpty() || currentUsername.isEmpty())
        return;

    juce::Component::SafePointer<MainComponent> safeThis(this);

    apiClient.getJson("/leaderboard/global?limit=50", onlineAccessToken, 6000,
        [safeThis](ApiClient::Response response)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr || !response.transportOk
                || !response.json.isObject())
                return;

            const auto* items =
                response.json.getProperty("items", juce::var()).getArray();

            if (items == nullptr)
                return;

            for (const auto& item : *items)
            {
                if (item.getProperty("username", juce::var()).toString()
                    == self->currentUsername)
                {
                    const int rank = static_cast<int>(item.getProperty("rank", 0));

                    if (rank > 0)
                    {
                        self->accountRankText = "#" + juce::String(rank);
                        self->accountRankNote = "in the global top 50";
                        self->repaint();
                    }

                    return;
                }
            }

            self->accountRankText = "-";
            self->accountRankNote = "not in the top 50";
            self->repaint();
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

            if (self->campaignPanel != nullptr)
                self->campaignPanel->setPlayerLevel (level);

            // HOME account summary cards.
            self->accountLevelText = juce::String(level);
            self->accountXpText = juce::String(xp);
            self->accountXpNote = juce::String(xpForNext) + " to level "
                                  + juce::String(level + 1);

            const auto accountStats = json.getProperty("stats", juce::var());
            const auto accountAccuracy =
                accountStats.getProperty("accuracy", juce::var());

            if (! accountAccuracy.isVoid())
                self->accountAccuracyText = juce::String(juce::roundToInt(
                    static_cast<double>(accountAccuracy))) + "%";

            const auto accountTiming =
                accountStats.getProperty("timing", juce::var());

            self->accountTimingText = accountTiming.isVoid()
                ? juce::String("n/a")
                : juce::String(juce::roundToInt(
                      static_cast<double>(accountTiming))) + "%";

            // The catalogue repaint keeps the level tag in sync after a level-up.
            self->songListBox.repaint();

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
    // The daily song is always published, so its attempt is never a preview,
    // and an official attempt is never a Practice-section run.
    currentSessionIsPreview = false;
    currentSessionIsPractice = false;

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

    if (publicProfileViewport == nullptr)
    {
        publicProfileViewport = std::make_unique<juce::Viewport>();
        publicProfileViewport->setScrollBarsShown(true, false);
        publicProfileViewport->setScrollBarThickness(10);
        publicProfileViewport->setColour(
            juce::ScrollBar::thumbColourId,
            UiColors::accent.withAlpha(0.45f));
        addChildComponent(*publicProfileViewport);
    }

    if (publicProfilePanel == nullptr)
    {
        auto panel = std::make_unique<PublicProfilePanel>(apiClient, onlineAccessToken);
        panel->onBack = [safeThis = juce::Component::SafePointer<MainComponent>(this)]
        {
            if (auto* self = safeThis.getComponent())
                self->closePublicProfile();
        };
        panel->onChallenge = [safeThis = juce::Component::SafePointer<MainComponent>(this)]
                             (const juce::String&)
        {
            // Duels are lobby based: the challenge opens the Online section
            // where the player can host or join a room.
            if (auto* self = safeThis.getComponent())
            {
                self->closePublicProfile();
                self->setSection(AppSection::OnlineDuel);
            }
        };
        panel->onEditOwnProfile = [safeThis = juce::Component::SafePointer<MainComponent>(this)]
        {
            if (auto* self = safeThis.getComponent())
            {
                self->closePublicProfile();
                self->setSection(AppSection::Profile);
            }
        };
        panel->onMessage = [safeThis = juce::Component::SafePointer<MainComponent>(this)]
                           (const juce::String& userId, const juce::String& name)
        {
            if (auto* self = safeThis.getComponent())
                self->openMessagesFor (userId, name);
        };

        publicProfilePanel = std::move(panel);
        publicProfileViewport->setViewedComponent(publicProfilePanel.get(), false);
    }

    publicProfileReturnSection = currentSection;
    publicProfilePanel->open(userId);
    publicProfilePanel->setVisible(true);
    publicProfileViewport->setVisible(true);
    publicProfileViewport->toFront(false);
    resized();
    // A new profile always starts at the header, not at the previous scroll.
    publicProfileViewport->setViewPosition(0, 0);
    repaint();
}

void MainComponent::closePublicProfile()
{
    if (publicProfileViewport != nullptr)
        publicProfileViewport->setVisible(false);

    const auto back = publicProfileReturnSection;

    if (currentSection != back)
        setSection(back);
    else
    {
        resized();
        repaint();
    }
}

void MainComponent::openNotifications()
{
    if (onlineAccessToken.isEmpty())
        return;

    closeNotificationPopup();

    if (sectionSidebar == nullptr)
        return;

    const auto anchor = sectionSidebar->getNotificationsBounds();

    auto popup = std::make_unique<NotificationPopup> (apiClient, onlineAccessToken,
                                                      anchor);

    // The popup lives until it is dismissed; the callback must not delete a
    // newer popup that replaced this one in the meantime.
    juce::Component::SafePointer<NotificationPopup> safePopup (popup.get());

    popup->onDismiss = [safeThis = juce::Component::SafePointer<MainComponent> (this),
                        safePopup]
    {
        if (auto* self = safeThis.getComponent())
            if (self->notificationPopup.get() == safePopup.getComponent())
                self->notificationPopup.reset();
    };

    popup->onOpenProfile = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
                           (const juce::String& userId)
    {
        if (auto* self = safeThis.getComponent())
            self->openPublicProfile (userId);
    };

    popup->onUnreadChanged = [safeThis = juce::Component::SafePointer<MainComponent> (this)]
                             (int unread)
    {
        if (auto* self = safeThis.getComponent())
        {
            self->socialUnreadNotifications = juce::jmax (0, unread);

            if (self->sectionSidebar != nullptr)
                self->sectionSidebar->setUnreadNotifications (
                    self->socialUnreadNotifications);
        }
    };

    notificationPopup = std::move (popup);
    addAndMakeVisible (*notificationPopup);
    notificationPopup->setBounds (getLocalBounds());
    notificationPopup->toFront (true);
}

void MainComponent::closeNotificationPopup()
{
    notificationPopup.reset();
}

void MainComponent::refreshSocialNotifications()
{
    if (onlineAccessToken.isEmpty())
    {
        socialUnreadNotifications = 0;
        seenNotificationIds.clear();
        notificationsPrimed = false;

        if (sectionSidebar != nullptr)
            sectionSidebar->setUnreadNotifications(0);

        return;
    }

    juce::Component::SafePointer<MainComponent> safeThis(this);

    apiClient.getJson("/social/notifications?limit=20", onlineAccessToken, 6000,
        [safeThis](ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr || ! reply.transportOk)
                return;

            self->socialUnreadNotifications =
                static_cast<int>(reply.json.getProperty("unread", 0));

            if (self->sectionSidebar != nullptr)
                self->sectionSidebar->setUnreadNotifications(
                    self->socialUnreadNotifications);

            const auto* items =
                reply.json.getProperty("items", juce::var()).getArray();

            if (items == nullptr)
                return;

            // The first answer after sign-in only records what already exists:
            // the history must not be replayed as a burst of pop-ups.
            if (! self->notificationsPrimed)
            {
                for (const auto& item : *items)
                    self->seenNotificationIds.insert(
                        item.getProperty("id", juce::var()).toString());

                self->notificationsPrimed = true;
                return;
            }

            // The server lists newest first; walk backwards so the newest card
            // ends up at the bottom of the stack.
            for (int i = items->size() - 1; i >= 0; --i)
            {
                const auto& item = (*items)[i];
                const auto id = item.getProperty("id", juce::var()).toString();

                if (id.isEmpty() || self->seenNotificationIds.count(id) > 0)
                    continue;

                self->seenNotificationIds.insert(id);

                if (self->notificationToasts == nullptr)
                    continue;

                const auto payload = item.getProperty("payload", juce::var());
                const auto kind = item.getProperty("kind", juce::var()).toString();

                self->notificationToasts->push(
                    id, kind, NotificationText::describe(kind, payload),
                    payload.getProperty("user_id", juce::var()).toString());
            }

            // Keep the seen set bounded on a long-running session.
            if (self->seenNotificationIds.size() > 400)
            {
                self->seenNotificationIds.clear();

                for (const auto& item : *items)
                    self->seenNotificationIds.insert(
                        item.getProperty("id", juce::var()).toString());
            }
        });
}

void MainComponent::refreshMessageUnread()
{
    if (onlineAccessToken.isEmpty())
    {
        if (sectionSidebar != nullptr)
            sectionSidebar->setUnreadMessages(0);

        return;
    }

    juce::Component::SafePointer<MainComponent> safeThis(this);

    apiClient.getJson("/messages/unread", onlineAccessToken, 6000,
        [safeThis](ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr || ! reply.transportOk)
                return;

            if (self->sectionSidebar != nullptr)
                self->sectionSidebar->setUnreadMessages(
                    static_cast<int>(reply.json.getProperty("unread_total", 0)));
        });
}

void MainComponent::downloadOnlineSong(const juce::String& slug,
                                       const juce::String& serverId,
                                       bool startAfterDownload,
                                       bool forDuel)
{
    juce::Component::SafePointer<MainComponent> safeThis(this);
    auto download = std::make_shared<OnlineCatalogDownload>();

    // Show the bar immediately: the detail lookup plus the first stem can take
    // a few seconds, and a disabled START button alone does not explain why
    // nothing happens. The worker publishes the overall fraction into this
    // shared atomic; timerCallback() copies it into the ProgressBar, so the
    // network thread never touches a component.
    songDownloadProgressState = std::make_shared<std::atomic<float>>(0.0f);
    songDownloadProgressValue = 0.0;
    songDownloadProgressBar.setVisible(true);

    // Campaign: the next opponent's download starts behind the finished-stage
    // card. Relayout now so the card's buttons are hidden under the loading
    // overlay instead of staying clickable through it.
    if (gameState.load (std::memory_order_relaxed) == GameState::Finished
        && campaignFightActive)
    {
        resized();
        overlayComponent.repaint();
    }

    // A draft is only reachable with the admin token: the detail lookup and
    // every file download carry it in preview mode. For a published song (or a
    // non-admin) the token stays empty and the requests are anonymous exactly
    // as before.
    const auto songToken = isAdminPreviewActive() ? onlineAccessToken : juce::String();

    // Detail lookup and file downloads run on the ApiClient worker threads.
    // Only the message-thread callback below touches components, the song library
    // or the gameplay state.
    auto progressState = songDownloadProgressState;
    auto backgroundWork = [download, slug, serverId, songToken, progressState]()
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

        // Every asset has a server-advertised size (assetsComplete above), so
        // the overall fraction is exact instead of an estimate per file.
        juce::int64 totalExpectedBytes = 0;
        for (const auto& asset : assets)
            totalExpectedBytes += juce::jmax<juce::int64>(0, asset.expectedSize);

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
            if (progressState != nullptr)
                progressState->store(1.0f, std::memory_order_relaxed);

            DBG("ONLINE CATALOG: using cached assets for version " + juce::String(serverVersion));
        }
        else
        {
            // A version mismatch forces every stem to be downloaded again, even
            // if the previously cached file happens to have the same size.
            auto allAssetsDownloaded = true;
            juce::int64 completedBytes = 0;

            for (const auto& asset : assets)
            {
                const auto fileSize = juce::jmax<juce::int64>(0, asset.expectedSize);

                const auto writtenBytes = ApiClient::downloadToFile(
                    asset.downloadUrl,
                    asset.targetFile,
                    songToken,
                    5000,
                    [progressState, completedBytes, fileSize, totalExpectedBytes](
                        juce::int64 bytesWritten)
                    {
                        if (progressState == nullptr || totalExpectedBytes <= 0)
                            return;

                        const auto current = juce::jlimit<juce::int64>(
                            0, fileSize, bytesWritten);
                        const auto fraction =
                            static_cast<double>(completedBytes + current)
                            / static_cast<double>(totalExpectedBytes);

                        progressState->store(
                            static_cast<float>(juce::jlimit(0.0, 1.0, fraction)),
                            std::memory_order_relaxed);
                    });

                completedBytes += fileSize;

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

            if (progressState != nullptr)
                progressState->store(1.0f, std::memory_order_relaxed);

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
        download->timeSignatureNumerator = static_cast<int>(
            detail.getProperty("time_signature_numerator", juce::var(0)));
        download->timeSignatureDenominator = static_cast<int>(
            detail.getProperty("time_signature_denominator", juce::var(0)));
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

    apiClient.runAsync(std::move(backgroundWork), [safeThis, download, startAfterDownload, forDuel]()
    {
        auto* self = safeThis.getComponent();
        if (self == nullptr)
            return;

        // The worker is done; the bar has served its purpose. Reset the shared
        // state so a later download starts from zero.
        self->songDownloadProgressBar.setVisible(false);
        self->songDownloadProgressValue = 0.0;
        self->songDownloadProgressState.reset();

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
                entry.timeSignatureNumerator = download->timeSignatureNumerator;
                entry.timeSignatureDenominator = download->timeSignatureDenominator;
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
                found->timeSignatureNumerator = download->timeSignatureNumerator;
                found->timeSignatureDenominator = download->timeSignatureDenominator;
            }

            // Mark the catalogue row as cached so the list reflects the state.
            for (auto& row : self->catalogSongs)
            {
                if (! row.online || row.slug != download->slug)
                    continue;

                row.serverId = download->songId;
                row.title = download->title;
                row.bpm = download->bpm;
                row.timeSignatureNumerator = download->timeSignatureNumerator;
                row.timeSignatureDenominator = download->timeSignatureDenominator;
                row.version = download->version;
                row.downloaded = true;
            }

            self->updateCatalogRows();

            // A campaign/duel start can be pending while the screen is still
            // the previous result card (GameState::Finished): the next chart
            // must be loaded there too, or the player is stuck on SONG
            // COMPLETE with the duel state already cleared.
            const bool pendingDuelStart = self->duelPending
                || self->duelOnlinePlaybackPending;

            if (startAfterDownload
                && (self->gameState == MainComponent::GameState::SongSelect
                    || pendingDuelStart))
            {
                // Open the online session for exactly this song.
                self->onlineSongUuid = download->songId;
                self->onlineGameSessionUuid.clear();
                loaded = self->loadSongMidi(download->title);

                // Keep the highlight on the song that was actually loaded, even
                // if the player clicked another row while the download ran.
                // A pending duel must not run selectCatalogSong(): it would
                // start the preview and rewrite the campaign's song choice.
                if (loaded && ! pendingDuelStart)
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

        const bool onlinePreparing =
            self->duelOnlinePhase != MainComponent::DuelOnlinePhase::None
            && self->duelOnlinePhase != MainComponent::DuelOnlinePhase::Finished;

        if (forDuel)
            self->completeOnlineDuelSongPreparation(download->ok, download->slug);
        else if (loaded && ! onlinePreparing)
            self->beginTuningForSelectedSong();
        else if (startAfterDownload && ! loaded)
            self->abortPendingDuelStart(
                download->ok
                    ? juce::String("Could not load the next song. Press NEXT OPPONENT to retry.")
                    : juce::String("Could not download the next song. Check the connection and press NEXT OPPONENT again."));
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

    /** Never trust a server-supplied name as a path: keep it a plain file name. */
    juce::String safeUpdateFileName (const juce::String& serverName,
                                     const juce::String& version)
    {
        auto name = juce::File::createLegalFileName (serverName.trim());

        if (name.isEmpty() || name.startsWithChar ('.'))
        {
           #if JUCE_MAC
            // The macOS update channel ships the portable ZIP; the DMG is the
            // download-page installer. A missing name must not turn the update
            // into a DMG download by accident.
            name = "FretFight-" + version + ".zip";
           #else
            name = "FretFight-" + version + "-win64.zip";
           #endif
        }

        return name;
    }

   #if JUCE_MAC
    /** Folder that holds the updater's log and its state files. */
    juce::File updateStateDirectory()
    {
        return juce::File::getSpecialLocation (
                   juce::File::userApplicationDataDirectory)
                   .getChildFile ("Application Support")
                   .getChildFile ("FretFight");
    }

    /** Diagnostics file of the self-updater. macOS has no console for a
        double-clicked bundle, so every step of the swap is appended here.
    */
    juce::File updateLogFile()
    {
        return updateStateDirectory().getChildFile ("update.log");
    }

    /** Written right before the helper is started and removed by the helper
        once the new bundle is in place. A marker that survives to the next
        launch means the swap never completed.
    */
    juce::File updateAttemptFile()
    {
        return updateStateDirectory().getChildFile ("update-attempt.txt");
    }

    /** Starts a process that is independent of this app's lifetime.

        juce::ChildProcess forks on POSIX, and a forked child of a
        multithreaded GUI app can die before it reaches exec() (a lock held by
        another thread at fork time). The update helper then never writes its
        first log line: the app quits and the same update prompt comes back on
        the next launch. posix_spawn lets the kernel create the process, and
        the new session keeps it alive after this app exits.
    */
    bool startDetachedProcess (const juce::StringArray& arguments, int& pidOut)
    {
        pidOut = 0;

        if (arguments.isEmpty())
            return false;

        std::vector<std::string> storage;
        storage.reserve (static_cast<size_t> (arguments.size()));

        for (const auto& argument : arguments)
            storage.push_back (argument.toStdString());

        std::vector<char*> argv;
        argv.reserve (storage.size() + 1);

        for (auto& argument : storage)
            argv.push_back (const_cast<char*> (argument.c_str()));

        argv.push_back (nullptr);

        posix_spawnattr_t attributes;
        posix_spawnattr_init (&attributes);

       #if defined (POSIX_SPAWN_SETSID)
        posix_spawnattr_setflags (&attributes, static_cast<short> (POSIX_SPAWN_SETSID));
       #endif

        pid_t pid = 0;
        const auto result = posix_spawn (&pid,
                                         arguments[0].toRawUTF8(),
                                         nullptr,
                                         &attributes,
                                         argv.data(),
                                         *_NSGetEnviron());

        posix_spawnattr_destroy (&attributes);

        if (result != 0 || pid <= 0)
            return false;

        pidOut = static_cast<int> (pid);
        return true;
    }

    void logUpdate (const juce::String& message)
    {
        auto file = updateLogFile();
        file.getParentDirectory().createDirectory();

        // The log is a diagnostic aid, not a journal: keep it small.
        if (file.existsAsFile() && file.getSize() > 64 * 1024)
            file.deleteFile();

        file.appendText (juce::Time::getCurrentTime().toString (true, true)
                             + " " + message + "\n");
    }

    void showUpdateProblem (const juce::String& message)
    {
        logUpdate ("update problem: " + message);

        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Update could not be installed",
            message,
            "OK");
    }

    /** Runs a command and reports whether it exited with status 0. */
    bool runAndWait (const juce::StringArray& command, int timeoutMs)
    {
        juce::ChildProcess process;

        if (! process.start (command, 0))
            return false;

        return process.waitForProcessToFinish (timeoutMs)
            && process.getExitCode() == 0;
    }

    /** CFBundleShortVersionString of a staged bundle, or an empty string.
        Info.plist is often a binary plist, so it is read through PlistBuddy
        instead of parsing the file as text.
    */
    juce::String readBundleVersion (const juce::File& bundle)
    {
        const auto plist = bundle.getChildFile ("Contents").getChildFile ("Info.plist");

        if (! plist.existsAsFile())
            return {};

        juce::ChildProcess process;

        if (! process.start ({ "/usr/libexec/PlistBuddy", "-c",
                               "Print :CFBundleShortVersionString",
                               plist.getFullPathName() },
                             juce::ChildProcess::wantStdOut))
            return {};

        process.waitForProcessToFinish (5000);
        return process.readAllProcessOutput().trim();
    }
   #endif

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

            // The server has a dedicated portable-update channel: installers
            // stay on the download page, while the app fetches a ZIP. Fall
            // back to `downloads` for servers that predate the split (the
            // Windows installer is refused later by the .zip check; the macOS
            // DMG is a valid update payload).
            const auto updates = response.json.getProperty ("updates", {});
            const auto downloads = response.json.getProperty ("downloads", {});
            juce::var download;

           #if JUCE_MAC
            download = updates.getProperty ("macos", {});
            if (! download.isObject())
                download = downloads.getProperty ("macos", {});
           #else
            download = updates.getProperty ("windows", {});
            if (! download.isObject())
                download = downloads.getProperty ("windows", {});
           #endif

            if (! download.isObject())
                return;

            if (! static_cast<bool> (download.getProperty ("available", false)))
                return;

            const auto remoteVersion = download.getProperty ("version", {}).toString().trim();
            const auto downloadUrl = download.getProperty ("url", {}).toString().trim();

            if (remoteVersion.isEmpty() || downloadUrl.isEmpty())
                return;

            if (compareVersionStrings (remoteVersion, localVersion) <= 0)
                return;

           #if JUCE_MAC
            // A previous attempt left its marker behind: the helper never
            // finished the swap. Offering "Update now" again would loop
            // forever, so point the player at the installer instead.
            const auto attempt = updateAttemptFile();

            if (attempt.existsAsFile())
            {
                const auto attemptedVersion = attempt.loadFileAsString().trim();
                attempt.deleteFile();

                if (compareVersionStrings (attemptedVersion, localVersion) > 0)
                {
                    self->showUpdateFailurePrompt (attemptedVersion);
                    return;
                }
            }
           #endif

            // The check runs shortly after launch, but the player may already
            // have started a song. Never replace the app mid-session; the next
            // launch retries.
            if (self->gameState != GameState::SongSelect)
                return;

            juce::String effectiveUrl = downloadUrl;
            juce::String effectiveName = download.getProperty ("filename", {}).toString();
            auto effectiveSize = static_cast<juce::int64> (download.getProperty ("size_bytes", 0));
            auto releaseNotes = download.getProperty ("release_notes", {}).toString();
            bool installerMode = false;

           #if JUCE_WINDOWS
            // A per-user install (the installer default) owns its folder, so
            // the portable ZIP replaces the exe directly. An older per-machine
            // copy in Program Files is read-only: only the installer can
            // upgrade it in place, and only it can elevate.
            const auto appFolder = juce::File::getSpecialLocation (
                juce::File::currentExecutableFile).getParentDirectory();

            if (! appFolder.hasWriteAccess())
            {
                const auto installer = downloads.getProperty ("windows", {});

                if (installer.isObject()
                    && static_cast<bool> (installer.getProperty ("available", false)))
                {
                    const auto installerUrl = installer.getProperty ("url", {}).toString().trim();
                    const auto installerVersion =
                        installer.getProperty ("version", {}).toString().trim();

                    // Never install an older build than the update that was
                    // advertised: the installer and the portable ZIP are
                    // uploaded separately and can lag behind each other.
                    if (installerUrl.isNotEmpty()
                        && compareVersionStrings (installerVersion, remoteVersion) >= 0)
                    {
                        effectiveUrl = installerUrl;
                        effectiveName = installer.getProperty ("filename", {}).toString();
                        effectiveSize = static_cast<juce::int64> (
                            installer.getProperty ("size_bytes", 0));
                        releaseNotes = installer.getProperty ("release_notes", {}).toString();
                        installerMode = true;
                    }
                }
            }
           #endif

            // Never install behind the player's back: the prompt shows the
            // version and the admin-written release notes, and only the
            // "Update now" button starts the download.
            self->updatePromptVersion = remoteVersion;
            self->updatePromptUrl = effectiveUrl;
            self->updatePromptFilename = effectiveName;
            self->updatePromptSize = effectiveSize;
            self->updatePromptNotes = releaseNotes;
            self->updatePromptInstallerMode = installerMode;
            self->updatePromptPending = true;
        });
}

   #if JUCE_MAC
void MainComponent::showUpdateFailurePrompt (const juce::String& version)
{
    const auto currentVersion = juce::String (ProjectInfo::versionString).trim();
    const auto logPath = updateLogFile().getFullPathName();

    juce::String message;
    message << "FretFight " << version << " could not be installed automatically, so the game "
            << "is still on " << currentVersion << ".\n\n"
            << "Install it from the download page:\n"
            << "1. Open https://fretfight.online in your browser.\n"
            << "2. Download the macOS version.\n"
            << "3. Quit FretFight, drag FretFight.app into Applications and choose Replace.\n\n"
            << "Diagnostics: " << logPath;

    logUpdate ("update problem: automatic install did not finish for " + version);

    auto options = juce::MessageBoxOptions()
                       .withIconType (juce::MessageBoxIconType::WarningIcon)
                       .withTitle ("Update could not be installed")
                       .withMessage (message)
                       .withButton ("Open download page")
                       .withButton ("Close")
                       .withAssociatedComponent (this);

    juce::AlertWindow::showAsync (options, [] (int result)
    {
        if (result == 1)
            juce::URL ("https://fretfight.online").launchInDefaultBrowser();
    });
}
   #endif

void MainComponent::showUpdatePrompt()
{
    if (! updatePromptPending || updateDownloadInProgress)
        return;

    // Clear the flag first: the dialog callback runs later, and a second
    // timer tick must not open another prompt.
    updatePromptPending = false;

    const auto version = updatePromptVersion;
    const auto url = updatePromptUrl;
    const auto filename = updatePromptFilename;
    const auto size = updatePromptSize;
    const auto installerMode = updatePromptInstallerMode;

    auto notes = updatePromptNotes.trim();

    if (notes.isEmpty())
        notes = "Bug fixes and stability improvements.";

    // The admin field allows 8000 characters; an alert window cannot show a
    // whole changelog, so keep the beginning and cut on a line boundary.
    constexpr int maxNotesLength = 1200;

    if (notes.length() > maxNotesLength)
    {
        auto shortened = notes.substring (0, maxNotesLength);
        const int lastLine = shortened.lastIndexOfChar ('\n');

        if (lastLine > maxNotesLength / 2)
            shortened = shortened.substring (0, lastLine);

        notes = shortened.trimEnd() + "\n...";
    }

    juce::String message;
    message << "FretFight " << version << " is ready to install.\n\n"
            << "What's new:\n"
            << notes << "\n\n"
            << (installerMode
                    ? "The game will close, run the installer and start again. "
                      "Your settings and scores are kept."
                    : "The game will download the update, restart and keep your settings and scores.");

    juce::Component::SafePointer<MainComponent> safeThis (this);

    auto options = juce::MessageBoxOptions()
                       .withIconType (juce::MessageBoxIconType::InfoIcon)
                       .withTitle ("Update available")
                       .withMessage (message)
                       .withButton ("Update now")
                       .withButton ("Later")
                       .withAssociatedComponent (this);

    // With two buttons the first one returns 1 and the second one (or a
    // dismissed window) returns 0.
    juce::AlertWindow::showAsync (options, [safeThis, version, url, filename, size, installerMode] (int result)
    {
        if (result != 1)
            return; // "Later": the next launch offers the update again

        if (auto* self = safeThis.getComponent())
            self->startAutomaticUpdate (version, url, filename, size, installerMode);
    });
}

void MainComponent::startAutomaticUpdate (const juce::String& version,
                                          const juce::String& downloadUrl,
                                          const juce::String& filename,
                                          juce::int64 sizeBytes,
                                          bool installerMode)
{
    if (updateDownloadInProgress)
        return;

    updateDownloadInProgress = true;

    auto updateDirectory = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("FretFightUpdate");
    updateDirectory.deleteRecursively();
    updateDirectory.createDirectory();

    const auto targetFile = updateDirectory.getChildFile (
        safeUpdateFileName (filename, version));

    juce::Component::SafePointer<MainComponent> safeThis (this);

    auto* task = new UpdateDownloadTask (
        targetFile,
        resolveUpdateUrl (downloadUrl),
        version,
        sizeBytes,
        [safeThis, installerMode, version] (bool success, juce::File downloadedFile)
        {
            // The progress window has already closed; defer the follow-up so
            // it is not created from inside the window's teardown.
            juce::MessageManager::callAsync ([safeThis, success, downloadedFile,
                                              installerMode, version]
            {
                auto* self = safeThis.getComponent();

                if (self == nullptr)
                    return;

                self->updateDownloadInProgress = false;

                if (! success)
                    return; // offline or cancelled: stay on the current build

                if (installerMode)
                    self->runDownloadedInstaller (downloadedFile);
                else
                    self->applyDownloadedUpdate (downloadedFile, version);
            });
        });

    task->launchThread();
}

void MainComponent::runDownloadedInstaller (const juce::File& installerFile)
{
    if (! installerFile.existsAsFile())
        return;

   #if JUCE_WINDOWS
    // A file downloaded from the server is about to be executed with the
    // user's privileges: require a PE header before handing it to the shell.
    juce::FileInputStream header (installerFile);
    char signature[2] {};

    if (header.read (signature, 2) != 2
        || signature[0] != 'M' || signature[1] != 'Z')
        return;

    const auto appFolder = juce::File::getSpecialLocation (
        juce::File::currentExecutableFile).getParentDirectory();

    juce::StringArray arguments;
    arguments.add (installerFile.getFullPathName());
    arguments.add ("/VERYSILENT");
    arguments.add ("/SUPPRESSMSGBOXES");
    arguments.add ("/NORESTART");
    // Setup closes FretFight, replaces the files and the [Run] entry starts the
    // new build again, so the player lands back in the game.
    arguments.add ("/CLOSEAPPLICATIONS");
    arguments.add ("/DIR=" + appFolder.getFullPathName());

    // Program Files belongs to the system account. The same AppId has to be
    // installed for all users again, otherwise Setup would create a second,
    // per-user copy next to the read-only one.
    if (appFolder.getFullPathName().containsIgnoreCase ("Program Files"))
        arguments.add ("/ALLUSERS");

    juce::ChildProcess process;

    if (! process.start (arguments, 0))
        return;

    juce::MessageManager::callAsync ([]()
    {
        if (auto* app = juce::JUCEApplication::getInstance())
            app->systemRequestedQuit();
    });
   #else
    // macOS updates the .app bundle directly; the installer path is Windows
    // only, so this is unreachable there.
    juce::ignoreUnused (installerFile);
   #endif
}

void MainComponent::applyDownloadedUpdate (const juce::File& downloadedFile,
                                           const juce::String& targetVersion)
{
    if (! downloadedFile.existsAsFile())
        return;

   #if JUCE_WINDOWS
    juce::ignoreUnused (targetVersion);
    // The active Windows build is a portable ZIP with FretFight.exe inside.
    // Never copy an arbitrary server file over the running executable: the
    // ZIP entry and the extracted PE header are both checked.
    if (! downloadedFile.hasFileExtension ("zip"))
        return;

    juce::ZipFile zip (downloadedFile);
    juce::File extractedExe;

    for (int i = 0; i < zip.getNumEntries(); ++i)
    {
        const auto* entry = zip.getEntry (i);

        if (entry == nullptr || ! entry->filename.endsWithIgnoreCase (".exe"))
            continue;

        std::unique_ptr<juce::InputStream> input (zip.createStreamForEntry (i));

        if (input == nullptr)
            continue;

        const auto candidate = downloadedFile.getSiblingFile ("FretFight-update.exe");
        candidate.deleteFile();

        juce::FileOutputStream output (candidate);

        if (! output.openedOk())
            return;

        if (output.writeFromInputStream (*input, entry->uncompressedSize) <= 0)
            return;

        output.flush();

        juce::FileInputStream check (candidate);
        char header[2] {};

        if (check.read (header, 2) != 2
            || header[0] != 'M' || header[1] != 'Z')
        {
            candidate.deleteFile();
            return;
        }

        extractedExe = candidate;
        break;
    }

    if (! extractedExe.existsAsFile())
        return;

    const auto currentExe = juce::File::getSpecialLocation (
        juce::File::currentExecutableFile);

    if (! currentExe.getParentDirectory().hasWriteAccess())
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Update needs the installer",
            "FretFight cannot replace itself in:\n"
                + currentExe.getParentDirectory().getFullPathName()
                + "\n\nDownload the new version from https://fretfight.online and run the "
                  "installer. It installs the game in a per-user folder, so later versions "
                  "update themselves.",
            "OK");
        return;
    }

    const auto helper = downloadedFile.getSiblingFile ("apply-update.bat");
    const auto pid = static_cast<int> (GetCurrentProcessId());

    juce::String script;
    script << "@echo off\r\n"
           << "setlocal\r\n"
           << "set \"TARGET=" << currentExe.getFullPathName() << "\"\r\n"
           << "set \"SOURCE=" << extractedExe.getFullPathName() << "\"\r\n"
           << ":wait\r\n"
           << "tasklist /FI \"PID eq " << pid << "\" 2>nul | find \"" << pid << "\" >nul\r\n"
           << "if not errorlevel 1 (\r\n"
           << "  timeout /t 1 /nobreak >nul\r\n"
           << "  goto wait\r\n"
           << ")\r\n"
           << "copy /Y \"%SOURCE%\" \"%TARGET%\" >nul\r\n"
           << "start \"\" \"%TARGET%\"\r\n"
           << "del \"%~f0\"\r\n";

    if (! helper.replaceWithText (script))
        return;

    juce::ChildProcess process;
    process.start (juce::StringArray { "cmd.exe", "/c", helper.getFullPathName() }, 0);

    juce::MessageManager::callAsync ([]()
    {
        if (auto* app = juce::JUCEApplication::getInstance())
            app->systemRequestedQuit();
    });

   #elif JUCE_MAC
    // The macOS update payload is the portable ZIP (the DMG is the installer
    // on the download page). Stage the new bundle, verify that it really is
    // the advertised build, then let a shell helper swap it in after this
    // process exits. Every step is logged: a double-clicked bundle has no
    // console, and a silent failure here leaves the player with the same
    // update prompt on every launch.
    auto updateDirectory = downloadedFile.getParentDirectory();
    logUpdate ("apply start: " + downloadedFile.getFullPathName()
                   + " target=" + targetVersion);

    juce::File stagedApp;

    if (downloadedFile.hasFileExtension ("dmg"))
    {
        const auto mountPoint = updateDirectory.getChildFile ("dmg");
        mountPoint.deleteRecursively();
        mountPoint.createDirectory();

        if (! runAndWait ({ "/usr/bin/hdiutil", "attach", "-nobrowse", "-readonly",
                            "-mountpoint", mountPoint.getFullPathName(),
                            downloadedFile.getFullPathName() }, 60000))
        {
            showUpdateProblem ("The downloaded disk image could not be mounted.\n\n"
                               "Download the installer from https://fretfight.online.");
            return;
        }

        const auto mountedApp = mountPoint.getChildFile ("FretFight.app");

        if (mountedApp.isDirectory())
        {
            stagedApp = updateDirectory.getChildFile ("FretFight.app");
            stagedApp.deleteRecursively();

            // ditto keeps the bundle metadata and the ad-hoc signature;
            // cp -R drops extended attributes on some macOS versions.
            if (! runAndWait ({ "/usr/bin/ditto", mountedApp.getFullPathName(),
                                stagedApp.getFullPathName() }, 120000))
            {
                logUpdate ("ditto from the dmg failed");
                stagedApp = juce::File();
            }
        }
        else
        {
            logUpdate ("no FretFight.app inside the dmg");
        }

        runAndWait ({ "/usr/bin/hdiutil", "detach", mountPoint.getFullPathName() }, 30000);
    }
    else if (downloadedFile.hasFileExtension ("zip"))
    {
        const auto extracted = updateDirectory.getChildFile ("zip");
        extracted.deleteRecursively();
        extracted.createDirectory();

        if (! runAndWait ({ "/usr/bin/ditto", "-x", "-k",
                            downloadedFile.getFullPathName(),
                            extracted.getFullPathName() }, 120000))
        {
            showUpdateProblem ("The downloaded archive could not be unpacked.\n\n"
                               "Download the installer from https://fretfight.online.");
            return;
        }

        stagedApp = extracted.getChildFile ("FretFight.app");
    }
    else
    {
        logUpdate ("unsupported payload: " + downloadedFile.getFileName());
        return;
    }

    if (! stagedApp.isDirectory())
    {
        showUpdateProblem ("The downloaded file does not contain FretFight.app.\n\n"
                           "Nothing was changed. Download the installer from "
                           "https://fretfight.online.");
        return;
    }

    // Never replace a working app with a truncated download or with a build
    // that is not the one the server advertised.
    const auto stagedVersion = readBundleVersion (stagedApp);
    logUpdate ("staged version=" + stagedVersion);

    if (stagedVersion.isEmpty() || stagedVersion != targetVersion)
    {
        showUpdateProblem ("The downloaded build is not FretFight " + targetVersion
                           + " (found: "
                           + (stagedVersion.isEmpty() ? juce::String ("unknown")
                                                      : stagedVersion)
                           + ").\n\nNothing was changed. Download the installer from "
                             "https://fretfight.online.");
        return;
    }

    auto currentApp = juce::File::getSpecialLocation (juce::File::currentApplicationFile);

    if (! currentApp.hasFileExtension (".app"))
        currentApp = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                         .getParentDirectory()
                         .getParentDirectory()
                         .getParentDirectory();

    const auto appPath = currentApp.getFullPathName();
    logUpdate ("running bundle: " + appPath);

    // A bundle that is still inside a mounted disk image, or one that
    // Gatekeeper moved to a read-only translocated folder, cannot be replaced
    // in place. Say so instead of quitting and coming back with the old build.
    if (appPath.startsWith ("/Volumes/")
        || appPath.containsIgnoreCase ("AppTranslocation"))
    {
        showUpdateProblem ("FretFight is running from a read-only location:\n"
                           + appPath
                           + "\n\nDrag FretFight.app into Applications, quit it and start it "
                             "from there. The next update will install itself.");
        return;
    }

    if (! currentApp.getParentDirectory().hasWriteAccess())
    {
        showUpdateProblem ("FretFight cannot replace itself in:\n"
                           + currentApp.getParentDirectory().getFullPathName()
                           + "\n\nMove FretFight.app to a folder you own (for example "
                             "/Applications) to let future versions update automatically.");
        return;
    }

    const auto helper = updateDirectory.getChildFile ("apply-update.sh");
    const auto appPid = static_cast<int> (getpid());
    const auto logPath = updateLogFile().getFullPathName();
    const auto attemptPath = updateAttemptFile().getFullPathName();

    // The helper waits for this process to exit, copies the verified bundle
    // next to the old one, swaps them with mv (so a failure never leaves the
    // player without an app), clears the quarantine flag and starts the new
    // build. Each step is logged for the case where it still fails.
    juce::String script;
    script << "#!/bin/sh\n"
           << "LOG=\"" << logPath << "\"\n"
           << "APP=\"" << appPath << "\"\n"
           << "STAGED=\"" << stagedApp.getFullPathName() << "\"\n"
           << "ATTEMPT=\"" << attemptPath << "\"\n"
           << "log() { printf '%s %s\\n' \"$(date '+%Y-%m-%d %H:%M:%S')\" \"$1\" >> \"$LOG\"; }\n"
           << "log \"helper start pid=" << appPid << "\"\n"
           << "i=0\n"
           << "while kill -0 " << appPid << " 2>/dev/null; do sleep 0.5; i=$((i+1)); "
              "if [ \"$i\" -gt 240 ]; then break; fi; done\n"
           << "log \"app stopped\"\n"
           << "PARENT=$(dirname \"$APP\")\n"
           << "NEW=\"$PARENT/.FretFight-new.$$\"\n"
           << "OLD=\"$PARENT/.FretFight-old.$$\"\n"
           << "rm -rf \"$NEW\"\n"
           << "if /usr/bin/ditto \"$STAGED\" \"$NEW\" >> \"$LOG\" 2>&1; then\n"
           << "  log \"copy ready\"\n"
           << "  if mv \"$APP\" \"$OLD\" >> \"$LOG\" 2>&1; then\n"
           << "    if mv \"$NEW\" \"$APP\" >> \"$LOG\" 2>&1; then\n"
           << "      log \"installed\"\n"
           << "      /usr/bin/xattr -dr com.apple.quarantine \"$APP\" >> \"$LOG\" 2>&1\n"
           << "      rm -rf \"$OLD\"\n"
           << "      rm -f \"$ATTEMPT\"\n"
           << "    else\n"
           << "      log \"move new failed, restoring\"\n"
           << "      mv \"$OLD\" \"$APP\" >> \"$LOG\" 2>&1\n"
           << "    fi\n"
           << "  else\n"
           << "    log \"move old failed\"\n"
           << "  fi\n"
           << "else\n"
           << "  log \"ditto failed\"\n"
           << "fi\n"
           << "rm -rf \"$STAGED\"\n"
           << "/usr/bin/open \"$APP\" >> \"$LOG\" 2>&1\n"
           << "rm -f \"$0\"\n";

    if (! helper.replaceWithText (script) || helper.getSize() == 0)
    {
        showUpdateProblem ("The update helper could not be written to:\n"
                           + updateDirectory.getFullPathName());
        return;
    }

    helper.setExecutePermission (true);

    // Record the attempt before starting anything. The helper deletes this
    // marker once the new bundle is in place, so a marker that is still here
    // on the next launch means the swap never happened and the player must be
    // offered the installer instead of the same prompt in a loop.
    updateAttemptFile().replaceWithText (targetVersion + "\n");

    int helperPid = 0;
    bool started = startDetachedProcess ({ "/bin/sh", helper.getFullPathName() },
                                         helperPid);

    if (! started)
    {
        // posix_spawn is the reliable path, but never leave the player stuck
        // if the kernel refuses it: fall back to an ordinary child process.
        juce::ChildProcess fallback;
        started = fallback.start (juce::StringArray { "/bin/sh",
                                                      helper.getFullPathName() }, 0);
        helperPid = 0;
    }

    if (! started)
    {
        updateAttemptFile().deleteFile();
        // Quitting here would look like a crash that installs nothing.
        showUpdateProblem ("The update helper could not be started.\n\n"
                           "Download the installer from https://fretfight.online.");
        return;
    }

    logUpdate ("helper started pid=" + juce::String (helperPid)
                   + ", quitting for the swap");

    juce::MessageManager::callAsync ([]()
    {
        if (auto* app = juce::JUCEApplication::getInstance())
            app->systemRequestedQuit();
    });
   #endif
}
