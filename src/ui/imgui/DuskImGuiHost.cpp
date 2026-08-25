#include "DuskImGuiHost.h"
#include "FirstFrameProbe.h"
#include "../../foundation/MessageThread.h"

#if defined (_WIN32)
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#endif

#include <OpenGL.hpp>

#include <cstdarg>
#include <cstdio>
#include <utility>

namespace duskstudio::imgui
{
namespace
{
const char* glString (unsigned int name)
{
    const auto* const value = reinterpret_cast<const char*> (glGetString (name));
    return value != nullptr ? value : "(null)";
}
} // namespace

struct DuskImGuiHost::Impl final : private dusk::Timer
{
    class EmbeddedApplication final : public DGL::Application
    {
    public:
        explicit EmbeddedApplication (const std::string& className)
            : DGL::Application (false)
        {
            setClassName (className.c_str());
        }
    };

    Impl (DuskImGuiHost& ownerRef, Identity id, std::filesystem::path marker)
        : owner (ownerRef), app (id.className), identity (std::move (id)),
          probe (std::move (marker))
    {
    }

    ~Impl() override
    {
        stopTimer();
        destroyEmbeddedWindow();
        // The marker means "armed a frame and never came back", so only a run
        // that armed one may clear it, and only if that is no longer what it
        // says. A completed frame settles it whatever happened afterwards, and
        // removing it here also covers a disarm that failed silently at the
        // time. Without a frame, a pump that failed keeps its marker: that is
        // the refusal the next launch has to see.
        if (armedMarker && (firstFrameConfirmed || ! graphicsFailed))
            probe.disarm();
    }

    bool open (std::uintptr_t nativeParent, Geometry geometry)
    {
        // Cleared before the first return so neither outlives the attempt it
        // describes.
        lastFailure.clear();
        armedMarker = false;
        stopTimer();
        destroyEmbeddedWindow();
        if (nativeParent == 0 || geometry.width < 2 || geometry.height < 2)
            return false;

        closeRequested = false;
        closeWasPumped = false;

       #if defined (DUSKSTUDIO_USE_WINDOWS_SOFTWARE_OPENGL)
        if (probe.clearLegacyPreviousFailure())
            owner.log ("discarded a first-frame marker written before the packaged renderer fix");
       #endif
        if (const auto failed = probe.previousFailure(); ! failed.empty())
        {
            const auto marker = probe.path().string();
            owner.log ("a previous run ended while the window was drawing its "
                       "first frame on %s; unavailable. Delete %s to try again",
                       failed.c_str(), marker.c_str());
            lastFailure = identity.displayName
                        + " off: a previous run ended while it drew its first frame on "
                        + failed + ". Delete " + marker + " to try again.";
            return false;
        }
        firstFrameConfirmed = false;
        graphicsFailed = false;

        try
        {
            window = std::make_unique<DGL::Window> (
                app, nativeParent, geometry.width, geometry.height, geometry.scaleFactor, false);
            // A display without a usable GL configuration leaves Pugl with an
            // unrealised view: no native handle, and a size hint that never took.
            if (window->getNativeWindowHandle() == 0
                || window->getWidth() < 2 || window->getHeight() < 2)
            {
                window.reset();
                return false;
            }

            // Dusk Studio owns both native windows, so it also owns the child's
            // in-parent placement. The explicit framework embed API leaves normal
            // plugin windows under host control.
            // Native Wayland cannot embed a surface owned by the framework's display
            // connection into the legacy shell's surface. Refuse that backend
            // instead of silently falling back to the separate top-level window
            // that Pugl otherwise creates for an unsupported parent.
            if (! window->setEmbeddedOffset (geometry.x, geometry.y))
            {
                window.reset();
                return false;
            }

            std::string renderer;
            std::string refusal;
            {
                DGL::Window::ScopedGraphicsContext context (*window);
                logGraphicsIdentity();
                renderer = glString (GL_RENDERER);
                if (callbacks.checkGraphics)
                    refusal = callbacks.checkGraphics (glString (GL_VERSION), renderer.c_str());
                if (refusal.empty() && callbacks.createWidget)
                {
                    widget = callbacks.createWidget (*window);
                    if (widget != nullptr)
                        // DGL sizes a top-level widget from a resize event. Window creation
                        // and resize are one synchronous operation on macOS, so that event
                        // has already been delivered by the time this widget exists and it
                        // would keep a 0x0 size forever, laying the view out into nothing.
                        // Apply the window's size the way DGL's own resize path does;
                        // TopLevelWidget::setSize only forwards to the window, which is
                        // already this size and so emits no event.
                        static_cast<DGL::Widget*> (widget.get())
                            ->setSize (window->getWidth(), window->getHeight());
                }
            }

            if (! refusal.empty() || widget == nullptr)
            {
                lastFailure = std::move (refusal);
                destroyEmbeddedWindow();
                return false;
            }

            window->focus();
            probe.arm (renderer);
            armedMarker = true;
        }
        catch (const std::exception& error)
        {
            // A display that cannot back the child with the context the framework
            // asks for throws out of the constructor. open() runs inside a native
            // window procedure, where an escaping C++ exception is a fatal
            // callback exception that kills the process instead of something
            // the caller can act on, so the failure has to be converted here
            // into the false return the caller already handles.
            owner.log ("cannot embed: %s", error.what());
            lastFailure = identity.displayName + " unavailable: cannot embed ("
                        + error.what() + ").";
            destroyEmbeddedWindowSafely();
            return false;
        }

        startTimer (16);
        return true;
    }

    void close()
    {
        if (window == nullptr)
            return;
        closeRequested = true;
        closeWasPumped = false;
    }

    bool isOpen() const noexcept { return window != nullptr || closeRequested; }

    void setGeometry (Geometry geometry)
    {
        if (window == nullptr || geometry.width < 2 || geometry.height < 2)
            return;
        window->setSize (geometry.width, geometry.height);
        window->setEmbeddedOffset (geometry.x, geometry.y);
    }

    void timerCallback() override
    {
        if (! pumpEvents())
            return;
        // Close requests come from the native host boundary. Wait until the
        // framework returns from the event pump before destroying its embedded
        // widget and native child.
        if (closeRequested && window != nullptr)
        {
            destroyEmbeddedWindow();
            closeWasPumped = true;
            return;
        }

        if (closeRequested && closeWasPumped)
        {
            // The destroy happened on the previous tick and returned, so the
            // pump at the top of this one is the tick the platform needed to
            // finish unmapping the child before focus returns to the DAW.
            closeRequested = false;
            closeWasPumped = false;
            stopTimer();
            if (callbacks.closed)
                callbacks.closed();
        }
    }

    // A graphics driver that fails a call from inside the event pump can throw
    // out of it. Letting that reach the host's message loop takes the whole
    // application down with the view. The view closes instead, and the caller
    // keeps whatever state it mirrored out as it changed.
    bool pumpEvents()
    {
        try
        {
            app.idle();
            if (! firstFrameConfirmed)
            {
                firstFrameConfirmed = true;
                probe.disarm();
            }
            return true;
        }
        catch (const std::exception& error)
        {
            owner.log ("graphics driver failed during the event pump: %s", error.what());
        }
        catch (...)
        {
            owner.log ("graphics driver failed during the event pump");
        }

        graphicsFailed = true;
        stopTimer();
        destroyEmbeddedWindowSafely();
        closeRequested = false;
        closeWasPumped = false;
        if (callbacks.closed)
            callbacks.closed();
        return false;
    }

    // Both failure paths run because the driver already failed once, and the
    // ordinary teardown makes the context current again to release the widget.
    // A second failure there must not escape into the host's message loop, so
    // the last resort drops the objects and whatever the atlas owned.
    void destroyEmbeddedWindowSafely()
    {
        try
        {
            destroyEmbeddedWindow();
        }
        catch (...)
        {
            widget.reset();
            window.reset();
            if (callbacks.widgetReleased)
                callbacks.widgetReleased();
        }
    }

    void destroyEmbeddedWindow()
    {
        if (window == nullptr)
            return;

        if (widget != nullptr)
        {
            DGL::Window::ScopedGraphicsContext context (*window);
            widget.reset();
            if (callbacks.widgetReleased)
                callbacks.widgetReleased();
        }
        window.reset();
    }

    void logGraphicsIdentity() const
    {
        GLint maxTexture = 0;
        glGetIntegerv (GL_MAX_TEXTURE_SIZE, &maxTexture);
        owner.log ("GL vendor=%s renderer=%s version=%s glsl=%s maxTexture=%d",
                   glString (GL_VENDOR), glString (GL_RENDERER), glString (GL_VERSION),
                   glString (GL_SHADING_LANGUAGE_VERSION), (int) maxTexture);
    }

    DuskImGuiHost& owner;
    EmbeddedApplication app;
    Identity identity;
    FirstFrameProbe probe;
    Callbacks callbacks;
    std::string lastFailure;
    std::unique_ptr<DGL::Window> window;
    std::unique_ptr<DGL::TopLevelWidget> widget;
    bool firstFrameConfirmed = false;
    bool graphicsFailed = false;
    bool armedMarker = false;
    bool closeRequested = false;
    bool closeWasPumped = false;
};

DuskImGuiHost::DuskImGuiHost (Identity identity, std::filesystem::path firstFrameMarker)
    : impl (std::make_unique<Impl> (*this, std::move (identity), std::move (firstFrameMarker)))
{
}

DuskImGuiHost::~DuskImGuiHost() = default;

void DuskImGuiHost::setCallbacks (Callbacks callbacks)
{
    impl->callbacks = std::move (callbacks);
}

bool DuskImGuiHost::open (std::uintptr_t nativeParent, Geometry geometry)
{
    return impl->open (nativeParent, geometry);
}

const std::string& DuskImGuiHost::lastOpenFailure() const noexcept
{
    return impl->lastFailure;
}

void DuskImGuiHost::setGeometry (Geometry geometry)
{
    impl->setGeometry (geometry);
}

void DuskImGuiHost::close()
{
    impl->close();
}

bool DuskImGuiHost::isOpen() const noexcept
{
    return impl->isOpen();
}

DGL::Window* DuskImGuiHost::window() const noexcept
{
    return impl->window.get();
}

void DuskImGuiHost::log (const char* format, ...) const
{
    char message[512] {};
    va_list args;
    va_start (args, format);
    std::vsnprintf (message, sizeof message, format, args);
    va_end (args);

    std::fprintf (stderr, "[Dusk Studio/%s] %s\n", impl->identity.logTag.c_str(), message);
    // setvbuf only binds if nothing has written to the stream yet, and static
    // initialisation order across translation units is unspecified, so the
    // unbuffered mode set in Main.cpp cannot be assumed here.
    std::fflush (stderr);
   #if defined (_WIN32)
    char forDebugger[560] {};
    std::snprintf (forDebugger, sizeof forDebugger, "[Dusk Studio/%s] %s\n",
                   impl->identity.logTag.c_str(), message);
    ::OutputDebugStringA (forDebugger);
   #endif
}
} // namespace duskstudio::imgui
