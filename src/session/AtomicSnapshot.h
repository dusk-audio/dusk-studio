#pragma once

#include <atomic>
#include <cassert>
#include <memory>
#include <utility>

namespace duskstudio
{
// Lock-free snapshot of an owned value of type T. The audio thread reads
// the snapshot via an acquire-load of a raw pointer; the message thread
// publishes a fresh value built off-thread, then atomically swaps.
//
// Mirrors the swap-load pattern used by PluginSlot for plugin instances,
// generalised to any default-constructible, copy-constructible T (typically
// std::vector<X>).
//
// Safety contract — the previous owned value is held alive across exactly
// one publish so the audio thread can finish its current block holding the
// old pointer. This is sufficient as long as publishes are not closer
// together than one audio block (~10 ms). For the call sites in this
// codebase (MIDI Learn capture, recording stop, session load) publishes
// happen seconds-to-minutes apart, so this constraint always holds. If
// future callers publish faster, a generation-counter or TimeSliceThread
// retire queue would be needed instead.
//
// Threading:
//   • read()          — audio thread.
//   • current()       — message thread (returns the owned value by const-
//                        ref; safe because only the message thread mutates
//                        the owner).
//   • currentMutable()— message thread; in-place mutation without
//                        publishing a new pointer. Safe for value-edits
//                        on existing elements; UNSAFE for resize / erase
//                        while the audio thread might iterate.
//   • publish()       — message thread.
//   • mutate()        — message thread; convenience wrapper that copies
//                        current(), applies the lambda, and publishes.
template <typename T>
class AtomicSnapshot
{
public:
    AtomicSnapshot()
        : owned (std::make_unique<T>())
    {
        currentPtr.store (owned.get(), std::memory_order_release);
    }

    // Audio thread. Returns a raw pointer to the current snapshot, valid
    // for the duration of the calling audio callback (and longer in
    // practice — see safety contract above). May briefly return nullptr
    // during a publish; callers must null-check.
    const T* read() const noexcept
    {
        return currentPtr.load (std::memory_order_acquire);
    }

    // Message thread. Read-only access to the current snapshot. Cheap; no
    // copy.
    const T& current() const noexcept { return *owned; }

    // Message thread. Mutable access to the current snapshot WITHOUT
    // publishing a new one. The audio thread observes mutations through
    // its existing acquire-loaded pointer (same address). Use this for
    // in-place edits where structural integrity is preserved at all
    // times (e.g. assigning a new value to an existing element). Do NOT
    // use this for operations that may reallocate (push_back beyond
    // capacity, insert) or shift elements (erase) while the audio thread
    // could be iterating - prefer `mutate()` or `publish()` for those.
    // PianoRoll's note-drag / velocity-edit handlers are the canonical
    // in-place callers.
    T& currentMutable() noexcept { return *owned; }

    // Message thread. Replace the snapshot wholesale with `fresh`. The
    // previous owner is held alive until the next publish so the audio
    // thread can finish its in-flight block on the old pointer.
    void publish (std::unique_ptr<T> fresh) noexcept
    {
        assert (fresh != nullptr && "AtomicSnapshot::publish requires a non-null value");
        if (fresh == nullptr) return;
        currentPtr.store (fresh.get(), std::memory_order_release);
        previous = std::move (owned);
        owned    = std::move (fresh);
    }

    // Message thread. Build a fresh copy of the current snapshot, apply
    // `fn` to it, then publish. Convenient for incremental mutations
    // (push_back, erase) on container types where most of the data is
    // preserved.
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
