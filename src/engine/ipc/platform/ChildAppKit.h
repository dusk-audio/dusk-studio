#pragma once

#if defined(__APPLE__)
namespace duskstudio::ipc::platform
{
// The plug-in host child has no JUCE application object, which is what
// normally creates the shared NSApplication, and without one the JUCE dispatch
// loop returns at once: the message thread never runs, and a plug-in load
// waiting on it never finishes. Call before the JUCE GUI initialiser.
void prepareChildAppKit();
} // namespace duskstudio::ipc::platform
#endif
