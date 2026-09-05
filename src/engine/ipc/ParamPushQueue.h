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
// once published. A producer reserves the next ticket only after seeing its
// slot published by the previous lap's occupant, so from the reservation on
// the slot has exactly one owner until it publishes: a second producer never
// enters it, however long the first is preempted, and the reader can wait on
// it rather than skip it. A push whose slot is still unpublished is dropped
// before reserving anything, which is the only way to keep it from blocking
// on the audio path. A slow reader is simply overtaken: the parent mirrors a
// control's current position, so the freshest value is the one worth keeping.

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

    // push() in two halves, so a test can hold a slot in the reserved-but-not-
    // published state a preempted producer leaves it in.
    bool claim (std::uint64_t& ticket) noexcept
    {
        auto t = writeTicket.load (std::memory_order_relaxed);
        for (;;)
        {
            auto& slot = slots[(std::size_t) (t & kMask)];
            if (slot.stamp.load (std::memory_order_acquire) != freeStamp (t)) return false;
            if (writeTicket.compare_exchange_weak (t, t + 1, std::memory_order_relaxed,
                                                   std::memory_order_relaxed))
            {
                // Sole owner from here: the next producer to want this slot
                // holds ticket t + kCapacity and needs published(t) first.
                slot.stamp.store (claimedStamp (t), std::memory_order_relaxed);
                ticket = t;
                return true;
            }
        }
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

            // Below the published stamp the ticket is reserved and its producer
            // has not finished, normally a handful of instructions away, so
            // leave it for the next drain rather than deliver out of order.
            // Above it a later lap has taken the slot, which needed this ticket
            // published first, so whatever it held is superseded.
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
    // What the slot must show before ticket t may take it: the previous lap's
    // record published, or the initial stamp on the first lap.
    static constexpr std::uint64_t freeStamp (std::uint64_t t) noexcept
    {
        return t >= kCapacity ? publishedStamp (t - kCapacity) : 0;
    }

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
