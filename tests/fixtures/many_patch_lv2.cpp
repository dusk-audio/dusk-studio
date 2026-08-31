#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/patch/patch.h>
#include <lv2/urid/urid.h>

#include <array>
#include <cstdio>
#include <cstring>

namespace
{
constexpr const char* kPluginUri = "urn:duskstudio:test:many-patches";
constexpr size_t kPropertyCount = 160;

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
    float* inL = nullptr;
    float* inR = nullptr;
    float* outL = nullptr;
    float* outR = nullptr;
    const LV2_Atom_Sequence* controlIn = nullptr;
    LV2_Atom_Sequence* controlOut = nullptr;
    LV2_URID atomObject = 0;
    LV2_URID patchGet = 0;
    LV2_URID patchPut = 0;
    LV2_URID patchBody = 0;
    std::array<LV2_URID, kPropertyCount> properties {};
};

LV2_Handle instantiate (const LV2_Descriptor*, double, const char*,
                        const LV2_Feature* const* features)
{
    auto* map = const_cast<LV2_URID_Map*> (
        feature<LV2_URID_Map> (features, LV2_URID__map));
    if (map == nullptr) return nullptr;

    auto* self = new Instance;
    self->map = map;
    self->atomObject = map->map (map->handle, LV2_ATOM__Object);
    self->patchGet = map->map (map->handle, LV2_PATCH__Get);
    self->patchPut = map->map (map->handle, LV2_PATCH__Put);
    self->patchBody = map->map (map->handle, LV2_PATCH__body);
    for (size_t i = 0; i < kPropertyCount; ++i)
    {
        char uri[64] {};
        std::snprintf (uri, sizeof (uri), "%s#p%zu", kPluginUri, i);
        self->properties[i] = map->map (map->handle, uri);
    }
    return self;
}

void connectPort (LV2_Handle instance, uint32_t port, void* data)
{
    auto& self = *static_cast<Instance*> (instance);
    switch (port)
    {
        case 0: self.inL = static_cast<float*> (data); break;
        case 1: self.inR = static_cast<float*> (data); break;
        case 2: self.outL = static_cast<float*> (data); break;
        case 3: self.outR = static_cast<float*> (data); break;
        case 4: self.controlIn = static_cast<const LV2_Atom_Sequence*> (data); break;
        case 5: self.controlOut = static_cast<LV2_Atom_Sequence*> (data); break;
        default: break;
    }
}

bool patchGetRequested (const Instance& self)
{
    if (self.controlIn == nullptr) return false;
    LV2_ATOM_SEQUENCE_FOREACH (self.controlIn, event)
    {
        if (event->body.type != self.atomObject) continue;
        const auto* object = reinterpret_cast<const LV2_Atom_Object*> (&event->body);
        if (object->body.otype == self.patchGet) return true;
    }
    return false;
}

void forgeResponse (Instance& self, bool includeProperties)
{
    if (self.controlOut == nullptr) return;
    const size_t capacity = sizeof (LV2_Atom) + self.controlOut->atom.size;

    LV2_Atom_Forge forge;
    lv2_atom_forge_init (&forge, self.map);
    lv2_atom_forge_set_buffer (
        &forge, reinterpret_cast<uint8_t*> (self.controlOut), capacity);
    LV2_Atom_Forge_Frame sequenceFrame;
    lv2_atom_forge_sequence_head (&forge, &sequenceFrame, 0);
    if (includeProperties)
    {
        lv2_atom_forge_frame_time (&forge, 0);
        LV2_Atom_Forge_Frame putFrame;
        lv2_atom_forge_object (&forge, &putFrame, 0, self.patchPut);
        lv2_atom_forge_key (&forge, self.patchBody);
        LV2_Atom_Forge_Frame bodyFrame;
        lv2_atom_forge_object (&forge, &bodyFrame, 0, 0);
        for (size_t i = 0; i < kPropertyCount; ++i)
        {
            lv2_atom_forge_key (&forge, self.properties[i]);
            lv2_atom_forge_float (&forge, static_cast<float> (i));
        }
        lv2_atom_forge_pop (&forge, &bodyFrame);
        lv2_atom_forge_pop (&forge, &putFrame);
    }
    lv2_atom_forge_pop (&forge, &sequenceFrame);
}

void run (LV2_Handle instance, uint32_t frames)
{
    auto& self = *static_cast<Instance*> (instance);
    for (uint32_t i = 0; i < frames; ++i)
    {
        if (self.outL != nullptr) self.outL[i] = self.inL != nullptr ? self.inL[i] : 0.0f;
        if (self.outR != nullptr) self.outR[i] = self.inR != nullptr ? self.inR[i] : 0.0f;
    }
    forgeResponse (self, patchGetRequested (self));
}

void cleanup (LV2_Handle instance) { delete static_cast<Instance*> (instance); }

const LV2_Descriptor descriptor {
    kPluginUri, &instantiate, &connectPort, nullptr, &run, nullptr, &cleanup, nullptr
};
}

extern "C" LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor (uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
