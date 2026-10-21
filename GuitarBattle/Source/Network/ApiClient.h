/*
    GuitarBattle - network layer.

    Every HTTP request of the desktop client goes through this class:

      * the server address is defined in exactly one place (baseUrl / serverOrigin),
      * the blocking helpers (get / post / download / downloadToFile) may only be
        called from a background thread,
      * the asynchronous helpers run the request on an internal juce::ThreadPool
        and always deliver the result on the message thread through
        juce::MessageManager::callAsync().

    The file is header-only on purpose: it is included by MainComponent.h and does
    not add a compile unit that would have to be registered in GuitarBattle.jucer.

    Nothing in this class is touched from the audio callback; no locks, allocations
    or network I/O are introduced into getNextAudioBlock()/prepareToPlay().
*/

#pragma once

#include <JuceHeader.h>

#include <functional>
#include <utility>

class ApiClient
{
public:
    /** Server root, used for asset downloads whose URL is relative to the origin
        (the API returns e.g. "/media/songs/.../track.mp3").
    */
    static constexpr const char* serverOrigin = "http://localhost:8000";

    /** REST API root: every JSON endpoint lives below this prefix. */
    static constexpr const char* baseUrl = "http://localhost:8000/api/v1";

    //==========================================================================
    /** Result of an HTTP request. */
    struct Response
    {
        /** True when an HTTP response was received (any status code). */
        bool transportOk = false;

        /** HTTP status code when the transport reported one, 0 otherwise. */
        int statusCode = 0;

        /** Parsed JSON body; void when the request failed or the body was not JSON. */
        juce::var json;

        bool isJsonObject() const noexcept { return json.isObject(); }
    };

    using ResponseCallback = std::function<void (Response)>;

    ApiClient() = default;

    // A request may still be in flight when the owner is destroyed: the pool
    // destructor waits for the running job (bounded by the request timeout).
    ~ApiClient() = default;

    JUCE_DECLARE_NON_COPYABLE (ApiClient)

    //==========================================================================
    // Blocking helpers - background threads only.

    /** GET a JSON document. @p apiPath is relative to baseUrl, e.g. "/auth/me". */
    static Response get (const juce::String& apiPath,
                         const juce::String& bearerToken,
                         int timeoutMs)
    {
        return perform ("GET", apiPath, nullptr, bearerToken, timeoutMs);
    }

    /** POST a JSON document. @p apiPath is relative to baseUrl, e.g. "/auth/login". */
    static Response post (const juce::String& apiPath,
                          const juce::var& jsonBody,
                          const juce::String& bearerToken,
                          int timeoutMs)
    {
        return perform ("POST", apiPath, &jsonBody, bearerToken, timeoutMs);
    }

    /** Downloads @p urlOrServerPath into memory (small files only). Empty on failure. */
    static juce::MemoryBlock download (const juce::String& urlOrServerPath, int timeoutMs)
    {
        // Never run a blocking download on the message thread.
        jassert (! juce::MessageManager::existsAndIsCurrentThread());

        juce::MemoryBlock bytes;

        juce::URL url (resolveServerUrl (urlOrServerPath));
        auto stream = url.createInputStream (streamOptions().withConnectionTimeoutMs (timeoutMs));

        if (stream == nullptr)
            return bytes;

        stream->readIntoMemoryBlock (bytes);
        return bytes;
    }

    /** Streams @p urlOrServerPath into @p targetFile.
        Returns the number of bytes written, or -1 on failure. The data is written
        to a temporary sibling first so a partial download never replaces a cached
        asset.
    */
    static juce::int64 downloadToFile (const juce::String& urlOrServerPath,
                                       const juce::File& targetFile,
                                       int timeoutMs)
    {
        // Never run a blocking download on the message thread.
        jassert (! juce::MessageManager::existsAndIsCurrentThread());

        juce::URL url (resolveServerUrl (urlOrServerPath));
        auto stream = url.createInputStream (streamOptions().withConnectionTimeoutMs (timeoutMs));

        if (stream == nullptr || ! targetFile.getParentDirectory().isDirectory())
            return -1;

        auto temporaryFile = targetFile.getSiblingFile (targetFile.getFileName() + ".part");
        temporaryFile.deleteFile();

        constexpr int bufferSize = 64 * 1024;
        juce::HeapBlock<char> buffer (bufferSize);
        juce::int64 totalBytes = 0;
        bool writeOk = true;

        {
            juce::FileOutputStream output (temporaryFile);

            if (! output.openedOk())
            {
                temporaryFile.deleteFile();
                return -1;
            }

            for (;;)
            {
                const auto numRead = stream->read (buffer.getData(), bufferSize);

                if (numRead <= 0)
                    break;

                if (! output.write (buffer.getData(), static_cast<size_t> (numRead)))
                {
                    writeOk = false;
                    break;
                }

                totalBytes += numRead;
            }
        }

        if (! writeOk || totalBytes <= 0 || ! temporaryFile.moveFileTo (targetFile))
        {
            temporaryFile.deleteFile();
            return -1;
        }

        return totalBytes;
    }

    //==========================================================================
    // Asynchronous helpers - safe to call from the message thread.

    /** GET @p apiPath on the pool; @p onResult runs on the message thread. */
    void getJson (const juce::String& apiPath,
                  const juce::String& bearerToken,
                  int timeoutMs,
                  ResponseCallback onResult)
    {
        enqueue ([apiPath, bearerToken, timeoutMs, onResult]
        {
            deliver (onResult, get (apiPath, bearerToken, timeoutMs));
        });
    }

    /** POST @p jsonBody to @p apiPath on the pool; @p onResult runs on the message thread. */
    void postJson (const juce::String& apiPath,
                   const juce::var& jsonBody,
                   const juce::String& bearerToken,
                   int timeoutMs,
                   ResponseCallback onResult)
    {
        enqueue ([apiPath, jsonBody, bearerToken, timeoutMs, onResult]
        {
            deliver (onResult, post (apiPath, jsonBody, bearerToken, timeoutMs));
        });
    }

    /** Runs @p backgroundWork on the pool, then @p onCompletion on the message thread.
        Use this for multi-step work (several requests + file writes) that must not
        touch the UI from the worker.
    */
    void runAsync (std::function<void()> backgroundWork,
                   std::function<void()> onCompletion)
    {
        enqueue ([work = std::move (backgroundWork), done = std::move (onCompletion)]() mutable
        {
            if (work != nullptr)
                work();

            if (done != nullptr)
                juce::MessageManager::callAsync (std::move (done));
        });
    }

private:
    //==========================================================================
    juce::ThreadPool pool { 2 };

    void enqueue (std::function<void()> job) { pool.addJob (std::move (job)); }

    static void deliver (const ResponseCallback& callback, Response response)
    {
        if (callback == nullptr)
            return;

        juce::MessageManager::callAsync ([callback, response] { callback (response); });
    }

    static juce::URL::InputStreamOptions streamOptions()
    {
        return juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress);
    }

    /** Resolves an API path ("/auth/me") or an absolute URL against baseUrl. */
    static juce::String resolveApiUrl (const juce::String& apiPath)
    {
        if (apiPath.startsWithIgnoreCase ("http://") || apiPath.startsWithIgnoreCase ("https://"))
            return apiPath;

        return apiPath.startsWithChar ('/') ? juce::String (baseUrl) + apiPath
                                            : juce::String (baseUrl) + "/" + apiPath;
    }

    /** Resolves a server-relative path ("/media/...") or an absolute URL against serverOrigin. */
    static juce::String resolveServerUrl (const juce::String& urlOrServerPath)
    {
        if (urlOrServerPath.startsWithIgnoreCase ("http://") || urlOrServerPath.startsWithIgnoreCase ("https://"))
            return urlOrServerPath;

        return urlOrServerPath.startsWithChar ('/') ? juce::String (serverOrigin) + urlOrServerPath
                                                    : juce::String (serverOrigin) + "/" + urlOrServerPath;
    }

    static Response perform (const juce::String& httpCommand,
                             const juce::String& apiPath,
                             const juce::var* jsonBody,
                             const juce::String& bearerToken,
                             int timeoutMs)
    {
        // Never run a blocking request on the message thread.
        jassert (! juce::MessageManager::existsAndIsCurrentThread());

        Response response;

        juce::URL url (resolveApiUrl (apiPath));

        if (jsonBody != nullptr)
            url = url.withPOSTData (juce::JSON::toString (*jsonBody));

        juce::String headers;

        if (jsonBody != nullptr)
            headers << "Content-Type: application/json\r\n";

        if (bearerToken.isNotEmpty())
            headers << "Authorization: Bearer " << bearerToken << "\r\n";

        auto options = streamOptions()
                           .withHttpRequestCmd (httpCommand)
                           .withConnectionTimeoutMs (timeoutMs)
                           .withExtraHeaders (headers)
                           .withStatusCode (&response.statusCode);

        auto stream = url.createInputStream (options);

        if (stream == nullptr)
            return response;

        response.transportOk = true;
        response.json = juce::JSON::parse (stream->readEntireStreamAsString());

        return response;
    }
};
