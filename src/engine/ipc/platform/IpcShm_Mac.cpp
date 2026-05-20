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
// the still-open fd. The fd inherits across fork (and is sent to the
// child via SCM_RIGHTS over the IPC channel the same way Linux does).

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

    static std::atomic<std::uint64_t> counter { 0 };
    const auto seq = counter.fetch_add (1, std::memory_order_relaxed);

    char name[64];
    std::snprintf (name, sizeof (name), "/duskstudio.%s.%lu.%llu",
                    debugName != nullptr ? debugName : "shm",
                    (unsigned long) ::getpid(),
                    (unsigned long long) seq);
    // shm_open on macOS only honours the leading 31 chars of the name;
    // truncate to be safe.
    name[31] = '\0';

    nativeHandle.fd = ::shm_open (name, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (nativeHandle.fd < 0)
    {
        errorOut = std::string ("shm_open failed: ") + std::strerror (errno);
        return false;
    }
    // Unlink immediately; the fd keeps the mapping alive for the
    // process(es) that have it open. The name itself never escapes.
    (void) ::shm_unlink (name);

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
