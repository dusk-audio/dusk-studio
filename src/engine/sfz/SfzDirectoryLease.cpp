#if defined(_WIN32) && ! defined(NOMINMAX)
 #define NOMINMAX
#endif

#include "SfzDirectoryLease.h"

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

bool DirectoryLease::acquire (const std::filesystem::path& directory, Access access, std::string& error)
{
    error.clear();
    if (*this) { error = "The SFZ directory lease is already held"; return false; }
    if (directory.empty()) { error = "No SFZ directory was supplied for ownership"; return false; }
    std::error_code ec;
    const auto absolute = std::filesystem::absolute (directory, ec);
    if (ec) { error = "Cannot locate the SFZ directory: " + ec.message(); return false; }
    auto current = absolute.root_path();
    const auto plain = [&]
    {
        const auto status = std::filesystem::symlink_status (current, ec);
        if (ec || ! std::filesystem::is_directory (status)) return false;
       #if defined(_WIN32)
        const auto attributes = GetFileAttributesW (current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
       #endif
        return true;
    };
    if (! plain()) { error = "The SFZ ownership directory is unavailable or not a plain directory"; return false; }
    for (const auto& component : absolute.relative_path())
    {
        current /= component;
        if (! plain()) { error = "The SFZ ownership directory is unavailable or not a plain directory"; return false; }
    }
    const auto path = absolute / ".dusk-sfz.lock";
    DirectoryLease candidate;
   #if defined(_WIN32)
    const auto file = CreateFileW (path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    { error = "Cannot open SFZ ownership: " + std::error_code (static_cast<int> (GetLastError()), std::system_category()).message(); return false; }
    candidate.handle = reinterpret_cast<std::intptr_t> (file);
    BY_HANDLE_FILE_INFORMATION info {};
    if (! GetFileInformationByHandle (file, &info) || info.nNumberOfLinks != 1
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
    { error = "The SFZ ownership file is not a plain single-link file"; return false; }
    OVERLAPPED position {};
    const auto flags = access == Access::Exclusive ? LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY : 0;
    if (! LockFileEx (file, flags, 0, 1, 0, &position))
    {
        const auto code = GetLastError();
        if (access != Access::Exclusive || code != ERROR_LOCK_VIOLATION)
            error = "Cannot lock SFZ ownership: " + std::error_code (static_cast<int> (code), std::system_category()).message();
        return false;
    }
   #else
    const auto fd = ::open (path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd == -1)
    { error = "Cannot open SFZ ownership: " + std::error_code (errno, std::generic_category()).message(); return false; }
    candidate.handle = fd;
    struct stat info {};
    if (::fstat (fd, &info) != 0 || ! S_ISREG (info.st_mode) || info.st_nlink != 1)
    { error = "The SFZ ownership file is not a plain single-link file"; return false; }
    const auto flags = access == Access::Exclusive ? LOCK_EX | LOCK_NB : LOCK_SH;
    int result;
    do { result = ::flock (fd, flags); } while (result != 0 && errno == EINTR);
    if (result != 0)
    {
        if (access != Access::Exclusive || (errno != EWOULDBLOCK && errno != EAGAIN))
            error = "Cannot lock SFZ ownership: " + std::error_code (errno, std::generic_category()).message();
        return false;
    }
   #endif
    *this = std::move (candidate);
    return true;
}
} // namespace duskstudio::sfz
