#pragma once

#include <JuceHeader.h>

// Persistent storage for the long-lived refresh token.
//
// Windows: the token is kept in the Credential Manager (CRED_TYPE_GENERIC,
// local machine scope), never in a plain settings file and never in logs.
// Other platforms fall back to "not supported" so the app simply asks for the
// password again instead of storing a secret insecurely.
namespace SessionStore
{
    bool saveRefreshToken (const juce::String& token);
    juce::String loadRefreshToken();
    bool hasRefreshToken();
    void clearRefreshToken();
}
