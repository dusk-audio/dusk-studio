#pragma once

#include "SfzCatalog.h"
#include "SfzTransport.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace duskstudio::sfz
{
// Every directory the installer touches hangs off one root so staging, the
// verified archive and the published pack always share a filesystem and the
// publish step can be a rename.
struct StoreLayout
{
    std::filesystem::path root;

    std::filesystem::path packsDirectory() const { return root / "packs"; }
    std::filesystem::path downloadsDirectory() const { return root / "downloads"; }
    std::filesystem::path stagingDirectory() const { return root / "staging"; }

    std::filesystem::path packDirectory (const std::string& packId,
                                         const std::string& releaseId) const
    {
        return packsDirectory() / packId / releaseId;
    }
};

struct InstallLimits
{
    std::uint64_t maximumFileBytes { 512ULL * 1024ULL * 1024ULL };
    unsigned maximumDownloadAttempts { 4 };
    TransferLimits transfer;
};

enum class InstallPhase
{
    downloading,
    extracting,
    validating,
    publishing
};

enum class InstallStatus
{
    installed,
    alreadyInstalled,
    cancelled,
    packRejected,
    downloadFailed,
    archiveRejected,
    validationFailed,
    storageFailed
};

struct InstallResult
{
    InstallStatus status { InstallStatus::storageFailed };
    std::filesystem::path installedPath;
    std::string error;

    explicit operator bool() const noexcept
    {
        return status == InstallStatus::installed
            || status == InstallStatus::alreadyInstalled;
    }
};

struct InstallCallbacks
{
    std::function<void (InstallPhase phase, std::uint64_t completed,
                        std::uint64_t total)> onProgress;
    // Real cancellation - the user cancelled, or the app is quitting. Interrupts
    // every phase.
    std::function<bool()> isCancelled;
    // Polled only during the download. Reflects isCancelled and, on top of it,
    // the activity gate, so a busy studio pauses a download without ever
    // restarting an in-progress expansion. Falls back to isCancelled when unset.
    std::function<bool()> isDownloadCancelled;
};

// Downloads, verifies, expands, validates and publishes one catalog pack. The
// pack directory appears only once the whole pack is on disk and has been
// checked against the signed metadata: everything before the final rename
// happens under the staging directory, and any failure leaves the store exactly
// as it was.
InstallResult installPack (Transport& transport,
                           const CatalogPack& pack,
                           const StoreLayout& layout,
                           const InstallLimits& limits,
                           const InstallCallbacks& callbacks) noexcept;

// Empty when the file's sample, default_path and include references all stay
// inside the pack, otherwise the reason the first offending reference was
// refused. Backslash separators are normalised first because SFZ files written
// on Windows use them for ordinary relative paths.
std::string findUnsafeSfzReference (const std::filesystem::path& sfzFile) noexcept;
} // namespace duskstudio::sfz
