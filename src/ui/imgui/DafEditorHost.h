#pragma once

#include "FirstFrameProbe.h"
#include "../../engine/builtin/DafPlugin.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace duskstudio::imgui
{
// Where a host keeps its first-frame marker: one file per log tag, so an editor
// that ends a run inside its first frame does not refuse the other native views.
std::filesystem::path firstFrameMarkerPath (const std::string& logTag);

// A built-in unit's own plug-in editor, hosted inside a native parent Dusk Studio
// owns. The editor brings its own window and its own event loop, so this gives it
// what DuskImGuiHost gives Dusk Studio's own views: a display that cannot carry an
// embedded child is refused, the loop is pumped on a message-thread timer, a
// graphics failure inside that pump closes the editor rather than the application,
// teardown takes the two ticks the platform needs, and a display-scale change
// rebuilds the window.
//
// It also carries the traffic between editor and unit: an edit reaches the unit's
// parameter, a gesture marks that parameter as the last touched and hands keyboard
// focus back when it ends, and every value the unit holds, its meters included, is
// pushed into the editor as it changes.
//
// The host owns the editor's size. A user-resizable editor asking for one of its
// own is left where the host put it, because the host is what knows the room.
class DafEditorHost final
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

    // The unit the editor belongs to, supplied by the caller so the host needs
    // neither the plug-in nor its framework.
    struct Unit
    {
        std::function<std::unique_ptr<builtin::DafEditor> (
            std::uintptr_t nativeParent, std::uint32_t width, std::uint32_t height,
            double scaleFactor, builtin::DafEditorCallbacks callbacks,
            std::string& errorOut)> createEditor;
        std::function<int()> paramCount;
        std::function<float (int index)> paramValue;
        std::function<void (int index, float value)> setParam;
        std::function<void (int index)> noteTouched;
    };

    struct Callbacks
    {
        // The editor has finished closing, whether the caller asked for it or a
        // graphics failure forced it.
        std::function<void()> closed;

        // A gesture in the editor has ended. The shell takes keyboard focus back
        // here, so the transport keys work again after a knob drag.
        std::function<void()> gestureEnded;

        // Where the editor belongs now, polled every tick so a host resized
        // underneath it follows.
        std::function<Geometry()> geometry;
    };

    // A pump interval of zero leaves the pumping to the caller's own tick().
    DafEditorHost (std::string logTag, std::string displayName,
                   std::filesystem::path firstFrameMarker, int pumpIntervalMs = 16);
    ~DafEditorHost();

    void setUnit (Unit unit);
    void setCallbacks (Callbacks callbacks);

    bool open (std::uintptr_t nativeParent, Geometry geometry);

    // Why the last open() returned false, phrased for the user. Empty when it
    // succeeded or was never called.
    const std::string& lastOpenFailure() const noexcept;

    void setGeometry (Geometry geometry);

    // Teardown is deferred over two pump ticks; close() only asks for it.
    void close();

    // True from a successful open() until the closed callback has run.
    bool isOpen() const noexcept;

    // One turn: pump the editor, push what the unit's parameters now hold, and
    // follow the caller's geometry.
    void tick();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::imgui
