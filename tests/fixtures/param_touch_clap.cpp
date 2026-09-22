#include <clap/clap.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace
{
constexpr const char* kPluginId = "studio.dusk.test.param-touch";
constexpr const char* kFeatures[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
                                      CLAP_PLUGIN_FEATURE_STEREO, nullptr };

// Ids deliberately far from the indices: a host that confuses the two binds the
// wrong parameter, and the fixture is here to catch exactly that.
constexpr clap_id kAlphaId = 40;
constexpr clap_id kBetaId  = 41;
constexpr uint32_t kParamCount = 2;

const clap_plugin_descriptor_t kDescriptor {
    CLAP_VERSION_INIT,
    kPluginId,
    "Dusk param-touch fixture",
    "Dusk Studio",
    "https://dusk.audio",
    "",
    "",
    "1.0.0",
    "Reports parameter moves back to the host the way a plug-in GUI does",
    kFeatures
};

struct Instance
{
    clap_plugin_t plugin {};
    double values[kParamCount] { 0.0, 0.0 };
    // Set by an incoming parameter change, consumed by the next process(): the
    // id whose value the plug-in is about to announce on its output queue.
    clap_id reportId = CLAP_INVALID_ID;
};

Instance& self (const clap_plugin_t* plugin)
{
    return *static_cast<Instance*> (plugin->plugin_data);
}

int indexForId (clap_id id)
{
    if (id == kAlphaId) return 0;
    if (id == kBetaId)  return 1;
    return -1;
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

uint32_t CLAP_ABI paramCount (const clap_plugin_t*) { return kParamCount; }

bool CLAP_ABI paramGetInfo (const clap_plugin_t*, uint32_t index, clap_param_info_t* info)
{
    if (index >= kParamCount || info == nullptr) return false;
    *info = {};
    info->id = index == 0 ? kAlphaId : kBetaId;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    std::snprintf (info->name, sizeof (info->name), "%s", index == 0 ? "Alpha" : "Beta");
    std::snprintf (info->module, sizeof (info->module), "%s", "");
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = 0.0;
    return true;
}

bool CLAP_ABI paramGetValue (const clap_plugin_t* plugin, clap_id id, double* out)
{
    const int index = indexForId (id);
    if (index < 0 || out == nullptr) return false;
    *out = self (plugin).values[index];
    return true;
}

bool CLAP_ABI paramValueToText (const clap_plugin_t*, clap_id, double value,
                                char* out, uint32_t size)
{
    if (out == nullptr || size == 0) return false;
    std::snprintf (out, size, "%.3f", value);
    return true;
}

bool CLAP_ABI paramTextToValue (const clap_plugin_t*, clap_id, const char* text, double* out)
{
    if (text == nullptr || out == nullptr) return false;
    *out = std::atof (text);
    return true;
}

// The host queues changes and applies them on the next block, so an input
// parameter event is the fixture's trigger for announcing the move back.
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
        const int index = indexForId (event->param_id);
        if (index < 0) continue;
        instance.values[index] = event->value;
        instance.reportId = event->param_id;
    }
}

// What a plug-in with a GUI emits when the user moves a knob: a gesture around
// a parameter value on the output queue. The host reads this to know which
// parameter was touched last.
void announceMove (Instance& instance, const clap_output_events_t* events)
{
    if (events == nullptr || events->try_push == nullptr
        || instance.reportId == CLAP_INVALID_ID)
        return;

    const int index = indexForId (instance.reportId);
    if (index < 0) { instance.reportId = CLAP_INVALID_ID; return; }

    clap_event_param_gesture_t gesture {};
    gesture.header.size = sizeof (gesture);
    gesture.header.time = 0;
    gesture.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    gesture.header.type = CLAP_EVENT_PARAM_GESTURE_BEGIN;
    gesture.header.flags = 0;
    gesture.param_id = instance.reportId;
    events->try_push (events, &gesture.header);

    clap_event_param_value_t value {};
    value.header.size = sizeof (value);
    value.header.time = 0;
    value.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    value.header.type = CLAP_EVENT_PARAM_VALUE;
    value.header.flags = 0;
    value.param_id = instance.reportId;
    value.cookie = nullptr;
    value.note_id = -1;
    value.port_index = -1;
    value.channel = -1;
    value.key = -1;
    value.value = instance.values[index];
    events->try_push (events, &value.header);

    gesture.header.type = CLAP_EVENT_PARAM_GESTURE_END;
    events->try_push (events, &gesture.header);

    instance.reportId = CLAP_INVALID_ID;
}

void CLAP_ABI paramFlush (const clap_plugin_t* plugin, const clap_input_events_t* in,
                          const clap_output_events_t* out)
{
    auto& instance = self (plugin);
    readInputEvents (instance, in);
    announceMove (instance, out);
}

const clap_plugin_params_t kParams {
    paramCount, paramGetInfo, paramGetValue, paramValueToText, paramTextToValue, paramFlush
};

bool CLAP_ABI pluginInit (const clap_plugin_t*) { return true; }
void CLAP_ABI pluginDestroy (const clap_plugin_t* plugin) { delete static_cast<Instance*> (plugin->plugin_data); }
bool CLAP_ABI pluginActivate (const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
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

    announceMove (instance, process->out_events);
    return CLAP_PROCESS_CONTINUE;
}

const void* CLAP_ABI pluginExtension (const clap_plugin_t*, const char* id)
{
    if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPorts;
    if (std::strcmp (id, CLAP_EXT_PARAMS) == 0) return &kParams;
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
