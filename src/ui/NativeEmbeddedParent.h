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
