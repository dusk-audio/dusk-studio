#if defined(_WIN32) && ! defined(NOMINMAX)
 #define NOMINMAX
#endif

#include "SfzDirectoryLease.h"
#include "../../foundation/Fs.h"

#include <system_error>
#include <utility>

#if defined(_WIN32)
 #include <windows.h>
#else
 #include <cerrno>
 #include <fcntl.h>
 #include <sys/file.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

namespace duskstudio::sfz
{
DirectoryLease::~DirectoryLease() { release(); }

DirectoryLease::DirectoryLease (DirectoryLease&& other) noexcept
    : handle (std::exchange (other.handle, -1)) {}

DirectoryLease& DirectoryLease::operator= (DirectoryLease&& other) noexcept
{
    if (this != &other)
    {
        release();
        handle = std::exchange (other.handle, -1);
    }
    return *this;
}

void DirectoryLease::release() noexcept
{
    if (! *this) return;
    // Keep the inode so concurrent owners cannot acquire locks on different files.
   #if defined(_WIN32)
    const auto file = reinterpret_cast<HANDLE> (handle);
    OVERLAPPED position {};
    UnlockFileEx (file, 0, 1, 0, &position);
    CloseHandle (file);
   #else
    const auto fd = static_cast<int> (handle);
    ::flock (fd, LOCK_UN);
    ::close (fd);
   #endif
    handle = -1;
}

bool DirectoryLease::acquire (const std::filesystem::path& directory, Access access, std::string& error,
                              std::error_code* nativeError)
{
    error.clear();
    if (nativeError) nativeError->clear();
    const auto fail = [&] (const char* message, std::error_code code)
    {
        if (nativeError) *nativeError = code;
        error = message;
        if (code) error += ": " + code.message();
        return false;
    };
    if (*this) { error = "The SFZ directory lease is already held"; return false; }
    if (directory.empty()) { error = "No SFZ directory was supplied for ownership"; return false; }
    std::error_code ec;
    const auto absolute = dusk::fs::absolutePathForInspection (directory, ec);
    if (ec) return fail ("Cannot locate the SFZ directory", ec);
    auto current = absolute.root_path();
    const auto relative = absolute.relative_path();
    auto component = relative.begin();
   #if defined(_WIN32)
    if (absolute.root_name().native().compare (0, 2, L"\\\\") == 0 && component != relative.end())
        current /= *component++;
   #endif
    const auto plain = [&]
    {
       #if defined(_WIN32)
        const auto file = CreateFileW (dusk::fs::nativeIOPath (current).c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        { ec = { static_cast<int> (GetLastError()), std::system_category() }; return false; }
        BY_HANDLE_FILE_INFORMATION info {};
        const bool read = GetFileInformationByHandle (file, &info) != 0;
        if (! read) ec = { static_cast<int> (GetLastError()), std::system_category() };
        const bool closed = CloseHandle (file) != 0;
        if (! closed && ! ec) ec = { static_cast<int> (GetLastError()), std::system_category() };
        return read && closed && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
            && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
       #else
        const auto status = std::filesystem::symlink_status (current, ec);
        return ! ec && std::filesystem::is_directory (status);
       #endif
    };
    if (! plain()) return fail ("The SFZ ownership directory is unavailable or not a plain directory", ec);
    for (; component != relative.end(); ++component)
    {
        current /= *component;
        if (! plain()) return fail ("The SFZ ownership directory is unavailable or not a plain directory", ec);
    }
    const auto path = absolute / ".dusk-sfz.lock";
    DirectoryLease candidate;
   #if defined(_WIN32)
    const auto file = CreateFileW (dusk::fs::nativeIOPath (path).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return fail ("Cannot open SFZ ownership", { static_cast<int> (GetLastError()), std::system_category() });
    candidate.handle = reinterpret_cast<std::intptr_t> (file);
    BY_HANDLE_FILE_INFORMATION info {};
    if (! GetFileInformationByHandle (file, &info))
        return fail ("Cannot inspect SFZ ownership", { static_cast<int> (GetLastError()), std::system_category() });
    if (info.nNumberOfLinks != 1 || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
    { error = "The SFZ ownership file is not a plain single-link file"; return false; }
    OVERLAPPED position {};
    const auto flags = access == Access::Exclusive ? LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY : 0;
    if (! LockFileEx (file, flags, 0, 1, 0, &position))
    {
        const auto code = GetLastError();
        if (access != Access::Exclusive || code != ERROR_LOCK_VIOLATION)
            return fail ("Cannot lock SFZ ownership", { static_cast<int> (code), std::system_category() });
        return false;
    }
   #else
    const auto fd = ::open (path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd == -1)
        return fail ("Cannot open SFZ ownership", { errno, std::generic_category() });
    candidate.handle = fd;
    struct stat info {};
    if (::fstat (fd, &info) != 0)
        return fail ("Cannot inspect SFZ ownership", { errno, std::generic_category() });
    if (! S_ISREG (info.st_mode) || info.st_nlink != 1)
    { error = "The SFZ ownership file is not a plain single-link file"; return false; }
    const auto flags = access == Access::Exclusive ? LOCK_EX | LOCK_NB : LOCK_SH;
    int result;
    do { result = ::flock (fd, flags); } while (result != 0 && errno == EINTR);
    if (result != 0)
    {
        const auto code = errno;
        if (access != Access::Exclusive || (code != EWOULDBLOCK && code != EAGAIN))
            return fail ("Cannot lock SFZ ownership", { code, std::generic_category() });
        return false;
    }
   #endif
    *this = std::move (candidate);
    return true;
}
} // namespace duskstudio::sfz
