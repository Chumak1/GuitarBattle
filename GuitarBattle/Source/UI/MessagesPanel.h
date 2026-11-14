#pragma once

#include <JuceHeader.h>

#include <functional>
#include <vector>

class ApiClient;

/*
    Direct messages between players.

    Left: the conversation list (peer, last message, unread count). Right: the
    thread with one player and the composer. Everything is server state from
    /api/v1/messages; the panel only polls (5 s) and caches the visible thread.
*/
class MessagesPanel final : public juce::Component,
                            private juce::Timer
{
public:
    MessagesPanel (ApiClient& apiToUse, juce::String bearerToken);
    ~MessagesPanel() override;

    /** Opens the full profile of the peer whose row / header was clicked. */
    std::function<void (const juce::String& userId)> onOpenProfile;

    /** Keeps the sidebar badge in sync with the server. */
    std::function<void (int unread)> onUnreadChanged;

    void refreshAll();

    /** Opens (and loads) the conversation with @p userId. */
    void openConversation (const juce::String& userId,
                           const juce::String& displayName = {});

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;
    void mouseWheelMove (const juce::MouseEvent& event,
                         const juce::MouseWheelDetails& wheel) override;

private:
    struct Conversation
    {
        juce::String userId;
        juce::String username;
        juce::String displayName;
        juce::String lastBody;
        juce::String lastTime;
        juce::String presence;
        int unread = 0;
    };

    struct ChatMessage
    {
        juce::String id;
        juce::String body;
        juce::String time;
        bool mine = false;
        bool read = false;
    };

    void timerCallback() override;
    void loadConversations();
    void loadThread (bool scrollToBottom);
    void sendMessage();
    void markThreadRead();
    int rowAt (juce::Point<int> position) const;
    int threadContentHeight (int width) const;

    static juce::String shortTime (const juce::String& iso);

    ApiClient& api;
    juce::String token;

    std::vector<Conversation> conversations;
    std::vector<ChatMessage> thread;

    juce::String activeUserId;
    juce::String activeName;
    juce::String activePresence;
    juce::String statusLine;

    int unreadTotal = 0;
    int hoveredRow = -1;
    int threadScroll = 0;
    int threadContentH = 0;
    int pollTicks = 0;
    int conversationsRequestId = 0;
    int threadRequestId = 0;
    bool sending = false;

    juce::TextEditor composer;
    juce::TextButton sendButton { "SEND" };
    juce::TextButton refreshButton { "REFRESH" };

    juce::Rectangle<int> listArea;
    juce::Rectangle<int> threadArea;
    juce::Rectangle<int> headerArea;
    juce::Rectangle<int> composerArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MessagesPanel)
};
