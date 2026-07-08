#include "KeyboardStateLinux.h"

// Sealed X11 translation unit — must NOT include any JUCE header. Xlib's
// `#define KeyPress 2` (and Bool/None/Status/Window) collide with JUCE names.
#include <X11/Xlib.h>

namespace duskstudio
{
namespace
{
    // One lazily-opened, process-lifetime connection to the same X server the
    // app window already uses ($DISPLAY -> the session's XWayland). XQueryKeymap
    // reports global server key state, so a dedicated connection observes the
    // same physical keyboard as JUCE's connection while staying free of JUCE's
    // headers. Never closed — reclaimed at process exit (one Display, like JUCE
    // itself leaks on Linux shutdown).
    Display* displayConnection() noexcept
    {
        static Display* d = XOpenDisplay (nullptr);
        return d;
    }
}

int isKeyPhysicallyDown (int juceKeyCode) noexcept
{
    Display* d = displayConnection();
    if (d == nullptr)
        return -1;

    // JUCE letter codes are uppercase ASCII; the X keysym for the unshifted
    // (base) key is the lowercase Latin-1 form, and Latin-1 keysyms equal their
    // codepoint, so the cast is the keysym. Digits / other mapped codes pass
    // through unchanged.
    const unsigned long keysym = (juceKeyCode >= 'A' && juceKeyCode <= 'Z')
                                   ? (unsigned long) (juceKeyCode + ('a' - 'A'))
                                   : (unsigned long) juceKeyCode;

    const KeyCode kc = XKeysymToKeycode (d, (KeySym) keysym);
    if (kc == 0)
        return -1;

    char keys[32];
    XQueryKeymap (d, keys);
    return (keys[kc >> 3] & (1 << (kc & 7))) != 0 ? 1 : 0;
}
} // namespace duskstudio
