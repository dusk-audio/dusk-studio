#include "SfzArchive.h"

#include <archive.h>
#include <archive_entry.h>

#include <cstddef>
#include <fstream>
#include <memory>

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
            // and the ASCII one does not, for instance. Never let the second
            // entry overwrite the first.
            if (stdfs::exists (target, ec))
            {
                result.status = ExtractionStatus::rejected;
                result.error = "contains a duplicate or case-colliding entry name";
                return result;
            }

            std::ofstream out (target, std::ios::binary | std::ios::trunc);
            if (! out)
            {
                result.status = ExtractionStatus::storageFailed;
                result.error = "a pack file could not be created";
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
                    result.error = readerError (reader.get(),
                                                "the archive could not be read");
                    return result;
                }
                // Sparse output would leave holes the size accounting cannot
                // see. ZIP never produces it, so a gap means the entry is not
                // what it claims.
                if (offset != written)
                {
                    result.status = ExtractionStatus::rejected;
                    result.error = "contains an entry with a sparse data layout";
                    return result;
                }
                if (! policy.addFileBytes (blockSize))
                {
                    result.status = ExtractionStatus::rejected;
                    result.error = policy.error();
                    return result;
                }

                out.write (static_cast<const char*> (block),
                           static_cast<std::streamsize> (blockSize));
                if (! out)
                {
                    result.status = ExtractionStatus::storageFailed;
                    result.error = "a pack file could not be written";
                    return result;
                }
                written += static_cast<std::int64_t> (blockSize);

                if (callbacks.onProgress)
                    callbacks.onProgress (policy.expandedBytes());
                if (cancelled())
                {
                    result.status = ExtractionStatus::cancelled;
                    return result;
                }
            }

            out.flush();
            const auto flushed = static_cast<bool> (out);
            out.close();
            if (! flushed)
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
