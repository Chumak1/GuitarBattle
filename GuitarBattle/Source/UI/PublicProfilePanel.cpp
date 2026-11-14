#include "PublicProfilePanel.h"

#include "GameStyle.h"
#include "../Network/ApiClient.h"

#include <cmath>

namespace
{
    juce::String presenceLabel (const juce::String& presence)
    {
        if (presence == "online")    return "ONLINE";
        if (presence == "in_lobby")  return "IN LOBBY";
        if (presence == "in_match")  return "IN MATCH";

        return "OFFLINE";
    }

    juce::Colour presenceColour (const juce::String& presence)
    {
        if (presence == "online")   return UiColors::green;
        if (presence == "in_lobby") return UiColors::accent;
        if (presence == "in_match") return UiColors::orange;

        return UiColors::muted;
    }

    juce::String percentText (double value)
    {
        if (value < 0.0)
            return "-";

        return juce::String (value, 1) + "%";
    }

    // Character sheets are 4 columns x 2 rows of animation frames. A profile
    // shows the first frame only; the whole sheet would read as a contact sheet.
    void drawCharacterFrame (juce::Graphics& g, const juce::Image& sheet,
                             juce::Rectangle<int> area)
    {
        if (! sheet.isValid() || area.isEmpty())
            return;

        const int frameW = sheet.getWidth() / 4;
        const int frameH = sheet.getHeight() / 2;

        if (frameW <= 0 || frameH <= 0)
            return;

        const juce::Rectangle<int> source (0, 0, frameW, frameH);
        const float scale = juce::jmin (static_cast<float> (area.getWidth())
                                            / static_cast<float> (frameW),
                                        static_cast<float> (area.getHeight())
                                            / static_cast<float> (frameH));
        const float width = static_cast<float> (frameW) * scale;
        const float height = static_cast<float> (frameH) * scale;

        g.drawImage (sheet,
                     juce::roundToInt (static_cast<float> (area.getCentreX()) - width * 0.5f),
                     juce::roundToInt (static_cast<float> (area.getCentreY()) - height * 0.5f),
                     juce::roundToInt (width), juce::roundToInt (height),
                     source.getX(), source.getY(), source.getWidth(), source.getHeight(),
                     false);
    }
}

PublicProfilePanel::PublicProfilePanel (ApiClient& apiToUse, juce::String bearerToken)
    : api (apiToUse), token (std::move (bearerToken))
{
    styleSecondaryButton (backButton);
    stylePrimaryButton (primaryButton);
    styleSecondaryButton (declineButton);
    styleSecondaryButton (challengeButton);
    styleSecondaryButton (compareButton);
    styleSecondaryButton (blockButton);
    styleSecondaryButton (messageButton);

    addAndMakeVisible (backButton);
    addAndMakeVisible (primaryButton);
    addChildComponent (messageButton);
    addChildComponent (declineButton);
    addChildComponent (challengeButton);
    addChildComponent (compareButton);
    addChildComponent (blockButton);

    backButton.onClick = [this]
    {
        if (onBack != nullptr)
            onBack();
    };

    primaryButton.onClick = [this] { performPrimary(); };

    messageButton.onClick = [this]
    {
        if (onMessage != nullptr && data.userId.isNotEmpty())
            onMessage (data.userId, data.name());
    };

    declineButton.onClick = [this]
    {
        if (pendingRequestId.isEmpty())
            return;

        post ("/social/requests/" + pendingRequestId + "/decline", false,
              "Request declined.");
    };

    challengeButton.onClick = [this]
    {
        if (onChallenge != nullptr)
            onChallenge (data.userId);
    };

    compareButton.onClick = [this]
    {
        if (compareVisible)
        {
            compareVisible = false;
            compareButton.setButtonText ("COMPARE STATS");
            repaint();
            return;
        }

        if (self.username.isNotEmpty())
        {
            compareVisible = true;
            compareButton.setButtonText ("HIDE COMPARE");
            repaint();
            return;
        }

        comparePending = true;
        compareButton.setButtonText ("LOADING...");
        loadOwnStats();
    };

    blockButton.onClick = [this]
    {
        if (data.userId.isEmpty() || data.relationship == "blocked")
            return;

        // Blocking is destructive: confirm it in the same style as the rest of
        // the client instead of blocking on a stray click.
        juce::Component::SafePointer<PublicProfilePanel> safeThis (this);

        juce::AlertWindow::showAsync (
            juce::MessageBoxOptions()
                .withIconType (juce::MessageBoxIconType::WarningIcon)
                .withTitle ("Block player")
                .withMessage ("Block " + data.name()
                              + "? You will not see their requests or duels.")
                .withButton ("BLOCK")
                .withButton ("CANCEL"),
            [safeThis] (int result)
            {
                if (auto* self = safeThis.getComponent())
                    if (result == 1)
                        self->post ("/social/blocks/" + self->data.userId, false,
                                    "Player blocked.");
            });
    };
}

PublicProfilePanel::~PublicProfilePanel() = default;

void PublicProfilePanel::styleSecondaryButton (juce::TextButton& button)
{
    button.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    button.setColour (juce::TextButton::buttonOnColourId, UiColors::surface2);
    button.setColour (juce::TextButton::textColourOffId, UiColors::text);
    button.setColour (juce::TextButton::textColourOnId, UiColors::text);
}

void PublicProfilePanel::stylePrimaryButton (juce::TextButton& button)
{
    button.setColour (juce::TextButton::buttonColourId, UiColors::accent);
    button.setColour (juce::TextButton::buttonOnColourId, UiColors::accentStrong);
    button.setColour (juce::TextButton::textColourOffId, UiColors::onAccent);
    button.setColour (juce::TextButton::textColourOnId, UiColors::onAccent);
}

void PublicProfilePanel::setStatus (const juce::String& text)
{
    statusLine = text;
    repaint();
}

void PublicProfilePanel::open (const juce::String& userId)
{
    data = Player();
    data.userId = userId;

    self = Player();
    cardLoaded = false;
    compareVisible = false;
    comparePending = false;
    pendingRequestId.clear();
    statusLine.clear();
    characterSheet = {};
    ++imageGeneration;

    compareButton.setButtonText ("COMPARE STATS");
    primaryButton.setEnabled (false);
    rebuildActions();
    resized();
    repaint();

    loadCard();
}

void PublicProfilePanel::loadCard()
{
    if (data.userId.isEmpty() || token.isEmpty())
    {
        setStatus ("Sign in to open player profiles.");
        return;
    }

    juce::Component::SafePointer<PublicProfilePanel> safeThis (this);
    const auto userId = data.userId;

    api.getJson ("/social/players/" + userId, token, 6000,
        [safeThis, userId] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr || self->data.userId != userId)
                return;

            if (! reply.transportOk || ! reply.json.isObject())
            {
                self->setStatus ("Could not load this profile. Check the connection.");
                return;
            }

            const auto& json = reply.json;

            self->data.username = json.getProperty ("username", juce::var()).toString();
            self->data.displayName = json.getProperty ("display_name", juce::var()).toString();
            self->data.level = static_cast<int> (json.getProperty ("level", 1));
            self->data.xpTotal = static_cast<int> (json.getProperty ("xp_total", 0));
            self->data.rating = static_cast<int> (json.getProperty ("rating", 1000));
            self->data.peakRating = static_cast<int> (json.getProperty ("peak_rating", 1000));
            self->data.characterName =
                json.getProperty ("character_name", juce::var()).toString();
            self->data.characterSheetUrl =
                json.getProperty ("character_sheet_url", juce::var()).toString();
            self->data.presence =
                json.getProperty ("presence", "offline").toString();
            self->data.relationship =
                json.getProperty ("relationship", "none").toString();
            self->data.canChallenge =
                static_cast<bool> (json.getProperty ("can_challenge", true));

            if (const auto* object = json.getDynamicObject())
            {
                if (object->hasProperty ("guitar_skills"))
                    self->data.guitarSkills = static_cast<double> (
                        object->getProperty ("guitar_skills"));

                if (object->hasProperty ("guitar_skills_rank")
                    && ! object->getProperty ("guitar_skills_rank").isVoid())
                    self->data.guitarSkillsRank = static_cast<int> (
                        object->getProperty ("guitar_skills_rank"));
            }

            self->data.provisional =
                static_cast<bool> (json.getProperty ("provisional", false));

            const auto stats = json.getProperty ("stats", juce::var());
            self->data.statsVisible = stats.isObject();

            if (self->data.statsVisible)
            {
                self->data.wins = static_cast<int> (stats.getProperty ("wins", 0));
                self->data.losses = static_cast<int> (stats.getProperty ("losses", 0));
                self->data.draws = static_cast<int> (stats.getProperty ("draws", 0));
                self->data.duels = static_cast<int> (stats.getProperty ("duels", 0));
                self->data.winRate = static_cast<double> (stats.getProperty ("win_rate", 0.0));
                self->data.bestAccuracy =
                    static_cast<double> (stats.getProperty ("best_accuracy", 0.0));
                self->data.songsMastered =
                    static_cast<int> (stats.getProperty ("songs_mastered", 0));
                self->data.achievementsUnlocked =
                    static_cast<int> (stats.getProperty ("achievements_unlocked", 0));
                self->data.achievementsTotal =
                    static_cast<int> (stats.getProperty ("achievements_total", 0));
                self->data.towersCompleted =
                    static_cast<int> (stats.getProperty ("towers_completed", 0));
            }

            self->cardLoaded = true;
            self->loadCharacterImage (self->data.characterSheetUrl);
            self->loadSkillsDetail();
            self->loadPendingRequest();
            self->rebuildActions();
            self->repaint();
        });
}

void PublicProfilePanel::loadSkillsDetail()
{
    if (data.userId.isEmpty() || token.isEmpty())
        return;

    const int requestId = ++skillsRequestId;
    const auto userId = data.userId;
    juce::Component::SafePointer<PublicProfilePanel> safeThis (this);

    api.getJson ("/leaderboard/global/" + userId, token, 6000,
        [safeThis, requestId, userId] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr || requestId != self->skillsRequestId
                || self->data.userId != userId)
                return;

            if (! reply.transportOk || ! reply.json.isObject())
                return;

            const auto& json = reply.json;

            if (json.getProperty ("guitar_skills", juce::var()).isVoid())
                return;

            self->data.guitarSkills = static_cast<double> (
                json.getProperty ("guitar_skills", 0.0));
            self->data.provisional =
                static_cast<bool> (json.getProperty ("provisional", false));
            self->data.skillsConfidence = static_cast<double> (
                json.getProperty ("confidence", 0.0));
            self->data.recentAccuracy = static_cast<double> (
                json.getProperty ("accuracy_percent", -1.0));

            if (const auto* rankObject = json.getDynamicObject())
            {
                if (rankObject->hasProperty ("rank")
                    && ! rankObject->getProperty ("rank").isVoid())
                    self->data.guitarSkillsRank = static_cast<int> (
                        rankObject->getProperty ("rank"));
            }

            auto readComponent = [] (const juce::var& components,
                                     const char* key,
                                     Player::SkillComponent& target)
            {
                const auto value = components.getProperty (key, juce::var());

                if (! value.isObject())
                    return;

                target.value = static_cast<double> (
                    value.getProperty ("value", -1.0));
                target.events = static_cast<int> (
                    value.getProperty ("events", 0));
                target.confidence = static_cast<double> (
                    value.getProperty ("confidence", 0.0));
            };

            const auto components = json.getProperty ("components", juce::var());

            if (components.isObject())
            {
                readComponent (components, "accuracy", self->data.accuracySkill);
                readComponent (components, "challenge", self->data.challengeSkill);
                readComponent (components, "score", self->data.scoreSkill);
                readComponent (components, "duel", self->data.duelSkill);
                readComponent (components, "progression", self->data.progressionSkill);
                self->data.skillsDetailLoaded = true;
            }

            self->repaint();
        });
}

void PublicProfilePanel::loadPendingRequest()
{
    pendingRequestId.clear();

    if (data.relationship != "incoming" && data.relationship != "outgoing")
    {
        rebuildActions();
        return;
    }

    const auto direction = data.relationship == "incoming" ? juce::String ("incoming")
                                                           : juce::String ("outgoing");
    juce::Component::SafePointer<PublicProfilePanel> safeThis (this);
    const auto userId = data.userId;
    const auto relationship = data.relationship;

    api.getJson ("/social/requests?direction=" + direction + "&limit=50", token, 6000,
        [safeThis, userId, relationship] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr || self->data.userId != userId
                || self->data.relationship != relationship)
                return;

            if (const auto* items = reply.json.getProperty ("items", juce::var()).getArray())
            {
                for (const auto& item : *items)
                {
                    const auto user = item.getProperty ("user", juce::var());

                    if (user.getProperty ("user_id", juce::var()).toString() != userId)
                        continue;

                    self->pendingRequestId =
                        item.getProperty ("id", juce::var()).toString();
                    break;
                }
            }

            self->rebuildActions();
            self->repaint();
        });
}

void PublicProfilePanel::loadOwnStats()
{
    juce::Component::SafePointer<PublicProfilePanel> safeThis (this);

    api.getJson ("/duels/me", token, 6000,
        [safeThis] (ApiClient::Response reply)
        {
            auto* self = safeThis.getComponent();

            if (self == nullptr)
                return;

            self->comparePending = false;

            if (! reply.transportOk || ! reply.json.isObject())
            {
                self->compareButton.setButtonText ("COMPARE STATS");
                self->setStatus ("Could not load your own statistics.");
                return;
            }

            const auto& json = reply.json;
            self->self.username = json.getProperty ("username", juce::var()).toString();
            self->self.displayName = json.getProperty ("display_name", juce::var()).toString();
            self->self.level = static_cast<int> (json.getProperty ("level", 1));
            self->self.rating = static_cast<int> (json.getProperty ("rating", 1000));
            self->self.peakRating = static_cast<int> (json.getProperty ("peak_rating", 1000));

            const auto online = json.getProperty ("online", juce::var());
            self->self.duels = static_cast<int> (online.getProperty ("duels", 0));
            self->self.wins = static_cast<int> (online.getProperty ("wins", 0));
            self->self.losses = static_cast<int> (online.getProperty ("losses", 0));
            self->self.draws = static_cast<int> (online.getProperty ("draws", 0));
            self->self.winRate = static_cast<double> (online.getProperty ("win_rate", 0.0));
            self->self.bestAccuracy =
                static_cast<double> (online.getProperty ("accuracy", 0.0));

            self->compareVisible = true;
            self->compareButton.setButtonText ("HIDE COMPARE");
            self->repaint();
        });
}

void PublicProfilePanel::loadCharacterImage (const juce::String& relativeUrl)
{
    characterSheet = {};

    if (relativeUrl.isEmpty())
        return;

    const auto absolute = relativeUrl.startsWith ("http")
        ? relativeUrl
        : juce::String (ApiClient::serverOrigin) + relativeUrl;

    juce::Component::SafePointer<PublicProfilePanel> safeThis (this);
    const auto generation = ++imageGeneration;

    api.runAsync (
        [absolute, safeThis, generation]
        {
            const auto bytes = ApiClient::download (absolute, 15000);

            if (bytes.getSize() == 0)
                return;

            const juce::MemoryBlock copy (bytes.getData(), bytes.getSize());

            juce::MessageManager::callAsync ([safeThis, generation, copy]
            {
                auto* self = safeThis.getComponent();

                if (self == nullptr || generation != self->imageGeneration)
                    return;

                self->characterSheet =
                    juce::ImageFileFormat::loadFrom (copy.getData(), copy.getSize());
                self->repaint();
            });
        },
        nullptr);
}

void PublicProfilePanel::rebuildActions()
{
    primaryAction = PrimaryAction::None;

    primaryButton.setVisible (true);
    messageButton.setVisible (data.relationship != "self");
    declineButton.setVisible (false);
    blockButton.setVisible (data.relationship != "self"
                            && data.relationship != "blocked");
    challengeButton.setVisible (data.relationship != "self"
                                && data.relationship != "blocked"
                                && data.canChallenge);
    compareButton.setVisible (data.relationship != "self");

    if (data.relationship == "self")
    {
        primaryAction = PrimaryAction::EditProfile;
        primaryButton.setButtonText ("EDIT MY PROFILE");
    }
    else if (data.relationship == "friend")
    {
        primaryAction = PrimaryAction::RemoveFriend;
        primaryButton.setButtonText ("REMOVE FRIEND");
    }
    else if (data.relationship == "incoming")
    {
        primaryAction = PrimaryAction::AcceptRequest;
        primaryButton.setButtonText ("ACCEPT REQUEST");
        declineButton.setVisible (true);
    }
    else if (data.relationship == "outgoing")
    {
        primaryAction = PrimaryAction::CancelRequest;
        primaryButton.setButtonText ("CANCEL REQUEST");
    }
    else if (data.relationship == "blocked")
    {
        primaryAction = PrimaryAction::Unblock;
        primaryButton.setButtonText ("UNBLOCK USER");
    }
    else
    {
        primaryAction = PrimaryAction::AddFriend;
        primaryButton.setButtonText ("ADD FRIEND");
    }

    const bool needsRequest = primaryAction == PrimaryAction::CancelRequest
                              || primaryAction == PrimaryAction::AcceptRequest;
    primaryButton.setEnabled (! cardLoaded ? false
                                           : (! needsRequest || pendingRequestId.isNotEmpty()));
    declineButton.setEnabled (pendingRequestId.isNotEmpty());

    resized();
}

void PublicProfilePanel::performPrimary()
{
    switch (primaryAction)
    {
        case PrimaryAction::AddFriend:
            post ("/social/friends/" + data.userId, false, "Friend request sent.");
            break;

        case PrimaryAction::CancelRequest:
            if (pendingRequestId.isNotEmpty())
                post ("/social/requests/" + pendingRequestId, true, "Request cancelled.");
            break;

        case PrimaryAction::AcceptRequest:
            if (pendingRequestId.isNotEmpty())
                post ("/social/requests/" + pendingRequestId + "/accept", false,
                      "Friend request accepted.");
            break;

        case PrimaryAction::RemoveFriend:
            post ("/social/friends/" + data.userId, true, "Friend removed.");
            break;

        case PrimaryAction::Unblock:
            post ("/social/blocks/" + data.userId, true, "Player unblocked.");
            break;

        case PrimaryAction::EditProfile:
            if (onEditOwnProfile != nullptr)
                onEditOwnProfile();
            break;

        case PrimaryAction::None:
            break;
    }
}

void PublicProfilePanel::post (const juce::String& path, bool httpDelete,
                               const juce::String& statusText)
{
    if (data.userId.isEmpty())
        return;

    juce::Component::SafePointer<PublicProfilePanel> safeThis (this);
    const auto userId = data.userId;

    auto done = [safeThis, userId, statusText] (ApiClient::Response reply)
    {
        auto* self = safeThis.getComponent();

        if (self == nullptr || self->data.userId != userId)
            return;

        if (! reply.transportOk || reply.statusCode >= 400)
        {
            self->setStatus (reply.statusCode == 409 ? "Already in that state."
                             : reply.statusCode == 403 ? "Not allowed."
                             : reply.statusCode == 404 ? "Player not found."
                                                       : "Request failed.");
            return;
        }

        self->setStatus (statusText);
        self->loadCard();
    };

    if (httpDelete)
        api.deleteJson (path, token, 6000, done);
    else
        api.postJson (path, juce::var (new juce::DynamicObject()), token, 6000, done);
}

PublicProfilePanel::Layout PublicProfilePanel::computeLayout() const
{
    Layout layout;

    const auto bounds = getLocalBounds();
    const int width = bounds.getWidth();
    const int height = bounds.getHeight();
    const int leftWidth = juce::jlimit (240, 340, width / 4);

    layout.header = { 0, 0, width, 116 };

    const int bodyTop = layout.header.getBottom() + 12;
    const int statsHeight = 140;

    // The panel is a fixed-size overlay, so a short window must not clip the
    // action buttons. Keep the tall layout when there is room and compress the
    // info card first, then the progress card, when the window gets shorter.
    int infoHeight = 190;
    int progressHeight = 112;
    const int minInfoHeight = 140;
    const int minProgressHeight = 96;

    auto layoutBottom = [&]
    {
        const int statsY = bodyTop + infoHeight + 12;
        const int progressY = statsY + statsHeight + 12;
        return progressY + progressHeight + 16 + 34;
    };

    int overflow = layoutBottom() - height;

    if (overflow > 0)
    {
        const int infoShrink = juce::jmin (overflow, infoHeight - minInfoHeight);
        infoHeight -= infoShrink;
        overflow -= infoShrink;
    }

    if (overflow > 0)
    {
        const int progressShrink = juce::jmin (overflow,
                                               progressHeight - minProgressHeight);
        progressHeight -= progressShrink;
    }

    const int statsY = bodyTop + infoHeight + 12;
    const int progressY = statsY + statsHeight + 12;

    layout.character = { 0, bodyTop, leftWidth, 300 };
    layout.info = { leftWidth + 12, bodyTop, width - leftWidth - 12, infoHeight };
    layout.stats = { leftWidth + 12, statsY, width - leftWidth - 12, statsHeight };
    layout.progress = { 0, progressY, width, progressHeight };

    return layout;
}

void PublicProfilePanel::resized()
{
    const auto layout = computeLayout();

    backButton.setBounds (layout.header.getRight() - 104, layout.header.getY() + 16,
                          92, 30);

    const int buttonY = layout.progress.getBottom() + 16;
    int x = 0;

    auto place = [&x, buttonY] (juce::TextButton& button, int width)
    {
        if (! button.isVisible())
            return;

        button.setBounds (x, buttonY, width, 34);
        x += width + 10;
    };

    place (primaryButton, 176);
    place (messageButton, 120);
    place (declineButton, 116);
    place (challengeButton, 132);
    place (compareButton, 160);
    place (blockButton, 132);
}

void PublicProfilePanel::paint (juce::Graphics& g)
{
    g.fillAll (UiColors::background);

    if (! cardLoaded)
    {
        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (13.0f));
        g.drawText (statusLine.isNotEmpty() ? statusLine
                                            : "Loading player profile...",
                    getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    const auto layout = computeLayout();

    paintHeader (g, layout.header);
    paintCharacter (g, layout.character);
    paintInfo (g, layout.info);
    paintStats (g, layout.stats);
    paintProgress (g, layout.progress);

    if (statusLine.isNotEmpty())
    {
        g.setColour (UiColors::accent);
        g.setFont (GameUi::font (11.5f));
        g.drawText (statusLine, 0, layout.progress.getBottom() + 60,
                    getWidth() - 320, 20, juce::Justification::centredLeft, true);
    }
}

void PublicProfilePanel::paintHeader (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto card = area.toFloat();

    GameUi::drawCard (g, card, GameUi::Metrics::radiusCard, true);

    const auto avatar = juce::Rectangle<int> (area.getX() + 20, area.getY() + 20, 76, 76);

    // The header avatar is the same initial-on-surface badge the player cards
    // use; the character itself is shown full size in the block below.
    g.setColour (UiColors::surface2);
    g.fillEllipse (avatar.toFloat());
    g.setColour (UiColors::accent.withAlpha (0.14f));
    g.fillEllipse (avatar.toFloat().reduced (6.0f));
    g.setColour (UiColors::accent);
    g.setFont (GameUi::heading (30.0f));
    g.drawText (data.name().substring (0, 1).toUpperCase (), avatar,
                juce::Justification::centred, false);

    const int textX = avatar.getRight() + 18;
    const int textWidth = juce::jmax (100, area.getWidth() - textX - 330);

    g.setColour (UiColors::text);
    g.setFont (GameUi::heading (26.0f));
    g.drawText (data.name(), textX, area.getY() + 22, textWidth, 32,
                juce::Justification::centredLeft, true);

    g.setColour (UiColors::muted);
    g.setFont (GameUi::font (12.0f));
    g.drawText ("@" + data.username, textX, area.getY() + 56, textWidth, 18,
                juce::Justification::centredLeft, true);

    const auto presenceArea = juce::Rectangle<float> (
        (float) textX, (float) area.getY() + 80.0f, 92.0f, 20.0f);
    GameUi::drawPill (g, presenceArea, presenceLabel (data.presence),
                      presenceColour (data.presence), true);

    // Rating and level on the right, in the same metric style as the tables.
    const int rightEdge = area.getRight() - 140;

    auto drawMetric = [&g, area] (int x, int width, const juce::String& label,
                                  const juce::String& value,
                                  juce::Colour colour = juce::Colour())
    {
        g.setColour (UiColors::muted);
        g.setFont (GameUi::eyebrow (9.5f));
        g.drawText (label, x, area.getY() + 42, width, 14,
                    juce::Justification::centredLeft, false);

        g.setColour (colour.isTransparent() ? UiColors::text : colour);
        g.setFont (GameUi::font (24.0f, true));
        g.drawText (value, x, area.getY() + 58, width, 30,
                    juce::Justification::centredLeft, false);
    };

    const auto skillsText = data.guitarSkills >= 0.0
                                ? juce::String (data.guitarSkills, 1)
                                : juce::String ("-");

    drawMetric (rightEdge - 250, 60, "LEVEL", juce::String (data.level));
    drawMetric (rightEdge - 176, 110, "SKILLS", skillsText, UiColors::gold);
    drawMetric (rightEdge - 56, 60, "RANK",
                data.guitarSkillsRank > 0
                    ? "#" + juce::String (data.guitarSkillsRank)
                    : juce::String ("-"));
}

void PublicProfilePanel::paintCharacter (juce::Graphics& g, juce::Rectangle<int> area)
{
    GameUi::drawCard (g, area.toFloat());

    g.setColour (UiColors::accent);
    g.setFont (GameUi::eyebrow (10.0f));
    g.drawText ("CHARACTER", area.getX() + 20, area.getY() + 16, area.getWidth() - 40, 14,
                juce::Justification::centredLeft, false);

    auto frame = area.reduced (16).withTrimmedTop (26);

    g.setColour (UiColors::surface2.withAlpha (0.45f));
    g.fillRoundedRectangle (frame.toFloat(), 12.0f);

    if (characterSheet.isValid())
    {
        drawCharacterFrame (g, characterSheet, frame.reduced (6));
    }
    else
    {
        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (12.0f));
        g.drawText ("No character selected", frame, juce::Justification::centred, false);
    }

    if (data.characterName.isNotEmpty())
    {
        g.setColour (UiColors::text);
        g.setFont (GameUi::font (14.0f, true));
        g.drawText (data.characterName, area.getX() + 20, area.getBottom() - 32,
                    area.getWidth() - 40, 20, juce::Justification::centredLeft, true);
    }
}

void PublicProfilePanel::paintInfo (juce::Graphics& g, juce::Rectangle<int> area)
{
    GameUi::drawCard (g, area.toFloat());

    g.setColour (UiColors::accent);
    g.setFont (GameUi::eyebrow (10.0f));
    g.drawText ("OVERVIEW", area.getX() + 20, area.getY() + 14, area.getWidth() - 40, 14,
                juce::Justification::centredLeft, false);

    const int tileY = area.getY() + 40;
    const int tileW = (area.getWidth() - 40 - 3 * 10) / 4;

    auto tile = [&g, tileY, tileW, &area] (int index, const juce::String& label,
                                           const juce::String& value)
    {
        const juce::Rectangle<int> r (area.getX() + 20 + index * (tileW + 10),
                                      tileY, tileW, 78);

        g.setColour (UiColors::surface2.withAlpha (0.55f));
        g.fillRoundedRectangle (r.toFloat(), 10.0f);

        g.setColour (UiColors::muted);
        g.setFont (GameUi::eyebrow (9.0f));
        g.drawText (label, r.reduced (12, 8).withHeight (12),
                    juce::Justification::centredLeft, false);

        g.setColour (UiColors::text);
        g.setFont (GameUi::font (20.0f, true));
        g.drawText (value, r.reduced (12, 8).withTrimmedTop (18),
                    juce::Justification::centredLeft, false);
    };

    const auto skillsText = data.guitarSkills >= 0.0
                                ? juce::String (data.guitarSkills, 1)
                                : juce::String ("-");

    tile (0, "LEVEL", juce::String (data.level));
    tile (1, "GUITAR SKILLS", skillsText);
    tile (2, "RATING", juce::String (data.rating));
    tile (3, "TOTAL XP", juce::String (data.xpTotal));

    // Guitar Skills breakdown: a stacked bar of the five weighted components.
    // Its filled length equals the total score on the 0..100 scale, so the bar
    // shows both the number and what it is made of.
    struct Part
    {
        const char* label;
        double weight;
        const Player::SkillComponent* component;
        juce::Colour colour;
    };

    const std::array<Part, 5> parts {{
        { "ACCURACY",    0.35, &data.accuracySkill,    UiColors::accent },
        { "CHALLENGE",   0.20, &data.challengeSkill,   UiColors::gold },
        { "SCORE",       0.10, &data.scoreSkill,       UiColors::info },
        { "DUELS",       0.25, &data.duelSkill,        UiColors::green },
        { "PROGRESSION", 0.10, &data.progressionSkill, UiColors::orange }
    }};

    const int barX = area.getX() + 20;
    const int barY = tileY + 78 + 16;
    const int barW = area.getWidth() - 40;
    const juce::Rectangle<float> bar ((float) barX, (float) barY,
                                      (float) barW, 10.0f);

    g.setColour (UiColors::surface2.withAlpha (0.7f));
    g.fillRoundedRectangle (bar, 5.0f);

    if (data.skillsDetailLoaded)
    {
        juce::Graphics::ScopedSaveState saved (g);
        juce::Path clip;
        clip.addRoundedRectangle (bar, 5.0f);
        g.reduceClipRegion (clip);

        float x = bar.getX();
        const double pixelsPerPoint = (double) barW / 100.0;

        for (const auto& part : parts)
        {
            const double value = part.component->value >= 0.0
                                     ? part.component->value
                                     : 50.0;
            const float width = (float) (part.weight * value * pixelsPerPoint);

            if (width > 0.5f)
            {
                g.setColour (part.colour.withAlpha (0.85f));
                g.fillRect (x, bar.getY(), width, bar.getHeight());
            }

            x += width;
        }
    }

    const int legendY = barY + 18;
    const int itemW = juce::jmax (90, barW / 5);

    for (size_t i = 0; i < parts.size(); ++i)
    {
        const auto& part = parts[i];
        const int x = barX + (int) i * itemW;

        g.setColour (part.colour);
        g.fillEllipse ((float) x, (float) legendY + 3.0f, 7.0f, 7.0f);

        g.setColour (UiColors::muted);
        g.setFont (GameUi::eyebrow (8.0f));

        const auto valueText = part.component->value >= 0.0
                                   ? "  " + juce::String (part.component->value, 1)
                                   : juce::String ("  -");
        const auto weightText = juce::String ((int) juce::roundToInt (part.weight * 100.0))
                                    + "%";

        g.drawFittedText (juce::String (part.label) + " " + weightText + valueText,
                          x + 11, legendY, itemW - 14, 12,
                          juce::Justification::centredLeft, 1, 0.7f);
    }

    g.setColour (data.provisional ? UiColors::accent : UiColors::muted);
    g.setFont (GameUi::font (10.0f, data.provisional));
    g.drawText (data.skillsDetailLoaded
                    ? juce::String (data.provisional ? "PROVISIONAL" : "RANKED")
                          + "   -   CONFIDENCE "
                          + juce::String (juce::roundToInt (data.skillsConfidence * 100.0))
                          + "%"
                    : juce::String ("Guitar Skills are calculated by the server."),
                barX, legendY + 18, barW, 14,
                juce::Justification::centredLeft, false);
}

void PublicProfilePanel::paintStats (juce::Graphics& g, juce::Rectangle<int> area)
{
    GameUi::drawCard (g, area.toFloat());

    g.setColour (UiColors::accent);
    g.setFont (GameUi::eyebrow (10.0f));
    g.drawText (compareVisible ? ("STATS  -  YOU VS " + data.name().toUpperCase ())
                               : juce::String ("GAME STATISTICS"),
                area.getX() + 20, area.getY() + 14, area.getWidth() - 40, 14,
                juce::Justification::centredLeft, true);

    if (! data.statsVisible)
    {
        g.setColour (UiColors::muted);
        g.setFont (GameUi::font (12.0f));
        g.drawText ("This player hides their statistics.", area.reduced (20),
                    juce::Justification::centred, false);
        return;
    }

    const int tileW = (area.getWidth() - 40 - 3 * 10) / 4;
    const int tileH = 44;

    auto tile = [&g, area, tileW, tileH] (int column, int row, const juce::String& label,
                                          const juce::String& value,
                                          const juce::String& compareValue,
                                          bool compare)
    {
        const juce::Rectangle<int> r (area.getX() + 20 + column * (tileW + 10),
                                      area.getY() + 38 + row * (tileH + 8),
                                      tileW, tileH);

        g.setColour (UiColors::surface2.withAlpha (0.55f));
        g.fillRoundedRectangle (r.toFloat(), 10.0f);

        g.setColour (UiColors::muted);
        g.setFont (GameUi::eyebrow (8.5f));
        g.drawText (label, r.getX() + 10, r.getY() + 5, r.getWidth() - 20, 11,
                    juce::Justification::centredLeft, false);

        g.setColour (UiColors::text);
        g.setFont (GameUi::font (14.0f, true));
        g.drawText (value, r.getX() + 10, r.getY() + 18, r.getWidth() - 20, 18,
                    juce::Justification::centredLeft, false);

        if (compare)
        {
            g.setColour (UiColors::accent);
            g.setFont (GameUi::font (11.0f, true));
            g.drawText ("YOU " + compareValue, r.getX() + 10, r.getY() + 18,
                        r.getWidth() - 20, 18, juce::Justification::centredRight, false);
        }
    };

    tile (0, 0, "DUELS", juce::String (data.duels),
          juce::String (self.duels), compareVisible);
    tile (1, 0, "WINS", juce::String (data.wins),
          juce::String (self.wins), compareVisible);
    tile (2, 0, "LOSSES", juce::String (data.losses),
          juce::String (self.losses), compareVisible);
    tile (3, 0, "WIN RATE", percentText (data.winRate),
          percentText (self.winRate), compareVisible);

    const auto accuracyValue = data.bestAccuracy > 0.0
                                   ? data.bestAccuracy
                                   : data.recentAccuracy;
    tile (0, 1, "ACCURACY", accuracyValue >= 0.0
                              ? percentText (accuracyValue)
                              : juce::String ("-"),
          percentText (self.bestAccuracy), compareVisible);
    tile (1, 1, "SONGS MASTERED", juce::String (data.songsMastered),
          juce::String (self.songsMastered), false);
}

void PublicProfilePanel::paintProgress (juce::Graphics& g, juce::Rectangle<int> area)
{
    GameUi::drawCard (g, area.toFloat());

    g.setColour (UiColors::accent);
    g.setFont (GameUi::eyebrow (10.0f));
    g.drawText ("PROGRESS", area.getX() + 20, area.getY() + 14, area.getWidth() - 40, 14,
                juce::Justification::centredLeft, false);

    const int barWidth = juce::jmax (160, (area.getWidth() - 40 - 20) / 2);
    const int barX = area.getX() + 20;
    const int barY = area.getY() + 56;

    auto drawBar = [&g, barX, barY, barWidth] (int offset, const juce::String& label,
                                              const juce::String& value, float fraction)
    {
        const int x = barX + offset;
        const int width = juce::jmax (120, barWidth - 10);

        g.setColour (UiColors::muted);
        g.setFont (GameUi::eyebrow (9.0f));
        g.drawText (label, x, barY - 18, width, 12,
                    juce::Justification::centredLeft, false);

        g.setColour (UiColors::surface2);
        g.fillRoundedRectangle ((float) x, (float) barY, (float) width, 10.0f, 5.0f);

        const auto clamped = juce::jlimit (0.0f, 1.0f, fraction);

        if (clamped > 0.0f)
        {
            g.setColour (UiColors::accent);
            g.fillRoundedRectangle ((float) x, (float) barY,
                                    (float) width * clamped, 10.0f, 5.0f);
        }

        g.setColour (UiColors::text);
        g.setFont (GameUi::font (11.5f, true));
        g.drawText (value, x, barY + 18, width, 16,
                    juce::Justification::centredLeft, false);
    };

    const float achievementFraction = data.achievementsTotal > 0
        ? static_cast<float> (data.achievementsUnlocked)
              / static_cast<float> (data.achievementsTotal)
        : 0.0f;

    drawBar (0, "ACHIEVEMENTS",
             juce::String (data.achievementsUnlocked) + " / "
                 + juce::String (data.achievementsTotal),
             achievementFraction);

    const float towerFraction = data.towersCompleted > 0
        ? juce::jmin (1.0f, static_cast<float> (data.towersCompleted) / 5.0f)
        : 0.0f;

    drawBar (barWidth, "BATTLE - TOWERS CLEARED",
             juce::String (data.towersCompleted),
             towerFraction);
}
