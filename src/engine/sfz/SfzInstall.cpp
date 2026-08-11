#include "SfzInstall.h"
#include "SfzArchive.h"
#include "SfzDownload.h"
#include "SfzPathRules.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <utility>

namespace duskstudio::sfz
{
namespace
{
namespace stdfs = std::filesystem;

constexpr std::size_t kMaxSfzTextBytes = 8ULL * 1024ULL * 1024ULL;

std::string uniqueStagingName (const CatalogPack& pack)
{
    static std::atomic<std::uint64_t> counter { 0 };
    const auto ticks = static_cast<std::uint64_t> (
        std::chrono::steady_clock::now().time_since_epoch().count());
    return pack.id + "-" + pack.releaseId + "-" + std::to_string (ticks) + "-"
         + std::to_string (counter.fetch_add (1, std::memory_order_relaxed));
}

// Always removes the staging tree; a published pack has been renamed out of
// it by then, so only leftovers are ever deleted.
struct StagingGuard
{
    explicit StagingGuard (stdfs::path pathToGuard) : path (std::move (pathToGuard)) {}

    ~StagingGuard()
    {
        std::error_code ec;
        stdfs::remove_all (path, ec);
    }

    StagingGuard (const StagingGuard&) = delete;
    StagingGuard& operator= (const StagingGuard&) = delete;

    stdfs::path path;
};

std::string trimAscii (std::string value)
{
    const auto isSpace = [] (char c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    std::size_t begin = 0;
    while (begin < value.size() && isSpace (value[begin]))
        ++begin;
    std::size_t end = value.size();
    while (end > begin && isSpace (value[end - 1]))
        --end;
    return value.substr (begin, end - begin);
}

bool startsWithAtBoundary (const std::string& line, std::size_t position,
                           const char* token, std::size_t tokenLength)
{
    if (position + tokenLength > line.size())
        return false;
    if (position > 0)
    {
        const auto before = line[position - 1];
        if (before != ' ' && before != '\t' && before != '>')
            return false;
    }
    for (std::size_t i = 0; i < tokenLength; ++i)
        if (paths::toLowerAscii (line[position + i]) != token[i])
            return false;
    return true;
}

// SFZ opcode values run to the end of the line unless another opcode or a
// region header follows, so the value ends at the whitespace before the next
// "name=" pair.
std::string extractOpcodeValue (const std::string& line, std::size_t valueStart)
{
    auto valueEnd = line.size();
    const auto header = line.find ('<', valueStart);
    if (header != std::string::npos)
        valueEnd = header;

    const auto nextAssignment = line.find ('=', valueStart);
    if (nextAssignment != std::string::npos && nextAssignment < valueEnd)
    {
        const auto separator = line.find_last_of (" \t", nextAssignment);
        if (separator != std::string::npos && separator > valueStart)
            valueEnd = separator;
    }
    return trimAscii (line.substr (valueStart, valueEnd - valueStart));
}

std::string describeUnsafeReference (const std::string& reference)
{
    if (reference.empty())
        return "an empty file reference";
    // Built-in waveform generators are names, not paths.
    if (reference.front() == '*')
        return {};

    std::string normalised = reference;
    std::replace (normalised.begin(), normalised.end(), '\\', '/');
    // default_path values conventionally end in a separator; it carries no
    // meaning for the safety check.
    while (normalised.size() > 1 && normalised.back() == '/')
        normalised.pop_back();
    if (const auto* reason = paths::relativePathRejectionReason (normalised))
        return std::string ("the reference '") + reference + "' " + reason;
    return {};
}

std::string scanSfzLine (const std::string& rawLine)
{
    auto line = rawLine;
    const auto comment = line.find ("//");
    if (comment != std::string::npos)
        line.erase (comment);

    for (std::size_t i = 0; i < line.size(); ++i)
    {
        std::size_t valueStart = 0;
        if (startsWithAtBoundary (line, i, "sample=", 7))
            valueStart = i + 7;
        else if (startsWithAtBoundary (line, i, "default_path=", 13))
            valueStart = i + 13;
        else if (startsWithAtBoundary (line, i, "#define", 7))
        {
            // The value is substituted into sample and include references
            // later, so it has to satisfy the same rule they do. That also
            // rules out building a traversal by pasting fragments together:
            // no fragment may contain a dot segment of its own.
            const auto rest = trimAscii (line.substr (i + 7));
            const auto afterName = rest.find_first_of (" \t");
            if (afterName == std::string::npos)
                return "a define directive without a value";

            auto problem = describeUnsafeReference (trimAscii (rest.substr (afterName + 1)));
            if (! problem.empty())
                return problem;
            break;
        }
        else if (startsWithAtBoundary (line, i, "#include", 8))
        {
            const auto open = line.find ('"', i + 8);
            const auto close = open == std::string::npos
                                   ? std::string::npos
                                   : line.find ('"', open + 1);
            if (open == std::string::npos || close == std::string::npos)
                return "an include directive without a quoted path";
            auto problem = describeUnsafeReference (
                trimAscii (line.substr (open + 1, close - open - 1)));
            if (! problem.empty())
                return problem;
            i = close;
            continue;
        }
        else
        {
            continue;
        }

        auto problem = describeUnsafeReference (extractOpcodeValue (line, valueStart));
        if (! problem.empty())
            return problem;
        i = valueStart;
    }
    return {};
}
} // namespace

std::string findUnsafeSfzReference (const std::filesystem::path& sfzFile) noexcept
{
    try
    {
        std::error_code ec;
        const auto size = stdfs::file_size (sfzFile, ec);
        if (ec)
            return "the instrument file could not be read";
        if (size > kMaxSfzTextBytes)
            return "the instrument file is too large to validate";

        std::ifstream in (sfzFile, std::ios::binary);
        if (! in)
            return "the instrument file could not be read";

        std::string line;
        while (std::getline (in, line))
        {
            auto problem = scanSfzLine (line);
            if (! problem.empty())
                return problem;
        }
        return in.bad() ? "the instrument file could not be read" : std::string();
    }
    catch (...)
    {
        return "the instrument file could not be validated";
    }
}

InstallResult installPack (Transport& transport,
                           const CatalogPack& pack,
                           const StoreLayout& layout,
                           const InstallLimits& limits,
                           const InstallCallbacks& callbacks) noexcept
{
    InstallResult result;
    try
    {
        const auto fail = [&result] (InstallStatus status, std::string error)
        {
            result.status = status;
            result.error = std::move (error);
            return result;
        };

        if (pack.yanked)
            return fail (InstallStatus::packRejected, "the pack was withdrawn by the catalog");
        if (layout.root.empty())
            return fail (InstallStatus::storageFailed, "the library location is not configured");

        // The catalog parser already enforces these, but every one of them
        // becomes a path under the library root here, so this layer refuses to
        // depend on a caller having gone through the verified parser.
        const auto isSafeSegment = [] (const std::string& value)
        {
            return value.find ('/') == std::string::npos
                && paths::relativePathRejectionReason (value) == nullptr;
        };
        if (! isSafeSegment (pack.id) || ! isSafeSegment (pack.releaseId)
            || ! isSafeSegment (pack.expectedRoot))
            return fail (InstallStatus::packRejected,
                         "the pack identity cannot be used as a folder name");
        if (paths::relativePathRejectionReason (pack.license.file) != nullptr)
            return fail (InstallStatus::packRejected, "the declared license path is unsafe");
        for (const auto& instrument : pack.instruments)
            if (paths::relativePathRejectionReason (instrument.relativeEntrypoint) != nullptr)
                return fail (InstallStatus::packRejected,
                             "an instrument entrypoint path is unsafe");

        const auto cancelled = [&callbacks]
        {
            return callbacks.isCancelled && callbacks.isCancelled();
        };
        const auto report = [&callbacks] (InstallPhase phase, std::uint64_t completed,
                                          std::uint64_t total)
        {
            if (callbacks.onProgress)
                callbacks.onProgress (phase, completed, total);
        };

        const auto destination = layout.packDirectory (pack.id, pack.releaseId);
        std::error_code ec;
        if (stdfs::exists (destination, ec))
        {
            result.status = InstallStatus::alreadyInstalled;
            result.installedPath = destination;
            return result;
        }

        stdfs::create_directories (layout.downloadsDirectory(), ec);
        if (ec)
            return fail (InstallStatus::storageFailed,
                         "the download directory could not be created");
        stdfs::create_directories (layout.stagingDirectory(), ec);
        if (ec)
            return fail (InstallStatus::storageFailed,
                         "the staging directory could not be created");

        StagingGuard staging (layout.stagingDirectory() / uniqueStagingName (pack));
        stdfs::remove_all (staging.path, ec);
        const auto expansion = staging.path / "expand";
        stdfs::create_directories (expansion, ec);
        if (ec)
            return fail (InstallStatus::storageFailed,
                         "the staging directory could not be created");

        const auto archiveName = pack.id + "-" + pack.releaseId + ".zip";
        DownloadRequest download;
        download.url = pack.downloadUrl;
        download.partFile = layout.downloadsDirectory() / (archiveName + ".part");
        download.destination = layout.downloadsDirectory() / archiveName;
        download.expectedSha256 = pack.archiveSha256;
        download.expectedBytes = pack.compressedBytes;
        download.maximumAttempts = limits.maximumDownloadAttempts;
        download.limits = limits.transfer;

        DownloadCallbacks downloadCallbacks;
        downloadCallbacks.isCancelled = callbacks.isCancelled;
        downloadCallbacks.onProgress = [&report] (std::uint64_t received, std::uint64_t total)
        {
            report (InstallPhase::downloading, received, total);
        };

        const auto downloaded = downloadArchive (transport, download, downloadCallbacks);
        if (downloaded.status == DownloadStatus::cancelled)
            return fail (InstallStatus::cancelled, {});
        if (! downloaded)
            return fail (downloaded.status == DownloadStatus::storageFailed
                             ? InstallStatus::storageFailed
                             : InstallStatus::downloadFailed,
                         downloaded.error);

        if (cancelled())
            return fail (InstallStatus::cancelled, {});

        ExtractionRequest extraction;
        extraction.archiveFile = download.destination;
        extraction.destinationDirectory = expansion;
        extraction.limits.expectedRoot = pack.expectedRoot;
        extraction.limits.maximumExpandedBytes = pack.expandedBytes;
        extraction.limits.maximumFileBytes = std::min (limits.maximumFileBytes,
                                                       pack.expandedBytes);
        extraction.limits.maximumEntries = pack.maxFiles;

        ExtractionCallbacks extractionCallbacks;
        extractionCallbacks.isCancelled = callbacks.isCancelled;
        extractionCallbacks.onProgress = [&report, &pack] (std::uint64_t expanded)
        {
            report (InstallPhase::extracting, expanded, pack.expandedBytes);
        };

        const auto expanded = extractZipArchive (extraction, extractionCallbacks);
        if (expanded.status == ExtractionStatus::cancelled)
            return fail (InstallStatus::cancelled, {});
        if (! expanded)
        {
            if (expanded.status == ExtractionStatus::rejected)
                stdfs::remove (download.destination, ec);
            return fail (expanded.status == ExtractionStatus::storageFailed
                             ? InstallStatus::storageFailed
                             : InstallStatus::archiveRejected,
                         expanded.error);
        }

        report (InstallPhase::validating, 0, 1);
        const auto packRoot = expansion / pack.expectedRoot;
        if (! stdfs::is_directory (packRoot, ec))
            return fail (InstallStatus::validationFailed,
                         "the archive does not contain the expected pack folder");

        const auto licenseFile = packRoot / pack.license.file;
        if (! stdfs::is_regular_file (licenseFile, ec))
            return fail (InstallStatus::validationFailed,
                         "the pack does not carry the license file the catalog declares");
        if (hashFileSha256 (licenseFile) != pack.license.fileSha256)
            return fail (InstallStatus::validationFailed,
                         "the license file does not match the catalog digest");

        for (const auto& instrument : pack.instruments)
        {
            if (! stdfs::is_regular_file (packRoot / instrument.relativeEntrypoint, ec))
                return fail (InstallStatus::validationFailed,
                             "the pack is missing instrument '" + instrument.id + "'");
        }

        for (auto it = stdfs::recursive_directory_iterator (packRoot, ec);
             ! ec && it != stdfs::recursive_directory_iterator(); it.increment (ec))
        {
            if (cancelled())
                return fail (InstallStatus::cancelled, {});
            if (! it->is_regular_file (ec)
                || paths::toLowerAscii (it->path().extension().string()) != ".sfz")
                continue;

            const auto problem = findUnsafeSfzReference (it->path());
            if (! problem.empty())
                return fail (InstallStatus::validationFailed,
                             "an instrument file points outside the pack: " + problem);
        }
        if (ec)
            return fail (InstallStatus::validationFailed,
                         "the expanded pack could not be inspected");

        report (InstallPhase::publishing, 0, 1);
        stdfs::create_directories (destination.parent_path(), ec);
        if (ec)
            return fail (InstallStatus::storageFailed,
                         "the library directory could not be created");
        if (stdfs::exists (destination, ec))
        {
            result.status = InstallStatus::alreadyInstalled;
            result.installedPath = destination;
            return result;
        }

        stdfs::rename (packRoot, destination, ec);
        if (ec)
            return fail (InstallStatus::storageFailed,
                         "the pack could not be published into the library");

        stdfs::remove (download.destination, ec);
        result.status = InstallStatus::installed;
        result.installedPath = destination;
        return result;
    }
    catch (...)
    {
        result.status = InstallStatus::storageFailed;
        result.error = "the install failed unexpectedly";
        return result;
    }
}
} // namespace duskstudio::sfz
