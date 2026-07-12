#pragma once

#include "IpcChannel.h"

#include <cstddef>
#include <string>

// Shared memory region used as the audio + state hot path between Dusk
// Studio and dusk-studio-plugin-host. The wire-format layout itself
// (BlockHeader, audio/MIDI/state offsets) lives in PluginIpc.h; this
// header is only the platform mechanism for creating + mapping the
// region and producing an inheritable handle for the child.
//
// Linux  : memfd_create + ftruncate + mmap; the memfd is passed to the
//          child over the channel via SCM_RIGHTS.
// macOS  : shm_open (named, immediately unlinked) + ftruncate + mmap;
//          the fd is passed via a Mach send-right or SCM_RIGHTS over a
//          fallback socketpair.
// Windows: CreateFileMapping (named or anonymous via DuplicateHandle)
//          + MapViewOfFile; HANDLE is passed via DuplicateHandle.

namespace duskstudio::ipc::platform
{

class SharedMemory
{
public:
    SharedMemory() = default;
    ~SharedMemory();

    SharedMemory (const SharedMemory&) = delete;
    SharedMemory& operator= (const SharedMemory&) = delete;

    // Parent path: allocate `size` bytes of anonymous RW shared memory
    // and mmap it into the caller's address space. `debugName` is
    // informational (memfd_create name on Linux; ignored on Windows).
    // Returns false + sets errorOut on failure.
    bool createAnonymous (const char* debugName,
                           std::size_t size,
                           std::string& errorOut) noexcept;

    // Child path: take ownership of a NativeHandle received over the
    // channel and mmap it. `inherited` is consumed - on return (success
    // or failure) its fd is closed and the handle is invalidated.
    bool mapInheritedHandle (NativeHandle& inherited,
                               std::size_t size,
                               std::string& errorOut) noexcept;

    // The underlying handle, owned by this SharedMemory. Valid after
    // createAnonymous(). Use to call platform::sendHandle so the child
    // can map the same region.
    const NativeHandle& handle() const noexcept { return nativeHandle; }

    void*       data() const noexcept { return mapped; }
    std::size_t size() const noexcept { return mappedSize; }
    bool        isMapped() const noexcept { return mapped != nullptr; }

    // Unmap + close. Idempotent.
    void close() noexcept;

private:
    NativeHandle nativeHandle {};
    void*        mapped     { nullptr };
    std::size_t  mappedSize { 0 };
};

} // namespace duskstudio::ipc::platform
