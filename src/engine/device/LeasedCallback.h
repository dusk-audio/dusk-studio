#pragma once

#include "GraphClock.h"
#include "IODeviceCallback.h"

#include <atomic>
#include <thread>

namespace duskstudio::device
{
// The callback a device thread runs every cycle, attached and detached by the
// message thread with no lock on the device thread: a mutex there would let a
// message thread preempted inside start() or stop() hold up the audio.
//
// A cycle raises a flag before it reads the pointer, and detach() clears the
// pointer before it reads the flag, all sequentially consistent. So either the
// cycle reads no callback, or detach() sees the flag and waits for that one
// cycle to end. CallbackFanout's snapshot lease in DeviceManager.cpp works the
// same way. One device thread runs cycles at a time.
class LeasedCallback
{
public:
    // Device thread, for one cycle.
    class Lease
    {
    public:
        explicit Lease (LeasedCallback& source) noexcept : owner (source)
        {
            owner.inCycle.store (true, std::memory_order_seq_cst);
            current = owner.callback.load (std::memory_order_seq_cst);
        }

        ~Lease() { owner.inCycle.store (false, std::memory_order_release); }

        Lease (const Lease&) = delete;
        Lease& operator= (const Lease&) = delete;

        // The attached callback or null. Whatever the message thread stored
        // before it attached this callback is visible to reads made after this.
        IODeviceCallback* get() const noexcept { return current; }

    private:
        LeasedCallback& owner;
        IODeviceCallback* current = nullptr;
    };

    // Message thread.
    void attach (IODeviceCallback* newCallback) noexcept
    {
        callback.store (newCallback, std::memory_order_seq_cst);
    }

    // Message thread. Returns the callback that was attached; no cycle runs it
    // once this returns. Waits for at most the cycle in progress.
    IODeviceCallback* detach() noexcept
    {
        auto* previous = callback.exchange (nullptr, std::memory_order_seq_cst);
        while (inCycle.load (std::memory_order_seq_cst))
            std::this_thread::yield();
        return previous;
    }

    // Message thread, when the device thread may be stuck inside a cycle:
    // detaches without waiting, and returns the callback only when no cycle
    // holds it.
    IODeviceCallback* detachIfIdle() noexcept
    {
        auto* previous = callback.exchange (nullptr, std::memory_order_seq_cst);
        return inCycle.load (std::memory_order_seq_cst) ? nullptr : previous;
    }

private:
    static_assert (std::atomic<IODeviceCallback*>::is_always_lock_free);
    static_assert (std::atomic<bool>::is_always_lock_free);

    std::atomic<IODeviceCallback*> callback { nullptr };
    std::atomic<bool> inCycle { false };
};

// Device thread: hands one cycle to the attached callback in blocks of the
// size it was started at. The size is read after the callback, so it is the
// one start() prepared before attaching that callback, never an older one.
// Returns false when no callback is attached.
inline bool dispatchCycle (LeasedCallback& source, const ClockWatch& clock,
                           const float** in, int numIn, float** out, int numOut,
                           int numSamples, const CallbackContext& context) noexcept
{
    const LeasedCallback::Lease lease (source);
    auto* cb = lease.get();
    if (cb == nullptr)
        return false;
    const int prepared = clock.preparedBufferSize();
    CallbackContext block = context;
    block.cycleSamples = prepared > 0 && numSamples > prepared ? numSamples : 0;
    forEachSubBlock (in, numIn, out, numOut, numSamples, prepared,
                     [cb, numIn, numOut, &block] (const float* const* blockIn, float* const* blockOut, int len)
                     {
                         cb->audioDeviceIOCallback (blockIn, numIn, blockOut, numOut, len, block);
                         if (block.cycleSamples > 0)
                             block.cycleOffset += len;
                     });
    return true;
}
} // namespace duskstudio::device
