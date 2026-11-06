#pragma once

#include <cstdint>

namespace duskstudio::ipc::x11
{
// Stable X11 protocol values, kept independent of Xlib so the policy is
// testable on every platform.
constexpr std::uint8_t kBadWindow = 3;
constexpr std::uint8_t kBadDrawable = 9;

// The host can destroy the windows around an embedded editor at any moment, so
// a child that asks about one of them a moment later gets BadWindow or
// BadDrawable on an ID it never created. That error alone is survivable. The
// same error on an ID this connection allocated, or outside an editor's life,
// is a bug in the child and keeps the fatal path.
inline bool isStaleForeignWindowError (std::uint8_t errorCode,
                                       std::uint64_t resourceId,
                                       std::uint64_t ownResourceBase,
                                       std::uint64_t ownResourceMask,
                                       bool editorAlive) noexcept
{
    if (! editorAlive || resourceId == 0)
        return false;
    if (errorCode != kBadWindow && errorCode != kBadDrawable)
        return false;
    return (resourceId & ~ownResourceMask) != ownResourceBase;
}

// Installs the child's process-wide handler. Call once, before anything opens a
// display. Errors the policy above does not claim go to the handler that was in
// place, which for this child is Xlib's own fatal one.
void installStaleWindowErrorHandler();

// Message thread: open from ShowEditor until the editor and its window are gone.
void setEditorAlive (bool alive) noexcept;
} // namespace duskstudio::ipc::x11
