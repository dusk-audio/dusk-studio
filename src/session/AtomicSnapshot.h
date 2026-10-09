#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace duskstudio
{
// Epoch-based reclamation for AtomicSnapshot. A thread that reads snapshots
// while another publishes them - the audio callback, together with the worker
// lanes it dispatches and joins - reads inside a SnapshotReadScope. A publish
// retires the value it replaces, tagged with a new epoch, and the publishing
// thread frees it once every open scope entered at or after that epoch: such a
// scope found the new value already published, so it cannot hold the old one.
// With no scope open, as while no device runs, the publish that retires a value
// frees it.
//
// A retired value outlives at most the callback that was running when it was
// retired, plus the wait for the snapshot's next publish or reclaim(). A scope
// that never closes, a callback wedged in a plug-in, holds back everything
// retired after it opened until it returns.
//
// Opening a scope (slot store, then pointer loads) against a publish (pointer
// store, then slot loads) is Dekker's pattern, as in the engine's process gate:
// all four are seq_cst, so either the publish sees the scope or the scope's
// reads, a worker lane's included, find the new pointer.
namespace snapshot_epoch
{
inline constexpr int kReaderSlots = 32;

struct Readers
{
    std::atomic<std::uint64_t> epoch { 1 };
    // The epoch each open scope entered at; 0 for a free slot.
    std::array<std::atomic<std::uint64_t>, kReaderSlots> entered {};
    // Scopes open without a slot. While there is one nothing is freed.
    std::atomic<int> unslotted { 0 };
};

inline Readers readers;

// Publishing thread, after storing the new value's pointer: the epoch the value
// it replaces retires at. A scope that loads this epoch or a later one also
// finds that pointer.
inline std::uint64_t advance() noexcept
{
    return readers.epoch.fetch_add (1, std::memory_order_seq_cst) + 1;
}

// Publishing thread: a value retired at or before this epoch is free to delete.
inline std::uint64_t reclaimableUpTo() noexcept
{
    if (readers.unslotted.load (std::memory_order_seq_cst) != 0)
        return 0;
    auto upTo = std::numeric_limits<std::uint64_t>::max();
    for (const auto& slot : readers.entered)
    {
        const auto entered = slot.load (std::memory_order_seq_cst);
        if (entered != 0)
            upTo = std::min (upTo, entered);
    }
    return upTo;
}
} // namespace snapshot_epoch

// Open while this thread, or a lane it dispatches and joins before the scope
// closes, may hold a value an AtomicSnapshot's read() returned. Realtime-safe:
// one compare-exchange takes a slot, no allocation, no lock.
class SnapshotReadScope
{
public:
    SnapshotReadScope() noexcept
    {
        auto& r = snapshot_epoch::readers;
        const auto entered = r.epoch.load (std::memory_order_seq_cst);
        for (int i = 0; i < snapshot_epoch::kReaderSlots; ++i)
        {
            auto& candidate = r.entered[(std::size_t) i];
            std::uint64_t empty = 0;
            if (candidate.load (std::memory_order_relaxed) == 0
                && candidate.compare_exchange_strong (empty, entered, std::memory_order_seq_cst))
            {
                slot = i;
                break;
            }
        }
        if (slot < 0)
            r.unslotted.fetch_add (1, std::memory_order_seq_cst);
    }

    ~SnapshotReadScope()
    {
        auto& r = snapshot_epoch::readers;
        if (slot >= 0)
            r.entered[(std::size_t) slot].store (0, std::memory_order_seq_cst);
        else
            r.unslotted.fetch_sub (1, std::memory_order_seq_cst);
    }

    SnapshotReadScope (const SnapshotReadScope&) = delete;
    SnapshotReadScope& operator= (const SnapshotReadScope&) = delete;

private:
    int slot = -1;
};

// A value one thread publishes and others read lock-free. Published values are
// immutable: every change is a new value, published whole.
//
// read()      : reading thread, inside a SnapshotReadScope; the value stays
//               valid until the scope closes. Never null.
// current()   : publishing thread.
// publish()   : publishing thread (the message thread). Any number per block.
// mutate()    : publishing thread; copy current, apply lambda, publish.
// reclaim()   : publishing thread; frees what no open scope can still hold,
//               as every publish also does.
// generation(): publishing thread; how many publishes so far, so an edit can
//               be checked to publish once however many entries it touches.
template <typename T>
class AtomicSnapshot
{
public:
    AtomicSnapshot()
        : owned (std::make_unique<T>())
    {
        currentPtr.store (owned.get(), std::memory_order_release);
    }

    // seq_cst for the scope handshake; on x86 and ARMv8 the same load as acquire.
    const T* read() const noexcept
    {
        return currentPtr.load (std::memory_order_seq_cst);
    }

    const T& current() const noexcept { return *owned; }

    void publish (std::unique_ptr<T> fresh)
    {
        assert (fresh != nullptr && "AtomicSnapshot::publish requires a non-null value");
        if (fresh == nullptr) return;
        currentPtr.store (fresh.get(), std::memory_order_seq_cst);
        retired.push_back ({ std::move (owned), snapshot_epoch::advance() });
        owned = std::move (fresh);
        ++publishes;
        reclaim();
    }

    void reclaim() noexcept
    {
        if (retired.empty()) return;
        const auto upTo = snapshot_epoch::reclaimableUpTo();
        const auto firstKept = std::find_if (retired.begin(), retired.end(),
                                             [upTo] (const Retired& r) { return r.epoch > upTo; });
        retired.erase (retired.begin(), firstKept);
    }

    // Retired values not yet freed.
    std::size_t retiredCount() const noexcept { return retired.size(); }

    std::uint64_t generation() const noexcept { return publishes; }

    template <typename Fn>
    void mutate (Fn&& fn)
    {
        auto fresh = std::make_unique<T> (*owned);
        fn (*fresh);
        publish (std::move (fresh));
    }

private:
    struct Retired
    {
        std::unique_ptr<T> value;
        std::uint64_t      epoch = 0;
    };

    std::atomic<const T*> currentPtr { nullptr };
    std::unique_ptr<T>    owned;
    std::vector<Retired>  retired;   // oldest first, so epochs ascend
    std::uint64_t         publishes = 0;
};
} // namespace duskstudio
