#pragma once

#include "PluginDescriptor.h"

#include <filesystem>
#include <vector>

#if DUSKSTUDIO_HAS_NATIVE_CLAP
 #include "clap/ClapBundle.h"
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
 #include "vst3/Vst3Bundle.h"
#endif

namespace duskstudio::nativescan
{
#if DUSKSTUDIO_HAS_NATIVE_CLAP
inline void appendClapRows (const std::filesystem::path& bundle,
                            std::vector<PluginDescriptor>& into)
{
    clap::ClapBundle loaded;
    std::string error;
    if (! loaded.load (bundle.string(), error))
        return;
    for (const auto& plugin : loaded.plugins())
    {
        PluginDescriptor descriptor;
        descriptor.name = plugin.name;
        descriptor.manufacturer = plugin.vendor;
        descriptor.version = plugin.version;
        descriptor.formatName = "CLAP";
        descriptor.backend = PluginBackend::Native;
        descriptor.location = bundle.string();
        descriptor.pluginId = plugin.id;
        descriptor.isInstrument = plugin.isInstrument();
        into.push_back (std::move (descriptor));
    }
}
#endif

#if DUSKSTUDIO_HAS_NATIVE_VST3
inline void appendVst3Rows (const std::filesystem::path& bundle,
                            std::vector<PluginDescriptor>& into)
{
    vst3::Vst3Bundle loaded;
    std::string error;
    if (! loaded.load (bundle.string(), error))
        return;
    for (const auto& plugin : loaded.plugins())
    {
        PluginDescriptor descriptor;
        descriptor.name = plugin.name;
        descriptor.manufacturer = plugin.vendor;
        descriptor.version = plugin.version;
        descriptor.formatName = "VST3";
        descriptor.backend = PluginBackend::Native;
        descriptor.location = bundle.string();
        descriptor.pluginId = plugin.id;
        descriptor.isInstrument = plugin.isInstrument;
        into.push_back (std::move (descriptor));
    }
}
#endif
} // namespace duskstudio::nativescan
