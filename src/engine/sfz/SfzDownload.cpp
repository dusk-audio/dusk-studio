#include "SfzDownload.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <vector>

namespace duskstudio::sfz
{
namespace
{
namespace stdfs = std::filesystem;

constexpr std::size_t kHashBlockBytes = 64 * 1024;

bool initialiseSodium()
{
    static const bool ready = sodium_init() >= 0;
    return ready;
}

bool isLowerHexDigest (const std::string& value)
{
    return value.size() == crypto_hash_sha256_BYTES * 2
        && std::all_of (value.begin(), value.end(), [] (unsigned char c)
           {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

std::uint64_t existingSize (const stdfs::path& file)
{
    std::error_code ec;
    const auto size = stdfs::file_size (file, ec);
    return ec ? 0 : static_cast<std::uint64_t> (size);
}

bool truncateToEmpty (const stdfs::path& file)
{
    std::ofstream out (file, std::ios::binary | std::ios::trunc);
    return static_cast<bool> (out);
}
} // namespace

std::string hashFileSha256 (const std::filesystem::path& file) noexcept
{
    try
    {
        if (! initialiseSodium())
            return {};

        std::ifstream in (file, std::ios::binary);
        if (! in)
            return {};

        crypto_hash_sha256_state state;
        if (crypto_hash_sha256_init (&state) != 0)
            return {};

        std::vector<char> block (kHashBlockBytes);
        while (in)
        {
            in.read (block.data(), static_cast<std::streamsize> (block.size()));
            const auto read = static_cast<std::size_t> (in.gcount());
            if (read == 0)
                break;
            if (crypto_hash_sha256_update (
                    &state, reinterpret_cast<const unsigned char*> (block.data()), read)
                != 0)
                return {};
        }
        if (in.bad())
            return {};

        std::array<unsigned char, crypto_hash_sha256_BYTES> digest {};
        if (crypto_hash_sha256_final (&state, digest.data()) != 0)
            return {};

        std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex {};
        sodium_bin2hex (hex.data(), hex.size(), digest.data(), digest.size());
        return std::string (hex.data());
    }
    catch (...)
    {
        return {};
    }
}

DownloadResult downloadArchive (Transport& transport,
                                const DownloadRequest& request,
                                const DownloadCallbacks& callbacks) noexcept
{
    DownloadResult result;
    try
    {
        if (! initialiseSodium())
            return { DownloadStatus::storageFailed, 0,
                     "the cryptography library could not be initialised" };

        if (request.expectedBytes == 0)
            return { DownloadStatus::transferFailed, 0, "the archive size is unknown" };
        if (! isLowerHexDigest (request.expectedSha256))
            return { DownloadStatus::transferFailed, 0, "the archive digest is malformed" };
        if (request.maximumAttempts == 0)
            return { DownloadStatus::transferFailed, 0, "no download attempts were allowed" };

        std::error_code ec;
        if (! request.partFile.parent_path().empty())
        {
            stdfs::create_directories (request.partFile.parent_path(), ec);
            if (ec)
                return { DownloadStatus::storageFailed, 0,
                         "the download directory could not be created" };
        }

        const auto cancelled = [&callbacks]
        {
            return callbacks.isCancelled && callbacks.isCancelled();
        };

        std::string transferError;
        auto exhaustedStatus = DownloadStatus::transferFailed;
        for (unsigned attempt = 0; attempt < request.maximumAttempts; ++attempt)
        {
            if (cancelled())
                return { DownloadStatus::cancelled, result.downloadedBytes, {} };

            auto alreadyHave = existingSize (request.partFile);
            if (alreadyHave > request.expectedBytes)
            {
                // Longer than the catalog promised, so the tail is not ours;
                // start over rather than hand a bad prefix to the hash.
                if (! truncateToEmpty (request.partFile))
                    return { DownloadStatus::storageFailed, result.downloadedBytes,
                             "the partial download could not be reset" };
                alreadyHave = 0;
            }

            if (alreadyHave < request.expectedBytes)
            {
                std::ofstream out (request.partFile,
                                   std::ios::binary
                                       | (alreadyHave > 0 ? std::ios::app : std::ios::trunc));
                if (! out)
                    return { DownloadStatus::storageFailed, result.downloadedBytes,
                             "the partial download could not be opened" };

                bool storageFailure = false;
                bool userCancelled = false;

                TransferCallbacks transferCallbacks;
                transferCallbacks.onResponse =
                    [&] (bool resumeAccepted, std::uint64_t)
                    {
                        if (alreadyHave == 0 || resumeAccepted)
                            return true;

                        // The server ignored the range and is sending the whole
                        // resource, so everything already on disk is a prefix of
                        // a different response and has to go.
                        out.close();
                        if (! truncateToEmpty (request.partFile))
                        {
                            storageFailure = true;
                            return false;
                        }
                        out.open (request.partFile, std::ios::binary | std::ios::app);
                        if (! out)
                        {
                            storageFailure = true;
                            return false;
                        }
                        alreadyHave = 0;
                        return true;
                    };
                transferCallbacks.onData =
                    [&] (const unsigned char* data, std::size_t bytes)
                    {
                        if (cancelled())
                        {
                            userCancelled = true;
                            return false;
                        }
                        out.write (reinterpret_cast<const char*> (data),
                                   static_cast<std::streamsize> (bytes));
                        if (! out)
                        {
                            storageFailure = true;
                            return false;
                        }
                        return true;
                    };
                transferCallbacks.onProgress =
                    [&] (std::uint64_t receivedBytes, std::uint64_t totalBytes)
                    {
                        if (cancelled())
                        {
                            userCancelled = true;
                            return false;
                        }
                        if (callbacks.onProgress)
                            callbacks.onProgress (receivedBytes,
                                                  totalBytes > 0 ? totalBytes
                                                                 : request.expectedBytes);
                        return true;
                    };

                TransferRequest transferRequest;
                transferRequest.url = request.url;
                transferRequest.resumeOffset = alreadyHave;
                transferRequest.limits = request.limits;
                transferRequest.limits.maximumBytes = request.expectedBytes;

                const auto transfer = transport.fetch (transferRequest, transferCallbacks);
                out.flush();
                const auto flushed = static_cast<bool> (out);
                out.close();
                result.downloadedBytes += transfer.receivedBytes;

                if (storageFailure || ! flushed)
                    return { DownloadStatus::storageFailed, result.downloadedBytes,
                             "the partial download could not be written" };
                if (userCancelled || transfer.status == TransferStatus::cancelled)
                    return { DownloadStatus::cancelled, result.downloadedBytes, {} };
                if (transfer.status != TransferStatus::completed)
                {
                    transferError = transfer.error;
                    exhaustedStatus = DownloadStatus::transferFailed;
                    continue;
                }
            }

            const auto onDisk = existingSize (request.partFile);
            if (onDisk != request.expectedBytes)
            {
                transferError = "the download ended before the archive was complete";
                exhaustedStatus = DownloadStatus::sizeMismatch;
                continue;
            }

            const auto digest = hashFileSha256 (request.partFile);
            if (digest.empty())
                return { DownloadStatus::storageFailed, result.downloadedBytes,
                         "the downloaded archive could not be read back" };
            if (sodium_memcmp (digest.data(), request.expectedSha256.data(), digest.size()) != 0)
            {
                stdfs::remove (request.partFile, ec);
                return { DownloadStatus::hashMismatch, result.downloadedBytes,
                         "the downloaded archive does not match the catalog digest" };
            }

            stdfs::remove (request.destination, ec);
            stdfs::rename (request.partFile, request.destination, ec);
            if (ec)
                return { DownloadStatus::storageFailed, result.downloadedBytes,
                         "the verified archive could not be published" };

            result.status = DownloadStatus::completed;
            return result;
        }

        return { exhaustedStatus, result.downloadedBytes,
                 transferError.empty() ? "the download did not complete" : transferError };
    }
    catch (...)
    {
        return { DownloadStatus::storageFailed, result.downloadedBytes,
                 "the download failed unexpectedly" };
    }
}
} // namespace duskstudio::sfz
