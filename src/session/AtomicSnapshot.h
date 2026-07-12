#pragma once

#include <atomic>
#include <cassert>
#include <memory>
#include <utility>

namespace duskstudio
{
// Lock-free snapshot of an owned T. Audio thread reads via acquire-load
// of a raw pointer; message thread publishes a fresh value off-thread
// then atomically swaps.
//
// The previous owned value is held alive across exactly one publish so
// the audio thread can finish its current block holding the old pointer.
// Sufficient as long as publishes are not closer than one audio block.
// All current call sites (MIDI Learn capture, recording stop, session
// load) publish seconds-to-minutes apart. Faster publishers need a
// generation counter or retire queue.
//
// read()          : audio thread.
// current()       : message thread, const-ref to owned value.
// currentMutable(): message thread, in-place mutation without publish.
//                   Safe for value-edits; UNSAFE for resize/erase while
//                   the audio thread might iterate.
// publish()       : message thread.
// mutate()        : message thread; copy current, apply lambda, publish.
template <typename T>
class AtomicSnapshot
{
public:
    AtomicSnapshot()
        : owned (std::make_unique<T>())
    {
        currentPtr.store (owned.get(), std::memory_order_release);
    }

    // Audio thread. Valid for the calling callback (and longer per the
    // safety contract above). May briefly be nullptr during publish.
    const T* read() const noexcept
    {
        return currentPtr.load (std::memory_order_acquire);
    }

    const T& current() const noexcept { return *owned; }

    // PianoRoll note-drag / velocity-edit handlers are the canonical
    // in-place callers - they edit existing entries without reshaping
    // the container so the audio thread observes mutation through its
    // existing acquire-loaded pointer.
    T& currentMutable() noexcept { return *owned; }

    void publish (std::unique_ptr<T> fresh) noexcept
    {
        assert (fresh != nullptr && "AtomicSnapshot::publish requires a non-null value");
        if (fresh == nullptr) return;
        currentPtr.store (fresh.get(), std::memory_order_release);
        previous = std::move (owned);
        owned    = std::move (fresh);
    }

    template <typename Fn>
    void mutate (Fn&& fn)
    {
        auto fresh = std::make_unique<T> (*owned);
        fn (*fresh);
        publish (std::move (fresh));
    }

private:
    std::atomic<const T*> currentPtr { nullptr };
    std::unique_ptr<T>    owned;
    std::unique_ptr<T>    previous;  // kept alive for one publish
};
} // namespace duskstudio
