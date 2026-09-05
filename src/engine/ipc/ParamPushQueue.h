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
// allocate, wait or make a syscall: the parent's audio thread is parked on
// replySeq with a 100 ms deadline for the duration of that block, and a
// producer that stalls there costs the parent a sticky auto-bypass. The single
// consumer is the child's message thread, where the socket write and its mutex
// are free to be slow.
//
// Each slot carries a seqlock stamp: odd while a producer is writing it, even
// once published. A producer takes a slot by compare-exchange, so a slot whose
// previous producer is still writing, however long it has been preempted, is
// never entered by a second one; the push that lands on it is dropped instead,
// which is the only way to keep it from blocking on the audio path. A full
// queue otherwise overwrites its oldest entry rather than refusing the newest:
// the parent mirrors a control's current position, so the freshest value is
// the one worth keeping.

class ParamPushQueue
{
public:
    static constexpr std::size_t kCapacity = 512;

    // Returns false when the record was dropped.
    bool push (const ParamPushRecord& in) noexcept
    {
        std::uint64_t ticket = 0;
        if (! claim (ticket)) return false;
        commit (ticket, in);
        return true;
    }

    // push() in two halves, so a test can hold a slot in the written-but-not-
    // published state a preempted producer leaves it in.
    bool claim (std::uint64_t& ticket) noexcept
    {
        ticket = writeTicket.fetch_add (1, std::memory_order_relaxed);
        auto& slot = slots[(std::size_t) (ticket & kMask)];

        auto expected = slot.stamp.load (std::memory_order_relaxed);
        // Odd: the previous producer is still inside the slot. Beyond our own
        // stamp: this producer was preempted between taking the ticket and
        // getting here for a whole lap, and the slot already holds a newer
        // record than the one we would write.
        if ((expected & 1u) != 0 || expected >= claimedStamp (ticket)) return false;
        return slot.stamp.compare_exchange_strong (expected, claimedStamp (ticket),
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed);
    }

    void commit (std::uint64_t ticket, const ParamPushRecord& in) noexcept
    {
        auto& slot = slots[(std::size_t) (ticket & kMask)];
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

            // Claimed but not published yet: the producer is normally a handful
            // of instructions from finishing, so leave the entry for the next
            // drain rather than dropping it out of order. Anything else here
            // was dropped at claim time, or the slot has already been recycled
            // by a later lap; either way this ticket holds nothing to deliver.
            if (before == claimedStamp (readTicket)) return false;
            if (before != published)
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
