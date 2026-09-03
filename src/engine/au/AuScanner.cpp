#include "AuScanner.h"

#include "AuBundle.h"

namespace duskstudio::au
{
std::vector<PluginDescriptor> AuScanner::scan (const std::atomic<bool>* abort)
{
    std::vector<PluginDescriptor> rows;
    for (const auto& plugin : AuBundle::enumerate (abort))
    {
        if (abort != nullptr && abort->load (std::memory_order_relaxed)) break;
        PluginDescriptor descriptor;
        descriptor.name = plugin.name;
        descriptor.descriptiveName = plugin.name;
        descriptor.manufacturer = plugin.manufacturer;
        descriptor.category = plugin.category;
        descriptor.version = plugin.version;
        descriptor.formatName = "AudioUnit";
        descriptor.backend = PluginBackend::Native;
        descriptor.location = plugin.id.toString();
        descriptor.pluginId = descriptor.location;
        descriptor.uniqueId = static_cast<std::int32_t> (
            plugin.id.type ^ plugin.id.subtype ^ plugin.id.manufacturer);
        descriptor.deprecatedUid = descriptor.uniqueId;
        descriptor.isInstrument = plugin.isInstrument;
        rows.push_back (std::move (descriptor));
    }
    return rows;
}
} // namespace duskstudio::au
