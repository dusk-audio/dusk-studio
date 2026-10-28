#include <clap/clap.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

namespace
{
constexpr const char* kPluginId = "studio.dusk.test.param-touch";
constexpr const char* kFeatures[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
                                      CLAP_PLUGIN_FEATURE_STEREO, nullptr };

// Ids deliberately far from the indices: a host that confuses the two binds the
// wrong parameter, and the fixture is here to catch exactly that.
constexpr clap_id kAlphaId = 40;
constexpr clap_id kBetaId  = 41;
constexpr clap_id kMeterId = 42;
constexpr uint32_t kParamCount = 3;

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

// Handed out as parameter cookies, so a test can tell whether the host passes
// back what get_info issued or nullptr.
char kCookies[kParamCount] {};

// What the host-facing test extension reports. Tests declare the same layout.
struct HostProbe
{
    std::uint32_t processCalls = 0;
    std::uint32_t flushCalls = 0;
    bool processOnAudioThread = false;
    bool flushOnAudioThread = false;
    bool flushOnMainThread = false;
    bool overlapped = false;
    const void* lastCookie = nullptr;
};

struct Instance
{
    clap_plugin_t plugin {};
    const clap_host_t* host = nullptr;
    const clap_host_thread_check_t* threadCheck = nullptr;
    const clap_host_state_t* hostState = nullptr;
    const clap_host_params_t* hostParams = nullptr;
    double values[kParamCount] { 0.0, 0.0, 0.0 };
    // Set by an incoming parameter change, consumed by the next process(): the
    // id whose value the plug-in is about to announce on its output queue.
    clap_id reportId = CLAP_INVALID_ID;
    clap_event_param_value_t outputValue {};
    clap_event_param_mod_t outputModulation {};
    clap_event_param_gesture_t outputGesture {};
    const clap_event_header_t* outputEvent = nullptr;
    // An edit made in the plug-in's own GUI: it reaches the audio side, and
    // the host, on the next process() or flush().
    clap_id guiId = CLAP_INVALID_ID;
    double guiValue = 0.0;
    void (*reentry) (void*) = nullptr;
    void* reentryContext = nullptr;
    bool inProcess = false;
    bool inFlush = false;
    bool markDirtyOnDestroy = false;
    bool markDirtyOnSave = false;
    HostProbe probe;
};

Instance& self (const clap_plugin_t* plugin)
{
    return *static_cast<Instance*> (plugin->plugin_data);
}

int indexForId (clap_id id)
{
    if (id == kAlphaId) return 0;
    if (id == kBetaId)  return 1;
    if (id == kMeterId) return 2;
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
    info->id = index == 0 ? kAlphaId : index == 1 ? kBetaId : kMeterId;
    info->flags = index == 2 ? CLAP_PARAM_IS_READONLY : CLAP_PARAM_IS_AUTOMATABLE;
    std::snprintf (info->name, sizeof (info->name), "%s", index == 0 ? "Alpha" : index == 1 ? "Beta" : "Meter");
    std::snprintf (info->module, sizeof (info->module), "%s", "");
    info->cookie = &kCookies[index];
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
        instance.probe.lastCookie = event->cookie;
        const int index = indexForId (event->param_id);
        if (index < 0 || index == 2) continue;
        instance.values[index] = event->value;
        instance.reportId = event->param_id;
    }
}

void applyGuiEdit (Instance& instance)
{
    const int index = indexForId (instance.guiId);
    if (index >= 0)
    {
        instance.values[index] = instance.guiValue;
        instance.reportId = instance.guiId;
    }
    instance.guiId = CLAP_INVALID_ID;
}

void runReentry (Instance& instance)
{
    if (instance.reentry == nullptr) return;
    const auto fn = instance.reentry;
    instance.reentry = nullptr;
    fn (instance.reentryContext);
}

bool onHostAudioThread (const Instance& instance)
{
    return instance.threadCheck != nullptr && instance.threadCheck->is_audio_thread (instance.host);
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

struct EventControl
{
    bool (CLAP_ABI *queue) (const clap_plugin_t*, const clap_event_header_t*);
};

bool CLAP_ABI queueOutputEvent (const clap_plugin_t* plugin, const clap_event_header_t* event)
{
    if (event == nullptr) return false;
    auto& instance = self (plugin);
    if (instance.outputEvent != nullptr) return false;
    switch (event->type)
    {
        case CLAP_EVENT_PARAM_VALUE:
            if (event->size != sizeof (instance.outputValue)) return false;
            std::memcpy (&instance.outputValue, event, event->size);
            instance.outputEvent = &instance.outputValue.header;
            break;
        case CLAP_EVENT_PARAM_MOD:
            if (event->size != sizeof (instance.outputModulation)) return false;
            std::memcpy (&instance.outputModulation, event, event->size);
            instance.outputEvent = &instance.outputModulation.header;
            break;
        case CLAP_EVENT_PARAM_GESTURE_BEGIN:
        case CLAP_EVENT_PARAM_GESTURE_END:
            if (event->size != sizeof (instance.outputGesture)) return false;
            std::memcpy (&instance.outputGesture, event, event->size);
            instance.outputEvent = &instance.outputGesture.header;
            break;
        default: return false;
    }
    return true;
}

void emitQueuedOutput (Instance& instance, const clap_output_events_t* events)
{
    const auto* event = instance.outputEvent;
    if (event == nullptr || events == nullptr || events->try_push == nullptr) return;
    if (event->space_id == CLAP_CORE_EVENT_SPACE_ID && event->type == CLAP_EVENT_PARAM_VALUE)
    {
        const auto& value = instance.outputValue;
        if (value.note_id == -1 && value.port_index == -1 && value.channel == -1 && value.key == -1)
        {
            const int index = indexForId (value.param_id);
            if (index >= 0) instance.values[index] = value.value;
        }
    }
    events->try_push (events, event);
    instance.outputEvent = nullptr;
}

const EventControl kEventControl { queueOutputEvent };

void CLAP_ABI paramFlush (const clap_plugin_t* plugin, const clap_input_events_t* in,
                          const clap_output_events_t* out)
{
    auto& instance = self (plugin);
    auto& probe = instance.probe;
    ++probe.flushCalls;
    probe.flushOnAudioThread = onHostAudioThread (instance);
    probe.flushOnMainThread = instance.threadCheck != nullptr
                           && instance.threadCheck->is_main_thread (instance.host);
    probe.overlapped = probe.overlapped || instance.inProcess || instance.inFlush;
    instance.inFlush = true;
    runReentry (instance);
    applyGuiEdit (instance);
    readInputEvents (instance, in);
    announceMove (instance, out);
    emitQueuedOutput (instance, out);
    instance.inFlush = false;
}

const clap_plugin_params_t kParams {
    paramCount, paramGetInfo, paramGetValue, paramValueToText, paramTextToValue, paramFlush
};

bool CLAP_ABI controlMarkDirty (const clap_plugin_t* plugin);

bool CLAP_ABI stateSave (const clap_plugin_t* plugin, const clap_ostream_t* stream)
{
    if (stream == nullptr || stream->write == nullptr) return false;
    std::array<std::uint8_t, 20> bytes { 'P', 'T', 1, 0 };
    std::memcpy (bytes.data() + 4, self (plugin).values, 2 * sizeof (double));
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const auto written = stream->write (stream, bytes.data() + offset, bytes.size() - offset);
        if (written <= 0 || (std::uint64_t) written > bytes.size() - offset) return false;
        offset += (std::size_t) written;
    }
    // A change landing while the host reads the state, which the bytes above may lack.
    if (std::exchange (self (plugin).markDirtyOnSave, false))
        controlMarkDirty (plugin);
    return true;
}

bool CLAP_ABI stateLoad (const clap_plugin_t* plugin, const clap_istream_t* stream)
{
    if (stream == nullptr || stream->read == nullptr) return false;
    std::array<std::uint8_t, 20> bytes {};
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const auto read = stream->read (stream, bytes.data() + offset, bytes.size() - offset);
        if (read <= 0 || (std::uint64_t) read > bytes.size() - offset) return false;
        offset += (std::size_t) read;
    }
    if (bytes[0] != 'P' || bytes[1] != 'T' || bytes[2] != 1 || bytes[3] != 0) return false;
    std::array<double, 2> values {};
    std::memcpy (values.data(), bytes.data() + 4, 2 * sizeof (double));
    for (const double value : values)
        if (! std::isfinite (value) || value < 0.0 || value > 1.0) return false;
    auto& instance = self (plugin);
    instance.values[0] = values[0];
    instance.values[1] = values[1];
    instance.reportId = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_state_t kState { stateSave, stateLoad };

// Drives the host side of the state and params extensions the way a plug-in's
// GUI or preset browser would.
struct HostControl
{
    bool (CLAP_ABI *markDirty) (const clap_plugin_t*);
    bool (CLAP_ABI *requestFlush) (const clap_plugin_t*);
    bool (CLAP_ABI *rescan) (const clap_plugin_t*, std::uint32_t flags);
    bool (CLAP_ABI *clear) (const clap_plugin_t*, clap_id id, std::uint32_t flags);
    // Records a GUI edit for the next process() or flush(), then asks the host
    // for a flush. False when the host has no params extension to ask.
    bool (CLAP_ABI *editFromGui) (const clap_plugin_t*, clap_id id, double value);
    // Runs fn once, inside the next process() or flush().
    void (CLAP_ABI *reenter) (const clap_plugin_t*, void (*fn) (void*), void* context);
    void (CLAP_ABI *markDirtyOnDestroy) (const clap_plugin_t*);
    const HostProbe* (CLAP_ABI *probe) (const clap_plugin_t*);
    // Marks the state dirty from inside the next state save, once.
    void (CLAP_ABI *markDirtyOnSave) (const clap_plugin_t*);
};

bool CLAP_ABI controlMarkDirty (const clap_plugin_t* plugin)
{
    const auto& instance = self (plugin);
    if (instance.hostState == nullptr || instance.hostState->mark_dirty == nullptr) return false;
    instance.hostState->mark_dirty (instance.host);
    return true;
}

bool CLAP_ABI controlRequestFlush (const clap_plugin_t* plugin)
{
    const auto& instance = self (plugin);
    if (instance.hostParams == nullptr || instance.hostParams->request_flush == nullptr) return false;
    instance.hostParams->request_flush (instance.host);
    return true;
}

bool CLAP_ABI controlRescan (const clap_plugin_t* plugin, std::uint32_t flags)
{
    const auto& instance = self (plugin);
    if (instance.hostParams == nullptr || instance.hostParams->rescan == nullptr) return false;
    instance.hostParams->rescan (instance.host, flags);
    return true;
}

bool CLAP_ABI controlClear (const clap_plugin_t* plugin, clap_id id, std::uint32_t flags)
{
    const auto& instance = self (plugin);
    if (instance.hostParams == nullptr || instance.hostParams->clear == nullptr) return false;
    instance.hostParams->clear (instance.host, id, flags);
    return true;
}

bool CLAP_ABI controlEditFromGui (const clap_plugin_t* plugin, clap_id id, double value)
{
    auto& instance = self (plugin);
    instance.guiId = id;
    instance.guiValue = value;
    return controlRequestFlush (plugin);
}

void CLAP_ABI controlReenter (const clap_plugin_t* plugin, void (*fn) (void*), void* context)
{
    auto& instance = self (plugin);
    instance.reentry = fn;
    instance.reentryContext = context;
}

void CLAP_ABI controlMarkDirtyOnDestroy (const clap_plugin_t* plugin)
{
    self (plugin).markDirtyOnDestroy = true;
}

const HostProbe* CLAP_ABI controlProbe (const clap_plugin_t* plugin) { return &self (plugin).probe; }

void CLAP_ABI controlMarkDirtyOnSave (const clap_plugin_t* plugin)
{
    self (plugin).markDirtyOnSave = true;
}

const HostControl kHostControl {
    controlMarkDirty, controlRequestFlush, controlRescan, controlClear,
    controlEditFromGui, controlReenter, controlMarkDirtyOnDestroy, controlProbe,
    controlMarkDirtyOnSave
};

bool CLAP_ABI pluginInit (const clap_plugin_t* plugin)
{
    auto& instance = self (plugin);
    const auto* host = instance.host;
    instance.threadCheck = static_cast<const clap_host_thread_check_t*> (
        host->get_extension (host, CLAP_EXT_THREAD_CHECK));
    instance.hostState = static_cast<const clap_host_state_t*> (
        host->get_extension (host, CLAP_EXT_STATE));
    instance.hostParams = static_cast<const clap_host_params_t*> (
        host->get_extension (host, CLAP_EXT_PARAMS));
    return true;
}

void CLAP_ABI pluginDestroy (const clap_plugin_t* plugin)
{
    auto* instance = static_cast<Instance*> (plugin->plugin_data);
    if (instance->markDirtyOnDestroy)
    {
        controlMarkDirty (plugin);
        controlRequestFlush (plugin);
    }
    delete instance;
}
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
    auto& probe = instance.probe;
    ++probe.processCalls;
    probe.processOnAudioThread = onHostAudioThread (instance);
    probe.overlapped = probe.overlapped || instance.inProcess || instance.inFlush;
    instance.inProcess = true;
    runReentry (instance);
    applyGuiEdit (instance);
    readInputEvents (instance, process->in_events);

    for (uint32_t channel = 0; channel < 2; ++channel)
        for (uint32_t frame = 0; frame < process->frames_count; ++frame)
            out.data32[channel][frame] = in.data32[channel][frame];

    announceMove (instance, process->out_events);
    emitQueuedOutput (instance, process->out_events);
    instance.inProcess = false;
    return CLAP_PROCESS_CONTINUE;
}

const void* CLAP_ABI pluginExtension (const clap_plugin_t*, const char* id)
{
    if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPorts;
    if (std::strcmp (id, CLAP_EXT_PARAMS) == 0) return &kParams;
    if (std::strcmp (id, CLAP_EXT_STATE) == 0) return &kState;
    if (std::strcmp (id, "studio.dusk.test.param-touch-events") == 0) return &kEventControl;
    if (std::strcmp (id, "studio.dusk.test.param-touch-host") == 0) return &kHostControl;
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
