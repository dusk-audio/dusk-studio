#pragma once

namespace duskstudio::platform
{
enum class LinuxPeerTeardown
{
    x11,
    wayland
};

[[nodiscard]] constexpr LinuxPeerTeardown linuxPeerTeardownForMapping (
    bool peerHasWaylandMapping) noexcept
{
    return peerHasWaylandMapping ? LinuxPeerTeardown::wayland
                                 : LinuxPeerTeardown::x11;
}
} // namespace duskstudio::platform
