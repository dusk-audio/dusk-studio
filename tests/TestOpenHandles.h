#pragma once

#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>

namespace duskstudio::test
{
// Descriptors this process holds on `file` (compare against a canonical path).
// Only Linux can count them; elsewhere this is 0 and renamedAway() carries the
// check, because Windows refuses to rename a file that is still open.
inline int openHandlesTo (const std::filesystem::path& file)
{
    int handles = 0;
#if defined(__linux__)
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator ("/proc/self/fd", ec))
    {
        std::error_code linkEc;
        if (std::filesystem::read_symlink (entry.path(), linkEc) == file && ! linkEc)
            ++handles;
    }
#else
    (void) file;
#endif
    return handles;
}

inline bool canCountOpenHandles()
{
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

inline bool renamedAway (const std::filesystem::path& file)
{
    auto moved = file;
    moved += ".moved";
    std::error_code ec;
    std::filesystem::rename (file, moved, ec);
    return ! ec;
}

// For a release that happens on another thread shortly after the state the
// test waited for: polls with a deadline instead of sleeping.
template <typename Predicate>
bool eventually (Predicate&& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (5);
    while (! predicate())
    {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::yield();
    }
    return true;
}
} // namespace duskstudio::test
