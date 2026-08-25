#pragma once

#include <Application.hpp>
#include <TopLevelWidget.hpp>
#include <Window.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace duskstudio::imgui
{
// The lifecycle every native Dusk view embedded in the JUCE shell shares: create the
// framework child over the host window, refuse a display that cannot carry it, pump the
// framework's event loop on a message-thread timer, survive a graphics driver that
// fails inside that pump, and tear the child down over the two ticks the platform needs
// to unmap it.
//
// The view itself is the widget the caller builds in createWidget; everything around it
// belongs here, so a phase of the GUI tower adds a view rather than another copy of
// this.
class DuskImGuiHost final
{
public:
    struct Geometry
    {
        int x = 0;
        int y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        double scaleFactor = 1.0;
    };

    struct Callbacks
    {
        // Build the view. Runs with the new window's graphics context current, so it may
        // build a font atlas or ask GL what it is. Returning null refuses the window.
        std::function<std::unique_ptr<DGL::TopLevelWidget> (DGL::Window&)> createWidget;

        // Decide whether this display can carry the view, with the context current.
        // Return an empty string to accept it, or the message open() should fail with.
        std::function<std::string (const char* glVersion, const char* glRenderer)> checkGraphics;

        // The widget has just been released with its context still current: drop
        // anything that points into the ImGui font atlas it owned.
        std::function<void()> widgetReleased;

        // The window has finished closing, whether the caller asked for it or a graphics
        // failure forced it.
        std::function<void()> closed;
    };

    struct Identity
    {
        std::string className;   // the native window class
        std::string logTag;      // prefixes this host's diagnostics
        std::string displayName; // how a failure to open is phrased to the user
    };

    // `firstFrameMarker` is where the first-frame guard keeps its marker; an empty path
    // turns that guard off.
    DuskImGuiHost (Identity identity, std::filesystem::path firstFrameMarker);
    ~DuskImGuiHost();

    void setCallbacks (Callbacks callbacks);

    // False when the embedded native child could not be created - no usable GL context,
    // a display backend that cannot embed a foreign surface, or a previous run that
    // never came back from its first frame. The host owns nothing afterwards.
    bool open (std::uintptr_t nativeParent, Geometry geometry);

    // Why the last open() returned false, phrased for the user. Empty when open()
    // succeeded or was never called.
    const std::string& lastOpenFailure() const noexcept;

    void setGeometry (Geometry geometry);

    // Teardown is deferred over two event-pump ticks; close() only asks for it.
    void close();

    // True from a successful open() until the closed callback has run, so a toggle
    // during that deferred teardown cannot restart the window underneath it.
    bool isOpen() const noexcept;

    // Null once the window is gone. For the things only the caller knows to ask for:
    // focus, the cursor, the portal parent handle.
    DGL::Window* window() const noexcept;

    // Diagnostics, on stderr and on Windows the debugger channel too.
    void log (const char* format, ...) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::imgui
