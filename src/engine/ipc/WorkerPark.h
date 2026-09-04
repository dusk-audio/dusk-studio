#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace duskstudio::ipc
{

constexpr auto kWorkerParkTimeout = std::chrono::milliseconds (50);

// Remove the processor from the audio worker's lookup path, then wait until
// every command already observed by the worker has been acknowledged. A
// caller may restore the current processor after a true result. A timeout
// is terminal for the child: the caller must not restore the pointer or touch
// the processor after this function returns false.
//
// The null store and the sequence loads are sequentially consistent, not
// release/acquire. They form a StoreLoad pair, which release/acquire does not
// order: with the weaker pair the parking thread may read a stale
// command == reply out of its own store buffer while the worker has already
// taken the parent's next command with the not-yet-visible old processor
// pointer, and the mutation then overlaps processBlock - the exact hazard the
// park exists to prevent. The parking thread is the socket reader, so the
// fence never lands on the audio path.
template <typename Processor, typename Fn>
bool withParkedWorker (std::atomic<Processor*>& currentProcessor,
                       const std::atomic<std::uint32_t>& commandSequence,
                       const std::atomic<std::uint32_t>& replySequence,
                       Fn&& mutation,
                       std::chrono::steady_clock::duration timeout = kWorkerParkTimeout)
{
    currentProcessor.store (nullptr, std::memory_order_seq_cst);

    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline)
    {
        const auto command = commandSequence.load (std::memory_order_seq_cst);
        const auto reply = replySequence.load (std::memory_order_seq_cst);
        if (command == reply)
        {
            mutation ();
            return true;
        }
        std::this_thread::sleep_for (std::chrono::microseconds (200));
    }

    return false;
}

} // namespace duskstudio::ipc
