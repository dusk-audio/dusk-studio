#include <clap/clap.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

namespace
{
constexpr const char* kPluginId = "studio.dusk.test.no-window";
constexpr const char* kFeatures[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
                                      CLAP_PLUGIN_FEATURE_STEREO, nullptr };

#if defined(_WIN32)
constexpr const char* kPlatformWindowApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
constexpr const char* kPlatformWindowApi = CLAP_WINDOW_API_COCOA;
#else
constexpr const char* kPlatformWindowApi = CLAP_WINDOW_API_X11;
#endif

const clap_plugin_descriptor_t kDescriptor {
    CLAP_VERSION_INIT,
    kPluginId,
    "Dusk no-window fixture",
    "Dusk Studio",
    "https://dusk.audio",
    "",
    "",
    "1.0.0",
    "Advertises a GUI and parents no window into the host's container",
    kFeatures
};

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

bool CLAP_ABI guiIsApiSupported (const clap_plugin_t*, const char* api, bool isFloating)
{
    return ! isFloating && api != nullptr && std::strcmp (api, kPlatformWindowApi) == 0;
}

bool CLAP_ABI guiGetPreferredApi (const clap_plugin_t*, const char** api, bool* isFloating)
{
    if (api == nullptr || isFloating == nullptr) return false;
    *api = kPlatformWindowApi;
    *isFloating = false;
    return true;
}

// Every call below succeeds without a window ever existing: the host must
// notice the empty container by itself rather than by an error return.
bool CLAP_ABI guiCreate (const clap_plugin_t* plugin, const char* api, bool isFloating)
{
    return guiIsApiSupported (plugin, api, isFloating);
}

void CLAP_ABI guiDestroy (const clap_plugin_t*) {}

bool CLAP_ABI guiSetScale (const clap_plugin_t*, double) { return false; }

bool CLAP_ABI guiGetSize (const clap_plugin_t*, uint32_t* width, uint32_t* height)
{
    if (width == nullptr || height == nullptr) return false;
    *width = 400;
    *height = 300;
    return true;
}

bool CLAP_ABI guiCanResize (const clap_plugin_t*) { return false; }
bool CLAP_ABI guiGetResizeHints (const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; }
bool CLAP_ABI guiAdjustSize (const clap_plugin_t* plugin, uint32_t* width, uint32_t* height)
{
    return guiGetSize (plugin, width, height);
}
bool CLAP_ABI guiSetSize (const clap_plugin_t*, uint32_t, uint32_t) { return true; }
bool CLAP_ABI guiSetParent (const clap_plugin_t*, const clap_window_t*) { return true; }
bool CLAP_ABI guiSetTransient (const clap_plugin_t*, const clap_window_t*) { return false; }
void CLAP_ABI guiSuggestTitle (const clap_plugin_t*, const char*) {}
bool CLAP_ABI guiShow (const clap_plugin_t*) { return true; }
bool CLAP_ABI guiHide (const clap_plugin_t*) { return true; }

const clap_plugin_gui_t kGui {
    guiIsApiSupported, guiGetPreferredApi, guiCreate, guiDestroy, guiSetScale,
    guiGetSize, guiCanResize, guiGetResizeHints, guiAdjustSize, guiSetSize,
    guiSetParent, guiSetTransient, guiSuggestTitle, guiShow, guiHide
};

bool CLAP_ABI pluginInit (const clap_plugin_t*) { return true; }
void CLAP_ABI pluginDestroy (const clap_plugin_t* plugin) { delete plugin; }
bool CLAP_ABI pluginActivate (const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
void CLAP_ABI pluginDeactivate (const clap_plugin_t*) {}
bool CLAP_ABI pluginStart (const clap_plugin_t*) { return true; }
void CLAP_ABI pluginStop (const clap_plugin_t*) {}
void CLAP_ABI pluginReset (const clap_plugin_t*) {}

clap_process_status CLAP_ABI pluginProcess (const clap_plugin_t*,
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

    for (uint32_t channel = 0; channel < 2; ++channel)
        for (uint32_t frame = 0; frame < process->frames_count; ++frame)
            out.data32[channel][frame] = in.data32[channel][frame];
    return CLAP_PROCESS_CONTINUE;
}

const void* CLAP_ABI pluginExtension (const clap_plugin_t*, const char* id)
{
    if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPorts;
    if (std::strcmp (id, CLAP_EXT_GUI) == 0) return &kGui;
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
    if (plugin == nullptr) return nullptr;

    plugin->desc = &kDescriptor;
    plugin->plugin_data = nullptr;
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
