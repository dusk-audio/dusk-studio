#include "DafEditorHost.h"
#include "DuskImGuiScale.h"
#include "../../foundation/AppConfigDir.h"
#include "../../foundation/MessageThread.h"
#include "PumpPacing.h"
#include "SharedWaylandApplication.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <exception>
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
    const auto config = dusk::fs::appConfigDir();
    if (config.empty())
        return {};
    return config / (logTag + "-first-frame");
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
        // The callbacks reach into the caller, which is already tearing down by
        // the time this runs, and neither means anything to it now.
        callbacks = {};
        shutdown();
    }

    void destroyEditor()
    {
        tearingDown = true;
        handedCallbacks = {};
       #if DUSKSTUDIO_HAS_SHARED_WAYLAND
        sharedRegistration.reset();
        if (editor != nullptr && sharedApplication != nullptr)
            sharedApplication->forgetSurface (editor->nativeWindow());
       #endif
        editor.reset();
       #if DUSKSTUDIO_HAS_SHARED_WAYLAND
        if (sharedApplication != nullptr)
            sharedApplication->commitParent (parentDescriptor);
       #endif
        turn = {};
        turnDepth = 0;
        turnFailed = false;
        tearingDown = false;
    }

    void clearSettledMarker()
    {
        if (armedMarker && (firstFrameConfirmed || ! graphicsFailed))
        {
            probe.disarm();
            armedMarker = false;
        }
    }

    void shutdown()
    {
        stopTimer();
        closeRequested = false;
        closeWasPumped = false;
        destroyEditor();
        clearSettledMarker();
       #if DUSKSTUDIO_HAS_SHARED_WAYLAND
        sharedApplication.reset();
       #endif
    }

    void finishClose (bool failed)
    {
        graphicsFailed = graphicsFailed || failed;
        shutdown();
        const auto closed = callbacks.closed;
        if (closed) closed();
    }

    bool open (std::uintptr_t nativeParent, Geometry geometry)
    {
        lastFailure.clear();
        shutdown();
        armedMarker = false;

        if (unit.createEditor == nullptr || nativeParent == 0
            || geometry.width < 2 || geometry.height < 2)
            return false;

       #if DUSKSTUDIO_HAS_SHARED_WAYLAND
        parentDescriptor = platform::describeEmbeddedParent (nativeParent);
        if (! platform::isEmbeddedParentAlive (parentDescriptor)) return false;
       #endif

        if (const auto failed = probe.previousFailure(); ! failed.empty())
        {
            const auto marker = probe.path().u8string();
            log ("a previous run ended while the editor drew its first frame on %s; "
                 "unavailable. Delete %s to try again", failed.c_str(), marker.c_str());
            lastFailure = displayName + " is off: a previous run ended while it drew its "
                          "first frame on " + failed + ". Delete " + marker
                        + " to try again.";
            return false;
        }
        firstFrameConfirmed = false;
        graphicsFailed = false;

        builtin::DafEditorCallbacks editorCallbacks;
        editorCallbacks.gesture = [this] (std::uint32_t index, bool started)
        {
            if (tearingDown) return;
            if (! turn.open)
            {
                deliver ({ false, started ? (int) index : -1, false, ! started });
                return;
            }
            if (started)
            {
                turn.several = turn.several || (turn.started >= 0 && turn.started != (int) index);
                turn.started = (int) index;
            }
            else
            {
                turn.ended = true;
            }
        };
        editorCallbacks.parameterEdited = [this] (std::uint32_t index, float value)
        {
            if (! tearingDown) applyEdit ((int) index, value);
        };

        std::string error;
        bool placed = false;
        try
        {
           #if DUSKSTUDIO_HAS_SHARED_WAYLAND
            if (parentDescriptor.backend == platform::NativeEmbeddedParent::Backend::wayland)
            {
                sharedApplication = SharedWaylandApplication::acquire (parentDescriptor);
                if (sharedApplication == nullptr)
                    error = "the native parent connection is unavailable.";
                else
                    editorCallbacks.hostApplication = std::shared_ptr<void> (
                        sharedApplication, &sharedApplication->application());
            }
           #endif
            handedCallbacks = editorCallbacks;
            handedCallbacks.hostApplication.reset();
            if (error.empty())
                editor = unit.createEditor (nativeParent, geometry.width, geometry.height,
                                            geometry.scaleFactor, std::move (editorCallbacks), error);
            if (editor != nullptr)
            {
               #if DUSKSTUDIO_HAS_SHARED_WAYLAND
                if (sharedApplication != nullptr)
                {
                    placed = editor->setEmbeddedGeometry (geometry.x, geometry.y, geometry.width,
                                                           geometry.height, geometry.scaleFactor)
                          && editor->setEmbeddedVisible (true)
                          && sharedApplication->commitParent (parentDescriptor);
                    if (placed)
                        sharedRegistration = sharedApplication->registerClient (parentDescriptor, {
                            [this] { beginTurn(); },
                            [this] (bool success) { confirmFrame (success); endTurn (success); },
                            [this] { finishClose (false); },
                            [this] (const char* message) {
                                log ("the shared editor event pump failed: %s", message);
                                finishClose (true);
                            }
                        });
                }
                else
               #endif
                    placed = editor->setOffset (geometry.x, geometry.y);
            }
        }
        catch (const std::exception& failure) { error = failure.what(); }
        catch (...) { error = "the editor could not be built."; }

        if (editor == nullptr || ! placed || ! error.empty())
        {
            lastFailure = editor != nullptr && error.empty()
                ? displayName + " cannot open on this display backend."
                : displayName + " cannot open: "
                  + (error.empty() ? std::string ("the editor could not be built.") : error);
            shutdown();
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
        editor->setEmbeddedVisible (false);
       #if DUSKSTUDIO_HAS_SHARED_WAYLAND
        if (sharedApplication != nullptr)
            sharedApplication->commitParent (parentDescriptor);
       #endif
    }

    bool isOpen() const noexcept { return editor != nullptr || closeRequested; }

    void setGeometry (Geometry geometry)
    {
        if (editor == nullptr || closeRequested
            || geometry.width < 2 || geometry.height < 2)
            return;

       #if DUSKSTUDIO_HAS_SHARED_WAYLAND
        if (! platform::isEmbeddedParentAlive (parentDescriptor))
        {
            close();
            return;
        }
        if (sharedApplication != nullptr)
        {
            if (! editor->setEmbeddedGeometry (geometry.x, geometry.y, geometry.width,
                                               geometry.height, geometry.scaleFactor)
                || ! sharedApplication->commitParent (parentDescriptor))
                close();
            else
                lastGeometry = geometry;
            return;
        }
       #endif

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

    void confirmFrame (bool success)
    {
        if (! success || editor == nullptr || firstFrameConfirmed) return;
       #if DUSKSTUDIO_HAS_SHARED_WAYLAND
        if (sharedApplication != nullptr && editor->completedDisplayCount() == 0) return;
       #endif
        firstFrameConfirmed = true;
        clearSettledMarker();
    }

    void tick()
    {
        if (closeRequested && editor != nullptr)
        {
            destroyEditor();
            clearSettledMarker();
            closeWasPumped = true;
            return;
        }
        if (closeRequested && closeWasPumped)
        {
            finishClose (false);
            return;
        }
        if (editor == nullptr) return;

       #if DUSKSTUDIO_HAS_SHARED_WAYLAND
        if (! platform::isEmbeddedParentAlive (parentDescriptor))
        {
            finishClose (false);
            return;
        }
        if (sharedApplication != nullptr)
        {
            const auto lifetime = sharedRegistration->lifetime();
            const auto shared = sharedApplication;
            const auto alive = shared->pump();
            if (lifetime.expired() || ! alive) return;
            if (closeRequested) return;
        }
       #endif

        beginTurn();
        const auto started = std::chrono::steady_clock::now();
        const bool alive = editor->idle();
        const auto finished = std::chrono::steady_clock::now();
        nextTickAt = nextPumpAfter (started, finished, pumpIntervalMs);
        confirmFrame (alive);
        endTurn (alive);
        if (! alive)
        {
            log ("the editor closed itself");
            finishClose (true);
            return;
        }
        if (editor == nullptr || closeRequested) return;

        pushChangedValues();
        followGeometry();
    }

    // Paced by nextPumpAfter: Sunset's editor on a software renderer takes about
    // 40 ms a frame, which unpaced would leave Escape and a click on the dim
    // unread. The wait is stamped in tick() before any callback runs, since a
    // closed callback may take this host down with it. A requested close is not
    // paced, so the teardown keeps its two prompt ticks.
    void timerCallback() override
    {
        if (! closeRequested && std::chrono::steady_clock::now() < nextTickAt)
            return;
        tick();
    }

    // The gestures of one turn of the editor's own loop. A hand works one control
    // at a time, so a turn that opens gestures on several parameters is a program
    // being loaded - a preset picked in the editor or a MIDI program change synced
    // into it, a gesture per parameter - and is neither a touch for MIDI Learn nor
    // a drag to hand the keyboard back from, which would take it from a preset
    // name being typed.
    struct Turn
    {
        bool open = false;
        int started = -1;
        bool several = false;
        bool ended = false;
    };

    void beginTurn()
    {
        if (turnDepth++ == 0)
        {
            turn = Turn { true };
            turnFailed = false;
        }
    }

    void endTurn (bool success)
    {
        if (turnDepth == 0) return;
        turnFailed = turnFailed || ! success;
        if (--turnDepth != 0) return;
        const auto worked = std::exchange (turn, Turn {});
        const auto failed = std::exchange (turnFailed, false);
        if (! failed) deliver (worked);
    }

    void deliver (const Turn& worked)
    {
        if (worked.several)
            return;
        if (worked.started >= 0 && unit.noteTouched)
            unit.noteTouched (worked.started);
        if (worked.ended && callbacks.gestureEnded)
            callbacks.gestureEnded();
    }

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
    std::chrono::steady_clock::time_point nextTickAt {};
    Unit unit;
    Callbacks callbacks;
    std::string lastFailure;
    std::unique_ptr<builtin::DafEditor> editor;
    // A copy of what the open editor was handed, so a scenario can work a
    // control through the same callbacks a drag in the editor calls.
    builtin::DafEditorCallbacks handedCallbacks;
    Turn turn;
    int turnDepth = 0;
    bool turnFailed = false;
    bool tearingDown = false;
   #if DUSKSTUDIO_HAS_SHARED_WAYLAND
    std::shared_ptr<SharedWaylandApplication> sharedApplication;
    std::unique_ptr<SharedWaylandApplication::Registration> sharedRegistration;
    platform::NativeEmbeddedParent parentDescriptor;
   #endif
    std::vector<float> pushedValues;
    Geometry lastGeometry;
    std::uintptr_t embeddedParent = 0;
    double appliedScaleFactor = 1.0;
    bool pushAllValues = true;
    bool firstFrameConfirmed = false;
    bool armedMarker = false;
    bool graphicsFailed = false;
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

void DafEditorHost::shutdown() { impl->shutdown(); }

bool DafEditorHost::isOpen() const noexcept { return impl->isOpen(); }

std::uintptr_t DafEditorHost::nativeWindow() const noexcept
{
    return impl->editor != nullptr && ! impl->closeRequested ? impl->editor->nativeWindow() : 0;
}

bool DafEditorHost::hasRenderedFrame() const noexcept
{
    return impl->editor != nullptr && ! impl->closeRequested && impl->firstFrameConfirmed;
}

void DafEditorHost::tick() { impl->tick(); }

bool DafEditorHost::editForScenario (int index, float value)
{
    const auto& handed = impl->handedCallbacks;
    if (! isOpen() || index < 0 || ! handed.gesture || ! handed.parameterEdited)
        return false;
    handed.gesture ((std::uint32_t) index, true);
    handed.parameterEdited ((std::uint32_t) index, value);
    handed.gesture ((std::uint32_t) index, false);
    return true;
}
} // namespace duskstudio::imgui
