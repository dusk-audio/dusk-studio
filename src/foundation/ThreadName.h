#pragma once

namespace dusk
{
// Names the calling thread for top -H, ps -L, /proc/<pid>/task/*/comm, debuggers
// and profilers. Linux keeps the first 15 bytes. Call once, first thing on the
// new thread, never from the audio callback.
void setCurrentThreadName (const char* name) noexcept;
} // namespace dusk
