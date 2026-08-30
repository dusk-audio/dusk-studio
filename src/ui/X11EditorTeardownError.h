#pragma once

#include <cstdint>

namespace duskstudio::platform::x11
{
// Stable X11 protocol values, kept independent of Xlib so the routing policy is
// testable on every platform.
constexpr std::uint8_t kBadWindow = 3;
constexpr std::uint8_t kChangeWindowAttributes = 2;
constexpr std::uint8_t kReparentWindow = 7;
constexpr std::uint8_t kUnmapWindow = 10;

inline bool shouldSuppressEditorTeardownError (std::uint8_t errorCode,
                                                std::uint8_t requestCode,
                                                std::uint64_t resourceId,
                                                std::uint64_t editorWindowId) noexcept
{
    if (editorWindowId == 0 || resourceId != editorWindowId || errorCode != kBadWindow)
        return false;

    return requestCode == kChangeWindowAttributes
        || requestCode == kReparentWindow
        || requestCode == kUnmapWindow;
}
} // namespace duskstudio::platform::x11
