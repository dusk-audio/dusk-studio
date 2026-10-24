#pragma once

#if defined (_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace duskstudio::platform
{
// Capture before close: an editor accessor can forget its handle while the
// native child still exists. An empty probe means no visible baseline was proven.
inline std::function<bool()> captureNativeWindowsHiddenProbe (
    const std::array<std::uintptr_t, 2>& handles)
{
    struct Window
    {
        HWND handle = nullptr;
        HWND parent = nullptr;
        DWORD thread = 0;
        std::string className;
    };
    std::array<Window, 2> windows {};
    std::size_t count = 0;
    bool visible = false;
    for (const auto handle : handles)
    {
        if (handle == 0) continue;
        const auto hwnd = reinterpret_cast<HWND> (handle);
        DWORD process = 0;
        const auto thread = ::GetWindowThreadProcessId (hwnd, &process);
        const auto parent = ::GetParent (hwnd);
        std::array<char, 256> className {};
        RECT bounds {};
        if (! ::IsWindow (hwnd) || thread == 0 || process != ::GetCurrentProcessId()
            || parent == nullptr || ::GetClassNameA (hwnd, className.data(), (int) className.size()) == 0
            || ! ::GetWindowRect (hwnd, &bounds) || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
            return {};
        windows[count++] = { hwnd, parent, thread, className.data() };
        visible = visible || ::IsWindowVisible (hwnd) != FALSE;
    }
    if (! visible) return {};
    return [windows, count]
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& window = windows[i];
            if (! ::IsWindow (window.handle)) continue;
            DWORD process = 0;
            const auto thread = ::GetWindowThreadProcessId (window.handle, &process);
            std::array<char, 256> className {};
            if (thread != window.thread || process != ::GetCurrentProcessId()
                || ::GetParent (window.handle) != window.parent
                || ::GetClassNameA (window.handle, className.data(), (int) className.size()) == 0
                || window.className != className.data() || ::IsWindowVisible (window.handle))
                return false;
        }
        return true;
    };
}
}

#endif
