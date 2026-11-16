#pragma once

#include "../TestOpenHandles.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace duskstudio::test
{
// True while `count` holds `value` across a real interval, polled. A thread
// woken by a timer rather than by work moves it dozens of times in it.
inline bool staysAt (const std::atomic<int>& count, int value)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (50);
    while (std::chrono::steady_clock::now() < until)
    {
        if (count.load() != value) return false;
        std::this_thread::yield();
    }
    return count.load() == value;
}

// Runs `call` on a thread of its own and says whether it returned before the
// deadline. A call that did not is left running, detached, so the caller must
// leave alive whatever it uses.
template <typename Call>
bool returnsPromptly (Call call)
{
    auto returned = std::make_shared<std::atomic<bool>> (false);
    std::thread caller ([returned, work = std::move (call)]() mutable
    {
        work();
        returned->store (true);
    });
    const bool prompt = eventually ([&] { return returned->load(); });
    if (prompt) caller.join();
    else caller.detach();
    return prompt;
}
} // namespace duskstudio::test
