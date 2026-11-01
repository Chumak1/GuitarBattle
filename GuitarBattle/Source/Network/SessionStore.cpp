#include "SessionStore.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <wincred.h>
 #pragma comment(lib, "Advapi32.lib")
#elif JUCE_MAC
 #include <Security/Security.h>
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
        // One generic-password item in the login keychain, keyed by service and
        // account exactly like the Windows Credential Manager entry.
        constexpr const char* keychainService = "FretFight/Session";
        constexpr const char* keychainAccount = "refresh_token";

        CFMutableDictionaryRef makeKeychainQuery()
        {
            CFMutableDictionaryRef query = CFDictionaryCreateMutable(
                kCFAllocatorDefault, 0,
                &kCFTypeDictionaryKeyCallBacks,
                &kCFTypeDictionaryValueCallBacks);

            CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);

            if (CFStringRef service = CFStringCreateWithCString(
                    kCFAllocatorDefault, keychainService, kCFStringEncodingUTF8))
            {
                CFDictionarySetValue(query, kSecAttrService, service);
                CFRelease(service);
            }

            if (CFStringRef account = CFStringCreateWithCString(
                    kCFAllocatorDefault, keychainAccount, kCFStringEncodingUTF8))
            {
                CFDictionarySetValue(query, kSecAttrAccount, account);
                CFRelease(account);
            }

            return query;
        }
    }

    void clearRefreshToken()
    {
        CFMutableDictionaryRef query = makeKeychainQuery();
        SecItemDelete(query);
        CFRelease(query);
    }

    bool saveRefreshToken(const juce::String& token)
    {
        if (token.isEmpty())
            return false;

        // Replacing is a delete + add: SecItemUpdate needs the same dictionary
        // shape and the delete keeps a stale entry from surviving.
        clearRefreshToken();

        CFMutableDictionaryRef query = makeKeychainQuery();

        CFDataRef data = CFDataCreate(
            kCFAllocatorDefault,
            reinterpret_cast<const UInt8*>(token.toRawUTF8()),
            static_cast<CFIndex>(token.getNumBytesAsUTF8()));

        if (data == nullptr)
        {
            CFRelease(query);
            return false;
        }

        CFDictionarySetValue(query, kSecValueData, data);
        CFDictionarySetValue(query, kSecAttrAccessible,
                             kSecAttrAccessibleAfterFirstUnlock);

        const OSStatus status = SecItemAdd(query, nullptr);

        CFRelease(data);
        CFRelease(query);

        return status == errSecSuccess;
    }

    juce::String loadRefreshToken()
    {
        CFMutableDictionaryRef query = makeKeychainQuery();
        CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

        CFTypeRef result = nullptr;
        const OSStatus status = SecItemCopyMatching(query, &result);
        CFRelease(query);

        if (status != errSecSuccess || result == nullptr)
            return {};

        juce::String token;

        if (CFGetTypeID(result) == CFDataGetTypeID())
        {
            const auto* bytes = reinterpret_cast<const char*>(
                CFDataGetBytePtr(static_cast<CFDataRef>(result)));
            const auto size = static_cast<int>(
                CFDataGetLength(static_cast<CFDataRef>(result)));

            if (bytes != nullptr && size > 0)
                token = juce::String::fromUTF8(bytes, size);
        }

        CFRelease(result);
        return token.trim();
    }

    bool hasRefreshToken()
    {
        return loadRefreshToken().isNotEmpty();
    }
#else
    bool saveRefreshToken(const juce::String&) { return false; }
    juce::String loadRefreshToken() { return {}; }
    bool hasRefreshToken() { return false; }
    void clearRefreshToken() {}
#endif
}
