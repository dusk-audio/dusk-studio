#pragma once

#include <filesystem>
#include <optional>

#if defined(__linux__)
 #include <stdexcept>
 #include <sys/inotify.h>
 #include <unistd.h>
#endif

namespace duskstudio::test
{
// How many times the call opened the file, through any path that reaches it.
// Empty where the platform offers no way to watch. lilv has changed which
// repeated loads it reports, but a load reads the bundle's manifest, so an open
// is the one sign of a load that every version gives.
//
// inotify merges an event into the identical one queued before it, so opens
// alone would count as one. The closes are watched to keep them apart; an open
// made while the file is still open is not counted.
template <typename Fn>
std::optional<int> countFileOpens (const std::filesystem::path& file, Fn&& fn)
{
   #if defined(__linux__)
    const int watcher = ::inotify_init1 (IN_NONBLOCK | IN_CLOEXEC);
    if (watcher < 0) throw std::runtime_error ("inotify_init1 failed");
    struct Close { int fd; ~Close() { ::close (fd); } } closeWatcher { watcher };
    if (::inotify_add_watch (watcher, file.c_str(), IN_OPEN | IN_CLOSE) < 0)
        throw std::runtime_error ("inotify_add_watch failed");
    fn();
    int opens = 0;
    alignas (inotify_event) char buffer[4096];
    for (;;)
    {
        const auto bytes = ::read (watcher, buffer, sizeof (buffer));
        if (bytes <= 0) break;
        for (auto* cursor = buffer; cursor < buffer + bytes;)
        {
            const auto* event = reinterpret_cast<const inotify_event*> (cursor);
            if ((event->mask & IN_OPEN) != 0) ++opens;
            cursor += sizeof (inotify_event) + event->len;
        }
    }
    return opens;
   #else
    (void) file;
    fn();
    return std::nullopt;
   #endif
}
} // namespace duskstudio::test
