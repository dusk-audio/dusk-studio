#include "PluginHostXErrors.h"

#if defined (__linux__)
 #include <atomic>
 #include <cstdio>

 // Last: it defines min and max as macros.
 #include <X11/Xlib.h>
 #include <X11/Xlibint.h>
#endif

namespace duskstudio::ipc::x11
{
#if defined (__linux__)
namespace
{
std::atomic<bool> editorAlive { false };
std::atomic<XErrorHandler> previousHandler { nullptr };

// Runs under Xlib's display lock, on whichever thread read the error, so it
// reads only the connection's own setup fields and makes no requests.
int staleWindowErrorHandler (Display* display, XErrorEvent* error)
{
    if (isStaleForeignWindowError (error->error_code,
                                   static_cast<std::uint64_t> (error->resourceid),
                                   static_cast<std::uint64_t> (display->resource_base),
                                   static_cast<std::uint64_t> (display->resource_mask),
                                   editorAlive.load (std::memory_order_acquire)))
    {
        std::fprintf (stderr,
                      "[dusk-studio-plugin-host/X] ignored stale host window 0x%lx "
                      "(error %u, request %u)\n",
                      static_cast<unsigned long> (error->resourceid),
                      static_cast<unsigned int> (error->error_code),
                      static_cast<unsigned int> (error->request_code));
        std::fflush (stderr);
        return 0;
    }

    return previousHandler.load (std::memory_order_acquire) (display, error);
}
} // namespace

void installStaleWindowErrorHandler()
{
    // With nothing installed yet, Xlib returns its own default here, never null.
    const auto previous = ::XSetErrorHandler (&staleWindowErrorHandler);
    if (previous != &staleWindowErrorHandler)
        previousHandler.store (previous, std::memory_order_release);
}

void setEditorAlive (bool alive) noexcept
{
    editorAlive.store (alive, std::memory_order_release);
}
#else
void installStaleWindowErrorHandler() {}
void setEditorAlive (bool) noexcept {}
#endif
} // namespace duskstudio::ipc::x11
