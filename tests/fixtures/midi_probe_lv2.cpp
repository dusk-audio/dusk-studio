#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>

#include <array>
#include <cstdint>
#include <cstring>

// An LV2 instrument that holds a voice per sounding key and plays a DC level of
// 0.1 per voice, so a test reads from its output what the host delivered.
namespace
{
constexpr const char* kPluginUri = "urn:duskstudio:test:midi-probe";

struct Instance
{
    const LV2_Atom_Sequence* events = nullptr;
    float* outL = nullptr;
    float* outR = nullptr;
    LV2_URID midiEvent = 0;
    std::array<bool, 16 * 128> held {};
    int voices = 0;

    void set (int channel, int key, bool down) noexcept
    {
        auto& slot = held[(std::size_t) (channel * 128 + key)];
        if (slot == down) return;
        slot = down;
        voices += down ? 1 : -1;
    }

    void silence (int channel) noexcept
    {
        for (int key = 0; key < 128; ++key)
            set (channel, key, false);
    }
};

LV2_Handle instantiate (const LV2_Descriptor*, double, const char*,
                        const LV2_Feature* const* features)
{
    const LV2_URID_Map* map = nullptr;
    for (std::size_t i = 0; features != nullptr && features[i] != nullptr; ++i)
        if (std::strcmp (features[i]->URI, LV2_URID__map) == 0)
            map = static_cast<const LV2_URID_Map*> (features[i]->data);
    if (map == nullptr) return nullptr;

    auto* self = new Instance;
    self->midiEvent = map->map (map->handle, LV2_MIDI__MidiEvent);
    return self;
}

void connectPort (LV2_Handle instance, uint32_t port, void* data)
{
    auto& self = *static_cast<Instance*> (instance);
    switch (port)
    {
        case 0: self.events = static_cast<const LV2_Atom_Sequence*> (data); break;
        case 1: self.outL = static_cast<float*> (data); break;
        case 2: self.outR = static_cast<float*> (data); break;
        default: break;
    }
}

void run (LV2_Handle instance, uint32_t frames)
{
    auto& self = *static_cast<Instance*> (instance);
    if (self.events != nullptr)
    {
        LV2_ATOM_SEQUENCE_FOREACH (self.events, event)
        {
            if (event->body.type != self.midiEvent || event->body.size < 3) continue;
            const auto* msg = reinterpret_cast<const std::uint8_t*> (event + 1);
            const int status = msg[0] & 0xF0;
            const int channel = msg[0] & 0x0F;
            if (status == 0x90 && msg[2] > 0)
                self.set (channel, msg[1] & 0x7F, true);
            else if (status == 0x80 || status == 0x90)
                self.set (channel, msg[1] & 0x7F, false);
            else if (status == 0xB0 && (msg[1] == 120 || msg[1] == 123))
                self.silence (channel);
        }
    }

    const float level = 0.1f * (float) self.voices;
    for (uint32_t i = 0; i < frames; ++i)
    {
        if (self.outL != nullptr) self.outL[i] = level;
        if (self.outR != nullptr) self.outR[i] = level;
    }
}

void cleanup (LV2_Handle instance) { delete static_cast<Instance*> (instance); }

const LV2_Descriptor descriptor {
    kPluginUri, &instantiate, &connectPort, nullptr, &run, nullptr, &cleanup, nullptr
};
} // namespace

extern "C" LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor (uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
