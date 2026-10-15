#if defined(_WIN32) && ! defined(NOMINMAX)
 #define NOMINMAX
#endif

#include "SfzArchive.h"

#include <archive.h>
#include <archive_entry.h>
#include <sodium.h>
#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
 #include <windows.h>
#else
 #include <dirent.h>
 #include <fcntl.h>
 #include <locale.h>
 #include <sys/stat.h>
 #include <unistd.h>
 #if defined(__APPLE__)
  #include <xlocale.h>
 #endif
#endif

namespace duskstudio::sfz
{
namespace
{
constexpr std::size_t kChunkBytes = 64 * 1024;

class ScopedUtf8Locale
{
public:
    ScopedUtf8Locale()
    {
       #if defined(_WIN32)
        const auto* current = std::setlocale (LC_CTYPE, nullptr);
        if (current == nullptr) return;
        previousName = current;
        previousMode = _configthreadlocale (_ENABLE_PER_THREAD_LOCALE);
        if (previousMode != -1) valid = std::setlocale (LC_CTYPE, ".UTF8") != nullptr;
       #else
        const auto current = uselocale (nullptr);
        for (const char* name : { "C.UTF-8", "en_US.UTF-8", "UTF-8" })
        {
            auto base = duplocale (current);
            if (base == nullptr) return;
            locale = newlocale (LC_CTYPE_MASK, name, base);
            if (locale == nullptr) { freelocale (base); continue; }
            previous = uselocale (locale);
            valid = previous != nullptr;
            return;
        }
       #endif
    }
    ~ScopedUtf8Locale()
    {
       #if defined(_WIN32)
        if (previousMode != -1)
        {
            std::setlocale (LC_CTYPE, previousName.c_str());
            _configthreadlocale (previousMode);
        }
       #else
        if (previous != nullptr) uselocale (previous);
        if (locale != nullptr) freelocale (locale);
       #endif
    }
    ScopedUtf8Locale (const ScopedUtf8Locale&) = delete;
    ScopedUtf8Locale& operator= (const ScopedUtf8Locale&) = delete;
    bool valid = false;

private:
   #if defined(_WIN32)
    int previousMode = -1;
    std::string previousName;
   #else
    locale_t previous = nullptr, locale = nullptr;
   #endif
};

#if defined(_WIN32)
std::wstring windowsPath (const std::filesystem::path& path)
{
    auto full = std::filesystem::absolute (path).lexically_normal().wstring();
    if (full.compare (0, 4, L"\\\\?\\") == 0) return full;
    if (full.compare (0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + full.substr (2);
    return L"\\\\?\\" + full;
}

struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle (HANDLE handle) : value (handle) {}
    Handle (const Handle&) = delete;
    Handle& operator= (const Handle&) = delete;
    Handle (Handle&& other) noexcept : value (other.value)
    {
        other.value = INVALID_HANDLE_VALUE;
    }
    Handle& operator= (Handle&& other) noexcept
    {
        if (value != INVALID_HANDLE_VALUE) CloseHandle (value);
        value = other.value;
        other.value = INVALID_HANDLE_VALUE;
        return *this;
    }
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle (value); }
};

bool ordinaryHandle (HANDLE handle, bool directory)
{
    BY_HANDLE_FILE_INFORMATION info {};
    return GetFileInformationByHandle (handle, &info)
        && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0
        && ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}
#else
struct Handle
{
    int value = -1;
    Handle() = default;
    explicit Handle (int handle) : value (handle) {}
    Handle (const Handle&) = delete;
    Handle& operator= (const Handle&) = delete;
    Handle (Handle&& other) noexcept : value (other.value) { other.value = -1; }
    Handle& operator= (Handle&& other) noexcept
    {
        if (value >= 0) ::close (value);
        value = other.value;
        other.value = -1;
        return *this;
    }
    ~Handle() { if (value >= 0) ::close (value); }
};
#endif

class Input
{
public:
    explicit Input (const std::filesystem::path& path)
    {
       #if defined(_WIN32)
        handle = Handle (CreateFileW (windowsPath (path).c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        LARGE_INTEGER length {};
        valid = handle.value != INVALID_HANDLE_VALUE
            && ordinaryHandle (handle.value, false)
            && GetFileSizeEx (handle.value, &length) && length.QuadPart >= 0;
        if (valid) size = static_cast<std::uint64_t> (length.QuadPart);
       #else
        handle = Handle (::open (path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        struct stat info {};
        valid = handle.value >= 0 && ::fstat (handle.value, &info) == 0
            && S_ISREG (info.st_mode) && info.st_size >= 0;
        if (valid) size = static_cast<std::uint64_t> (info.st_size);
       #endif
    }

    bool readAt (std::uint64_t offset, void* destination, std::size_t bytes)
    {
        if (! valid || offset > size || bytes > size - offset) return false;
        auto* data = static_cast<unsigned char*> (destination);
        std::size_t done = 0;
        while (done < bytes)
        {
            const auto chunk = std::min (kChunkBytes, bytes - done);
           #if defined(_WIN32)
            LARGE_INTEGER where {};
            where.QuadPart = static_cast<LONGLONG> (offset + done);
            DWORD count = 0;
            if (! SetFilePointerEx (handle.value, where, nullptr, FILE_BEGIN)
                || ! ReadFile (handle.value, data + done, static_cast<DWORD> (chunk),
                              &count, nullptr) || count == 0) return false;
            done += count;
           #else
            const auto count = ::pread (handle.value, data + done, chunk,
                                        static_cast<off_t> (offset + done));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            done += static_cast<std::size_t> (count);
           #endif
        }
        return true;
    }

    static la_ssize_t read (struct archive* reader, void* context, const void** out)
    {
        auto& input = *static_cast<Input*> (context);
        const auto bytes = static_cast<std::size_t> (
            std::min<std::uint64_t> (input.buffer.size(), input.size - input.cursor));
        if (! input.readAt (input.cursor, input.buffer.data(), bytes))
        {
            archive_set_error (reader, EIO, "Cannot read ZIP payload");
            return -1;
        }
        input.cursor += bytes;
        *out = input.buffer.data();
        return static_cast<la_ssize_t> (bytes);
    }

    static la_int64_t seek (struct archive* reader, void* context,
                            la_int64_t offset, int origin)
    {
        auto& input = *static_cast<Input*> (context);
        const auto base = origin == SEEK_SET ? 0
            : origin == SEEK_CUR ? static_cast<la_int64_t> (input.cursor)
            : origin == SEEK_END ? static_cast<la_int64_t> (input.size) : -1;
        if (base < 0 || offset < -base
            || (offset >= 0 && static_cast<std::uint64_t> (offset) > input.size
                                                               - static_cast<std::uint64_t> (base)))
        {
            archive_set_error (reader, EINVAL, "Invalid ZIP seek");
            return ARCHIVE_FATAL;
        }
        input.cursor = static_cast<std::uint64_t> (base + offset);
        return static_cast<la_int64_t> (input.cursor);
    }

    bool valid = false;
    std::uint64_t size = 0;

private:
    Handle handle;
    std::uint64_t cursor = 0;
    std::array<unsigned char, kChunkBytes> buffer {};
};

class Staging
{
public:
    explicit Staging (const std::filesystem::path& directory) : root (directory)
    {
        if (! root.is_absolute()) return;
       #if defined(_WIN32)
        if (! CreateDirectoryW (windowsPath (root).c_str(), nullptr)
            && GetLastError() != ERROR_ALREADY_EXISTS) return;
        handle = openDirectory (root);
        if (handle.value == INVALID_HANDLE_VALUE) return;
        WIN32_FIND_DATAW entry {};
        const auto search = FindFirstFileW (windowsPath (root / L"*").c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE)
        {
            valid = GetLastError() == ERROR_FILE_NOT_FOUND;
            return;
        }
        valid = true;
        do
        {
            if (std::wcscmp (entry.cFileName, L".") != 0
                && std::wcscmp (entry.cFileName, L"..") != 0) valid = false;
        } while (valid && FindNextFileW (search, &entry));
        if (valid && GetLastError() != ERROR_NO_MORE_FILES) valid = false;
        FindClose (search);
       #else
        if (::mkdir (root.c_str(), 0700) != 0 && errno != EEXIST) return;
        handle = Handle (::open (root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (handle.value < 0) return;
        const int copy = ::dup (handle.value);
        if (copy < 0) return;
        auto* directoryEntries = ::fdopendir (copy);
        if (directoryEntries == nullptr) { ::close (copy); return; }
        valid = true;
        errno = 0;
        while (const auto* entry = ::readdir (directoryEntries))
            if (std::strcmp (entry->d_name, ".") != 0
                && std::strcmp (entry->d_name, "..") != 0) { valid = false; break; }
        if (errno != 0) valid = false;
        ::closedir (directoryEntries);
       #endif
    }

    bool create (const std::string& relative, bool directory)
    {
        output = Handle {};
        parents.clear();
        if (! valid) return false;
       #if defined(_WIN32)
        auto current = root;
       #else
        auto parent = Handle (::dup (handle.value));
        if (parent.value < 0) return false;
       #endif
        std::size_t begin = 0;
        std::string spelling;
        while (begin < relative.size())
        {
            const auto slash = relative.find ('/', begin);
            const bool last = slash == std::string::npos;
            const auto part = relative.substr (begin, (last ? relative.size() : slash) - begin);
            if (! spelling.empty()) spelling += '/';
            spelling += part;
           #if defined(_WIN32)
            current /= std::filesystem::u8path (part);
            if (last && ! directory)
            {
                output = Handle (CreateFileW (windowsPath (current).c_str(), GENERIC_WRITE,
                    0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr));
                return output.value != INVALID_HANDLE_VALUE
                    && ordinaryHandle (output.value, false);
            }
            if (! CreateDirectoryW (windowsPath (current).c_str(), nullptr)
                && GetLastError() != ERROR_ALREADY_EXISTS) return false;
            auto child = openDirectory (current);
            if (child.value == INVALID_HANDLE_VALUE || ! rememberDirectory (child, spelling))
                return false;
            parents.push_back (std::move (child));
           #else
            if (last && ! directory)
            {
                output = Handle (::openat (parent.value, part.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
                return output.value >= 0;
            }
            if (::mkdirat (parent.value, part.c_str(), 0700) != 0 && errno != EEXIST)
                return false;
            auto child = Handle (::openat (parent.value, part.c_str(),
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (child.value < 0 || ! rememberDirectory (child, spelling)) return false;
            parent = std::move (child);
           #endif
            if (last) return true;
            begin = slash + 1;
        }
        return false;
    }

    bool write (const void* source, std::size_t bytes)
    {
        const auto* data = static_cast<const unsigned char*> (source);
        std::size_t done = 0;
        while (done < bytes)
        {
           #if defined(_WIN32)
            DWORD count = 0;
            if (! WriteFile (output.value, data + done,
                             static_cast<DWORD> (bytes - done), &count, nullptr)
                || count == 0) return false;
            done += count;
           #else
            const auto count = ::write (output.value, data + done, bytes - done);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            done += static_cast<std::size_t> (count);
           #endif
        }
        return true;
    }

    bool closeFile()
    {
       #if defined(_WIN32)
        if (output.value != INVALID_HANDLE_VALUE)
        {
            const bool flushed = FlushFileBuffers (output.value) != 0;
            const bool closed = CloseHandle (output.value) != 0;
            output.value = INVALID_HANDLE_VALUE;
            parents.clear();
            return flushed && closed;
        }
       #else
        if (output.value >= 0)
        {
            const bool flushed = ::fsync (output.value) == 0;
            const bool closed = ::close (output.value) == 0;
            output.value = -1;
            return flushed && closed;
        }
       #endif
        return true;
    }

    bool valid = false;

private:
    bool rememberDirectory (const Handle& directory, const std::string& spelling)
    {
        std::array<std::uint64_t, 2> identity {};
       #if defined(_WIN32)
        BY_HANDLE_FILE_INFORMATION info {};
        if (! GetFileInformationByHandle (directory.value, &info)) return false;
        identity = { info.dwVolumeSerialNumber,
                     (std::uint64_t (info.nFileIndexHigh) << 32) | info.nFileIndexLow };
       #else
        struct stat info {};
        if (::fstat (directory.value, &info) != 0) return false;
        identity = { static_cast<std::uint64_t> (info.st_dev),
                     static_cast<std::uint64_t> (info.st_ino) };
       #endif
        // Covers filesystem aliases beyond Unicode folding, including DOS 8.3.
        const auto inserted = directoryNames.emplace (identity, spelling);
        return inserted.second || inserted.first->second == spelling;
    }

   #if defined(_WIN32)
    static Handle openDirectory (const std::filesystem::path& path)
    {
        Handle opened (CreateFileW (windowsPath (path).c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (opened.value != INVALID_HANDLE_VALUE && ! ordinaryHandle (opened.value, true))
            return Handle {};
        return opened;
    }
   #endif
    std::filesystem::path root;
    Handle handle;
    Handle output;
    std::vector<Handle> parents;
    std::map<std::array<std::uint64_t, 2>, std::string> directoryNames;
};

std::uint16_t little16 (const unsigned char* data)
{
    return static_cast<std::uint16_t> (data[0] | (std::uint16_t (data[1]) << 8));
}

std::uint32_t little32 (const unsigned char* data)
{
    return std::uint32_t (little16 (data)) | (std::uint32_t (little16 (data + 2)) << 16);
}

std::uint64_t little64 (const unsigned char* data)
{
    return std::uint64_t (little32 (data)) | (std::uint64_t (little32 (data + 4)) << 32);
}

struct ZipEntry
{
    std::string name;
    std::string relative;
    std::uint64_t expanded = 0;
    bool directory = false;
};

struct PathNode
{
    std::string spelling;
    bool directory = false;
    bool explicitEntry = false;
};

bool recordPath (const ZipEntry& entry, std::map<std::string, PathNode>& nodes,
                 std::uint32_t maxNodes, std::string& error)
{
    for (std::size_t end = entry.relative.find ('/');;
         end = entry.relative.find ('/', end + 1))
    {
        const bool last = end == std::string::npos;
        const auto spelling = entry.relative.substr (0, last ? entry.relative.size() : end);
        const auto key = portablePathKey (spelling);
        if (! key) { error = "ZIP path cannot be normalized"; return false; }
        const bool directory = ! last || entry.directory;
        auto inserted = nodes.emplace (*key, PathNode { spelling, directory, last });
        if (nodes.size() > maxNodes)
        {
            error = "ZIP paths exceed the file and directory limit";
            return false;
        }
        if (! inserted.second)
        {
            auto& previous = inserted.first->second;
            if (previous.spelling != spelling || previous.directory != directory
                || (last && previous.explicitEntry))
            {
                error = "ZIP contains duplicate, case-colliding, or conflicting paths";
                return false;
            }
            if (last) previous.explicitEntry = true;
        }
        if (last) return true;
    }
}

bool zip64Extra (const std::vector<unsigned char>& extra, std::string_view name,
                 std::uint64_t& expanded, std::uint64_t& compressed,
                 std::uint64_t& localOffset, bool needsExpanded, bool needsCompressed,
                 bool needsOffset, bool needsDisk, std::string& error)
{
    bool found = false;
    for (std::size_t at = 0; at < extra.size();)
    {
        if (extra.size() - at < 4) { error = "Truncated ZIP extra field"; return false; }
        const auto id = little16 (extra.data() + at);
        const auto size = little16 (extra.data() + at + 2);
        at += 4;
        if (size > extra.size() - at) { error = "Truncated ZIP extra field"; return false; }
        if (id == 0x0001)
        {
            if (found) { error = "Duplicate ZIP64 size field"; return false; }
            found = true;
            std::size_t position = at;
            const auto readValue = [&] (bool needed, std::uint64_t& value)
            {
                if (! needed) return true;
                if (at + size - position < 8) return false;
                value = little64 (extra.data() + position);
                position += 8;
                return true;
            };
            if (! readValue (needsExpanded, expanded) || ! readValue (needsCompressed, compressed)
                || ! readValue (needsOffset, localOffset)
                || (needsDisk && (at + size - position < 4
                                  || little32 (extra.data() + position) != 0)))
            { error = "Invalid ZIP64 size or disk field"; return false; }
        }
        else if (id == 0x7075)
        {
            if (size < 5 || extra[at] != 1
                || std::string_view (reinterpret_cast<const char*> (extra.data() + at + 5),
                                     size - 5) != name)
            { error = "ZIP contains a conflicting Unicode pathname"; return false; }
        }
        at += size;
    }
    if ((needsExpanded || needsCompressed || needsOffset || needsDisk) && ! found)
    { error = "Missing ZIP64 size field"; return false; }
    return true;
}

bool preflight (Input& input, const ArchiveRequest& request,
                const OperationControl& control, std::vector<ZipEntry>& entries,
                ArchiveResult& result)
{
    const auto fail = [&] (const char* error) { result.error = error; return false; };
    if (! input.valid) return fail ("Cannot open a regular ZIP archive");
    if (input.size > request.limits.maxArchiveBytes) return fail ("ZIP exceeds archive byte limit");
    if (input.size < 22) return fail ("Not a ZIP archive");
    const auto tailBytes = static_cast<std::size_t> (
        std::min<std::uint64_t> (input.size, 22 + 65535));
    std::vector<unsigned char> tail (tailBytes);
    if (! input.readAt (input.size - tailBytes, tail.data(), tailBytes))
        return fail ("Cannot read ZIP directory");
    std::size_t end = tailBytes - 22;
    for (;;)
    {
        if (little32 (tail.data() + end) == 0x06054b50
            && little16 (tail.data() + end + 20) == tailBytes - end - 22) break;
        if (end == 0) return fail ("Missing ZIP end directory");
        --end;
    }
    const auto* eocd = tail.data() + end;
    if (little16 (eocd + 4) != 0 || little16 (eocd + 6) != 0
        || little16 (eocd + 8) != little16 (eocd + 10))
        return fail ("Multi-volume ZIP archives are not supported");
    std::uint64_t count = little16 (eocd + 10);
    std::uint64_t directoryBytes = little32 (eocd + 12);
    std::uint64_t directoryOffset = little32 (eocd + 16);
    std::uint64_t directoryEnd = input.size - tailBytes + end;
    std::array<unsigned char, 20> locator {};
    const bool hasZip64 = directoryEnd >= locator.size()
        && input.readAt (directoryEnd - locator.size(), locator.data(), locator.size())
        && little32 (locator.data()) == 0x07064b50;
    if (! hasZip64 && (count == 0xffff || directoryBytes == 0xffffffff
                                      || directoryOffset == 0xffffffff))
        return fail ("Missing ZIP64 locator");
    if (hasZip64)
    {
        if (little32 (locator.data() + 4) != 0 || little32 (locator.data() + 16) != 1)
            return fail ("Invalid ZIP64 locator");
        std::array<unsigned char, 56> zip64 {};
        const auto offset = little64 (locator.data() + 8);
        if (directoryEnd - locator.size() < zip64.size()
            || offset > directoryEnd - locator.size() - zip64.size()
            || ! input.readAt (offset, zip64.data(), zip64.size())
            || little32 (zip64.data()) != 0x06064b50
            || little64 (zip64.data() + 4) < 44
            || little64 (zip64.data() + 4) != directoryEnd - locator.size() - offset - 12
            || little32 (zip64.data() + 16) != 0 || little32 (zip64.data() + 20) != 0
            || little64 (zip64.data() + 24) != little64 (zip64.data() + 32))
            return fail ("Invalid ZIP64 directory");
        if ((count != 0xffff && count != little64 (zip64.data() + 32))
            || (directoryBytes != 0xffffffff && directoryBytes != little64 (zip64.data() + 40))
            || (directoryOffset != 0xffffffff && directoryOffset != little64 (zip64.data() + 48)))
            return fail ("ZIP64 and ZIP directories disagree");
        count = little64 (zip64.data() + 32);
        directoryBytes = little64 (zip64.data() + 40);
        directoryOffset = little64 (zip64.data() + 48);
        directoryEnd = offset;
    }
    if (count == 0 || count > request.limits.maxFiles)
        return fail ("ZIP entry count is empty or exceeds the file limit");
    if (directoryOffset > directoryEnd || directoryBytes > directoryEnd - directoryOffset
        || directoryBytes < count * 46) return fail ("Invalid ZIP directory bounds");
    const auto directoryLimit = directoryOffset + directoryBytes;
    std::map<std::string, PathNode> nodes;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::uint64_t declaredBytes = 0;
    std::uint64_t position = directoryOffset;
    entries.reserve (static_cast<std::size_t> (count));
    for (std::uint64_t index = 0; index < count; ++index)
    {
        const auto status = control.status();
        if (status != OperationStatus::Complete) { result.status = status; return false; }
        std::array<unsigned char, 46> header {};
        if (position > directoryLimit || header.size() > directoryLimit - position
            || ! input.readAt (position, header.data(), header.size())
            || little32 (header.data()) != 0x02014b50)
            return fail ("Invalid ZIP entry directory");
        const auto flags = little16 (header.data() + 8);
        if ((flags & (1 | 0x40 | 0x2000)) != 0) return fail ("Encrypted ZIP entries are not allowed");
        const auto nameBytes = little16 (header.data() + 28);
        const auto extraBytes = little16 (header.data() + 30);
        const auto commentBytes = little16 (header.data() + 32);
        const auto variableBytes = std::uint64_t (nameBytes) + extraBytes + commentBytes;
        position += header.size();
        if (variableBytes > directoryLimit - position
            || nameBytes == 0 || nameBytes > request.limits.paths.maxBytes + 1)
            return fail ("Invalid or oversized ZIP pathname");
        ZipEntry entry;
        entry.name.resize (nameBytes);
        std::vector<unsigned char> extra (extraBytes);
        if (! input.readAt (position, entry.name.data(), nameBytes)
            || ! input.readAt (position + nameBytes, extra.data(), extraBytes))
            return fail ("Cannot read ZIP pathname");
        position += variableBytes;
        entry.directory = entry.name.back() == '/';
        auto path = entry.name;
        if (entry.directory) path.pop_back();
        if (! validateRelativePath (path, result.error, request.limits.paths)) return false;
        const auto attributes = little32 (header.data() + 38);
        const auto type = (attributes >> 16) & 0xf000;
        if ((attributes & 0x400) != 0
            || (type != 0 && type != 0x8000 && type != 0x4000)
            || (type == 0x4000 && ! entry.directory)
            || (type == 0x8000 && entry.directory)
            || ((attributes & 0x10) != 0 && ! entry.directory))
            return fail ("ZIP links, devices, and reparse points are not allowed");
        entry.expanded = little32 (header.data() + 24);
        std::uint64_t compressed = little32 (header.data() + 20);
        std::uint64_t localOffset = little32 (header.data() + 42);
        const auto disk = little16 (header.data() + 34);
        if (disk != 0 && disk != 0xffff) return fail ("Multi-volume ZIP entry");
        if (! zip64Extra (extra, entry.name, entry.expanded, compressed, localOffset,
                          entry.expanded == 0xffffffff, compressed == 0xffffffff,
                          localOffset == 0xffffffff, disk == 0xffff, result.error)) return false;
        if (entry.expanded > request.limits.maxFileBytes
            || entry.expanded > request.limits.maxExpandedBytes - declaredBytes)
            return fail ("ZIP declared size exceeds the expanded byte limit");
        if (entry.directory && entry.expanded != 0) return fail ("ZIP directory contains data");
        declaredBytes += entry.expanded;

        std::array<unsigned char, 30> local {};
        if (localOffset > directoryOffset || local.size() > directoryOffset - localOffset
            || ! input.readAt (localOffset, local.data(), local.size())
            || little32 (local.data()) != 0x04034b50
            || little16 (local.data() + 6) != flags
            || little16 (local.data() + 8) != little16 (header.data() + 10)
            || little16 (local.data() + 26) != nameBytes)
            return fail ("ZIP local header disagrees with its directory");
        const auto localExtraBytes = little16 (local.data() + 28);
        const auto headerBytes = std::uint64_t (local.size()) + nameBytes + localExtraBytes;
        if (headerBytes > directoryOffset - localOffset
            || compressed > directoryOffset - localOffset - headerBytes)
            return fail ("ZIP data exceeds archive bounds");
        std::string localName (nameBytes, '\0');
        std::vector<unsigned char> localExtra (localExtraBytes);
        if (! input.readAt (localOffset + local.size(), localName.data(), localName.size())
            || localName != entry.name
            || ! input.readAt (localOffset + local.size() + nameBytes,
                               localExtra.data(), localExtra.size()))
            return fail ("ZIP contains conflicting local pathnames");
        std::uint64_t ignoredExpanded = little32 (local.data() + 22);
        std::uint64_t ignoredCompressed = little32 (local.data() + 18);
        std::uint64_t ignoredOffset = 0;
        if (! zip64Extra (localExtra, entry.name, ignoredExpanded, ignoredCompressed,
                          ignoredOffset, ignoredExpanded == 0xffffffff,
                          ignoredCompressed == 0xffffffff, false, false, result.error))
            return false;
        if ((flags & 8) == 0 && (ignoredExpanded != entry.expanded
                                || ignoredCompressed != compressed
                                || little32 (local.data() + 14) != little32 (header.data() + 16)))
            return fail ("ZIP local sizes or checksum disagree with its directory");
        ranges.emplace_back (localOffset, localOffset + headerBytes + compressed);
        if (request.expectedRoot.empty()) entry.relative = path;
        else if (path == request.expectedRoot && entry.directory) entry.relative.clear();
        else if (path.compare (0, request.expectedRoot.size() + 1,
                               request.expectedRoot + "/") == 0)
            entry.relative = path.substr (request.expectedRoot.size() + 1);
        else return fail ("ZIP entry is outside the expected root");
        if (! entry.relative.empty()
            && ! recordPath (entry, nodes, request.limits.maxFiles, result.error)) return false;
        entries.push_back (std::move (entry));
    }
    if (position != directoryLimit) return fail ("ZIP directory has unaccounted entries");
    std::sort (ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i)
        if (ranges[i].first < ranges[i - 1].second) return fail ("ZIP entry data overlaps");
    return true;
}

std::optional<std::string> canonicalZipPath (std::string_view path)
{
    if (path.size() > static_cast<std::size_t> (std::numeric_limits<utf8proc_ssize_t>::max()))
        return std::nullopt;
    utf8proc_uint8_t* mapped = nullptr;
    const auto size = utf8proc_map (reinterpret_cast<const utf8proc_uint8_t*> (path.data()),
                                  static_cast<utf8proc_ssize_t> (path.size()), &mapped,
                                  static_cast<utf8proc_option_t> (UTF8PROC_STABLE | UTF8PROC_COMPOSE));
    const std::unique_ptr<utf8proc_uint8_t, decltype (&std::free)> owned (mapped, std::free);
    if (size < 0) return std::nullopt;
    if (size == 0) return std::string {};
    return std::string (reinterpret_cast<const char*> (mapped), static_cast<std::size_t> (size));
}

bool sfzName (const std::string& path)
{
    if (path.size() < 4) return false;
    auto extension = path.substr (path.size() - 4);
    for (auto& c : extension) if (c >= 'A' && c <= 'Z') c = static_cast<char> (c - 'A' + 'a');
    return extension == ".sfz";
}
} // namespace

ArchiveResult extractZip (const ArchiveRequest& request, const OperationControl& control)
{
    ArchiveResult result;
    const auto fail = [&result] (std::string error)
    {
        result.status = OperationStatus::Failed;
        result.error = std::move (error);
        return result;
    };
    try
    {
        auto status = control.status();
        if (status != OperationStatus::Complete) { result.status = status; return result; }
        if (! request.expectedRoot.empty()
            && ! validateRelativePath (request.expectedRoot, result.error, request.limits.paths))
            return result;
        const bool checkLicense = ! request.licensePath.empty() || ! request.licenseSha256.empty();
        if (checkLicense && (! validateRelativePath (request.licensePath, result.error,
                                                    request.limits.paths)
                             || ! isSha256 (request.licenseSha256)))
            return fail ("Invalid license path or SHA-256");
        for (const auto& path : request.expectedEntrypoints)
            if (! validateRelativePath (path, result.error, request.limits.paths) || ! sfzName (path))
                return fail ("Invalid expected SFZ entrypoint");
        Input input (request.archive);
        std::vector<ZipEntry> entries;
        if (! preflight (input, request, control, entries, result)) return result;
        std::uint64_t totalBytes = 0;
        for (const auto& entry : entries) totalBytes += entry.expanded;
        // libarchive converts header names through CTYPE even when ZIP names are UTF-8.
        const ScopedUtf8Locale utf8;
        if (! utf8.valid) return fail ("Cannot enable UTF-8 ZIP pathname decoding");
        Staging staging (request.stagingDirectory);
        if (! staging.valid) return fail ("Staging directory is not empty or cannot be safely opened");
        std::unique_ptr<struct archive, decltype (&archive_read_free)> reader (
            archive_read_new(), archive_read_free);
        if (! reader || archive_read_support_format_zip_seekable (reader.get()) != ARCHIVE_OK
            || archive_read_set_format_option (reader.get(), "zip", "hdrcharset", "UTF-8") != ARCHIVE_OK
            || archive_read_set_seek_callback (reader.get(), Input::seek) != ARCHIVE_OK
            || archive_read_open (reader.get(), &input, nullptr, Input::read, nullptr) != ARCHIVE_OK)
            return fail ("Cannot open ZIP decoder");
        std::map<std::string, const ZipEntry*> indexed;
        for (const auto& entry : entries)
        {
            const auto key = canonicalZipPath (entry.name);
            if (! key || ! indexed.emplace (*key, &entry).second) return fail ("Duplicate or invalid ZIP entry");
        }
        std::set<std::string> extracted;
        bool licenseFound = false;
        std::array<unsigned char, kChunkBytes> chunk {};
        for (;;)
        {
            status = control.status();
            if (status != OperationStatus::Complete) { result.status = status; return result; }
            struct archive_entry* decoded = nullptr;
            const auto readStatus = archive_read_next_header (reader.get(), &decoded);
            if (readStatus == ARCHIVE_EOF) break;
            if (readStatus != ARCHIVE_OK || decoded == nullptr)
            {
                const auto* detail = archive_error_string (reader.get());
                return fail (std::string ("ZIP entry cannot be decoded") + (detail ? std::string (": ") + detail : ""));
            }
            if ((archive_format (reader.get()) & ARCHIVE_FORMAT_BASE_MASK) != ARCHIVE_FORMAT_ZIP
                || archive_entry_is_encrypted (decoded) != 0
                || archive_entry_symlink (decoded) != nullptr
                || archive_entry_hardlink (decoded) != nullptr)
                return fail ("ZIP entry is encrypted or is a link");
            const auto* pathname = archive_entry_pathname_utf8 (decoded);
            if (pathname == nullptr) return fail ("ZIP pathname is not UTF-8");
            // macOS libarchive decomposes UTF-8 names; only canonical equivalence is allowed here.
            const auto key = canonicalZipPath (pathname);
            if (! key) return fail ("ZIP pathname is not UTF-8");
            const auto found = indexed.find (*key);
            if (found == indexed.end() || ! extracted.insert (*key).second)
                return fail ("ZIP decoder pathname disagrees with its directory");
            const auto& entry = *found->second;
            if (archive_entry_filetype (decoded) != (entry.directory ? AE_IFDIR : AE_IFREG)
                || ! archive_entry_size_is_set (decoded) || archive_entry_size (decoded) < 0
                || static_cast<std::uint64_t> (archive_entry_size (decoded)) != entry.expanded)
                return fail ("ZIP decoder type or size disagrees with its directory");
            if (! entry.relative.empty() && ! staging.create (entry.relative, entry.directory))
                return fail ("Cannot create a contained staging path");
            if (entry.directory)
            {
                if (archive_read_data (reader.get(), chunk.data(), chunk.size()) != 0)
                    return fail ("ZIP directory contains invalid data");
                continue;
            }
            crypto_hash_sha256_state hash {};
            const bool license = checkLicense && entry.relative == request.licensePath;
            if (license && crypto_hash_sha256_init (&hash) != 0)
                return fail ("Cannot initialize license checksum");
            std::uint64_t fileBytes = 0;
            for (;;)
            {
                status = control.status();
                if (status != OperationStatus::Complete) { result.status = status; return result; }
                const auto bytes = archive_read_data (reader.get(), chunk.data(), chunk.size());
                if (bytes < 0) return fail ("ZIP data or checksum is invalid");
                if (bytes == 0) break;
                const auto count = static_cast<std::uint64_t> (bytes);
                if (count > entry.expanded - fileBytes
                    || count > request.limits.maxFileBytes - fileBytes
                    || count > request.limits.maxExpandedBytes - result.expandedBytes)
                    return fail ("ZIP actual data exceeds the expanded byte limit");
                if (! staging.write (chunk.data(), static_cast<std::size_t> (bytes)))
                    return fail ("Cannot write staging file");
                if (license && crypto_hash_sha256_update (&hash, chunk.data(),
                                                          static_cast<unsigned long long> (bytes)) != 0)
                    return fail ("Cannot checksum license");
                fileBytes += count;
                result.expandedBytes += count;
                if (control.progress)
                    control.progress ({ result.expandedBytes, totalBytes, result.files });
            }
            if (fileBytes != entry.expanded || ! staging.closeFile())
                return fail ("ZIP entry length or staging flush failed");
            if (license)
            {
                std::array<unsigned char, crypto_hash_sha256_BYTES> digest {};
                std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex {};
                if (crypto_hash_sha256_final (&hash, digest.data()) != 0)
                    return fail ("Cannot finalize license checksum");
                sodium_bin2hex (hex.data(), hex.size(), digest.data(), digest.size());
                if (request.licenseSha256 != hex.data()) return fail ("License SHA-256 mismatch");
                licenseFound = true;
            }
            ++result.files;
            if (sfzName (entry.relative)) result.sfzPaths.push_back (entry.relative);
            if (control.progress) control.progress ({ result.expandedBytes, totalBytes, result.files });
        }
        if (extracted.size() != entries.size()) return fail ("ZIP entries were not all decoded");
        if (checkLicense && ! licenseFound) return fail ("Expected license file is missing");
        if (result.sfzPaths.empty()) return fail ("ZIP contains no SFZ instruments");
        for (const auto& path : request.expectedEntrypoints)
            if (std::find (result.sfzPaths.begin(), result.sfzPaths.end(), path) == result.sfzPaths.end())
                return fail ("Expected SFZ entrypoint is missing");
        std::sort (result.sfzPaths.begin(), result.sfzPaths.end());
        status = control.status();
        result.status = status;
        return result;
    }
    catch (const std::exception& error)
    {
        return fail (std::string ("ZIP extraction failed: ") + error.what());
    }
}
} // namespace duskstudio::sfz
