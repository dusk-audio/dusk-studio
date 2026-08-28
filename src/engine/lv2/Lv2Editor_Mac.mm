#include "Lv2Editor.h"
#include "Lv2Instance.h"

#import <AppKit/AppKit.h>

#include <lilv/lilv.h>
#include <suil/suil.h>
#include <lv2/data-access/data-access.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/ui/ui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace duskstudio::lv2
{
struct Lv2Editor::Impl
{
    Lv2Editor*   owner    = nullptr;
    Lv2Instance* instance = nullptr;

    std::string uiUri, uiTypeUri, uiBundlePath, uiBinaryPath, pluginUri;

    SuilHost*     suilHost     = nullptr;
    SuilInstance* suilInstance = nullptr;

    NSView* container = nil;
    NSView* uiView    = nil;

    const LV2UI_Idle_Interface* idleIface = nullptr;

    LV2UI_Resize               resizeData {};
    LV2_Extension_Data_Feature extData {};
    LV2_Feature parentFeature {}, instanceFeature {}, dataFeature {},
                resizeFeature {}, idleFeature {};
    std::vector<const LV2_Feature*> features;

    int  prefW = 0, prefH = 0;
    bool discovered = false, embedded = false, visible = false, leakOnClose = false;
    std::vector<float> sentParameterValues;
    std::vector<uint8_t> parameterValueSent;

    static void writePort (SuilController controller, uint32_t portIndex,
                           uint32_t bufferSize, uint32_t protocol, const void* buffer)
    {
        auto* self = static_cast<Impl*> (controller);
        if (self->instance == nullptr) return;
        if (protocol == 0 && bufferSize == sizeof (float))
            self->instance->setControlPortValueFromUi (
                portIndex, *static_cast<const float*> (buffer));
        else if (protocol == self->instance->uiEventTransferUrid())
            self->instance->forwardUiAtomEvent (buffer, bufferSize);
    }

    static uint32_t portIndex (SuilController controller, const char* symbol)
    {
        auto* self = static_cast<Impl*> (controller);
        const int index = self->instance != nullptr
                            ? self->instance->portIndexForSymbol (symbol) : -1;
        return index >= 0 ? static_cast<uint32_t> (index) : LV2UI_INVALID_PORT_INDEX;
    }

    void syncParameterValues (bool force)
    {
        if (instance == nullptr || suilInstance == nullptr) return;
        instance->drainPatchFeedback();

        const int count = instance->uiParameterEventCount();
        if (sentParameterValues.size() != static_cast<size_t> (count))
        {
            sentParameterValues.assign (static_cast<size_t> (count), 0.0f);
            parameterValueSent.assign (static_cast<size_t> (count), 0);
            force = true;
        }

        Lv2Instance::UiParameterEvent event;
        for (int i = 0; i < count; ++i)
        {
            if (! instance->currentUiParameterEvent (i, event)) continue;
            if (! force && parameterValueSent[static_cast<size_t> (i)] != 0
                && std::memcmp (&sentParameterValues[static_cast<size_t> (i)], &event.value,
                                sizeof (event.value)) == 0)
                continue;

            suil_instance_port_event (suilInstance, event.portIndex,
                                      event.sizeBytes, event.protocol, event.data.data());
            sentParameterValues[static_cast<size_t> (i)] = event.value;
            parameterValueSent[static_cast<size_t> (i)] = 1;
        }
    }

    static int uiResize (LV2UI_Feature_Handle handle, int w, int h)
    {
        auto* self = static_cast<Impl*> (handle);
        if (self->instance == nullptr) return 1;
        if (w <= 0 || h <= 0) return 1;

        self->prefW = w;
        self->prefH = h;
        if (self->container != nil)
        {
            auto frame = [self->container frame];
            frame.size = NSMakeSize (static_cast<CGFloat> (w), static_cast<CGFloat> (h));
            [self->container setFrame:frame];
        }
        if (self->uiView != nil)
            [self->uiView setFrame:NSMakeRect (
                0.0, 0.0, static_cast<CGFloat> (w), static_cast<CGFloat> (h))];

        if (self->owner != nullptr && self->owner->onResize)
            self->owner->onResize (w, h);
        return 0;
    }
};

Lv2Editor::Lv2Editor() : impl (std::make_unique<Impl>()) { impl->owner = this; }
Lv2Editor::~Lv2Editor() { close(); }

void Lv2Editor::setLeakOnClose (bool b) noexcept
{
    if (impl != nullptr) impl->leakOnClose = b;
}

void Lv2Editor::abandonPlugin() noexcept
{
    if (impl != nullptr) impl->instance = nullptr;
}
int  Lv2Editor::preferredWidth()  const noexcept { return impl != nullptr ? impl->prefW : 0; }
int  Lv2Editor::preferredHeight() const noexcept { return impl != nullptr ? impl->prefH : 0; }
bool Lv2Editor::isOpen()     const noexcept { return impl != nullptr && impl->discovered; }
bool Lv2Editor::isEmbedded() const noexcept { return impl != nullptr && impl->embedded; }

bool Lv2Editor::open (Lv2Instance& inst, std::string& errorOut)
{
    const auto* plugin = static_cast<const LilvPlugin*> (inst.lilvPlugin());
    if (plugin == nullptr) { errorOut = "instance has no plugin"; return false; }

    impl->instance  = &inst;
    impl->pluginUri = lilv_node_as_uri (lilv_plugin_get_uri (plugin));

    LilvUIs* uis = lilv_plugin_get_uis (plugin);
    if (uis == nullptr || lilv_uis_size (uis) == 0)
    {
        lilv_uis_free (uis);
        errorOut = "plugin has no UI";
        return false;
    }

    auto* world = static_cast<LilvWorld*> (inst.lilvWorld());
    LilvNode* cocoaContainer = lilv_new_uri (world, LV2_UI__CocoaUI);

    unsigned bestQuality = 0;
    LILV_FOREACH (uis, it, uis)
    {
        const LilvUI* ui = lilv_uis_get (uis, it);
        const LilvNode* uiType = nullptr;
        const unsigned quality =
            lilv_ui_is_supported (ui, suil_ui_supported, cocoaContainer, &uiType);
        if (quality == 0 || (bestQuality != 0 && quality >= bestQuality))
            continue;

        bestQuality = quality;
        impl->uiUri = lilv_node_as_uri (lilv_ui_get_uri (ui));
        impl->uiTypeUri = uiType != nullptr ? lilv_node_as_uri (uiType) : LV2_UI__CocoaUI;

        char* bundle =
            lilv_file_uri_parse (lilv_node_as_uri (lilv_ui_get_bundle_uri (ui)), nullptr);
        char* binary =
            lilv_file_uri_parse (lilv_node_as_uri (lilv_ui_get_binary_uri (ui)), nullptr);
        impl->uiBundlePath = bundle != nullptr ? bundle : "";
        impl->uiBinaryPath = binary != nullptr ? binary : "";
        lilv_free (bundle);
        lilv_free (binary);
    }

    lilv_node_free (cocoaContainer);
    lilv_uis_free (uis);

    if (bestQuality == 0)
    {
        errorOut = "plugin has no Cocoa-embeddable UI";
        return false;
    }

    impl->discovered = true;
    return true;
}

bool Lv2Editor::embed (std::uintptr_t parentHandle, int x, int y, int w, int h,
                       std::string& errorOut)
{
    if (! impl->discovered) { errorOut = "no UI discovered"; return false; }
    if (impl->embedded) return true;
    if (parentHandle == 0) { errorOut = "null parent view"; return false; }

    auto* parent = reinterpret_cast<NSView*> (parentHandle);
    const int ww = w > 0 ? w : (impl->prefW > 0 ? impl->prefW : 480);
    const int hh = h > 0 ? h : (impl->prefH > 0 ? impl->prefH : 320);

    impl->container = [[NSView alloc] initWithFrame:NSMakeRect (
        static_cast<CGFloat> (x), static_cast<CGFloat> (y),
        static_cast<CGFloat> (ww), static_cast<CGFloat> (hh))];
    if (impl->container == nil)
    {
        errorOut = "could not create Cocoa container";
        return false;
    }
    [impl->container setAutoresizingMask:NSViewNotSizable];
    [impl->container setHidden:YES];
    [parent addSubview:impl->container];

    impl->suilHost = suil_host_new (&Impl::writePort, &Impl::portIndex, nullptr, nullptr);
    if (impl->suilHost == nullptr)
    {
        errorOut = "suil_host_new failed";
        close();
        return false;
    }

    impl->resizeData = { impl.get(), &Impl::uiResize };
    auto* lilvInst = static_cast<LilvInstance*> (impl->instance->lilvInstance());
    impl->extData = {
        lilvInst != nullptr ? lilv_instance_get_descriptor (lilvInst)->extension_data
                            : nullptr
    };
    impl->parentFeature   = { LV2_UI__parent,          impl->container };
    impl->instanceFeature = { LV2_INSTANCE_ACCESS_URI, lilvInst != nullptr
                                                         ? lilv_instance_get_handle (lilvInst)
                                                         : nullptr };
    impl->dataFeature     = { LV2_DATA_ACCESS_URI,     &impl->extData };
    impl->resizeFeature   = { LV2_UI__resize,          &impl->resizeData };
    impl->idleFeature     = { LV2_UI__idleInterface,   nullptr };
    impl->features = {
        static_cast<const LV2_Feature*> (impl->instance->uridMapFeature()),
        static_cast<const LV2_Feature*> (impl->instance->uridUnmapFeature()),
        &impl->parentFeature, &impl->instanceFeature, &impl->dataFeature,
        &impl->resizeFeature, &impl->idleFeature, nullptr
    };

    impl->suilInstance = suil_instance_new (
        impl->suilHost, impl.get(), LV2_UI__CocoaUI, impl->pluginUri.c_str(),
        impl->uiUri.c_str(), impl->uiTypeUri.c_str(), impl->uiBundlePath.c_str(),
        impl->uiBinaryPath.c_str(), impl->features.data());
    if (impl->suilInstance == nullptr)
    {
        errorOut = "suil_instance_new failed";
        close();
        return false;
    }

    impl->uiView = static_cast<NSView*> (
        suil_instance_get_widget (impl->suilInstance));
    if (impl->uiView == nil)
    {
        errorOut = "UI produced no Cocoa view";
        close();
        return false;
    }

    impl->instance->requestPatchParameterValuesForUi();
    impl->syncParameterValues (true);

    if ([impl->uiView superview] != impl->container)
        [impl->container addSubview:impl->uiView];

    const auto widgetFrame = [impl->uiView frame];
    if (impl->prefW <= 0)
    {
        impl->prefW = static_cast<int> (std::lround (NSWidth (widgetFrame)));
        impl->prefH = static_cast<int> (std::lround (NSHeight (widgetFrame)));
    }

    const int viewW = impl->prefW > 0 ? impl->prefW : ww;
    const int viewH = impl->prefH > 0 ? impl->prefH : hh;
    [impl->uiView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [impl->uiView setFrame:NSMakeRect (
        0.0, 0.0, static_cast<CGFloat> (viewW), static_cast<CGFloat> (viewH))];

    impl->idleIface = static_cast<const LV2UI_Idle_Interface*> (
        suil_instance_extension_data (impl->suilInstance, LV2_UI__idleInterface));

    impl->embedded = true;
    return true;
}

void Lv2Editor::setBounds (int x, int y, int w, int h)
{
    if (impl->container == nil) return;

    const int ww = std::max (1, w);
    const int hh = std::max (1, h);
    [impl->container setFrame:NSMakeRect (
        static_cast<CGFloat> (x), static_cast<CGFloat> (y),
        static_cast<CGFloat> (ww), static_cast<CGFloat> (hh))];
    if (impl->uiView != nil)
        [impl->uiView setFrame:NSMakeRect (
            0.0, 0.0, static_cast<CGFloat> (ww), static_cast<CGFloat> (hh))];
}

void Lv2Editor::reveal()
{
    if (impl->container == nil || impl->visible) return;
    [impl->container setHidden:NO];
    impl->visible = true;
}

void Lv2Editor::hide()
{
    if (impl->container == nil || ! impl->visible) return;
    [impl->container setHidden:YES];
    impl->visible = false;
}

void Lv2Editor::quiesce() noexcept
{
    if (impl == nullptr) return;
    // setHidden: propagates viewDidHide through the plugin subtree, so this is
    // deliberately a live-instance phase rather than stale-owner cleanup.
    @try
    {
        if (impl->container != nil && impl->visible)
            [impl->container setHidden:YES];
    }
    @catch (NSException*)
    {
    }
    impl->visible = false;
}

void Lv2Editor::abandonPluginAndContainer() noexcept
{
    if (impl == nullptr) return;
    // suil retains Impl as both its controller and resize feature handle. The
    // instance is already disposed, so no suil call or Cocoa hierarchy message
    // is safe: strand the complete callback state for the life of the process
    // and leave close() a true no-op in the allocation-free terminal state.
    impl->owner    = nullptr;
    impl->instance = nullptr;
    (void) impl.release();
}

void Lv2Editor::close()
{
    if (impl == nullptr) return;
    if (impl->leakOnClose && impl->suilInstance != nullptr)
    {
        // Shutdown path: the suil instance is deliberately leaked because a
        // foreign-toolkit UI can hang in its own teardown. That leaked UI still
        // holds this Impl as its suil controller and ui:resize handle, so the
        // Impl has to outlive us too - a UI ticking its own NSTimer would
        // otherwise write ports through freed memory. The DSP instance is NOT
        // leaked (the slot may unload it right after), so the pointer to it
        // goes first.
        impl->owner    = nullptr;
        impl->instance = nullptr;
        static auto* leakedAtShutdown = new std::vector<std::unique_ptr<Impl>>;
        if (impl->container != nil)
            [impl->container removeFromSuperview];
        leakedAtShutdown->push_back (std::move (impl));
        impl = std::make_unique<Impl>();
        impl->owner = this;
        return;
    }

    if (impl->uiView != nil)
    {
        [impl->uiView removeFromSuperview];
        impl->uiView = nil;
    }

    if (! impl->leakOnClose)
    {
        if (impl->suilInstance != nullptr) suil_instance_free (impl->suilInstance);
        if (impl->suilHost != nullptr)     suil_host_free (impl->suilHost);
    }
    impl->suilInstance = nullptr;
    impl->suilHost     = nullptr;
    impl->idleIface    = nullptr;
    impl->sentParameterValues.clear();
    impl->parameterValueSent.clear();

    if (impl->container != nil)
    {
        [impl->container removeFromSuperview];
        [impl->container release];
        impl->container = nil;
    }

    impl->instance = nullptr;
    impl->discovered = impl->embedded = impl->visible = false;
    impl->prefW = impl->prefH = 0;
}

bool Lv2Editor::getRootRelativePosition (std::uintptr_t, int&, int&) const
{
    return false;
}

bool Lv2Editor::getActualGeometry (int&, int&, int&, int&) const
{
    return false;
}

void Lv2Editor::pump()
{
    impl->syncParameterValues (false);

    if (impl->embedded && impl->idleIface != nullptr && impl->suilInstance != nullptr
        && impl->idleIface->idle (suil_instance_get_handle (impl->suilInstance)) != 0)
    {
        impl->idleIface = nullptr;
        if (onClosed) onClosed();
    }
}
} // namespace duskstudio::lv2
