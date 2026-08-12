#include "SfzArchive.h"
#include "SfzFsync.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#if defined(_WIN32)
 #include <io.h>
 #include <fcntl.h>
 #include <sys/stat.h>
#else
 #include <fcntl.h>
 #include <unistd.h>
#endif

namespace duskstudio::sfz
{
namespace
{
namespace stdfs = std::filesystem;

constexpr std::size_t kReadBlockBytes = 256 * 1024;

struct ArchiveReaderDeleter
{
    void operator() (archive* reader) const noexcept
    {
        if (reader != nullptr)
            archive_read_free (reader);
    }
};

using ArchiveReader = std::unique_ptr<archive, ArchiveReaderDeleter>;

ArchiveEntryKind classify (archive_entry* entry)
{
    // A ZIP can describe a symlink either through the unix mode in the external
    // attributes or through a link target, so both are checked before the mode
    // is trusted.
    if (archive_entry_symlink (entry) != nullptr
        || archive_entry_hardlink (entry) != nullptr)
        return ArchiveEntryKind::unsupported;

    switch (archive_entry_filetype (entry))
    {
        case AE_IFREG: return ArchiveEntryKind::regularFile;
        case AE_IFDIR: return ArchiveEntryKind::directory;
        default:       return ArchiveEntryKind::unsupported;
    }
}

stdfs::path appendRelative (const stdfs::path& base, const std::string& relative)
{
    auto result = base;
    std::size_t begin = 0;
    while (begin < relative.size())
    {
        const auto end = relative.find ('/', begin);
        const auto length = (end == std::string::npos ? relative.size() : end) - begin;
        result /= relative.substr (begin, length);
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return result;
}

std::string readerError (archive* reader, const char* fallback)
{
    const auto* detail = archive_error_string (reader);
    return detail != nullptr ? std::string (detail) : std::string (fallback);
}

// A v1 pack is deflate or stored. archive_read_support_format_zip decodes every
// method the running libarchive was built with - bzip2, lzma, zstd, ppmd - so a
// distro build and the feature-stripped Windows build would install different
// packs from the same archive. The per-entry method is not exposed as a filter
// (the stream filter is always "none"); the descriptive format name is the only
// signal, and it reads "ZIP 2.0 (deflation)" / "ZIP 2.0 (uncompressed)" for the
// two that are allowed.
bool isSupportedZipCompression (archive* reader) noexcept
{
    const auto* name = archive_format_name (reader);
    if (name == nullptr)
        return false;
    const std::string format (name);
    return format.find ("(deflation)") != std::string::npos
        || format.find ("(uncompressed)") != std::string::npos;
}

enum class OpenOutcome
{
    opened,
    collision,
    link,
    failed
};

// Creates a fresh output file, refusing to reuse an existing name (O_EXCL) or to
// write through a symlink (O_NOFOLLOW). An exists()+ofstream check has a window
// between the test and the open, and ofstream happily follows a planted link to
// a target outside the tree; this closes both.
int openExclusiveFile (const stdfs::path& path, OpenOutcome& outcome) noexcept
{
    errno = 0;
#if defined(_WIN32)
    // Windows has no O_NOFOLLOW; _O_CREAT|_O_EXCL is CREATE_NEW, which still
    // refuses an existing name. Reparse-point following is a Windows-only gap
    // tracked for a later hardening pass.
    int fd = -1;
    _wsopen_s (&fd, path.wstring().c_str(),
               _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
               _SH_DENYNO, _S_IREAD | _S_IWRITE);
    if (fd >= 0)
    {
        outcome = OpenOutcome::opened;
        return fd;
    }
    outcome = errno == EEXIST ? OpenOutcome::collision : OpenOutcome::failed;
    return -1;
#else
    const int fd = ::open (path.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd >= 0)
    {
        outcome = OpenOutcome::opened;
        return fd;
    }
    if (errno == EEXIST)
        outcome = OpenOutcome::collision;
    else if (errno == ELOOP)
        outcome = OpenOutcome::link;
    else
        outcome = OpenOutcome::failed;
    return -1;
#endif
}

bool writeAllToFd (int fd, const char* data, std::size_t bytes) noexcept
{
    while (bytes > 0)
    {
#if defined(_WIN32)
        const auto request = static_cast<unsigned> (std::min<std::size_t> (bytes, 1u << 30));
        const int written = _write (fd, data, request);
#else
        const auto written = ::write (fd, data, bytes);
#endif
        if (written < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        data += written;
        bytes -= static_cast<std::size_t> (written);
    }
    return true;
}

bool closeFd (int fd) noexcept
{
#if defined(_WIN32)
    return _close (fd) == 0;
#else
    return ::close (fd) == 0;
#endif
}
} // namespace

ExtractionResult extractZipArchive (const ExtractionRequest& request,
                                    const ExtractionCallbacks& callbacks) noexcept
{
    ExtractionResult result;
    try
    {
        std::error_code ec;
        if (! stdfs::is_directory (request.destinationDirectory, ec))
        {
            result.status = ExtractionStatus::storageFailed;
            result.error = "the extraction directory does not exist";
            return result;
        }
        if (! stdfs::is_empty (request.destinationDirectory, ec) || ec)
        {
            result.status = ExtractionStatus::storageFailed;
            result.error = "the extraction directory is not empty";
            return result;
        }

        ArchiveReader reader (archive_read_new());
        if (reader == nullptr)
        {
            result.error = "the archive reader could not be created";
            return result;
        }

        // ZIP only, and no decompression filters: a v1 pack is a plain ZIP, and
        // enabling the other readers would widen the attack surface to formats
        // nothing in the catalog can describe.
        if (archive_read_support_format_zip (reader.get()) != ARCHIVE_OK
            || archive_read_open_filename (reader.get(),
                                           request.archiveFile.string().c_str(),
                                           kReadBlockBytes)
                   != ARCHIVE_OK)
        {
            result.error = readerError (reader.get(), "the archive could not be opened");
            return result;
        }

        const auto cancelled = [&callbacks]
        {
            return callbacks.isCancelled && callbacks.isCancelled();
        };

        ArchivePolicy policy (request.limits);
        for (;;)
        {
            if (cancelled())
            {
                result.status = ExtractionStatus::cancelled;
                return result;
            }

            archive_entry* entry = nullptr;
            const auto next = archive_read_next_header (reader.get(), &entry);
            if (next == ARCHIVE_EOF)
                break;
            if (next != ARCHIVE_OK)
            {
                result.error = readerError (reader.get(), "the archive could not be read");
                return result;
            }

            // Route an encrypted entry to rejected (which deletes the archive)
            // rather than letting the later read fail as readFailed.
            if (archive_entry_is_encrypted (entry) != 0)
            {
                result.status = ExtractionStatus::rejected;
                result.error = "contains an encrypted entry";
                return result;
            }
            if (! isSupportedZipCompression (reader.get()))
            {
                result.status = ExtractionStatus::rejected;
                result.error = "contains an entry compressed with an unsupported method";
                return result;
            }

            const auto* pathname = archive_entry_pathname (entry);
            ArchiveEntry described;
            described.pathname = pathname != nullptr ? pathname : std::string();
            described.kind = classify (entry);
            described.sizeKnown = archive_entry_size_is_set (entry) != 0;
            if (described.sizeKnown)
            {
                const auto declared = archive_entry_size (entry);
                if (declared < 0)
                {
                    result.status = ExtractionStatus::rejected;
                    result.error = "contains an entry with a negative size";
                    return result;
                }
                described.sizeBytes = static_cast<std::uint64_t> (declared);
            }

            if (! policy.accept (described))
            {
                result.status = ExtractionStatus::rejected;
                result.error = policy.error();
                return result;
            }

            const auto target = appendRelative (request.destinationDirectory,
                                                policy.acceptedPath());
            if (described.kind == ArchiveEntryKind::directory)
            {
                stdfs::create_directories (target, ec);
                if (ec)
                {
                    result.status = ExtractionStatus::storageFailed;
                    result.error = "a pack directory could not be created";
                    return result;
                }
                continue;
            }

            stdfs::create_directories (target.parent_path(), ec);
            if (ec)
            {
                result.status = ExtractionStatus::storageFailed;
                result.error = "a pack directory could not be created";
                return result;
            }

            // The destination started empty and every path below it is created
            // here, so an existing target means two entries collided in a way
            // the policy could not see - a case fold this filesystem performs
            // and the ASCII one does not, for instance. O_EXCL refuses to let the
            // second entry overwrite the first, and O_NOFOLLOW refuses to write
            // through a link a prior entry planted, both without a check-then-open
            // window.
            OpenOutcome outcome = OpenOutcome::failed;
            const int fd = openExclusiveFile (target, outcome);
            if (fd < 0)
            {
                if (outcome == OpenOutcome::collision)
                {
                    result.status = ExtractionStatus::rejected;
                    result.error = "contains a duplicate or case-colliding entry name";
                }
                else if (outcome == OpenOutcome::link)
                {
                    result.status = ExtractionStatus::rejected;
                    result.error = "contains an entry that resolves through a link";
                }
                else
                {
                    result.status = ExtractionStatus::storageFailed;
                    result.error = "a pack file could not be created";
                }
                return result;
            }

            std::int64_t written = 0;
            for (;;)
            {
                const void* block = nullptr;
                std::size_t blockSize = 0;
                la_int64_t offset = 0;
                const auto read = archive_read_data_block (reader.get(), &block,
                                                           &blockSize, &offset);
                if (read == ARCHIVE_EOF)
                    break;
                if (read != ARCHIVE_OK)
                {
                    closeFd (fd);
                    result.error = readerError (reader.get(),
                                                "the archive could not be read");
                    return result;
                }
                // Sparse output would leave holes the size accounting cannot
                // see. ZIP never produces it, so a gap means the entry is not
                // what it claims.
                if (offset != written)
                {
                    closeFd (fd);
                    result.status = ExtractionStatus::rejected;
                    result.error = "contains an entry with a sparse data layout";
                    return result;
                }
                if (! policy.addFileBytes (blockSize))
                {
                    closeFd (fd);
                    result.status = ExtractionStatus::rejected;
                    result.error = policy.error();
                    return result;
                }

                if (! writeAllToFd (fd, static_cast<const char*> (block), blockSize))
                {
                    closeFd (fd);
                    result.status = ExtractionStatus::storageFailed;
                    result.error = "a pack file could not be written";
                    return result;
                }
                written += static_cast<std::int64_t> (blockSize);

                if (callbacks.onProgress)
                    callbacks.onProgress (policy.expandedBytes());
                if (cancelled())
                {
                    closeFd (fd);
                    result.status = ExtractionStatus::cancelled;
                    return result;
                }
            }

            // Each file is fsynced before the whole tree is renamed into the
            // library, or a crash mid-publish leaves a verified-looking pack of
            // zeros that nothing re-checks.
            const bool synced = fsyncFileDescriptor (fd);
            const bool closed = closeFd (fd);
            if (! synced || ! closed)
            {
                result.status = ExtractionStatus::storageFailed;
                result.error = "a pack file could not be written";
                return result;
            }
        }

        result.fileCount = policy.fileCount();
        result.expandedBytes = policy.expandedBytes();
        if (! policy.finish())
        {
            result.status = ExtractionStatus::rejected;
            result.error = policy.error();
            return result;
        }

        result.status = ExtractionStatus::completed;
        return result;
    }
    catch (...)
    {
        result.status = ExtractionStatus::storageFailed;
        result.error = "the archive could not be expanded";
        return result;
    }
}
} // namespace duskstudio::sfz
