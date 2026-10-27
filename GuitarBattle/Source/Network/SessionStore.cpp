#include "SessionStore.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <wincred.h>
 #pragma comment(lib, "Advapi32.lib")
#endif

namespace SessionStore
{
#if JUCE_WINDOWS
    namespace
    {
        constexpr const wchar_t* credentialTarget = L"FretFight/Session";
        constexpr const wchar_t* credentialUser = L"refresh_token";
    }

    bool saveRefreshToken(const juce::String& token)
    {
        if (token.isEmpty())
            return false;

        // The blob is stored as UTF-8 bytes; the Windows API treats it as
        // opaque application data and never displays it.
        const auto utf8 = token.toRawUTF8();
        const auto size = static_cast<DWORD>(token.getNumBytesAsUTF8());

        CREDENTIALW credential {};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = const_cast<LPWSTR>(credentialTarget);
        credential.CredentialBlobSize = size;
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(
            const_cast<char*>(utf8));
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        credential.UserName = const_cast<LPWSTR>(credentialUser);

        return CredWriteW(&credential, 0) == TRUE;
    }

    juce::String loadRefreshToken()
    {
        PCREDENTIALW credential = nullptr;

        if (! CredReadW(credentialTarget, CRED_TYPE_GENERIC, 0, &credential))
            return {};

        juce::String token;

        if (credential->CredentialBlob != nullptr
            && credential->CredentialBlobSize > 0)
        {
            token = juce::String::fromUTF8(
                reinterpret_cast<const char*>(credential->CredentialBlob),
                static_cast<int>(credential->CredentialBlobSize));
        }

        CredFree(credential);
        return token.trim();
    }

    bool hasRefreshToken()
    {
        return loadRefreshToken().isNotEmpty();
    }

    void clearRefreshToken()
    {
        CredDeleteW(credentialTarget, CRED_TYPE_GENERIC, 0);
    }
#else
    bool saveRefreshToken(const juce::String&) { return false; }
    juce::String loadRefreshToken() { return {}; }
    bool hasRefreshToken() { return false; }
    void clearRefreshToken() {}
#endif
}
