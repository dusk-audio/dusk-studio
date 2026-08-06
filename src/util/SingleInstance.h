#pragma once

#include <functional>
#include <string>

namespace duskstudio::single_instance
{
// Claim the single-instance slot for this user + desktop session.
//
// Returns true if this process is the primary; onCommandLine then fires on the
// message thread for every later launch, carrying that launch's payload -
// which may be empty, meaning "raise the window". A launch that finds the slot
// taken hands its payload over and gets false back, so it can quit before
// opening a device or starting an autosave loop the primary already owns.
// Paths in the payload must already be absolute: the primary resolves relative
// ones against its own working directory, not the launching shell's.
//
// A failed handshake reports "primary" rather than blocking the launch: a
// broken socket must not stop the app from starting. Only a refused connection
// is read as "the primary is gone" and clears the slot; every other handshake
// failure leaves the existing socket untouched and this launch runs without
// owning the slot, because the process behind it may still be alive and two
// primaries would contend for one audio device. Linux only - the slot
// lives under XDG_RUNTIME_DIR, whose per-user ownership is half of what keeps
// a hostile local process out of the handoff, and the .desktop %f double
// launch it guards against is a Linux problem. Elsewhere this is a no-op that
// always returns true.
bool acquire (const std::string& payload,
              std::function<void (std::string)> onCommandLine);

// Drop the slot and join the listener. Idempotent; safe if acquire() was
// never called or returned false.
void release();
} // namespace duskstudio::single_instance
