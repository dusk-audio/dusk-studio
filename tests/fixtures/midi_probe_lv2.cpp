#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

// An LV2 instrument that holds a voice per sounding key and plays a DC level of
// 0.1 per voice, so a test reads from its output what the host delivered.
namespace
{
constexpr const char* kPluginUri = "urn:duskstudio:test:midi-probe";

void checkLifecycleThread (bool required)
{
    static const auto creator = std::this_thread::get_id();
    if (required && std::this_thread::get_id() != creator)
    {
        std::fputs ("LV2 lifecycle moved off its creator thread\n", stderr);
        std::abort();
    }
}

struct Instance
{
    bool checkThread = false;
    const LV2_Atom_Sequence* events = nullptr;
    const LV2_Atom_Sequence* timeEvents = nullptr;
    const float* reportTransport = nullptr;
    float* outL = nullptr;
    float* outR = nullptr;
    LV2_URID midiEvent = 0;
    LV2_URID atomObject = 0, atomFloat = 0, atomDouble = 0, atomInt = 0, atomLong = 0;
    LV2_URID timePosition = 0;
    std::array<LV2_URID, 9> timeProperties {};
    std::array<bool, 16 * 128> held {};
    int voices = 0;

    std::array<float, 12> timeReport (const LV2_Atom_Sequence* sequence) const noexcept
    {
        std::array<float, 12> report {};
        if (sequence == nullptr) return report;
        std::int64_t previousFrame = 0;
        bool ordered = true;
        LV2_ATOM_SEQUENCE_FOREACH (sequence, event)
        {
            ordered = ordered && event->time.frames >= previousFrame;
            previousFrame = event->time.frames;
            if (event->body.type != atomObject || event->body.size < sizeof (LV2_Atom_Object_Body))
                continue;
            const auto* object = reinterpret_cast<const LV2_Atom_Object*> (&event->body);
            if (object->body.otype != timePosition) continue;
            report[0] += 1.0f;
            report[1] = (float) event->time.frames;
            for (std::size_t property = 0; property < timeProperties.size(); ++property)
            {
                const LV2_Atom* value = nullptr;
                lv2_atom_object_get (object, timeProperties[property], &value, 0);
                float number = -10000.0f;
                if (value != nullptr)
                {
                    if (value->type == atomFloat && value->size == sizeof (float))
                        number = reinterpret_cast<const LV2_Atom_Float*> (value)->body;
                    else if (value->type == atomDouble && value->size == sizeof (double))
                        number = (float) reinterpret_cast<const LV2_Atom_Double*> (value)->body;
                    else if (value->type == atomInt && value->size == sizeof (std::int32_t))
                        number = (float) reinterpret_cast<const LV2_Atom_Int*> (value)->body;
                    else if (value->type == atomLong && value->size == sizeof (std::int64_t))
                        number = (float) reinterpret_cast<const LV2_Atom_Long*> (value)->body;
                }
                report[property + 2] = number;
            }
        }
        report[11] = ordered ? 1.0f : 0.0f;
        return report;
    }

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

LV2_Handle instantiate (const LV2_Descriptor*, double, const char* bundlePath,
                        const LV2_Feature* const* features)
{
    std::error_code error;
    const bool checkThread = std::filesystem::is_regular_file (
        std::filesystem::u8path (bundlePath) / "require-creator-thread", error);
    checkLifecycleThread (checkThread);
    const LV2_URID_Map* map = nullptr;
    for (std::size_t i = 0; features != nullptr && features[i] != nullptr; ++i)
        if (std::strcmp (features[i]->URI, LV2_URID__map) == 0)
            map = static_cast<const LV2_URID_Map*> (features[i]->data);
    if (map == nullptr) return nullptr;

    auto* self = new Instance;
    self->checkThread = checkThread;
    self->midiEvent = map->map (map->handle, LV2_MIDI__MidiEvent);
    self->atomObject = map->map (map->handle, LV2_ATOM__Object);
    self->atomFloat = map->map (map->handle, LV2_ATOM__Float);
    self->atomDouble = map->map (map->handle, LV2_ATOM__Double);
    self->atomInt = map->map (map->handle, LV2_ATOM__Int);
    self->atomLong = map->map (map->handle, LV2_ATOM__Long);
    self->timePosition = map->map (map->handle, LV2_TIME__Position);
    const std::array<const char*, 9> properties {
        LV2_TIME__frame, LV2_TIME__speed, LV2_TIME__beat, LV2_TIME__bar,
        LV2_TIME__barBeat, LV2_TIME__beatUnit, LV2_TIME__beatsPerBar,
        LV2_TIME__beatsPerMinute, LV2_TIME__framesPerSecond
    };
    for (std::size_t property = 0; property < properties.size(); ++property)
        self->timeProperties[property] = map->map (map->handle, properties[property]);
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
        case 3: self.timeEvents = static_cast<const LV2_Atom_Sequence*> (data); break;
        case 4: self.reportTransport = static_cast<const float*> (data); break;
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
    if (self.reportTransport != nullptr && *self.reportTransport > 0.5f)
    {
        const auto controlReport = self.timeReport (self.events);
        const auto dedicatedReport = self.timeReport (self.timeEvents);
        for (std::size_t sample = 0; sample < controlReport.size() && sample < frames; ++sample)
        {
            if (self.outL != nullptr) self.outL[sample] = controlReport[sample];
            if (self.outR != nullptr) self.outR[sample] = dedicatedReport[sample];
        }
    }
}

void cleanup (LV2_Handle instance)
{
    auto* self = static_cast<Instance*> (instance);
    checkLifecycleThread (self->checkThread);
    delete self;
}

const LV2_Descriptor descriptor {
    kPluginUri, &instantiate, &connectPort, nullptr, &run, nullptr, &cleanup, nullptr
};
} // namespace

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor (uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}
