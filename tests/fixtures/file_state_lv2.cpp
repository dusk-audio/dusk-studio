#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
constexpr const char* kPluginUri = "urn:duskstudio:test:file-state";
constexpr const char* kPathKey = "urn:duskstudio:test:file-state#path";
constexpr const char* kPayload = "dusk-lv2-file-state-v1";

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
    std::string restoredPath;
    bool restoredReadable = false;
};

LV2_Handle instantiate (const LV2_Descriptor*, double, const char*,
                        const LV2_Feature* const* features)
{
    auto* map = const_cast<LV2_URID_Map*> (feature<LV2_URID_Map> (features, LV2_URID__map));
    if (map == nullptr) return nullptr;
    auto* self = new Instance;
    self->map = map;
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
        default: break;
    }
}

void run (LV2_Handle instance, uint32_t frames)
{
    const auto& self = *static_cast<Instance*> (instance);
    for (uint32_t i = 0; i < frames; ++i)
    {
        if (self.outL != nullptr)
            self.outL[i] = self.restoredReadable && self.inL != nullptr ? self.inL[i] : 0.0f;
        if (self.outR != nullptr)
            self.outR[i] = self.restoredReadable && self.inR != nullptr ? self.inR[i] : 0.0f;
    }
}

void cleanup (LV2_Handle instance) { delete static_cast<Instance*> (instance); }

LV2_State_Status save (LV2_Handle instance, LV2_State_Store_Function store,
                       LV2_State_Handle handle, uint32_t,
                       const LV2_Feature* const* features)
{
    auto& self = *static_cast<Instance*> (instance);
    const auto* mapPath = feature<LV2_State_Map_Path> (features, LV2_STATE__mapPath);
    const auto* makePath = feature<LV2_State_Make_Path> (features, LV2_STATE__makePath);
    const auto* freePath = feature<LV2_State_Free_Path> (features, LV2_STATE__freePath);
    if (mapPath == nullptr || freePath == nullptr)
        return LV2_STATE_ERR_NO_FEATURE;

    std::string absolute = self.restoredPath;
    char* made = nullptr;
    if (absolute.empty())
    {
        if (makePath == nullptr) return LV2_STATE_ERR_NO_FEATURE;
        made = makePath->path (makePath->handle, "payload.txt");
        if (made == nullptr) return LV2_STATE_ERR_UNKNOWN;
        absolute = made;
        std::ofstream output (absolute, std::ios::binary | std::ios::trunc);
        output << kPayload;
        if (! output)
        {
            freePath->free_path (freePath->handle, made);
            return LV2_STATE_ERR_UNKNOWN;
        }
    }

    char* abstract = mapPath->abstract_path (mapPath->handle, absolute.c_str());
    if (made != nullptr) freePath->free_path (freePath->handle, made);
    if (abstract == nullptr) return LV2_STATE_ERR_UNKNOWN;

    const LV2_URID key = self.map->map (self.map->handle, kPathKey);
    const LV2_URID type = self.map->map (self.map->handle, LV2_ATOM__Path);
    const auto status = store (handle, key, abstract,
                               std::strlen (abstract) + 1, type,
                               LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    freePath->free_path (freePath->handle, abstract);
    return status;
}

LV2_State_Status restore (LV2_Handle instance, LV2_State_Retrieve_Function retrieve,
                          LV2_State_Handle handle, uint32_t,
                          const LV2_Feature* const* features)
{
    auto& self = *static_cast<Instance*> (instance);
    const auto* mapPath = feature<LV2_State_Map_Path> (features, LV2_STATE__mapPath);
    const auto* freePath = feature<LV2_State_Free_Path> (features, LV2_STATE__freePath);
    if (mapPath == nullptr || freePath == nullptr)
        return LV2_STATE_ERR_NO_FEATURE;

    size_t size = 0;
    uint32_t type = 0;
    uint32_t flags = 0;
    const LV2_URID key = self.map->map (self.map->handle, kPathKey);
    const void* value = retrieve (handle, key, &size, &type, &flags);
    if (value == nullptr || size == 0
        || type != self.map->map (self.map->handle, LV2_ATOM__Path))
        return LV2_STATE_ERR_BAD_TYPE;

    char* absolute = mapPath->absolute_path (
        mapPath->handle, static_cast<const char*> (value));
    if (absolute == nullptr) return LV2_STATE_ERR_UNKNOWN;
    self.restoredPath = absolute;
    freePath->free_path (freePath->handle, absolute);

    std::ifstream input (self.restoredPath, std::ios::binary);
    std::string payload ((std::istreambuf_iterator<char> (input)),
                         std::istreambuf_iterator<char>());
    self.restoredReadable = input.is_open() && payload == kPayload;
    return self.restoredReadable ? LV2_STATE_SUCCESS : LV2_STATE_ERR_UNKNOWN;
}

const LV2_State_Interface stateInterface { &save, &restore };

const void* extensionData (const char* uri)
{
    return std::strcmp (uri, LV2_STATE__interface) == 0 ? &stateInterface : nullptr;
}

const LV2_Descriptor descriptor {
    kPluginUri, &instantiate, &connectPort, nullptr, &run, nullptr, &cleanup,
    &extensionData
};
}

extern "C" LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor (uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
