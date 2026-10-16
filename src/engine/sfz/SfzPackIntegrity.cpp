#include "SfzPackIntegrity.h"
#include "../../foundation/Fs.h"

#include <sodium.h>
#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <set>
#include <system_error>

#if ! defined(_WIN32)
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

namespace duskstudio::sfz
{
namespace
{
namespace fs = std::filesystem;
constexpr const char* kReceipt = ".dusk-pack.json";

OperationResult failure (const char* message, FailureKind kind = FailureKind::None)
{
    return { OperationStatus::Failed, message, kind };
}

OperationResult interrupted (OperationStatus status)
{
    return { status, "SFZ file verification interrupted" };
}

struct EntryInfo
{
    bool directory = false;
    std::uint64_t bytes = 0;
    std::array<std::uint64_t, 2> identity {};
    std::array<std::uint64_t, 4> modified {};

    bool operator== (const EntryInfo& other) const noexcept
    {
        return directory == other.directory && bytes == other.bytes
            && identity == other.identity && modified == other.modified;
    }
};

class EntryHandle
{
public:
    EntryHandle() = default;
    EntryHandle (const EntryHandle&) = delete;
    EntryHandle& operator= (const EntryHandle&) = delete;
    ~EntryHandle()
    {
        if (handle == -1) return;
       #if defined(_WIN32)
        CloseHandle (reinterpret_cast<HANDLE> (handle));
       #else
        ::close (static_cast<int> (handle));
       #endif
    }

    OperationResult open (const fs::path& path, bool directory, EntryInfo& info)
    {
       #if defined(_WIN32)
        const auto file = CreateFileW (dusk::fs::nativeIOPath (path).c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT
                | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
        if (file == INVALID_HANDLE_VALUE) return failure ("Cannot open an SFZ pack entry for verification");
        handle = reinterpret_cast<std::intptr_t> (file);
       #else
        const auto file = ::open (path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
                                                   | (directory ? O_DIRECTORY : 0));
        if (file < 0) return failure ("Cannot open an SFZ pack entry for verification",
            errno == ELOOP ? FailureKind::Integrity : FailureKind::None);
        handle = file;
       #endif
        return inspect (directory, info);
    }

    OperationResult inspect (bool directory, EntryInfo& info) const
    {
        info = {};
        info.directory = directory;
       #if defined(_WIN32)
        BY_HANDLE_FILE_INFORMATION data {};
        if (! GetFileInformationByHandle (reinterpret_cast<HANDLE> (handle), &data))
            return failure ("Cannot inspect an SFZ pack entry");
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
            || ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory
            || (! directory && (GetFileType (reinterpret_cast<HANDLE> (handle)) != FILE_TYPE_DISK
                                || data.nNumberOfLinks != 1)))
            return failure ("The SFZ pack contains a link or unsupported entry", FailureKind::Integrity);
        info.identity = { data.dwVolumeSerialNumber,
            (std::uint64_t (data.nFileIndexHigh) << 32) | data.nFileIndexLow };
        FILE_BASIC_INFO times {};
        if (! GetFileInformationByHandleEx (reinterpret_cast<HANDLE> (handle), FileBasicInfo, &times, sizeof times))
            return failure ("Cannot inspect SFZ file change times");
        info.modified[0] = static_cast<std::uint64_t> (times.LastWriteTime.QuadPart);
        info.modified[1] = static_cast<std::uint64_t> (times.ChangeTime.QuadPart);
        if (! directory) info.bytes = (std::uint64_t (data.nFileSizeHigh) << 32) | data.nFileSizeLow;
       #else
        struct stat data {};
        if (::fstat (static_cast<int> (handle), &data) != 0) return failure ("Cannot inspect an SFZ pack entry");
        if ((directory ? ! S_ISDIR (data.st_mode) : ! S_ISREG (data.st_mode))
            || (! directory && (data.st_nlink != 1 || data.st_size < 0)))
            return failure ("The SFZ pack contains a link or unsupported entry", FailureKind::Integrity);
        info.identity = { static_cast<std::uint64_t> (data.st_dev), static_cast<std::uint64_t> (data.st_ino) };
       #if defined(__APPLE__)
        info.modified = { static_cast<std::uint64_t> (data.st_mtimespec.tv_sec), static_cast<std::uint64_t> (data.st_mtimespec.tv_nsec),
                          static_cast<std::uint64_t> (data.st_ctimespec.tv_sec), static_cast<std::uint64_t> (data.st_ctimespec.tv_nsec) };
       #else
        info.modified = { static_cast<std::uint64_t> (data.st_mtim.tv_sec), static_cast<std::uint64_t> (data.st_mtim.tv_nsec),
                          static_cast<std::uint64_t> (data.st_ctim.tv_sec), static_cast<std::uint64_t> (data.st_ctim.tv_nsec) };
       #endif
        if (! directory) info.bytes = static_cast<std::uint64_t> (data.st_size);
       #endif
        return { OperationStatus::Complete, {} };
    }

    std::int64_t read (void* buffer, std::uint32_t bytes) const noexcept
    {
       #if defined(_WIN32)
        DWORD count = 0;
        return ReadFile (reinterpret_cast<HANDLE> (handle), buffer, bytes, &count, nullptr) != 0
            ? static_cast<std::int64_t> (count) : -1;
       #else
        ssize_t count;
        do { count = ::read (static_cast<int> (handle), buffer, bytes); } while (count < 0 && errno == EINTR);
        return count;
       #endif
    }

private:
    std::intptr_t handle = -1;
};

struct Entry
{
    std::string relative, digestPath;
    EntryInfo info;

    bool operator== (const Entry& other) const noexcept
    { return relative == other.relative && digestPath == other.digestPath && info == other.info; }
};

OperationResult collect (const fs::path& root, std::vector<Entry>& entries, std::uint64_t& bytes,
                         const OperationControl& control, const ArchiveLimits& limits)
{
    std::error_code error;
    std::set<std::string> paths;
    for (fs::recursive_directory_iterator it (root, error), end; ! error && it != end; it.increment (error))
    {
        if (const auto state = control.status(); state != OperationStatus::Complete) return interrupted (state);
        const auto relative = it->path().lexically_relative (root).generic_u8string();
        const auto status = it->symlink_status (error);
        if (error) break;
        if (! fs::is_directory (status) && ! fs::is_regular_file (status))
            return failure ("The SFZ pack contains a link or unsupported entry", FailureKind::Integrity);
        EntryHandle handle;
        EntryInfo info;
        const auto opened = handle.open (it->path(), fs::is_directory (status), info);
        if (! opened) return opened;
        if (relative == kReceipt)
        {
            if (info.directory) return failure ("The SFZ installation receipt is not a file", FailureKind::Integrity);
            continue;
        }
        std::string diagnostic;
        const std::unique_ptr<utf8proc_uint8_t, decltype (&std::free)> normalized (
            utf8proc_NFC (reinterpret_cast<const utf8proc_uint8_t*> (relative.c_str())), std::free);
        if (! normalized) return failure ("An SFZ pack path cannot be normalized", FailureKind::Integrity);
        const std::string digestPath (reinterpret_cast<const char*> (normalized.get()));
        const auto key = portablePathKey (digestPath);
        if (entries.size() >= limits.maxFiles || ! validateRelativePath (digestPath, diagnostic, limits.paths)
            || ! key || *key == kReceipt || ! paths.insert (*key).second)
            return failure ("The SFZ pack has unsafe, conflicting, or excessive paths", FailureKind::Integrity);
        if (info.bytes > limits.maxFileBytes || bytes > limits.maxExpandedBytes
            || info.bytes > limits.maxExpandedBytes - bytes)
            return failure ("The SFZ pack exceeds the verification size limit", FailureKind::Integrity);
        bytes += info.bytes;
        entries.push_back ({ relative, digestPath, info });
    }
    if (error) return failure ("Cannot enumerate the SFZ pack for verification");
    std::sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b) { return a.digestPath < b.digestPath; });
    return { OperationStatus::Complete, {} };
}

void hashNumber (crypto_hash_sha256_state& hash, std::uint64_t value)
{
    std::array<unsigned char, 8> encoded {};
    for (std::size_t i = encoded.size(); i > 0; --i) { encoded[i - 1] = static_cast<unsigned char> (value); value >>= 8; }
    crypto_hash_sha256_update (&hash, encoded.data(), encoded.size());
}
} // namespace

PackContentsResult hashPackContents (const fs::path& root, const OperationControl& control,
                                    const ArchiveLimits& limits)
{
    PackContentsResult result;
    const auto finish = [&] (const OperationResult& status)
    { result.status = status.status; result.error = status.error; result.failureKind = status.failureKind; return result; };
    if (const auto status = control.status(); status != OperationStatus::Complete) return finish (interrupted (status));
    if (sodium_init() < 0) return finish (failure ("Cannot initialize SFZ file verification"));
    const auto ioRoot = dusk::fs::nativeIOPath (root);
    EntryHandle rootHandle;
    EntryInfo rootInfo;
    auto state = rootHandle.open (ioRoot, true, rootInfo);
    if (! state) return finish (state);
    std::vector<Entry> entries;
    std::uint64_t total = 0;
    state = collect (ioRoot, entries, total, control, limits);
    if (! state) return finish (state);
    crypto_hash_sha256_state hash {};
    crypto_hash_sha256_init (&hash);
    constexpr unsigned char domain[] = "dusk-sfz-installed-tree-v1";
    crypto_hash_sha256_update (&hash, domain, sizeof domain);
    std::array<char, 65536> buffer {};
    std::uint64_t completed = 0, files = 0;
    const auto changed = [] { return failure ("The SFZ pack changed during verification", FailureKind::Integrity); };
    for (const auto& entry : entries)
    {
        if (const auto status = control.status(); status != OperationStatus::Complete) return finish (interrupted (status));
        EntryHandle handle;
        EntryInfo current;
        state = handle.open (ioRoot / fs::u8path (entry.relative), entry.info.directory, current);
        if (! state) return finish (state);
        if (! (current == entry.info)) return finish (changed());
        const unsigned char kind = entry.info.directory ? 'd' : 'f';
        crypto_hash_sha256_update (&hash, &kind, 1);
        hashNumber (hash, entry.digestPath.size());
        crypto_hash_sha256_update (&hash, reinterpret_cast<const unsigned char*> (entry.digestPath.data()), entry.digestPath.size());
        hashNumber (hash, entry.info.bytes);
        if (! entry.info.directory)
        {
            auto remaining = entry.info.bytes;
            while (remaining != 0)
            {
                if (const auto status = control.status(); status != OperationStatus::Complete) return finish (interrupted (status));
                const auto count = handle.read (buffer.data(), static_cast<std::uint32_t> (std::min<std::uint64_t> (remaining, buffer.size())));
                if (count < 0) return finish (failure ("Cannot read an SFZ pack file for verification"));
                if (count == 0) return finish (changed());
                crypto_hash_sha256_update (&hash, reinterpret_cast<const unsigned char*> (buffer.data()), static_cast<std::size_t> (count));
                remaining -= static_cast<std::uint64_t> (count);
                completed += static_cast<std::uint64_t> (count);
                if (control.progress) control.progress ({ completed, total, files });
            }
            if (const auto status = control.status(); status != OperationStatus::Complete) return finish (interrupted (status));
            const auto extra = handle.read (buffer.data(), 1);
            if (extra < 0) return finish (failure ("Cannot read an SFZ pack file for verification"));
            if (extra != 0) return finish (changed());
            ++files;
        }
        state = handle.inspect (entry.info.directory, current);
        if (! state) return finish (state);
        if (! (current == entry.info)) return finish (changed());
    }
    std::vector<Entry> after;
    std::uint64_t afterBytes = 0;
    state = collect (ioRoot, after, afterBytes, control, limits);
    if (! state) return finish (state);
    EntryHandle currentRoot;
    EntryInfo currentInfo;
    state = currentRoot.open (ioRoot, true, currentInfo);
    if (! state) return finish (state);
    if (! (currentInfo == rootInfo) || entries != after) return finish (changed());
    if (const auto status = control.status(); status != OperationStatus::Complete) return finish (interrupted (status));
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest {};
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex {};
    crypto_hash_sha256_final (&hash, digest.data());
    sodium_bin2hex (hex.data(), hex.size(), digest.data(), digest.size());
    result.status = OperationStatus::Complete;
    result.sha256 = hex.data();
    return result;
}
} // namespace duskstudio::sfz
