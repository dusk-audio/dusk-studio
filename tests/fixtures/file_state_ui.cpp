#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/core/lv2.h>
#include <lv2/patch/patch.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>

#include <X11/Xlib.h>

#include <cstdint>
#include <cstring>
#include <new>

namespace
{
constexpr const char* kUiUri = "urn:duskstudio:test:file-state#ui";
// The property the UI announces on open. Not the first of the plug-in's
// parameters, so a host that reports index 0 for everything fails here.
constexpr const char* kTouchedProperty = "urn:duskstudio:test:file-state#mu";
constexpr float kTouchedValue = 0.625f;
constexpr uint32_t kControlInPort = 5;

template <typename T>
T* feature (const LV2_Feature* const* features, const char* uri)
{
    if (features == nullptr) return nullptr;
    for (size_t i = 0; features[i] != nullptr; ++i)
        if (std::strcmp (features[i]->URI, uri) == 0)
            return static_cast<T*> (features[i]->data);
    return nullptr;
}

struct Ui
{
    Display* display = nullptr;
    Window window = 0;
};

// What a JUCE-built LV2 UI does the moment it comes up: push its parameters to
// the host as patch:Set messages. One property is enough to prove the host
// stamps the touch and can name the parameter afterwards.
void announceProperty (LV2_URID_Map* map, LV2UI_Write_Function write,
                       LV2UI_Controller controller)
{
    uint8_t buffer[128];
    LV2_Atom_Forge forge;
    lv2_atom_forge_init (&forge, map);
    lv2_atom_forge_set_buffer (&forge, buffer, sizeof (buffer));

    LV2_Atom_Forge_Frame frame;
    if (lv2_atom_forge_object (&forge, &frame, 0,
                               map->map (map->handle, LV2_PATCH__Set)) == 0)
        return;
    lv2_atom_forge_key (&forge, map->map (map->handle, LV2_PATCH__property));
    lv2_atom_forge_urid (&forge, map->map (map->handle, kTouchedProperty));
    lv2_atom_forge_key (&forge, map->map (map->handle, LV2_PATCH__value));
    lv2_atom_forge_float (&forge, kTouchedValue);
    lv2_atom_forge_pop (&forge, &frame);

    const auto* atom = reinterpret_cast<const LV2_Atom*> (buffer);
    write (controller, kControlInPort, (uint32_t) sizeof (LV2_Atom) + atom->size,
           map->map (map->handle, LV2_ATOM__eventTransfer), buffer);
}

LV2UI_Handle instantiate (const LV2UI_Descriptor*, const char*, const char*,
                          LV2UI_Write_Function write, LV2UI_Controller controller,
                          LV2UI_Widget* widget, const LV2_Feature* const* features)
{
    if (write == nullptr || widget == nullptr) return nullptr;
    auto* map = feature<LV2_URID_Map> (features, LV2_URID__map);
    auto* parent = feature<void> (features, LV2_UI__parent);
    if (map == nullptr || parent == nullptr) return nullptr;

    auto* display = XOpenDisplay (nullptr);
    if (display == nullptr) return nullptr;

    auto* self = new (std::nothrow) Ui;
    if (self == nullptr) { XCloseDisplay (display); return nullptr; }
    self->display = display;
    self->window = XCreateSimpleWindow (display, (Window) (uintptr_t) parent, 0, 0, 160, 80, 0,
                                        BlackPixel (display, DefaultScreen (display)),
                                        BlackPixel (display, DefaultScreen (display)));
    XFlush (display);

    *widget = (LV2UI_Widget) (uintptr_t) self->window;
    announceProperty (map, write, controller);
    return self;
}

void cleanup (LV2UI_Handle handle)
{
    auto* self = static_cast<Ui*> (handle);
    if (self == nullptr) return;
    if (self->display != nullptr)
    {
        if (self->window != 0) XDestroyWindow (self->display, self->window);
        XCloseDisplay (self->display);
    }
    delete self;
}

void portEvent (LV2UI_Handle, uint32_t, uint32_t, uint32_t, const void*) {}

const void* extensionData (const char*) { return nullptr; }

const LV2UI_Descriptor kDescriptor {
    kUiUri, instantiate, cleanup, portEvent, extensionData
};
} // namespace

extern "C" LV2_SYMBOL_EXPORT
const LV2UI_Descriptor* lv2ui_descriptor (uint32_t index)
{
    return index == 0 ? &kDescriptor : nullptr;
}
