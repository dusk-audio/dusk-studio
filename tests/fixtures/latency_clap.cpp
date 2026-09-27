#include <clap/clap.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace
{
constexpr const char* kPluginId = "studio.dusk.test.latency";
constexpr const char* kFeatures[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
                                      CLAP_PLUGIN_FEATURE_STEREO, nullptr };

constexpr clap_id kLookAheadId = 7;

const clap_plugin_descriptor_t kDescriptor {
    CLAP_VERSION_INIT,
    kPluginId,
    "Dusk latency fixture",
    "Dusk Studio",
    "https://dusk.audio",
    "",
    "",
    "1.0.0",
    "A look-ahead in samples that changes the latency the way CLAP allows: by restart",
    kFeatures
};

// A look-ahead limiter in miniature. Its latency may only change inside
// activate(), so a new look-ahead set while it runs is held until the host
// restarts it, and process() asks for that restart.
struct Instance
{
    clap_plugin_t plugin {};
    const clap_host_t* host = nullptr;
    double lookAhead = 0.0;
    uint32_t activeLatency = 0;
    bool restartWanted = false;
};

Instance& self (const clap_plugin_t* plugin)
{
    return *static_cast<Instance*> (plugin->plugin_data);
}

uint32_t CLAP_ABI audioPortCount (const clap_plugin_t*, bool) { return 1; }

bool CLAP_ABI audioPortGet (const clap_plugin_t*, uint32_t index, bool isInput,
                            clap_audio_port_info_t* info)
{
    if (index != 0 || info == nullptr) return false;
    *info = {};
    info->id = 0;
    std::snprintf (info->name, sizeof (info->name), "%s", isInput ? "Input" : "Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t kAudioPorts { audioPortCount, audioPortGet };

uint32_t CLAP_ABI latencyGet (const clap_plugin_t* plugin)
{
    return self (plugin).activeLatency;
}

const clap_plugin_latency_t kLatency { latencyGet };

uint32_t CLAP_ABI paramCount (const clap_plugin_t*) { return 1; }

bool CLAP_ABI paramGetInfo (const clap_plugin_t*, uint32_t index, clap_param_info_t* info)
{
    if (index != 0 || info == nullptr) return false;
    *info = {};
    info->id = kLookAheadId;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
    std::snprintf (info->name, sizeof (info->name), "%s", "Look-ahead");
    std::snprintf (info->module, sizeof (info->module), "%s", "");
    info->min_value = 0.0;
    info->max_value = 4096.0;
    info->default_value = 0.0;
    return true;
}

bool CLAP_ABI paramGetValue (const clap_plugin_t* plugin, clap_id id, double* out)
{
    if (id != kLookAheadId || out == nullptr) return false;
    *out = self (plugin).lookAhead;
    return true;
}

bool CLAP_ABI paramValueToText (const clap_plugin_t*, clap_id, double value,
                                char* out, uint32_t size)
{
    if (out == nullptr || size == 0) return false;
    std::snprintf (out, size, "%.0f", value);
    return true;
}

bool CLAP_ABI paramTextToValue (const clap_plugin_t*, clap_id, const char* text, double* out)
{
    if (text == nullptr || out == nullptr) return false;
    *out = std::atof (text);
    return true;
}

void readInputEvents (Instance& instance, const clap_input_events_t* events)
{
    if (events == nullptr || events->size == nullptr || events->get == nullptr) return;
    const uint32_t count = events->size (events);
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto* header = events->get (events, i);
        if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID
            || header->type != CLAP_EVENT_PARAM_VALUE)
            continue;
        const auto* event = reinterpret_cast<const clap_event_param_value_t*> (header);
        if (event->param_id != kLookAheadId) continue;
        instance.lookAhead = event->value;
        if ((uint32_t) event->value != instance.activeLatency)
            instance.restartWanted = true;
    }
}

void askForRestart (Instance& instance)
{
    if (! instance.restartWanted) return;
    instance.restartWanted = false;
    if (instance.host != nullptr && instance.host->request_restart != nullptr)
        instance.host->request_restart (instance.host);
}

void CLAP_ABI paramFlush (const clap_plugin_t* plugin, const clap_input_events_t* in,
                          const clap_output_events_t*)
{
    auto& instance = self (plugin);
    readInputEvents (instance, in);
    askForRestart (instance);
}

const clap_plugin_params_t kParams {
    paramCount, paramGetInfo, paramGetValue, paramValueToText, paramTextToValue, paramFlush
};

bool CLAP_ABI pluginInit (const clap_plugin_t*) { return true; }
void CLAP_ABI pluginDestroy (const clap_plugin_t* plugin) { delete static_cast<Instance*> (plugin->plugin_data); }

bool CLAP_ABI pluginActivate (const clap_plugin_t* plugin, double, uint32_t, uint32_t)
{
    auto& instance = self (plugin);
    const auto latency = (uint32_t) instance.lookAhead;
    if (latency == instance.activeLatency) return true;
    instance.activeLatency = latency;
    if (instance.host != nullptr && instance.host->get_extension != nullptr)
        if (const auto* hostLatency = static_cast<const clap_host_latency_t*> (
                instance.host->get_extension (instance.host, CLAP_EXT_LATENCY));
            hostLatency != nullptr && hostLatency->changed != nullptr)
            hostLatency->changed (instance.host);
    return true;
}

void CLAP_ABI pluginDeactivate (const clap_plugin_t*) {}
bool CLAP_ABI pluginStart (const clap_plugin_t*) { return true; }
void CLAP_ABI pluginStop (const clap_plugin_t*) {}
void CLAP_ABI pluginReset (const clap_plugin_t*) {}

clap_process_status CLAP_ABI pluginProcess (const clap_plugin_t* plugin,
                                            const clap_process_t* process)
{
    if (process == nullptr || process->audio_inputs_count != 1
        || process->audio_outputs_count != 1
        || process->audio_inputs == nullptr || process->audio_outputs == nullptr)
        return CLAP_PROCESS_ERROR;

    const auto& in = process->audio_inputs[0];
    const auto& out = process->audio_outputs[0];
    if (in.channel_count != 2 || out.channel_count != 2
        || in.data32 == nullptr || out.data32 == nullptr
        || in.data32[0] == nullptr || in.data32[1] == nullptr
        || out.data32[0] == nullptr || out.data32[1] == nullptr)
        return CLAP_PROCESS_ERROR;

    auto& instance = self (plugin);
    readInputEvents (instance, process->in_events);

    for (uint32_t channel = 0; channel < 2; ++channel)
        for (uint32_t frame = 0; frame < process->frames_count; ++frame)
            out.data32[channel][frame] = in.data32[channel][frame];

    // request_restart is [thread-safe], so the audio thread may ask directly.
    askForRestart (instance);
    return CLAP_PROCESS_CONTINUE;
}

const void* CLAP_ABI pluginExtension (const clap_plugin_t*, const char* id)
{
    if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPorts;
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
        || std::strcmp (pluginId, kPluginId) != 0)
        return nullptr;

    auto* instance = new (std::nothrow) Instance;
    if (instance == nullptr) return nullptr;
    instance->host = host;

    auto& plugin = instance->plugin;
    plugin.desc = &kDescriptor;
    plugin.plugin_data = instance;
    plugin.init = pluginInit;
    plugin.destroy = pluginDestroy;
    plugin.activate = pluginActivate;
    plugin.deactivate = pluginDeactivate;
    plugin.start_processing = pluginStart;
    plugin.stop_processing = pluginStop;
    plugin.reset = pluginReset;
    plugin.process = pluginProcess;
    plugin.get_extension = pluginExtension;
    plugin.on_main_thread = pluginMainThread;
    return &plugin;
}

uint32_t CLAP_ABI pluginCount (const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* CLAP_ABI pluginDescriptor (const clap_plugin_factory_t*,
                                                           uint32_t index)
{
    return index == 0 ? &kDescriptor : nullptr;
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
