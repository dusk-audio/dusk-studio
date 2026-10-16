#include <clap/clap.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace
{
constexpr const char* kPluginId = "studio.dusk.test.panic-probe";
constexpr const char* kLatencyPluginId = "studio.dusk.test.panic-probe-latency-400";
constexpr const char* kFeatures[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                      CLAP_PLUGIN_FEATURE_STEREO, nullptr };
constexpr uint32_t kNotePortCount = 2;
constexpr std::size_t kMaxVoices = 128;

enum ParamId : clap_id
{
    kParamVoicesHeld = 0,
    kParamChokesSeen,
    kParamCc120Seen,
    kParamCc123Seen,
    kParamNoteOffsSeen,
    kParamNoteOnsSeen,
    kParamLastNoteOnOffset,
    kParamLastNoteOffOffset,
    kParamCc121Seen,
    kParamPitchCentersSeen,
    kParamPressureZerosSeen,
    kParamBentChannels,
    kParamPressedChannels,
    kParamCount
};

struct Voice
{
    int16_t port = -1;
    int16_t key = -1;
    int16_t channel = -1;
};

struct PluginData
{
    // Fixed table: process() never allocates, so a voice past the ceiling is
    // dropped rather than grown into.
    std::array<Voice, kMaxVoices> voices {};
    std::uint32_t voiceCount = 0;

    std::uint32_t chokesSeen = 0;
    std::uint32_t cc120Seen = 0;
    std::uint32_t cc123Seen = 0;
    std::uint32_t noteOffsSeen = 0;
    std::uint32_t noteOnsSeen = 0;
    std::uint32_t lastNoteOnOffset = 0;
    std::uint32_t lastNoteOffOffset = 0;
    std::uint32_t cc121Seen = 0;
    std::uint32_t pitchCentersSeen = 0;
    std::uint32_t pressureZerosSeen = 0;
    std::uint32_t bentChannels = 0;
    std::uint32_t pressedChannels = 0;
    std::uint32_t latencySamples = 0;

    // A voice is per note port: the same key and channel on two ports are two
    // voices, so a choke that reaches only one port leaves the other held.
    void addVoice (int16_t port, int16_t key, int16_t channel) noexcept
    {
        for (std::uint32_t i = 0; i < voiceCount; ++i)
            if (voices[i].port == port && voices[i].key == key && voices[i].channel == channel)
                return;
        if (voiceCount >= kMaxVoices) return;
        voices[voiceCount++] = { port, key, channel };
    }

    // -1 is the CLAP wildcard for port / key / channel; a raw MIDI note-off
    // always names the key and channel.
    void removeVoices (int16_t port, int16_t key, int16_t channel) noexcept
    {
        std::uint32_t kept = 0;
        for (std::uint32_t i = 0; i < voiceCount; ++i)
        {
            const auto& v = voices[i];
            const bool matches = (port < 0 || v.port == port)
                              && (key < 0 || v.key == key)
                              && (channel < 0 || v.channel == channel);
            if (! matches) voices[kept++] = v;
        }
        voiceCount = kept;
    }

    void clearVoices() noexcept { voiceCount = 0; }
};

const clap_plugin_descriptor_t kDescriptor {
    CLAP_VERSION_INIT,
    kPluginId,
    "Dusk panic probe fixture",
    "Dusk Studio",
    "https://dusk.audio",
    "",
    "",
    "1.0.0",
    "Counts the note, choke and controller events a transport panic delivers",
    kFeatures
};

const auto kLatencyDescriptor = []
{
    auto descriptor = kDescriptor;
    descriptor.id = kLatencyPluginId;
    descriptor.name = "Dusk fixed-latency MIDI probe fixture";
    return descriptor;
}();

uint32_t CLAP_ABI audioPortCount (const clap_plugin_t*, bool isInput)
{
    return isInput ? 0u : 1u;
}

bool CLAP_ABI audioPortGet (const clap_plugin_t*, uint32_t index, bool isInput,
                            clap_audio_port_info_t* info)
{
    if (isInput || index != 0 || info == nullptr) return false;
    *info = {};
    info->id = 0;
    std::snprintf (info->name, sizeof (info->name), "Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t kAudioPorts { audioPortCount, audioPortGet };

uint32_t CLAP_ABI notePortCount (const clap_plugin_t*, bool isInput)
{
    return isInput ? kNotePortCount : 0u;
}

bool CLAP_ABI notePortGet (const clap_plugin_t*, uint32_t index, bool isInput,
                           clap_note_port_info_t* info)
{
    if (! isInput || index >= kNotePortCount || info == nullptr) return false;
    *info = {};
    info->id = index;
    // Port 0 takes CLAP notes only, so a host panic has to reach it as a
    // NOTE_CHOKE; port 1 also takes raw MIDI, so the controller path stays
    // observable on the same plugin.
    if (index == 0)
    {
        std::snprintf (info->name, sizeof (info->name), "CLAP notes");
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    }
    else
    {
        std::snprintf (info->name, sizeof (info->name), "CLAP + MIDI notes");
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    }
    return true;
}

const clap_plugin_note_ports_t kNotePorts { notePortCount, notePortGet };

uint32_t CLAP_ABI paramCount (const clap_plugin_t*) { return (uint32_t) kParamCount; }

bool CLAP_ABI paramGetInfo (const clap_plugin_t*, uint32_t index,
                            clap_param_info_t* info)
{
    if (index >= (uint32_t) kParamCount || info == nullptr) return false;
    static const char* const names[kParamCount] = {
        "voicesHeld", "chokesSeen", "cc120Seen", "cc123Seen", "noteOffsSeen",
        "noteOnsSeen", "lastNoteOnOffset", "lastNoteOffOffset", "cc121Seen",
        "pitchCentersSeen", "pressureZerosSeen", "bentChannels", "pressedChannels"
    };
    *info = {};
    info->id = (clap_id) index;
    info->flags = CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_STEPPED;
    info->cookie = nullptr;
    std::snprintf (info->name, sizeof (info->name), "%s", names[index]);
    info->min_value = 0.0;
    info->max_value = 4294967295.0;
    info->default_value = 0.0;
    return true;
}

bool CLAP_ABI paramGetValue (const clap_plugin_t* plugin, clap_id id, double* out)
{
    if (plugin == nullptr || out == nullptr) return false;
    const auto* data = static_cast<const PluginData*> (plugin->plugin_data);
    switch (id)
    {
        case kParamVoicesHeld:   *out = (double) data->voiceCount;   return true;
        case kParamChokesSeen:   *out = (double) data->chokesSeen;   return true;
        case kParamCc120Seen:    *out = (double) data->cc120Seen;    return true;
        case kParamCc123Seen:    *out = (double) data->cc123Seen;    return true;
        case kParamNoteOffsSeen: *out = (double) data->noteOffsSeen; return true;
        case kParamNoteOnsSeen: *out = (double) data->noteOnsSeen; return true;
        case kParamLastNoteOnOffset: *out = (double) data->lastNoteOnOffset; return true;
        case kParamLastNoteOffOffset: *out = (double) data->lastNoteOffOffset; return true;
        case kParamCc121Seen: *out = (double) data->cc121Seen; return true;
        case kParamPitchCentersSeen: *out = (double) data->pitchCentersSeen; return true;
        case kParamPressureZerosSeen: *out = (double) data->pressureZerosSeen; return true;
        case kParamBentChannels: *out = (double) data->bentChannels; return true;
        case kParamPressedChannels: *out = (double) data->pressedChannels; return true;
        default: return false;
    }
}

bool CLAP_ABI paramValueToText (const clap_plugin_t*, clap_id, double value,
                                char* buffer, uint32_t capacity)
{
    if (buffer == nullptr || capacity == 0) return false;
    std::snprintf (buffer, capacity, "%.0f", value);
    return true;
}

bool CLAP_ABI paramTextToValue (const clap_plugin_t*, clap_id, const char* text,
                                double* out)
{
    if (text == nullptr || out == nullptr) return false;
    *out = std::strtod (text, nullptr);
    return true;
}

void CLAP_ABI paramFlush (const clap_plugin_t*, const clap_input_events_t*,
                          const clap_output_events_t*)
{
}

const clap_plugin_params_t kParams {
    paramCount, paramGetInfo, paramGetValue, paramValueToText, paramTextToValue,
    paramFlush
};

void handleNoteEvent (PluginData& data, const clap_event_header_t* header)
{
    const auto* note = reinterpret_cast<const clap_event_note_t*> (header);
    switch (header->type)
    {
        case CLAP_EVENT_NOTE_ON:
            ++data.noteOnsSeen;
            data.lastNoteOnOffset = header->time;
            data.addVoice (note->port_index, note->key, note->channel);
            break;
        case CLAP_EVENT_NOTE_OFF:
            data.lastNoteOffOffset = header->time;
            data.removeVoices (note->port_index, note->key, note->channel);
            ++data.noteOffsSeen;
            break;
        case CLAP_EVENT_NOTE_CHOKE:
            data.removeVoices (note->port_index, note->key, note->channel);
            ++data.chokesSeen;
            break;
        default:
            break;
    }
}

void handleMidiEvent (PluginData& data, const clap_event_header_t* header)
{
    const auto* midi = reinterpret_cast<const clap_event_midi_t*> (header);
    const auto status = (std::uint8_t) (midi->data[0] & 0xF0u);
    const auto port = (int16_t) midi->port_index;
    const auto channel = (int16_t) (midi->data[0] & 0x0Fu);
    const auto d1 = midi->data[1];
    const auto d2 = midi->data[2];

    if (status == 0xB0 && d1 == 120)
    {
        data.clearVoices();
        ++data.cc120Seen;
    }
    else if (status == 0xB0 && d1 == 123)
    {
        data.clearVoices();
        ++data.cc123Seen;
    }
    else if (status == 0xB0 && d1 == 121)
        ++data.cc121Seen;
    else if (status == 0xE0)
    {
        const auto mask = std::uint32_t { 1 } << channel;
        if (d1 == 0 && d2 == 64)
        {
            data.bentChannels &= ~mask;
            ++data.pitchCentersSeen;
        }
        else data.bentChannels |= mask;
    }
    else if (status == 0xD0)
    {
        const auto mask = std::uint32_t { 1 } << channel;
        if (d1 == 0)
        {
            data.pressedChannels &= ~mask;
            ++data.pressureZerosSeen;
        }
        else data.pressedChannels |= mask;
    }
    else if (status == 0x90 && d2 > 0)
    {
        ++data.noteOnsSeen;
        data.lastNoteOnOffset = header->time;
        data.addVoice (port, (int16_t) d1, channel);
    }
    else if (status == 0x80 || (status == 0x90 && d2 == 0))
    {
        data.lastNoteOffOffset = header->time;
        data.removeVoices (port, (int16_t) d1, channel);
        ++data.noteOffsSeen;
    }
}

bool CLAP_ABI pluginInit (const clap_plugin_t*) { return true; }
void CLAP_ABI pluginDestroy (const clap_plugin_t* plugin)
{
    delete static_cast<PluginData*> (plugin->plugin_data);
    delete plugin;
}
bool CLAP_ABI pluginActivate (const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
void CLAP_ABI pluginDeactivate (const clap_plugin_t*) {}
bool CLAP_ABI pluginStart (const clap_plugin_t*) { return true; }
void CLAP_ABI pluginStop (const clap_plugin_t*) {}

void CLAP_ABI pluginReset (const clap_plugin_t* plugin)
{
    // Voices go, counters stay: a scenario reads them after the reset that
    // silenced the plugin.
    static_cast<PluginData*> (plugin->plugin_data)->clearVoices();
}

clap_process_status CLAP_ABI pluginProcess (const clap_plugin_t* plugin,
                                            const clap_process_t* process)
{
    if (process == nullptr || process->audio_outputs_count != 1
        || process->audio_outputs == nullptr)
        return CLAP_PROCESS_ERROR;

    auto& data = *static_cast<PluginData*> (plugin->plugin_data);
    if (process->in_events != nullptr)
    {
        const auto count = process->in_events->size (process->in_events);
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto* event = process->in_events->get (process->in_events, i);
            if (event == nullptr || event->space_id != CLAP_CORE_EVENT_SPACE_ID)
                continue;
            if ((event->type == CLAP_EVENT_NOTE_ON || event->type == CLAP_EVENT_NOTE_OFF
                 || event->type == CLAP_EVENT_NOTE_CHOKE)
                && event->size >= sizeof (clap_event_note_t))
                handleNoteEvent (data, event);
            else if (event->type == CLAP_EVENT_MIDI
                     && event->size >= sizeof (clap_event_midi_t))
                handleMidiEvent (data, event);
        }
    }

    const auto& out = process->audio_outputs[0];
    if (out.channel_count != 2 || out.data32 == nullptr
        || out.data32[0] == nullptr || out.data32[1] == nullptr)
        return CLAP_PROCESS_ERROR;

    const float dc = (float) data.voiceCount * 0.1f;
    for (uint32_t frame = 0; frame < process->frames_count; ++frame)
    {
        out.data32[0][frame] = dc;
        out.data32[1][frame] = dc;
    }
    return CLAP_PROCESS_CONTINUE;
}

uint32_t CLAP_ABI latencyGet (const clap_plugin_t* plugin)
{
    return static_cast<const PluginData*> (plugin->plugin_data)->latencySamples;
}
const clap_plugin_latency_t kLatency { latencyGet };

const void* CLAP_ABI pluginExtension (const clap_plugin_t*, const char* id)
{
    if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPorts;
    if (std::strcmp (id, CLAP_EXT_NOTE_PORTS) == 0) return &kNotePorts;
    if (std::strcmp (id, CLAP_EXT_PARAMS) == 0) return &kParams;
    if (std::strcmp (id, CLAP_EXT_LATENCY) == 0) return &kLatency;
    return nullptr;
}
void CLAP_ABI pluginMainThread (const clap_plugin_t*) {}

const clap_plugin_t* CLAP_ABI createPlugin (const clap_plugin_factory_t*,
                                            const clap_host_t* host,
                                            const char* pluginId)
{
    if (host == nullptr || pluginId == nullptr
        || ! clap_version_is_compatible (host->clap_version)
        || (std::strcmp (pluginId, kPluginId) != 0 && std::strcmp (pluginId, kLatencyPluginId) != 0))
        return nullptr;

    auto* plugin = new (std::nothrow) clap_plugin_t {};
    auto* data = new (std::nothrow) PluginData {};
    if (plugin == nullptr || data == nullptr)
    {
        delete plugin;
        delete data;
        return nullptr;
    }

    const bool latency = std::strcmp (pluginId, kLatencyPluginId) == 0;
    data->latencySamples = latency ? 400 : 0;
    plugin->desc = latency ? &kLatencyDescriptor : &kDescriptor;
    plugin->plugin_data = data;
    plugin->init = pluginInit;
    plugin->destroy = pluginDestroy;
    plugin->activate = pluginActivate;
    plugin->deactivate = pluginDeactivate;
    plugin->start_processing = pluginStart;
    plugin->stop_processing = pluginStop;
    plugin->reset = pluginReset;
    plugin->process = pluginProcess;
    plugin->get_extension = pluginExtension;
    plugin->on_main_thread = pluginMainThread;
    return plugin;
}

uint32_t CLAP_ABI pluginCount (const clap_plugin_factory_t*) { return 2; }
const clap_plugin_descriptor_t* CLAP_ABI pluginDescriptor (const clap_plugin_factory_t*,
                                                           uint32_t index)
{
    return index == 0 ? &kDescriptor : (index == 1 ? &kLatencyDescriptor : nullptr);
}
const clap_plugin_factory_t kFactory { pluginCount, pluginDescriptor, createPlugin };

bool CLAP_ABI entryInit (const char*) { return true; }
void CLAP_ABI entryDeinit() {}
const void* CLAP_ABI entryFactory (const char* id)
{
    return std::strcmp (id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}
} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry {
    CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory
};
