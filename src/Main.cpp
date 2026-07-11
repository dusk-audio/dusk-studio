#include "DuskStudioApp.h"

#if defined (__linux__)
#include <cstdlib>

// Hand-rolled START_JUCE_APPLICATION expansion (JUCE_CREATE_APPLICATION_DEFINE
// + JUCE_MAIN_FUNCTION_DEFINITION) so the environment can be staged before any
// JUCE code runs.
//
// Dusk Studio is an X11/XWayland client on Linux: every window (main UI and
// plugin-editor peers) is forced to an X11 peer, so the JUCE fork's hybrid
// Wayland connection is never used for windowing. Leaving it open is not
// harmless: its startup handshake (registry binds at the compositor's
// advertised versions, libdecor init) aborts on some compositors
// (wlroots/smithay families — GitHub issue #56) before any window exists.
// JUCE_XWAYLAND makes the fork skip the Wayland connection entirely — the
// behavior every 0.9.x release had. DUSKSTUDIO_NATIVE_WAYLAND=1 re-enables
// the hybrid backend for development.
juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() { return new duskstudio::DuskStudioApp(); }

int main (int argc, char* argv[])
{
    if (std::getenv ("DUSKSTUDIO_NATIVE_WAYLAND") == nullptr)
        setenv ("JUCE_XWAYLAND", "1", 0);

    juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;
    return juce::JUCEApplicationBase::main (argc, (const char**) argv);
}

#else
START_JUCE_APPLICATION (duskstudio::DuskStudioApp)
#endif
