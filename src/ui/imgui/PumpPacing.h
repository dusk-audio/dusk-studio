#pragma once

#include <algorithm>
#include <chrono>

namespace duskstudio::imgui
{
// An embedded view pumped from a message-thread timer draws inside the timer
// callback, and the shell reads keys and clicks from the window system only
// while that thread is idle. A frame that outlasts the pump interval would find
// the next tick already queued, and the shell would never get the thread back,
// so the next pump waits out the overrun: a slow view then shares the thread
// with the shell, and one that draws within the interval runs at full rate. The
// overrun counted is capped, so a single stalled frame (a shader build, a
// debugger stop) does not hold the view still for as long again.
inline std::chrono::steady_clock::time_point nextPumpAfter (std::chrono::steady_clock::time_point started,
                                                            std::chrono::steady_clock::time_point finished,
                                                            int intervalMs) noexcept
{
    using Duration = std::chrono::steady_clock::duration;
    const Duration took = std::min<Duration> (finished - started, std::chrono::milliseconds (100));
    const Duration overrun = took - std::chrono::milliseconds (intervalMs);
    return finished + std::max<Duration> (overrun, Duration::zero());
}
} // namespace duskstudio::imgui
