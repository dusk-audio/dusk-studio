#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include "StateProbeLv2.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// The level property is plug-in state that only run() changes: a patch:Set
// queued by the host reaches it no sooner than the next run. An Announce
// message makes the plug-in report a change of its own.
namespace
{
namespace Probe = duskstudio::test::stateprobelv2;

template <typename T>
const T* feature (const LV2_Feature* const* features, const char* uri)
{
    if (features == nullptr) return nullptr;
    for (size_t i = 0; features[i] != nullptr; ++i)
        if (std::strcmp (features[i]->URI, uri) == 0)
            return static_cast<const T*> (features[i]->data);
    return nullptr;
}

struct Instance
{
    LV2_URID_Map* map = nullptr;
    const float* inL = nullptr;
    const float* inR = nullptr;
    float* outL = nullptr;
    float* outR = nullptr;
    const float* gain = nullptr;
    float* meter = nullptr;
    const LV2_Atom_Sequence* controlIn = nullptr;
    LV2_Atom_Sequence* controlOut = nullptr;
    LV2_URID atomFloat = 0, atomInt = 0, atomObject = 0, atomUrid = 0;
    LV2_URID patchGet = 0, patchSet = 0, patchPut = 0, patchBody = 0;
    LV2_URID patchProperty = 0, patchValue = 0;
    LV2_URID stateChanged = 0, level = 0, announce = 0, what = 0;
    float levelValue = 0.0f;
};

LV2_Handle instantiate (const LV2_Descriptor*, double, const char*,
                        const LV2_Feature* const* features)
{
    auto* map = const_cast<LV2_URID_Map*> (feature<LV2_URID_Map> (features, LV2_URID__map));
    if (map == nullptr) return nullptr;
    auto* self = new Instance;
    self->map = map;
    const auto urid = [map] (const char* uri) { return map->map (map->handle, uri); };
    self->atomFloat = urid (LV2_ATOM__Float);
    self->atomInt = urid (LV2_ATOM__Int);
    self->atomObject = urid (LV2_ATOM__Object);
    self->atomUrid = urid (LV2_ATOM__URID);
    self->patchGet = urid (LV2_PATCH__Get);
    self->patchSet = urid (LV2_PATCH__Set);
    self->patchPut = urid (LV2_PATCH__Put);
    self->patchBody = urid (LV2_PATCH__body);
    self->patchProperty = urid (LV2_PATCH__property);
    self->patchValue = urid (LV2_PATCH__value);
    self->stateChanged = urid (LV2_STATE__StateChanged);
    self->level = urid (Probe::kLevelUri);
    self->announce = urid (Probe::kAnnounceUri);
    self->what = urid (Probe::kWhatUri);
    return self;
}

void connectPort (LV2_Handle instance, uint32_t port, void* data)
{
    auto& self = *static_cast<Instance*> (instance);
    switch (port)
    {
        case 0: self.inL = static_cast<const float*> (data); break;
        case 1: self.inR = static_cast<const float*> (data); break;
        case 2: self.outL = static_cast<float*> (data); break;
        case 3: self.outR = static_cast<float*> (data); break;
        case 4: self.gain = static_cast<const float*> (data); break;
        case 5: self.meter = static_cast<float*> (data); break;
        case 6: self.controlIn = static_cast<const LV2_Atom_Sequence*> (data); break;
        case 7: self.controlOut = static_cast<LV2_Atom_Sequence*> (data); break;
        default: break;
    }
}

void forgeLevel (Instance& self, LV2_Atom_Forge& forge, LV2_URID type)
{
    lv2_atom_forge_frame_time (&forge, 0);
    LV2_Atom_Forge_Frame frame;
    lv2_atom_forge_object (&forge, &frame, 0, type);
    if (type == self.patchSet)
    {
        lv2_atom_forge_key (&forge, self.patchProperty);
        lv2_atom_forge_urid (&forge, self.level);
        lv2_atom_forge_key (&forge, self.patchValue);
        lv2_atom_forge_float (&forge, self.levelValue);
    }
    else
    {
        lv2_atom_forge_key (&forge, self.patchBody);
        LV2_Atom_Forge_Frame body;
        lv2_atom_forge_object (&forge, &body, 0, 0);
        lv2_atom_forge_key (&forge, self.level);
        lv2_atom_forge_float (&forge, self.levelValue);
        lv2_atom_forge_pop (&forge, &body);
    }
    lv2_atom_forge_pop (&forge, &frame);
}

void handle (Instance& self, const LV2_Atom_Object& object, LV2_Atom_Forge& forge)
{
    if (object.body.otype == self.patchSet)
    {
        const LV2_Atom* property = nullptr;
        const LV2_Atom* value = nullptr;
        lv2_atom_object_get (&object, self.patchProperty, &property, self.patchValue, &value, 0);
        if (property == nullptr || property->type != self.atomUrid
            || reinterpret_cast<const LV2_Atom_URID*> (property)->body != self.level
            || value == nullptr || value->type != self.atomFloat)
            return;
        self.levelValue = reinterpret_cast<const LV2_Atom_Float*> (value)->body;
        forgeLevel (self, forge, self.patchSet);
    }
    else if (object.body.otype == self.patchGet)
        forgeLevel (self, forge, self.patchPut);
    else if (object.body.otype == self.announce)
    {
        const LV2_Atom* what = nullptr;
        lv2_atom_object_get (&object, self.what, &what, 0);
        if (what == nullptr || what->type != self.atomInt) return;
        switch (reinterpret_cast<const LV2_Atom_Int*> (what)->body)
        {
            case Probe::StateChanged:
            {
                lv2_atom_forge_frame_time (&forge, 0);
                LV2_Atom_Forge_Frame frame;
                lv2_atom_forge_object (&forge, &frame, 0, self.stateChanged);
                lv2_atom_forge_pop (&forge, &frame);
                break;
            }
            case Probe::ChangeLevel:
                self.levelValue = Probe::kAnnouncedLevel;
                forgeLevel (self, forge, self.patchSet);
                break;
            case Probe::RepeatLevel:
                forgeLevel (self, forge, self.patchSet);
                break;
            default:
                break;
        }
    }
}

void run (LV2_Handle instance, uint32_t frames)
{
    auto& self = *static_cast<Instance*> (instance);
    float peak = 0.0f;
    for (uint32_t i = 0; i < frames; ++i)
    {
        const float l = self.inL != nullptr ? self.inL[i] : 0.0f;
        const float r = self.inR != nullptr ? self.inR[i] : 0.0f;
        if (self.outL != nullptr) self.outL[i] = l;
        if (self.outR != nullptr) self.outR[i] = r;
        peak = std::max ({ peak, std::abs (l), std::abs (r) });
    }
    if (self.meter != nullptr) *self.meter = peak;
    if (self.controlOut == nullptr) return;

    LV2_Atom_Forge forge;
    lv2_atom_forge_init (&forge, self.map);
    lv2_atom_forge_set_buffer (&forge, reinterpret_cast<uint8_t*> (self.controlOut),
                               sizeof (LV2_Atom) + self.controlOut->atom.size);
    LV2_Atom_Forge_Frame sequence;
    lv2_atom_forge_sequence_head (&forge, &sequence, 0);
    if (self.controlIn != nullptr)
        LV2_ATOM_SEQUENCE_FOREACH (self.controlIn, event)
            if (event->body.type == self.atomObject)
                handle (self, *reinterpret_cast<const LV2_Atom_Object*> (&event->body), forge);
    lv2_atom_forge_pop (&forge, &sequence);
}

void cleanup (LV2_Handle instance) { delete static_cast<Instance*> (instance); }

LV2_State_Status save (LV2_Handle instance, LV2_State_Store_Function store,
                       LV2_State_Handle handle, uint32_t, const LV2_Feature* const*)
{
    auto& self = *static_cast<Instance*> (instance);
    return store (handle, self.level, &self.levelValue, sizeof (self.levelValue),
                  self.atomFloat, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

LV2_State_Status restore (LV2_Handle instance, LV2_State_Retrieve_Function retrieve,
                          LV2_State_Handle handle, uint32_t, const LV2_Feature* const*)
{
    auto& self = *static_cast<Instance*> (instance);
    size_t size = 0;
    uint32_t type = 0, flags = 0;
    const void* value = retrieve (handle, self.level, &size, &type, &flags);
    if (value == nullptr || size != sizeof (float) || type != self.atomFloat)
        return LV2_STATE_ERR_BAD_TYPE;
    std::memcpy (&self.levelValue, value, sizeof (float));
    return LV2_STATE_SUCCESS;
}

const LV2_State_Interface stateInterface { &save, &restore };

const void* extensionData (const char* uri)
{
    return std::strcmp (uri, LV2_STATE__interface) == 0 ? &stateInterface : nullptr;
}

const LV2_Descriptor descriptor {
    Probe::kPluginUri, &instantiate, &connectPort, nullptr, &run, nullptr, &cleanup, &extensionData
};
} // namespace

extern "C" LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor (uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
