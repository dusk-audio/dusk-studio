#pragma once

// JUCE-free by design. This header is included from JUCE translation units
// (VirtualKeyboardComponent.cpp), while its .cpp includes <X11/Xlib.h>, whose
// `#define KeyPress` / `Bool` / `None` macros would shred juce:: names. Keeping
// the X11 include sealed inside KeyboardStateLinux.cpp lets both sides coexist.

namespace duskstudio
{
// Ground-truth physical key state via X11 XQueryKeymap, for the typing-keyboard
// MIDI input. JUCE's juce::KeyPress::isKeyCurrentlyDown derives "down" from the
// X11 event stream, which under XWayland goes stale during OS key auto-repeat
// (a held key reads false between repeats) — so a held note would drop and
// re-trigger. XQueryKeymap reports the server's physical key state directly,
// unaffected by the auto-repeat event pairs.
//
// juceKeyCode is JUCE's key code (uppercase ASCII for the mapped letters/
// digits). Returns:
//   1  -> key physically down
//   0  -> key physically up
//  -1  -> unknown (no X display, or keysym unmapped) — caller should fall back
//         to juce::KeyPress::isKeyCurrentlyDown.
int isKeyPhysicallyDown (int juceKeyCode) noexcept;
} // namespace duskstudio
