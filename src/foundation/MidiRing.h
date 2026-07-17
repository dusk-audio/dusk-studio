#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

// Single-producer / single-consumer lock-free ring of variable-length MIDI
// event records. One primitive serves both device directions: the MIDI thread
// produces into it and the audio thread consumes (input collector), and the
// audio thread produces into it while the pump thread consumes (output queue).
//
// Capacity is fixed once, off the RT path (ctor or reset). After that neither
// side allocates. A record is a small header (timeMs + numBytes) followed by
// the raw MIDI bytes; records may straddle the physical end of the buffer and
// are stitched back by a two-segment copy - the ring never pads to keep a
// record contiguous. Overflow drops the whole incoming record (push returns
// false); the ring never grows. That drop-don't-grow policy is the deliberate
// divergence from JUCE's collector (which grows under a mutex): dropping a
// clock byte beats an xrun.
namespace dusk
{
class MidiRing
{
public:
    MidiRing() = default;
    explicit MidiRing (std::size_t capacityBytes) { reset (capacityBytes); }

    // Off-RT only. Allocates the backing store and empties the ring. Not safe
    // to call while either side is running.
    void reset (std::size_t capacityBytes)
    {
        buffer.assign (capacityBytes, 0);
        staging.assign (capacityBytes, 0);
        cap = capacityBytes;
        head.store (0, std::memory_order_relaxed);
        tail.store (0, std::memory_order_relaxed);
    }

    std::size_t capacity() const noexcept { return cap; }

    // Producer thread. Copies one record in. Returns false and drops the whole
    // record when it does not fit (over the ring capacity, or the free space is
    // too small right now) - never a partial write, never a realloc.
    bool push (const std::uint8_t* bytes, int n, double timeMs) noexcept
    {
        if (n <= 0 || bytes == nullptr || cap == 0) return false;
        const std::size_t need = kHeader + (std::size_t) n;
        if (need > cap) return false;

        const std::size_t h = head.load (std::memory_order_relaxed);
        const std::size_t t = tail.load (std::memory_order_acquire);
        if (h - t + need > cap) return false;   // not enough free space: drop

        std::uint8_t hdr[kHeader];
        std::memcpy (hdr,                  &timeMs, sizeof (double));
        std::memcpy (hdr + sizeof (double), &n,     sizeof (std::int32_t));
        writeWrapped (h,            hdr,   kHeader);
        writeWrapped (h + kHeader,  bytes, (std::size_t) n);

        head.store (h + need, std::memory_order_release);   // publish after the bytes
        return true;
    }

    // Consumer thread. The producer's current publish position. Capture it once
    // to bound a scan+drain pair (forEachUntil / drainUntil) to the same record
    // set, so anything pushed after the cursor was taken stays pending for the
    // next pass instead of being consumed with a stale-computed backlog.
    std::size_t producerCursor() const noexcept
    {
        return head.load (std::memory_order_acquire);
    }

    // Consumer thread. Invokes fn(const std::uint8_t* bytes, int n, double
    // timeMs) once per record in FIFO order, then frees the space. Wrapped
    // payloads are stitched into a reusable staging buffer so fn always sees a
    // contiguous pointer. Returns the number of records drained.
    template <class Fn>
    int drain (Fn&& fn) noexcept
    {
        return drainUntil (producerCursor(), std::forward<Fn> (fn));
    }

    // Consumer thread. As drain(), but stops at `end` (a value from
    // producerCursor()) rather than the live head, leaving records pushed after
    // the cursor was captured pending.
    template <class Fn>
    int drainUntil (std::size_t end, Fn&& fn) noexcept
    {
        std::size_t t = tail.load (std::memory_order_relaxed);
        if (end < t) return 0;   // stale cursor behind tail: the t != end walk would never terminate
        int count = 0;
        while (t != end)
        {
            double timeMs;
            int    n;
            readWrapped (t, staging.data(), kHeader);
            std::memcpy (&timeMs, staging.data(),                 sizeof (double));
            std::memcpy (&n,      staging.data() + sizeof (double), sizeof (std::int32_t));

            readWrapped (t + kHeader, staging.data(), (std::size_t) n);
            fn (staging.data(), n, timeMs);

            t += kHeader + (std::size_t) n;
            ++count;
        }
        tail.store (t, std::memory_order_release);
        return count;
    }

    // Consumer thread. Read-only walk of the pending records without freeing
    // them; fn(int n, double timeMs) per record. For a two-pass consumer (scan
    // then drain). Consumer-side only - never call from the producer.
    template <class Fn>
    void forEach (Fn&& fn) const noexcept
    {
        forEachUntil (producerCursor(), std::forward<Fn> (fn));
    }

    // Consumer thread. As forEach(), bounded at `end` (from producerCursor()) so
    // a paired scan and drain cover the identical record set.
    template <class Fn>
    void forEachUntil (std::size_t end, Fn&& fn) const noexcept
    {
        std::size_t t = tail.load (std::memory_order_relaxed);
        if (end < t) return;     // stale cursor behind tail: the t != end walk would never terminate
        std::uint8_t hdr[kHeader];
        while (t != end)
        {
            readWrapped (t, hdr, kHeader);
            double timeMs;
            int    n;
            std::memcpy (&timeMs, hdr,                 sizeof (double));
            std::memcpy (&n,      hdr + sizeof (double), sizeof (std::int32_t));
            fn (n, timeMs);
            t += kHeader + (std::size_t) n;
        }
    }

    bool isEmpty() const noexcept
    {
        return head.load (std::memory_order_acquire) == tail.load (std::memory_order_relaxed);
    }

    // Consumer thread (or single-thread). Discards every pending record.
    void clear() noexcept
    {
        tail.store (head.load (std::memory_order_acquire), std::memory_order_release);
    }

private:
    // Header laid out by hand (12 bytes, no struct padding) so a record that
    // wraps carries no alignment assumptions across the split.
    static constexpr std::size_t kHeader = sizeof (double) + sizeof (std::int32_t);

    void writeWrapped (std::size_t pos, const std::uint8_t* src, std::size_t len) noexcept
    {
        const std::size_t idx   = pos % cap;
        const std::size_t first = (len < cap - idx) ? len : cap - idx;
        std::memcpy (buffer.data() + idx, src, first);
        if (first < len)
            std::memcpy (buffer.data(), src + first, len - first);
    }

    void readWrapped (std::size_t pos, std::uint8_t* dst, std::size_t len) const noexcept
    {
        const std::size_t idx   = pos % cap;
        const std::size_t first = (len < cap - idx) ? len : cap - idx;
        std::memcpy (dst, buffer.data() + idx, first);
        if (first < len)
            std::memcpy (dst + first, buffer.data(), len - first);
    }

    std::vector<std::uint8_t> buffer;   // producer writes, consumer reads
    std::vector<std::uint8_t> staging;  // consumer-only reassembly scratch
    std::size_t cap = 0;

    // Monotonic byte counters (never wrap in practice); index = counter % cap.
    // Producer owns head, consumer owns tail; each publishes with release and
    // observes the other with acquire.
    std::atomic<std::size_t> head { 0 };
    std::atomic<std::size_t> tail { 0 };
};
} // namespace dusk
