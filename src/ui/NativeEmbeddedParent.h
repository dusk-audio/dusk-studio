#pragma once

#include <cstdint>

namespace duskstudio::platform
{
struct NativeEmbeddedParent
{
    enum class Backend { unavailable, platform, wayland };

    Backend backend = Backend::unavailable;
    std::uintptr_t handle = 0;
    std::uint32_t peerId = 0;
};

inline bool operator== (const NativeEmbeddedParent& a, const NativeEmbeddedParent& b) noexcept
{
    return a.backend == b.backend && a.handle == b.handle && a.peerId == b.peerId;
}

inline bool operator!= (const NativeEmbeddedParent& a, const NativeEmbeddedParent& b) noexcept
{
    return ! (a == b);
}

// Keeps a native child that failed from reopening on the parent it failed on. A
// close nobody asked for is a failure only while that parent is still alive; one
// that came from losing the parent, and any latch once the live parent is gone or
// replaced, lets the child open again.
class EmbeddedChildLatch
{
public:
    void opening (const NativeEmbeddedParent& parent) noexcept { on = parent; latched = false; }
    void openFailed() noexcept { latched = true; }
    void closed (bool requested, bool parentAlive) noexcept { latched = ! requested && parentAlive; }

    bool blocks (const NativeEmbeddedParent& live) noexcept
    {
        if (latched && live != on)
            latched = false;
        return latched;
    }

    bool isLatched() const noexcept { return latched; }
    const NativeEmbeddedParent& parent() const noexcept { return on; }

private:
    NativeEmbeddedParent on;
    bool latched = false;
};

#if defined (__linux__)
// Message-thread only. A Linux handle must belong to a live shell peer; its ID
// prevents a recycled surface address from becoming a different embed parent.
NativeEmbeddedParent describeEmbeddedParent (std::uintptr_t handle);
bool isEmbeddedParentAlive (const NativeEmbeddedParent& parent);
#else
inline NativeEmbeddedParent describeEmbeddedParent (std::uintptr_t handle)
{
    return { handle != 0 ? NativeEmbeddedParent::Backend::platform
                         : NativeEmbeddedParent::Backend::unavailable, handle, 0 };
}

inline bool isEmbeddedParentAlive (const NativeEmbeddedParent& parent)
{
    return parent.handle != 0;
}
#endif
} // namespace duskstudio::platform
