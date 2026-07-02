#include "Lv2Editor.h"
#include "Lv2Instance.h"

#include <lilv/lilv.h>
#include <suil/suil.h>
#include <lv2/data-access/data-access.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>

#include <X11/Xlib.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace duskstudio::lv2
{
struct Lv2Editor::Impl
{
    Lv2Editor*   owner    = nullptr;
    Lv2Instance* instance = nullptr;

    // Discovered UI (URIs + paths copied out of lilv at open()).
    std::string uiUri, uiTypeUri, uiBundlePath, uiBinaryPath, pluginUri;

    SuilHost*     suilHost     = nullptr;
    SuilInstance* suilInstance = nullptr;

    void*         display    = nullptr;   // Display*
    unsigned long hostWindow = 0;         // our container Window
    unsigned long uiWindow   = 0;         // suil widget's Window (child of hostWindow)

    // ui:idleInterface, queried from the instantiated UI. Optional.
    const LV2UI_Idle_Interface* idleIface = nullptr;

    // Feature payloads handed to suil_instance_new — must outlive the instance.
    LV2UI_Resize               resizeData {};
    LV2_Extension_Data_Feature extData {};
    LV2_Feature parentFeature {}, instanceFeature {}, dataFeature {},
                resizeFeature {}, idleFeature {};
    std::vector<const LV2_Feature*> features;

    int  prefW = 0, prefH = 0;
    bool discovered = false, embedded = false, mapped = false, leakOnClose = false;

    // ── suil host callbacks (message thread: the UI runs off our idle pump) ──
    static void writePort (SuilController c, uint32_t portIndex, uint32_t bufferSize,
                           uint32_t protocol, const void* buffer)
    {
        auto* self = static_cast<Impl*> (c);
        // ui:floatProtocol only — atom/event writes need a plugin-input atom
        // routing that isn't wired yet.
        if (protocol == 0 && bufferSize == sizeof (float) && self->instance != nullptr)
            self->instance->setControlPortValue (portIndex,
                                                 *static_cast<const float*> (buffer));
    }

    static uint32_t portIndex (SuilController c, const char* symbol)
    {
        auto* self = static_cast<Impl*> (c);
        const int idx = self->instance != nullptr
                          ? self->instance->portIndexForSymbol (symbol) : -1;
        return idx >= 0 ? (uint32_t) idx : LV2UI_INVALID_PORT_INDEX;
    }

    static int uiResize (LV2UI_Feature_Handle handle, int w, int h)
    {
        auto* self = static_cast<Impl*> (handle);
        if (w <= 0 || h <= 0) return 1;
        self->prefW = w; self->prefH = h;
        if (self->display != nullptr && self->hostWindow != 0)
        {
            XResizeWindow ((Display*) self->display, self->hostWindow,
                           (unsigned) w, (unsigned) h);
            XFlush ((Display*) self->display);
        }
        if (self->owner->onResize) self->owner->onResize (w, h);
        return 0;
    }
};

Lv2Editor::Lv2Editor() : impl (std::make_unique<Impl>()) { impl->owner = this; }
Lv2Editor::~Lv2Editor() { close(); }

void Lv2Editor::setLeakOnClose (bool b) noexcept { impl->leakOnClose = b; }
int  Lv2Editor::preferredWidth()  const noexcept { return impl->prefW; }
int  Lv2Editor::preferredHeight() const noexcept { return impl->prefH; }
bool Lv2Editor::isOpen()     const noexcept { return impl->discovered; }
bool Lv2Editor::isEmbedded() const noexcept { return impl->embedded; }

bool Lv2Editor::open (Lv2Instance& inst, std::string& errorOut)
{
    const auto* plugin = static_cast<const LilvPlugin*> (inst.lilvPlugin());
    if (plugin == nullptr) { errorOut = "instance has no plugin"; return false; }
    impl->instance  = &inst;
    impl->pluginUri = lilv_node_as_uri (lilv_plugin_get_uri (plugin));

    LilvUIs* uis = lilv_plugin_get_uis (plugin);
    if (uis == nullptr || lilv_uis_size (uis) == 0)
    { lilv_uis_free (uis); errorOut = "plugin has no UI"; return false; }

    // Pick the best-supported UI for an X11 container: 1 = native X11UI,
    // higher = wrappable by a suil module. Lower nonzero quality wins.
    auto* world = static_cast<LilvWorld*> (inst.lilvWorld());
    LilvNode* x11Container = lilv_new_uri (world, LV2_UI__X11UI);

    unsigned bestQuality = 0;
    LILV_FOREACH (uis, it, uis)
    {
        const LilvUI* ui = lilv_uis_get (uis, it);
        const LilvNode* uiType = nullptr;
        const unsigned q = lilv_ui_is_supported (ui, suil_ui_supported, x11Container, &uiType);
        if (q == 0) continue;
        if (bestQuality == 0 || q < bestQuality)
        {
            bestQuality = q;
            impl->uiUri     = lilv_node_as_uri (lilv_ui_get_uri (ui));
            impl->uiTypeUri = uiType != nullptr ? lilv_node_as_uri (uiType) : LV2_UI__X11UI;
            char* bundle = lilv_file_uri_parse (lilv_node_as_uri (lilv_ui_get_bundle_uri (ui)), nullptr);
            char* binary = lilv_file_uri_parse (lilv_node_as_uri (lilv_ui_get_binary_uri (ui)), nullptr);
            impl->uiBundlePath = bundle != nullptr ? bundle : "";
            impl->uiBinaryPath = binary != nullptr ? binary : "";
            lilv_free (bundle);
            lilv_free (binary);
        }
    }
    lilv_node_free (x11Container);
    lilv_uis_free (uis);

    if (bestQuality == 0)
    { errorOut = "plugin has no X11-embeddable UI"; return false; }

    impl->discovered = true;
    return true;
}

bool Lv2Editor::embed (unsigned long parentX11, int x, int y, int w, int h, std::string& errorOut)
{
    if (! impl->discovered) { errorOut = "no UI discovered"; return false; }
    if (impl->embedded) return true;

    auto* dpy = XOpenDisplay (nullptr);
    if (dpy == nullptr) { errorOut = "XOpenDisplay failed"; return false; }
    impl->display = dpy;

    const int ww = w > 0 ? w : 480;
    const int hh = h > 0 ? h : 320;

    // Solid background (not None) so the map fills with black instead of stale
    // framebuffer — same rationale as ClapEditor::embed.
    XSetWindowAttributes swa {};
    swa.background_pixel = BlackPixel (dpy, DefaultScreen (dpy));
    swa.border_pixel     = 0;
    swa.event_mask       = StructureNotifyMask;
    impl->hostWindow = XCreateWindow (dpy, (Window) parentX11, x, y,
                                      (unsigned) ww, (unsigned) hh, 0,
                                      CopyFromParent, InputOutput, CopyFromParent,
                                      CWBackPixel | CWBorderPixel | CWEventMask, &swa);
    // Viewable BEFORE the UI instantiates into it — toolkit wrappers (and JUCE-
    // wrapped UIs) can abort realising into an unmapped parent.
    XMapWindow (dpy, impl->hostWindow);
    XSync (dpy, False);

    if (impl->suilHost == nullptr)
        impl->suilHost = suil_host_new (&Impl::writePort, &Impl::portIndex, nullptr, nullptr);

    // Features: the instance's URID map/unmap (shared URID space), the parent
    // window, instance/data access for UIs that reach into their DSP side, and
    // our ui:resize handler. idleInterface is declared so UIs can rely on it.
    impl->resizeData = { impl.get(), &Impl::uiResize };
    auto* lilvInst = static_cast<LilvInstance*> (impl->instance->lilvInstance());
    impl->extData  = { lilvInst != nullptr ? lilv_instance_get_descriptor (lilvInst)->extension_data
                                           : nullptr };
    impl->parentFeature   = { LV2_UI__parent,          (void*) (uintptr_t) impl->hostWindow };
    impl->instanceFeature = { LV2_INSTANCE_ACCESS_URI, lilvInst != nullptr
                                                         ? lilv_instance_get_handle (lilvInst) : nullptr };
    impl->dataFeature     = { LV2_DATA_ACCESS_URI,     &impl->extData };
    impl->resizeFeature   = { LV2_UI__resize,          &impl->resizeData };
    impl->idleFeature     = { LV2_UI__idleInterface,   nullptr };
    impl->features = {
        static_cast<const LV2_Feature*> (impl->instance->uridMapFeature()),
        static_cast<const LV2_Feature*> (impl->instance->uridUnmapFeature()),
        &impl->parentFeature, &impl->instanceFeature, &impl->dataFeature,
        &impl->resizeFeature, &impl->idleFeature, nullptr };

    impl->suilInstance = suil_instance_new (impl->suilHost, impl.get(), LV2_UI__X11UI,
                                            impl->pluginUri.c_str(),
                                            impl->uiUri.c_str(), impl->uiTypeUri.c_str(),
                                            impl->uiBundlePath.c_str(),
                                            impl->uiBinaryPath.c_str(),
                                            impl->features.data());
    if (impl->suilInstance == nullptr)
    { errorOut = "suil_instance_new failed"; close(); return false; }

    impl->uiWindow = (unsigned long) (uintptr_t) suil_instance_get_widget (impl->suilInstance);
    if (impl->uiWindow == 0)
    { errorOut = "UI produced no widget"; close(); return false; }

    impl->idleIface = static_cast<const LV2UI_Idle_Interface*> (
        suil_instance_extension_data (impl->suilInstance, LV2_UI__idleInterface));

    // Size: the UI's own window tells us its preferred size unless ui:resize
    // already did; fit the widget to our host area either way.
    XWindowAttributes attrs {};
    if (impl->prefW <= 0 && XGetWindowAttributes (dpy, (Window) impl->uiWindow, &attrs) != 0)
    { impl->prefW = attrs.width; impl->prefH = attrs.height; }

    // Belt-and-braces: most UIs parent themselves under ui:parent, but the spec
    // doesn't force it — reparent explicitly (a no-op when already a child).
    XReparentWindow (dpy, (Window) impl->uiWindow, (Window) impl->hostWindow, 0, 0);

    XMapWindow (dpy, (Window) impl->uiWindow);
    XSync (dpy, False);

    impl->embedded = true;
    impl->mapped   = true;
    return true;
}

void Lv2Editor::setBounds (int x, int y, int w, int h)
{
    if (impl->display == nullptr || impl->hostWindow == 0) return;
    auto* dpy = (Display*) impl->display;
    XMoveResizeWindow (dpy, (Window) impl->hostWindow, x, y,
                       (unsigned) std::max (1, w), (unsigned) std::max (1, h));
    if (impl->uiWindow != 0)
        XResizeWindow (dpy, (Window) impl->uiWindow,
                       (unsigned) std::max (1, w), (unsigned) std::max (1, h));
    XFlush (dpy);
}

void Lv2Editor::reveal()
{
    if (impl->display == nullptr || impl->hostWindow == 0 || impl->mapped) return;
    XMapWindow ((Display*) impl->display, (Window) impl->hostWindow);
    XFlush ((Display*) impl->display);
    impl->mapped = true;
}

void Lv2Editor::hide()
{
    if (impl->display == nullptr || impl->hostWindow == 0 || ! impl->mapped) return;
    XUnmapWindow ((Display*) impl->display, (Window) impl->hostWindow);
    XFlush ((Display*) impl->display);
    impl->mapped = false;
}

void Lv2Editor::close()
{
    if (! impl->leakOnClose)
    {
        if (impl->suilInstance != nullptr) suil_instance_free (impl->suilInstance);
        if (impl->suilHost != nullptr)     suil_host_free (impl->suilHost);
    }
    impl->suilInstance = nullptr;
    impl->suilHost     = nullptr;
    impl->idleIface    = nullptr;

    if (impl->display != nullptr)
    {
        if (impl->hostWindow != 0)
            XDestroyWindow ((Display*) impl->display, (Window) impl->hostWindow);
        // Flush the destroy against a still-valid parent peer before dropping the
        // connection (see ClapEditor::close).
        XSync ((Display*) impl->display, False);
        XCloseDisplay ((Display*) impl->display);
        impl->display = nullptr;
    }
    impl->hostWindow = impl->uiWindow = 0;
    impl->discovered = impl->embedded = impl->mapped = false;
}

void Lv2Editor::pump()
{
    if (impl->embedded && impl->idleIface != nullptr && impl->suilInstance != nullptr)
    {
        // Non-zero: the UI wants to close (spec: "non-zero if the UI has been
        // closed"). Report once; the owner tears us down.
        if (impl->idleIface->idle (suil_instance_get_handle (impl->suilInstance)) != 0)
        {
            impl->idleIface = nullptr;
            if (onClosed) onClosed();
            return;
        }
    }

    if (impl->display != nullptr)
    {
        auto* dpy = (Display*) impl->display;
        while (XPending (dpy) > 0)
        {
            XEvent e;
            XNextEvent (dpy, &e);
        }
    }
}
} // namespace duskstudio::lv2
