#pragma once

#include <atomic>
#include <juce_core/juce_core.h>

namespace duskstudio
{
// Atomic-park bracketing for a pointer that's being read by another
// thread (typically the audio thread) and we need to do something with
// the pointee from the message thread that mustn't overlap with the
// audio thread's use.
//
// Sequence:
//   1. Acquire-load the pointer; bail if null.
//   2. Release-store nullptr - the other thread sees null on its NEXT
//      acquire-load and bails before dereferencing.
//   3. Sleep sleepMs - covers any in-flight access on the other thread
//      that loaded the pointer just BEFORE step 2. ~25 ms is plenty
//      even for 1024-sample buffers at 44.1 kHz (~23 ms per block).
//   4. Run fn (the work that needs exclusive access).
//   5. Release-store the original pointer back so the other thread
//      resumes normal use on its next callback.
//
// Used by PluginSlot to bracket getStateInformation /
// fillInPluginDescription so the audio thread isn't inside processBlock
// on the same plugin while we're reading its state. JUCE's contract is
// that processBlock and getStateInformation MUST NOT overlap; a number
// of widely-used plugins crash hard when the host violates it. On
// Linux/Wayland one observed manifestation is the compositor (Mutter)
// faulting out from corrupted state in the plugin's own GUI peer,
// which takes down the desktop session - the same root-cause race
// produces a milder host segfault on macOS / Windows.
//
// Returns the parked pointer (or nullptr if it was null at entry) so
// the caller can detect the no-op case.
template <typename T, typename Fn>
T* withParkedAtomicPointer (std::atomic<T*>& slot, Fn&& fn, int sleepMs = 25)
{
    T* p = slot.load (std::memory_order_acquire);
    if (p == nullptr) return nullptr;
    slot.store (nullptr, std::memory_order_release);
    if (sleepMs > 0)
        juce::Thread::sleep (sleepMs);
    fn (*p);
    slot.store (p, std::memory_order_release);
    return p;
}
} // namespace duskstudio
