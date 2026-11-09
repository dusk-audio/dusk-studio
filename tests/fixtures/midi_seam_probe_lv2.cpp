#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
struct Instance
{
    struct Voice
    {
        enum State { Silent, Held, Sustained, Tail } state = Silent;
        std::uint32_t tailLeft = 0;
    };
    const LV2_Atom_Sequence* events = nullptr;
    float* left = nullptr;
    float* right = nullptr;
    LV2_URID midi = 0, string = 0, counters = 0;
    std::array<std::array<Voice, 128>, 16> voices {};
    std::uint16_t pedals = 0;
    int offs = 0, panics = 0, retriggers = 0, lifts = 0, tailsCut = 0;

    int count (Voice::State state) const
    {
        int total = 0;
        for (const auto& channel : voices)
            for (const auto& voice : channel)
                total += voice.state == state;
        return total;
    }

    void release (Voice& voice) { voice.state = Voice::Tail; voice.tailLeft = 12000; }

    void hear (const std::uint8_t* bytes, std::uint32_t size)
    {
        if (size < 3) return;
        const int channel = bytes[0] & 15, key = bytes[1] & 127, value = bytes[2] & 127;
        const int kind = bytes[0] & 0xf0;
        auto& voice = voices[channel][key];
        const auto bit = (std::uint16_t) (1u << channel);
        if (kind == 0x90 && value != 0)
        {
            retriggers += voice.state == Voice::Held;
            voice.state = Voice::Held;
        }
        else if (kind == 0x80 || kind == 0x90)
        {
            ++offs;
            if (voice.state != Voice::Held) return;
            if ((pedals & bit) != 0) voice.state = Voice::Sustained;
            else release (voice);
        }
        else if (kind == 0xb0)
        {
            if (key == 64 || key == 121)
            {
                const bool down = key == 64 && value >= 64;
                if (! down && (pedals & bit) != 0)
                {
                    ++lifts;
                    for (auto& v : voices[channel])
                        if (v.state == Voice::Sustained) release (v);
                }
                pedals = (std::uint16_t) (down ? pedals | bit : pedals & ~bit);
            }
            if (key == 120 || key == 123)
            {
                ++panics;
                for (auto& v : voices[channel])
                    if (key == 120)
                    {
                        tailsCut += v.state == Voice::Tail;
                        v.state = Voice::Silent;
                    }
                    else if (v.state == Voice::Held)
                    {
                        if ((pedals & bit) != 0) v.state = Voice::Sustained;
                        else release (v);
                    }
            }
        }
    }

    void render (std::uint32_t start, std::uint32_t end)
    {
        const float level = 0.1f * (float) (count (Voice::Held) + count (Voice::Sustained));
        if (left) std::fill (left + start, left + end, level);
        if (right) std::fill (right + start, right + end, level);
        for (auto& channel : voices)
            for (auto& voice : channel)
                if (voice.state == Voice::Tail)
                {
                    const auto length = std::min (end - start, voice.tailLeft);
                    for (auto i = start; i < start + length; ++i)
                    {
                        if (left) left[i] += 0.1f;
                        if (right) right[i] += 0.1f;
                    }
                    voice.tailLeft -= length;
                    if (voice.tailLeft == 0) voice.state = Voice::Silent;
                }
    }
};

LV2_Handle instantiate (const LV2_Descriptor*, double, const char*, const LV2_Feature* const* features)
{
    const LV2_URID_Map* map = nullptr;
    for (std::size_t i = 0; features && features[i]; ++i)
        if (std::strcmp (features[i]->URI, LV2_URID__map) == 0)
            map = static_cast<const LV2_URID_Map*> (features[i]->data);
    if (! map) return nullptr;
    auto* self = new Instance;
    self->midi = map->map (map->handle, LV2_MIDI__MidiEvent);
    self->string = map->map (map->handle, LV2_ATOM__String);
    self->counters = map->map (map->handle, "urn:duskstudio:test:midi-seam-probe#counters");
    return self;
}

void connect (LV2_Handle instance, std::uint32_t port, void* data)
{
    auto& self = *static_cast<Instance*> (instance);
    if (port == 0) self.events = static_cast<const LV2_Atom_Sequence*> (data);
    if (port == 1) self.left = static_cast<float*> (data);
    if (port == 2) self.right = static_cast<float*> (data);
}

void run (LV2_Handle instance, std::uint32_t frames)
{
    auto& self = *static_cast<Instance*> (instance);
    std::uint32_t cursor = 0;
    if (self.events)
        LV2_ATOM_SEQUENCE_FOREACH (self.events, event)
        {
            if (event->body.type != self.midi || event->time.frames < cursor || event->time.frames >= frames)
                continue;
            const auto at = (std::uint32_t) event->time.frames;
            self.render (cursor, at);
            self.hear (reinterpret_cast<const std::uint8_t*> (event + 1), event->body.size);
            cursor = at;
        }
    self.render (cursor, frames);
}

LV2_State_Status save (LV2_Handle instance, LV2_State_Store_Function store,
                       LV2_State_Handle handle, std::uint32_t, const LV2_Feature* const*)
{
    const auto& self = *static_cast<Instance*> (instance);
    char text[256] {};
    std::snprintf (text, sizeof (text), "DUSK-MIDI-PROBE %d %d %d %d %d %u %d %d %d",
                   self.count (Instance::Voice::Held), self.offs, self.panics, self.retriggers,
                   self.count (Instance::Voice::Sustained), (unsigned) self.pedals, self.lifts,
                   self.count (Instance::Voice::Tail), self.tailsCut);
    return store (handle, self.counters, text, std::strlen (text) + 1, self.string,
                  LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

LV2_State_Status restore (LV2_Handle, LV2_State_Retrieve_Function, LV2_State_Handle,
                          std::uint32_t, const LV2_Feature* const*) { return LV2_STATE_SUCCESS; }

const void* extension (const char* uri)
{
    static const LV2_State_Interface state { &save, &restore };
    return std::strcmp (uri, LV2_STATE__interface) == 0 ? &state : nullptr;
}

void cleanup (LV2_Handle instance) { delete static_cast<Instance*> (instance); }
const LV2_Descriptor descriptor {
    "urn:duskstudio:test:midi-seam-probe", &instantiate, &connect, nullptr, &run, nullptr, &cleanup, &extension
};
}

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor (std::uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
