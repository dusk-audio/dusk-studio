#include "DafEditorHost.h"
#include "DuskImGuiScale.h"
#include "../../foundation/Fs.h"
#include "../../foundation/MessageThread.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <utility>
#include <vector>

namespace duskstudio::imgui
{
namespace
{
bool differs (const DafEditorHost::Geometry& a, const DafEditorHost::Geometry& b)
{
    return a.x != b.x || a.y != b.y || a.width != b.width || a.height != b.height
        || a.scaleFactor < b.scaleFactor || b.scaleFactor < a.scaleFactor;
}
} // namespace

std::filesystem::path firstFrameMarkerPath (const std::string& logTag)
{
    const auto config = dusk::fs::userConfigDir();
    if (config.empty())
        return {};
    return config / "Dusk Studio" / (logTag + "-first-frame");
}

struct DafEditorHost::Impl final : private dusk::Timer
{
    Impl (std::string tag, std::string name, std::filesystem::path marker, int interval)
        : logTag (std::move (tag)), displayName (std::move (name)),
          probe (std::move (marker)), pumpIntervalMs (interval)
    {
    }

    ~Impl() override
    {
        stopTimer();
        // The callbacks reach into the caller, which is already tearing down by
        // the time this runs, and neither means anything to it now.
        callbacks = {};
        editor.reset();
        // The marker means "armed a frame and never came back", so only a run that
        // armed one may clear it, and only once that is no longer what it says.
        if (armedMarker && firstFrameConfirmed)
            probe.disarm();
    }

    bool open (std::uintptr_t nativeParent, Geometry geometry)
    {
        lastFailure.clear();
        armedMarker = false;
        stopTimer();
        editor.reset();
        closeRequested = false;
        closeWasPumped = false;

        if (unit.createEditor == nullptr || nativeParent == 0
            || geometry.width < 2 || geometry.height < 2)
            return false;

        if (const auto failed = probe.previousFailure(); ! failed.empty())
        {
            const auto marker = probe.path().string();
            log ("a previous run ended while the editor drew its first frame on %s; "
                 "unavailable. Delete %s to try again", failed.c_str(), marker.c_str());
            lastFailure = displayName + " is off: a previous run ended while it drew its "
                          "first frame on " + failed + ". Delete " + marker
                        + " to try again.";
            return false;
        }
        firstFrameConfirmed = false;

        builtin::DafEditorCallbacks editorCallbacks;
        editorCallbacks.gesture = [this] (std::uint32_t index, bool started)
        {
            if (started)
            {
                if (unit.noteTouched)
                    unit.noteTouched ((int) index);
            }
            else if (callbacks.gestureEnded)
            {
                callbacks.gestureEnded();
            }
        };
        editorCallbacks.parameterEdited = [this] (std::uint32_t index, float value)
        {
            applyEdit ((int) index, value);
        };

        std::string error;
        editor = unit.createEditor (nativeParent, geometry.width, geometry.height,
                                    geometry.scaleFactor, std::move (editorCallbacks), error);
        if (editor == nullptr)
        {
            lastFailure = displayName + " cannot open: "
                        + (error.empty() ? std::string ("the editor could not be built.")
                                         : error);
            log ("%s", lastFailure.c_str());
            return false;
        }

        // Dusk Studio owns both windows, so it places the child. A backend that
        // cannot place an embedded child at all - native Wayland - is refused here
        // rather than left to open a window of its own beside the mixer.
        if (! editor->setOffset (geometry.x, geometry.y))
        {
            editor.reset();
            lastFailure = displayName + " cannot open on this display backend.";
            log ("%s", lastFailure.c_str());
            return false;
        }

        embeddedParent = nativeParent;
        appliedScaleFactor = geometry.scaleFactor;
        lastGeometry = geometry;
        pushAllValues = true;
        probe.arm ("this display");
        armedMarker = true;
        if (pumpIntervalMs > 0)
            startTimer (pumpIntervalMs);
        return true;
    }

    void close()
    {
        if (editor == nullptr)
            return;
        closeRequested = true;
        closeWasPumped = false;
    }

    bool isOpen() const noexcept { return editor != nullptr || closeRequested; }

    void setGeometry (Geometry geometry)
    {
        if (editor == nullptr || closeRequested
            || geometry.width < 2 || geometry.height < 2)
            return;

        if (requiresScaleRecreation (appliedScaleFactor, geometry.scaleFactor))
        {
            // An embedded window's draw scale is fixed when it is built, so the
            // editor is rebuilt for the new one. Its parameters live in the unit,
            // which the rebuild does not touch.
            const auto parent = embeddedParent;
            probe.disarm();
            armedMarker = false;
            if (! open (parent, geometry) && callbacks.closed)
                callbacks.closed();
            return;
        }

        editor->setSize (geometry.width, geometry.height);
        editor->setOffset (geometry.x, geometry.y);
        lastGeometry = geometry;
    }

    void tick()
    {
        if (editor != nullptr)
        {
            if (! editor->idle())
            {
                // The editor quit, or its graphics stack failed inside its own
                // pump. Either way it goes, and the unit keeps every value.
                log ("the editor closed itself");
                stopTimer();
                editor.reset();
                closeRequested = false;
                closeWasPumped = false;
                if (callbacks.closed)
                    callbacks.closed();
                return;
            }

            if (! firstFrameConfirmed)
            {
                firstFrameConfirmed = true;
                probe.disarm();
            }
        }

        // A close is asked for from the host boundary; the editor goes only after
        // its own loop has returned, and the caller hears about it one tick later,
        // which is what the platform needs to finish unmapping the child.
        if (closeRequested && editor != nullptr)
        {
            editor.reset();
            closeWasPumped = true;
            return;
        }
        if (closeRequested && closeWasPumped)
        {
            closeRequested = false;
            closeWasPumped = false;
            stopTimer();
            if (callbacks.closed)
                callbacks.closed();
            return;
        }
        if (editor == nullptr)
            return;

        pushChangedValues();
        followGeometry();
    }

    void timerCallback() override { tick(); }

    void applyEdit (int index, float value)
    {
        if (unit.setParam)
            unit.setParam (index, value);
        // The control under the pointer already shows what it set, and the unit
        // may hold a conformed value; take that as pushed so the next tick does
        // not send a value back into the gesture.
        if (unit.paramValue != nullptr && index >= 0
            && index < (int) pushedValues.size())
            pushedValues[(std::size_t) index] = unit.paramValue (index);
    }

    void pushChangedValues()
    {
        if (unit.paramCount == nullptr || unit.paramValue == nullptr)
            return;

        const int count = unit.paramCount();
        if ((int) pushedValues.size() != count)
        {
            pushedValues.assign ((std::size_t) std::max (0, count), 0.0f);
            pushAllValues = true;
        }

        const bool all = pushAllValues;
        pushAllValues = false;
        for (int i = 0; i < count; ++i)
        {
            const float value = unit.paramValue (i);
            if (! all && ! (std::abs (value - pushedValues[(std::size_t) i]) > 0.0f))
                continue;
            pushedValues[(std::size_t) i] = value;
            editor->parameterChanged ((std::uint32_t) i, value);
        }
    }

    void followGeometry()
    {
        if (! callbacks.geometry)
            return;
        const auto wanted = callbacks.geometry();
        if (wanted.width < 2 || wanted.height < 2 || ! differs (wanted, lastGeometry))
            return;
        setGeometry (wanted);
    }

    void log (const char* format, ...) const
    {
        char message[512] {};
        va_list args;
        va_start (args, format);
        std::vsnprintf (message, sizeof message, format, args);
        va_end (args);
        std::fprintf (stderr, "[Dusk Studio/%s] %s\n", logTag.c_str(), message);
        std::fflush (stderr);
    }

    std::string logTag;
    std::string displayName;
    FirstFrameProbe probe;
    int pumpIntervalMs = 16;
    Unit unit;
    Callbacks callbacks;
    std::string lastFailure;
    std::unique_ptr<builtin::DafEditor> editor;
    std::vector<float> pushedValues;
    Geometry lastGeometry;
    std::uintptr_t embeddedParent = 0;
    double appliedScaleFactor = 1.0;
    bool pushAllValues = true;
    bool firstFrameConfirmed = false;
    bool armedMarker = false;
    bool closeRequested = false;
    bool closeWasPumped = false;
};

DafEditorHost::DafEditorHost (std::string logTag, std::string displayName,
                              std::filesystem::path firstFrameMarker, int pumpIntervalMs)
    : impl (std::make_unique<Impl> (std::move (logTag), std::move (displayName),
                                    std::move (firstFrameMarker), pumpIntervalMs))
{
}

DafEditorHost::~DafEditorHost() = default;

void DafEditorHost::setUnit (Unit unit) { impl->unit = std::move (unit); }

void DafEditorHost::setCallbacks (Callbacks callbacks)
{
    impl->callbacks = std::move (callbacks);
}

bool DafEditorHost::open (std::uintptr_t nativeParent, Geometry geometry)
{
    return impl->open (nativeParent, geometry);
}

const std::string& DafEditorHost::lastOpenFailure() const noexcept
{
    return impl->lastFailure;
}

void DafEditorHost::setGeometry (Geometry geometry) { impl->setGeometry (geometry); }

void DafEditorHost::close() { impl->close(); }

bool DafEditorHost::isOpen() const noexcept { return impl->isOpen(); }

std::uintptr_t DafEditorHost::nativeWindow() const noexcept
{
    return impl->editor != nullptr && ! impl->closeRequested ? impl->editor->nativeWindow() : 0;
}

bool DafEditorHost::hasRenderedFrame() const noexcept { return impl->firstFrameConfirmed; }

void DafEditorHost::tick() { impl->tick(); }
} // namespace duskstudio::imgui
