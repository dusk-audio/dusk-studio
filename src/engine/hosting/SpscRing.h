#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace duskstudio::hosting
{
// Fixed-capacity single-producer / single-consumer ring for UI→RT parameter
// traffic. The producer is the message thread (a host param set, an editor's
// performEdit); the consumer is the audio thread, draining at the top of each
// process block. No locks, no allocation; one slot is sacrificed to tell full
// from empty.
template <typename T, uint32_t Capacity>
class SpscRing
{
public:
    // Single producer. False when full — the caller drops the item (only under a
    // pathological flood; the next block drains up to Capacity-1 items).
    bool push (const T& v) noexcept
    {
        const uint32_t w    = writeIdx.load (std::memory_order_relaxed);
        const uint32_t next = (w + 1u) % Capacity;
        if (next == readIdx.load (std::memory_order_acquire))
            return false;
        slots[(size_t) w] = v;
        writeIdx.store (next, std::memory_order_release);
        return true;
    }

    // Single consumer (audio thread): fn(item) per queued item, oldest first, at
    // most maxItems. Returns the number consumed.
    template <typename Fn>
    uint32_t drain (Fn&& fn, uint32_t maxItems = Capacity) noexcept
    {
        uint32_t r = readIdx.load (std::memory_order_relaxed);
        const uint32_t w = writeIdx.load (std::memory_order_acquire);
        uint32_t n = 0;
        while (r != w && n < maxItems)
        {
            fn (slots[(size_t) r]);
            r = (r + 1u) % Capacity;
            ++n;
        }
        readIdx.store (r, std::memory_order_release);
        return n;
    }

    // Consumer side: discard everything queued so far (e.g. entries aimed at a
    // plugin that was just replaced). Items the producer pushes concurrently
    // may survive — callers reset again once the swap is fully published.
    void clear() noexcept
    {
        readIdx.store (writeIdx.load (std::memory_order_acquire),
                       std::memory_order_release);
    }

private:
    std::array<T, (size_t) Capacity> slots {};
    std::atomic<uint32_t> writeIdx { 0 };
    std::atomic<uint32_t> readIdx  { 0 };
};
} // namespace duskstudio::hosting
