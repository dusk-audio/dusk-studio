#include <clap/clap.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

namespace
{
constexpr const char* kPluginId = "studio.dusk.test.multi-bus";
constexpr std::array<std::uint8_t, 8> kStateHeader {
    'D', 'S', 'K', 'C', 1, 0, 0, 0
};
constexpr const char* kFeatures[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
                                      CLAP_PLUGIN_FEATURE_STEREO, nullptr };

struct PluginData
{
    std::uint32_t processCount = 0;
};

const clap_plugin_descriptor_t kDescriptor {
    CLAP_VERSION_INIT,
    kPluginId,
    "Dusk multi-bus fixture",
    "Dusk Studio",
    "https://dusk.audio",
    "",
    "",
    "1.0.0",
    "Requires every advertised CLAP audio bus to be connected",
    kFeatures
};

uint32_t CLAP_ABI audioPortCount (const clap_plugin_t*, bool) { return 2; }

bool CLAP_ABI audioPortGet (const clap_plugin_t*, uint32_t index, bool isInput,
                            clap_audio_port_info_t* info)
{
    if (index >= 2 || info == nullptr) return false;
    *info = {};
    info->id = index;
    std::snprintf (info->name, sizeof (info->name), "%s %u",
                   isInput ? "Input" : "Output", index + 1);
    // Deliberately reproduce the malformed layout seen in the field: both
    // ports claim to be main. A compatible host selects the first and still
    // connects the later port as an auxiliary bus.
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t kAudioPorts { audioPortCount, audioPortGet };

bool writeAll (const clap_ostream_t* stream, const void* data, std::uint64_t size)
{
    if (stream == nullptr || stream->write == nullptr) return false;
    auto* bytes = static_cast<const std::uint8_t*> (data);
    std::uint64_t written = 0;
    while (written < size)
    {
        const auto n = stream->write (stream, bytes + written, size - written);
        if (n <= 0 || static_cast<std::uint64_t> (n) > size - written) return false;
        written += static_cast<std::uint64_t> (n);
    }
    return true;
}

bool readAll (const clap_istream_t* stream, void* data, std::uint64_t size)
{
    if (stream == nullptr || stream->read == nullptr) return false;
    auto* bytes = static_cast<std::uint8_t*> (data);
    std::uint64_t read = 0;
    while (read < size)
    {
        const auto n = stream->read (stream, bytes + read, size - read);
        if (n <= 0 || static_cast<std::uint64_t> (n) > size - read) return false;
        read += static_cast<std::uint64_t> (n);
    }
    return true;
}

bool CLAP_ABI stateSave (const clap_plugin_t* plugin, const clap_ostream_t* stream)
{
    const auto* data = static_cast<const PluginData*> (plugin->plugin_data);
    const std::array<std::uint8_t, 4> count {
        static_cast<std::uint8_t> (data->processCount),
        static_cast<std::uint8_t> (data->processCount >> 8),
        static_cast<std::uint8_t> (data->processCount >> 16),
        static_cast<std::uint8_t> (data->processCount >> 24)
    };
    return writeAll (stream, kStateHeader.data(), kStateHeader.size())
        && writeAll (stream, count.data(), count.size());
}

bool CLAP_ABI stateLoad (const clap_plugin_t* plugin, const clap_istream_t* stream)
{
    std::array<std::uint8_t, 8> header {};
    std::array<std::uint8_t, 4> count {};
    if (! readAll (stream, header.data(), header.size())
        || ! readAll (stream, count.data(), count.size())
        || header != kStateHeader)
        return false;

    auto* data = static_cast<PluginData*> (plugin->plugin_data);
    data->processCount = static_cast<std::uint32_t> (count[0])
                       | (static_cast<std::uint32_t> (count[1]) << 8)
                       | (static_cast<std::uint32_t> (count[2]) << 16)
                       | (static_cast<std::uint32_t> (count[3]) << 24);
    return true;
}

const clap_plugin_state_t kState { stateSave, stateLoad };

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
void CLAP_ABI pluginReset (const clap_plugin_t*) {}

clap_process_status CLAP_ABI pluginProcess (const clap_plugin_t* plugin,
                                            const clap_process_t* process)
{
    if (process == nullptr || process->audio_inputs_count != 2
        || process->audio_outputs_count != 2)
        return CLAP_PROCESS_ERROR;

    for (uint32_t port = 0; port < 2; ++port)
    {
        const auto& in = process->audio_inputs[port];
        const auto& out = process->audio_outputs[port];
        if (in.channel_count != 2 || out.channel_count != 2
            || in.data32 == nullptr || out.data32 == nullptr
            || in.data32[0] == nullptr || in.data32[1] == nullptr
            || out.data32[0] == nullptr || out.data32[1] == nullptr)
            return CLAP_PROCESS_ERROR;
    }

    for (uint32_t frame = 0; frame < process->frames_count; ++frame)
    {
        // The main bus passes through. Touch both auxiliary input/output
        // channels too: a host that omits those buffers fails deterministically.
        for (uint32_t channel = 0; channel < 2; ++channel)
        {
            process->audio_outputs[0].data32[channel][frame]
                = process->audio_inputs[0].data32[channel][frame];
            process->audio_outputs[1].data32[channel][frame]
                = process->audio_inputs[1].data32[channel][frame];
        }
    }
    ++static_cast<PluginData*> (plugin->plugin_data)->processCount;
    return CLAP_PROCESS_CONTINUE;
}

const void* CLAP_ABI pluginExtension (const clap_plugin_t*, const char* id)
{
    if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPorts;
    if (std::strcmp (id, CLAP_EXT_STATE) == 0) return &kState;
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

    auto* plugin = new (std::nothrow) clap_plugin_t {};
    auto* data = new (std::nothrow) PluginData {};
    if (plugin == nullptr || data == nullptr)
    {
        delete plugin;
        delete data;
        return nullptr;
    }

    plugin->desc = &kDescriptor;
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
