#include "DuskStudioApp.h"

#if defined (__linux__)
#include <cstdlib>
#include <strings.h>

// Dusk Studio is an X11/XWayland client on Linux: every window (main UI and
// plugin-editor peers) is forced to an X11 peer, so the JUCE fork's hybrid
// Wayland connection is never used for windowing. Leaving it open is not
// harmless: its startup handshake (registry binds at the compositor's
// advertised versions, libdecor init) aborts on some compositors
// (wlroots/smithay families - GitHub issue #56) before any window exists.
// JUCE_XWAYLAND makes the fork skip the Wayland connection entirely - the
// behavior every 0.9.x release had. DUSKSTUDIO_NATIVE_WAYLAND=1 re-enables
// the hybrid backend for development. Static initializer so it runs before
// the app framework touches the window system, without replacing the
// framework's entry-point macro.
namespace
{
struct ForceXWaylandByDefault
{
    ForceXWaylandByDefault()
    {
        // Truthy per the DUSKSTUDIO_* env-flag convention (envFlagSet): a
        // non-zero integer or "true". Unset, "0" or junk keep the XWayland
        // default - setting the flag to 0 must not enable the native path.
        const char* v = std::getenv ("DUSKSTUDIO_NATIVE_WAYLAND");
        const bool nativeWayland = v != nullptr
                                    && (std::atoi (v) != 0 || strcasecmp (v, "true") == 0);
        if (! nativeWayland)
            setenv ("JUCE_XWAYLAND", "1", 0);
    }
};
const ForceXWaylandByDefault forceXWaylandByDefault;
} // namespace
#endif

START_JUCE_APPLICATION (duskstudio::DuskStudioApp)
