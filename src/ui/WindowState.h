#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace duskstudio
{
// Window position / size / fullscreen-flag persistence. Stored at
//   <userApplicationDataDirectory>/Dusk Studio/window-state.txt
// - separate from session.json (session-portable) and recent.txt (per-user
// session list) because window geometry is per-machine state.
//
// Internally we save the string returned by ResizableWindow's
// getWindowStateAsString(), which encodes bounds + fullscreen flag in a
// JUCE-native format that round-trips through restoreWindowStateFromString().
// We don't roll our own XML - JUCE already gets this right.
//
// Validation: a restored rectangle is sanity-checked against connected
// displays AFTER restore; if the window ends up entirely off-screen (the
// "monitor unplugged" case), the caller falls back to a centred default.
class WindowState
{
public:
    static constexpr int kMinOnscreenPx = 100;

    // Returns the previously-saved JUCE state string, or empty if no state
    // has been saved yet (or the store can't be read).
    static juce::String load();

    // Saves the JUCE state string. No-op if the string is empty.
    static void save (const juce::String& stateString);

    // True if `rect` overlaps any connected display by at least
    // kMinOnscreenPx² of area. Used by MainWindow to validate post-restore.
    static bool rectIsUsable (juce::Rectangle<int> rect);

private:
    static juce::File getStorePath();
};
} // namespace duskstudio
