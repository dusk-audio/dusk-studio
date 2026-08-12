#pragma once

#include <cerrno>
#include <filesystem>

#if defined(_WIN32)
 #include <io.h>
 #include <fcntl.h>
 #include <sys/stat.h>
#else
 #include <fcntl.h>
 #include <unistd.h>
#endif

// A verified archive or published pack that has only reached the page cache
// reads back as its verified self, yet a power cut leaves a file of zeros behind
// the final name that nothing ever re-checks. Durability therefore has to happen
// before the rename that names the bytes, and the rename itself has to reach the
// directory. These helpers are the one place that platform difference lives.
namespace duskstudio::sfz
{
inline bool fsyncFileDescriptor (int fd) noexcept
{
    if (fd < 0)
        return false;
#if defined(_WIN32)
    return _commit (fd) == 0;
#else
    while (::fsync (fd) != 0)
    {
        if (errno == EINTR)
            continue;
        return false;
    }
    return true;
#endif
}

inline bool syncFileContents (const std::filesystem::path& file) noexcept
{
#if defined(_WIN32)
    // _commit needs a writable handle; opening without _O_TRUNC keeps the
    // contents intact.
    int fd = -1;
    if (_wsopen_s (&fd, file.wstring().c_str(), _O_WRONLY | _O_BINARY,
                   _SH_DENYNO, _S_IWRITE) != 0 || fd < 0)
        return false;
    const bool ok = fsyncFileDescriptor (fd);
    _close (fd);
    return ok;
#else
    // fsync flushes the file's data regardless of the open mode, so read-only is
    // enough and avoids demanding write permission we do not otherwise need.
    const int fd = ::open (file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool ok = fsyncFileDescriptor (fd);
    ::close (fd);
    return ok;
#endif
}

inline void syncParentDirectory (const std::filesystem::path& child) noexcept
{
#if !defined(_WIN32)
    const auto parent = child.parent_path();
    if (parent.empty())
        return;
    const int fd = ::open (parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return;
    fsyncFileDescriptor (fd);
    ::close (fd);
#else
    (void) child;
#endif
}
} // namespace duskstudio::sfz
