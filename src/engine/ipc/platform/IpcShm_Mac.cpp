#include "IpcShm.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// macOS has no memfd_create. We use shm_open with a unique name +
// immediate shm_unlink so the named region is reachable only through
// the still-open fd. The fd is sent to the child via SCM_RIGHTS over
// the IPC channel the same way Linux does.

namespace duskstudio::ipc::platform
{

SharedMemory::~SharedMemory()
{
    close();
}

bool SharedMemory::createAnonymous (const char* debugName,
                                       std::size_t size,
                                       std::string& errorOut) noexcept
{
    close();

    // shm_open honours only the leading 31 characters, and the name has to stay
    // unique inside them: a fixed prefix long enough to crowd out the pid and
    // counter makes every connection ask for the same name, so O_EXCL fails a
    // second concurrent load and keeps failing for good against a name a crash
    // left behind. debugName is deliberately not part of it - the name is
    // unlinked immediately and never escapes this function.
    (void) debugName;
    static std::atomic<std::uint64_t> counter { 0 };

    char name[32];
    constexpr int kMaxCreateAttempts = 8;

    // A stale name from a crashed run with this pid is the one case O_EXCL
    // cannot distinguish from a live peer, so a fresh counter value is tried
    // rather than failing the connection outright.
    for (int attempt = 0; attempt < kMaxCreateAttempts; ++attempt)
    {
        const auto seq = counter.fetch_add (1, std::memory_order_relaxed);
        const int written = std::snprintf (name, sizeof (name), "/ds.%lu.%llx",
                                            (unsigned long) ::getpid(),
                                            (unsigned long long) (seq & 0xffffffull));
        if (written < 0 || written >= (int) sizeof (name))
        {
            errorOut = "shared memory name did not fit";
            return false;
        }

        nativeHandle.fd = ::shm_open (name, O_CREAT | O_RDWR | O_EXCL, 0600);
        if (nativeHandle.fd >= 0) break;
        if (errno != EEXIST)
        {
            errorOut = std::string ("shm_open failed: ") + std::strerror (errno);
            return false;
        }
    }

    if (nativeHandle.fd < 0)
    {
        errorOut = "shm_open failed: no free shared memory name";
        return false;
    }
    // Unlink immediately; the fd keeps the mapping alive for the
    // process(es) that have it open. The name itself never escapes.
    (void) ::shm_unlink (name);

    const int descriptorFlags = ::fcntl (nativeHandle.fd, F_GETFD);
    if (descriptorFlags < 0
        || ::fcntl (nativeHandle.fd, F_SETFD,
                    descriptorFlags | FD_CLOEXEC) < 0)
    {
        errorOut = std::string ("fcntl(FD_CLOEXEC) failed: ")
            + std::strerror (errno);
        close();
        return false;
    }

    if (::ftruncate (nativeHandle.fd, (off_t) size) < 0)
    {
        errorOut = std::string ("ftruncate failed: ") + std::strerror (errno);
        close();
        return false;
    }
    mapped = ::mmap (nullptr, size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, nativeHandle.fd, 0);
    if (mapped == MAP_FAILED)
    {
        mapped = nullptr;
        errorOut = std::string ("mmap failed: ") + std::strerror (errno);
        close();
        return false;
    }
    mappedSize = size;
    return true;
}

bool SharedMemory::mapInheritedHandle (NativeHandle& inherited,
                                          std::size_t size,
                                          std::string& errorOut) noexcept
{
    close();

    if (! isValid (inherited))
    {
        errorOut = "inherited handle is invalid";
        return false;
    }

    mapped = ::mmap (nullptr, size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, inherited.fd, 0);
    if (mapped == MAP_FAILED)
    {
        mapped = nullptr;
        errorOut = std::string ("mmap failed: ") + std::strerror (errno);
        closeHandle (inherited);
        return false;
    }
    closeHandle (inherited);
    mappedSize = size;
    return true;
}

void SharedMemory::close() noexcept
{
    if (mapped != nullptr)
    {
        ::munmap (mapped, mappedSize);
        mapped = nullptr;
        mappedSize = 0;
    }
    closeHandle (nativeHandle);
}

} // namespace duskstudio::ipc::platform
