#include "SessionStore.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <wincred.h>
 #pragma comment(lib, "Advapi32.lib")
#elif JUCE_MAC
 #include <sys/stat.h>
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
#elif JUCE_MAC
    namespace
    {
        // The bundle is ad-hoc signed, so every build carries a different code
        // signature. A keychain item written by the previous version belongs
        // to a different application, and macOS asks for the login password on
        // the first launch after each update (and denies access if the prompt
        // is dismissed). A 0600 file in the user's Application Support avoids
        // the prompt: it is readable only by this account, it survives updates
        // and it never leaves the Mac.
        juce::File tokenFile()
        {
            return juce::File::getSpecialLocation (
                       juce::File::userApplicationDataDirectory)
                       .getChildFile ("Application Support")
                       .getChildFile ("FretFight")
                       .getChildFile ("session.token");
        }

        juce::File logFile()
        {
            return juce::File::getSpecialLocation (
                       juce::File::userApplicationDataDirectory)
                       .getChildFile ("Application Support")
                       .getChildFile ("GuitarBattle")
                       .getChildFile ("session.log");
        }

        // Status codes only, never the token itself: a failed write has to be
        // visible, otherwise broken auto-login looks like a server problem.
        void logStatus (const char* operation, int status)
        {
            auto file = logFile();
            file.getParentDirectory().createDirectory();

            file.appendText (juce::String (operation)
                                 + " status="
                                 + juce::String (status)
                                 + "\n");
        }

        void restrictToOwner (const juce::File& file, int mode)
        {
            // 0600 for the token, 0700 for its folder: another account on a
            // shared Mac must not be able to read the refresh token.
            ::chmod (file.getFullPathName().toRawUTF8(),
                     static_cast<mode_t> (mode));
        }
    }

    void clearRefreshToken()
    {
        auto file = tokenFile();

        if (file.existsAsFile() && ! file.deleteFile())
            logStatus ("clear", -1);
    }

    bool saveRefreshToken (const juce::String& token)
    {
        if (token.isEmpty())
            return false;

        auto file = tokenFile();
        auto directory = file.getParentDirectory();

        directory.createDirectory();

        if (! directory.isDirectory())
        {
            logStatus ("save", -1);
            return false;
        }

        restrictToOwner (directory, S_IRWXU);

        // Write through a temporary file: a crash must never truncate a valid
        // token and log the player out.
        auto temp = file.getSiblingFile ("session.token.tmp");
        temp.deleteFile();

        if (! temp.replaceWithText (token))
        {
            logStatus ("save", -2);
            return false;
        }

        restrictToOwner (temp, S_IRUSR | S_IWUSR);

        if (! temp.moveFileTo (file))
        {
            logStatus ("save", -3);
            temp.deleteFile();
            return false;
        }

        restrictToOwner (file, S_IRUSR | S_IWUSR);
        return true;
    }

    juce::String loadRefreshToken()
    {
        auto file = tokenFile();

        if (! file.existsAsFile())
            return {};

        return file.loadFileAsString().trim();
    }

    bool hasRefreshToken()
    {
        return tokenFile().existsAsFile() && loadRefreshToken().isNotEmpty();
    }
#else
    bool saveRefreshToken(const juce::String&) { return false; }
    juce::String loadRefreshToken() { return {}; }
    bool hasRefreshToken() { return false; }
    void clearRefreshToken() {}
#endif
}
