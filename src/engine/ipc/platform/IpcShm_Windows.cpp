#include "IpcShm.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>

// CreateFileMapping with INVALID_HANDLE_VALUE (anonymous, page-file
// backed) + SECURITY_ATTRIBUTES.bInheritHandle = TRUE so the child
// process inherits the same numeric HANDLE value through
// CreateProcess(bInheritHandles = TRUE). The parent then writes that
// value down the channel; the child does MapViewOfFile against it.

namespace duskstudio::ipc::platform
{
namespace
{
inline HANDLE asWinHandle (const NativeHandle& h) noexcept
{
    return reinterpret_cast<HANDLE> (h.h);
}

inline void storeWinHandle (NativeHandle& dst, HANDLE src) noexcept
{
    dst.h = reinterpret_cast<void*> (src);
}

inline bool handleIsValid (HANDLE h) noexcept
{
    return h != nullptr && h != INVALID_HANDLE_VALUE;
}
} // namespace

SharedMemory::~SharedMemory()
{
    close();
}

bool SharedMemory::createAnonymous (const char* /*debugName*/,
                                       std::size_t size,
                                       std::string& errorOut) noexcept
{
    close();

    SECURITY_ATTRIBUTES sa {};
    sa.nLength              = sizeof (sa);
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    const DWORD hi = (DWORD) ((std::uint64_t) size >> 32);
    const DWORD lo = (DWORD) ((std::uint64_t) size & 0xffffffffULL);

    HANDLE w = ::CreateFileMappingA (
        INVALID_HANDLE_VALUE,   // anonymous (page-file backed)
        &sa,
        PAGE_READWRITE,
        hi, lo,
        nullptr);

    if (! handleIsValid (w))
    {
        char buf[128]; std::snprintf (buf, sizeof (buf),
            "CreateFileMapping failed: %lu", (unsigned long) ::GetLastError());
        errorOut = buf;
        return false;
    }
    storeWinHandle (nativeHandle, w);

    mapped = ::MapViewOfFile (w, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
    if (mapped == nullptr)
    {
        char buf[128]; std::snprintf (buf, sizeof (buf),
            "MapViewOfFile failed: %lu", (unsigned long) ::GetLastError());
        errorOut = buf;
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

    HANDLE w = asWinHandle (inherited);
    mapped = ::MapViewOfFile (w, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
    if (mapped == nullptr)
    {
        char buf[128]; std::snprintf (buf, sizeof (buf),
            "MapViewOfFile failed: %lu", (unsigned long) ::GetLastError());
        errorOut = buf;
        closeHandle (inherited);
        return false;
    }
    // The mapping retains its own reference; closing the handle is safe
    // and matches Linux behaviour (the SHM lives for the life of the
    // view, not the handle).
    closeHandle (inherited);
    mappedSize = size;
    return true;
}

void SharedMemory::close() noexcept
{
    if (mapped != nullptr)
    {
        ::UnmapViewOfFile (mapped);
        mapped = nullptr;
        mappedSize = 0;
    }
    closeHandle (nativeHandle);
}

} // namespace duskstudio::ipc::platform
