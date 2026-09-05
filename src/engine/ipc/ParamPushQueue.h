#pragma once

#include "PluginIpc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace duskstudio::ipc
{

// One queued push, tagged with the plugin instance that produced it. The drain
// runs a tick after the value was queued, by which time a plugin swap may have
// retired the listener that queued it, and the old plugin's value applied to
// the new plugin's parameter index would leave the parent mirroring a control
// that never moved.
struct ParamPushRecord
{
    ParamChangedPayload payload;
    std::uint32_t       generation;
};

// Bounded lock-free MPSC queue carrying child -> parent parameter pushes off
// the thread that produced them.
//
// Producers are the plugin's own threads. A parameter listener may be called
// from any thread, including the child's audio worker while it is inside
// processBlock, and from more than one at a time, so push() must not lock,
// allocate or make a syscall: the parent's audio thread is parked on replySeq
// with a 100 ms deadline for the duration of that block, and a producer that
// stalls there costs the parent a sticky auto-bypass. The single consumer is
// the child's message thread, where the socket write and its mutex are free to
// be slow.
//
// A full queue overwrites its oldest entry instead of refusing the newest: the
// parent mirrors a control's current position, so the freshest value is the one
// worth keeping. Each slot carries a seqlock stamp - odd while a producer is
// filling it, even once published - so the consumer can tell a half-written or
// already-recycled slot from a complete one and skip it.

class ParamPushQueue
{
public:
    static constexpr std::size_t kCapacity = 512;

    void push (const ParamPushRecord& in) noexcept
    {
        const auto ticket = writeTicket.fetch_add (1, std::memory_order_relaxed);
        auto& slot = slots[(std::size_t) (ticket & kMask)];

        slot.stamp.store (claimedStamp (ticket), std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        slot.value = in;
        slot.stamp.store (publishedStamp (ticket), std::memory_order_release);
    }

    bool pop (ParamPushRecord& out) noexcept
    {
        for (;;)
        {
            const auto written = writeTicket.load (std::memory_order_acquire);
            if (readTicket >= written) return false;

            if (written - readTicket > kCapacity)
                readTicket = written - kCapacity;

            auto& slot = slots[(std::size_t) (readTicket & kMask)];
            const auto published = publishedStamp (readTicket);
            const auto before = slot.stamp.load (std::memory_order_acquire);

            // Claimed but not published yet: the producer is a handful of
            // instructions from finishing, so leave the entry for the next
            // drain rather than dropping it out of order.
            if (before < published) return false;

            if (before > published)
            {
                ++readTicket;
                continue;
            }

            out = slot.value;
            std::atomic_thread_fence (std::memory_order_acquire);
            const auto after = slot.stamp.load (std::memory_order_relaxed);

            ++readTicket;
            if (after == published) return true;
        }
    }

private:
    static constexpr std::uint64_t kMask = kCapacity - 1;

    static constexpr std::uint64_t claimedStamp   (std::uint64_t t) noexcept { return 2 * t + 1; }
    static constexpr std::uint64_t publishedStamp (std::uint64_t t) noexcept { return 2 * t + 2; }

    struct Slot
    {
        std::atomic<std::uint64_t> stamp { 0 };
        ParamPushRecord            value {};
    };

    static_assert ((kCapacity & (kCapacity - 1)) == 0, "kCapacity must be a power of two");
    static_assert (std::atomic<std::uint64_t>::is_always_lock_free,
                   "push() runs on threads that must not block");

    Slot slots[kCapacity];
    std::atomic<std::uint64_t> writeTicket { 0 };
    std::uint64_t readTicket = 0;
};

} // namespace duskstudio::ipc
