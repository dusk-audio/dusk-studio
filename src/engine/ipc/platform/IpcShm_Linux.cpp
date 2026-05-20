#include "IpcShm.h"

#include <cerrno>
#include <cstring>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace duskstudio::ipc::platform
{
namespace
{
inline int memfdCreate (const char* name, unsigned int flags) noexcept
{
    return (int) ::syscall (SYS_memfd_create, name, flags);
}
}

SharedMemory::~SharedMemory()
{
    close();
}

bool SharedMemory::createAnonymous (const char* debugName,
                                       std::size_t size,
                                       std::string& errorOut) noexcept
{
    close();

    nativeHandle.fd = memfdCreate (debugName != nullptr ? debugName : "duskstudio-shm", 0);
    if (nativeHandle.fd < 0)
    {
        errorOut = std::string ("memfd_create failed: ") + std::strerror (errno);
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
    // The mapping retains its own reference to the underlying memfd, so
    // closing the fd here is safe and frees the descriptor table slot.
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
