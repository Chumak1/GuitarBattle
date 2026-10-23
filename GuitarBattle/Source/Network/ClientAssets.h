/*
    GuitarBattle - server-hosted sound pack.

    The metronome clicks and the MIDI guitar/backing sample banks are published
    by the API as one versioned zip archive plus a JSON manifest.  This class
    downloads and unpacks that archive into the per-user application data
    directory, so the client never depends on a development path such as
    D:\GuitarBattle\GuitarBattle\Sounds.

    All network and file work runs on an ApiClient worker thread.  The callback
    passed to refresh() is delivered on the message thread.  The audio callback
    never touches this class: MainComponent only swaps its decoded sample
    buffers while the audio device is stopped.
*/

#pragma once

#include <JuceHeader.h>

#include "ApiClient.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

class ClientAssets
{
public:
    struct FileInfo
    {
        juce::String path;
        juce::int64 sizeBytes = 0;
    };

    struct Manifest
    {
        juce::String version;
        juce::String archiveUrl;
        juce::int64 archiveSizeBytes = 0;
        std::vector<FileInfo> files;

        bool isValid() const noexcept
        {
            return version.isNotEmpty()
                && archiveUrl.isNotEmpty()
                && ! files.empty();
        }
    };

    struct RefreshResult
    {
        bool ok = false;
        bool usedCache = false;
        bool updated = false;
        juce::String version;
        juce::String message;
    };

    //==========================================================================
    // Cache locations

    static juce::File cacheRoot()
    {
        return juce::File::getSpecialLocation(
                   juce::File::userApplicationDataDirectory)
            .getChildFile("GuitarBattle")
            .getChildFile("ClientAssets");
    }

    static juce::File currentRoot()
    {
        return cacheRoot().getChildFile("current");
    }

    //==========================================================================
    // Manifest / cache validation

    static Manifest readCachedManifest()
    {
        const auto file = cacheRoot().getChildFile("manifest.json");

        if (! file.existsAsFile())
            return {};

        return parseManifest(juce::JSON::parse(file));
    }

    static bool cacheMatches(const Manifest& manifest)
    {
        return cacheMatches(manifest, currentRoot(), true);
    }

    /** Checks that @p root contains every file of @p manifest with the expected
        size.  When @p requireVersionFile is false the staging directory can be
        validated before version.txt is written.
    */
    static bool cacheMatches(const Manifest& manifest,
                             const juce::File& root,
                             bool requireVersionFile)
    {
        if (! manifest.isValid() || ! root.isDirectory())
            return false;

        if (requireVersionFile)
        {
            const auto versionFile = cacheRoot().getChildFile("version.txt");

            if (! versionFile.existsAsFile()
                || versionFile.loadFileAsString().trim() != manifest.version)
                return false;
        }

        for (const auto& info : manifest.files)
        {
            if (! isSafeRelativePath(info.path))
                return false;

            const auto file = root.getChildFile(info.path);

            if (! file.existsAsFile() || file.getSize() != info.sizeBytes)
                return false;
        }

        return true;
    }

    //==========================================================================
    /** Fetches the manifest, downloads a new archive when its version differs
        from the cache, and installs it atomically.  @p onFinished is invoked on
        the message thread.  The static form deliberately captures no `this`, so
        a pending job is safe while MainComponent is being destroyed.
    */
    static void refresh(ApiClient& apiClient,
                        std::function<void (RefreshResult)> onFinished)
    {
        auto result = std::make_shared<RefreshResult>();

        apiClient.runAsync(
            [result]
            {
                *result = performRefresh();
            },
            [result, finished = std::move (onFinished)]
            {
                if (finished != nullptr)
                    finished(*result);
            });
    }

private:
    //==========================================================================
    static RefreshResult performRefresh()
    {
        RefreshResult result;

        const auto cached = readCachedManifest();
        const bool cacheOk = cacheMatches(cached);

        const auto response = ApiClient::get("/client-assets/manifest", {}, 10000);

        if (! response.transportOk || ! response.json.isObject())
        {
            if (cacheOk)
            {
                result.ok = true;
                result.usedCache = true;
                result.version = cached.version;
                result.message = "Using the cached sound pack.";
            }
            else
            {
                result.message = "The sound pack server is unavailable.";
            }

            return result;
        }

        const auto remote = parseManifest(response.json);

        if (! remote.isValid())
        {
            if (cacheOk)
            {
                result.ok = true;
                result.usedCache = true;
                result.version = cached.version;
                result.message = "Using the cached sound pack.";
            }
            else
            {
                result.message = "The sound pack manifest is invalid.";
            }

            return result;
        }

        if (cacheOk && cached.version == remote.version)
        {
            result.ok = true;
            result.usedCache = true;
            result.version = remote.version;
            result.message = "The sound pack is up to date.";
            return result;
        }

        //--------------------------------------------------------------------------
        // Download and stage the archive.

        const auto root = cacheRoot();

        if (! root.createDirectory())
        {
            result.message = "Cannot create the sound pack cache directory.";
            return result;
        }

        const auto archive = root.getChildFile("bundle.zip");
        archive.deleteFile();

        const auto downloaded = ApiClient::downloadToFile(remote.archiveUrl,
                                                          archive,
                                                          180000);

        if (downloaded <= 0
            || (remote.archiveSizeBytes > 0
                && downloaded != remote.archiveSizeBytes))
        {
            archive.deleteFile();
            result.message = "The sound pack download failed.";
            return result;
        }

        const auto staging = root.getChildFile("current.new");
        staging.deleteRecursively();

        juce::ZipFile zip(archive);

        if (zip.getNumEntries() <= 0)
        {
            archive.deleteFile();
            staging.deleteRecursively();
            result.message = "The sound pack archive is empty.";
            return result;
        }

        if (zip.uncompressTo(staging, true).failed())
        {
            archive.deleteFile();
            staging.deleteRecursively();
            result.message = "The sound pack archive could not be extracted.";
            return result;
        }

        if (! cacheMatches(remote, staging, false))
        {
            archive.deleteFile();
            staging.deleteRecursively();
            result.message = "The sound pack archive is incomplete.";
            return result;
        }

        //--------------------------------------------------------------------------
        // Replace the previous cache only after the new copy is complete.

        const auto current = currentRoot();
        const auto previous = root.getChildFile("current.old");
        previous.deleteRecursively();

        if (current.isDirectory() && ! current.moveFileTo(previous))
        {
            archive.deleteFile();
            staging.deleteRecursively();
            result.message = "Cannot replace the cached sound pack.";
            return result;
        }

        if (! staging.moveFileTo(current))
        {
            if (previous.isDirectory())
                previous.moveFileTo(current);

            archive.deleteFile();
            staging.deleteRecursively();
            result.message = "Cannot install the sound pack.";
            return result;
        }

        previous.deleteRecursively();
        archive.deleteFile();

        const auto manifestFile = root.getChildFile("manifest.json");
        const auto versionFile = root.getChildFile("version.txt");

        const bool indexWritten =
            manifestFile.replaceWithText(juce::JSON::toString(response.json))
            && versionFile.replaceWithText(remote.version);

        // The unpacked files are already in place, so the current session can
        // use them even if the small cache index could not be written.  A later
        // launch will notice the missing/invalid index and download again.
        result.ok = true;
        result.updated = true;
        result.version = remote.version;
        result.message = indexWritten
            ? "The sound pack was updated."
            : "The sound pack was installed but its cache index could not be written.";
        return result;
    }

    //==========================================================================
    static Manifest parseManifest(const juce::var& json)
    {
        Manifest manifest;

        if (! json.isObject())
            return manifest;

        manifest.version = json.getProperty("version", juce::var())
                               .toString().trim();
        manifest.archiveUrl = json.getProperty("archive_url", juce::var())
                                  .toString().trim();
        manifest.archiveSizeBytes = static_cast<juce::int64>(
            json.getProperty("archive_size_bytes", 0));

        const auto filesValue = json.getProperty("files", juce::var());

        if (const auto* files = filesValue.getArray())
        {
            for (const auto& item : *files)
            {
                FileInfo info;
                info.path = item.getProperty("path", juce::var())
                                .toString().replaceCharacter('\\', '/');
                info.sizeBytes = static_cast<juce::int64>(
                    item.getProperty("size_bytes", 0));

                if (isSafeRelativePath(info.path) && info.sizeBytes > 0)
                    manifest.files.push_back(std::move(info));
            }
        }

        return manifest;
    }

    static bool isSafeRelativePath(const juce::String& path)
    {
        if (path.isEmpty()
            || path.startsWithChar('/')
            || path.startsWithChar('\\')
            || path.containsChar(':'))
            return false;

        if (juce::File::isAbsolutePath(path))
            return false;

        const auto parts = juce::StringArray::fromTokens(
            path.replaceCharacter('\\', '/'), "/", "");

        for (const auto& part : parts)
            if (part.isEmpty() || part == "..")
                return false;

        return ! parts.isEmpty();
    }

    JUCE_DECLARE_NON_COPYABLE(ClientAssets)
};
