#include "NativePluginCache.h"

#include <nlohmann/json.hpp>

namespace duskstudio::nativecache
{
namespace
{
constexpr int kCacheVersion = 1;
}

std::string serialize (const std::vector<PluginDescriptor>& descriptors)
{
    nlohmann::ordered_json root {
        { "version", kCacheVersion },
        { "descriptors", nlohmann::ordered_json::array() }
    };
    for (const auto& descriptor : descriptors)
        root["descriptors"].push_back (
            nlohmann::ordered_json::parse (descriptor.toJson()));
    return root.dump();
}

bool parse (std::string_view source, const LocationExists& locationExists,
            std::vector<PluginDescriptor>& out)
{
    const auto root = nlohmann::json::parse (source, nullptr, false);
    if (! root.is_object())
        return false;

    const auto version = root.find ("version");
    const auto descriptors = root.find ("descriptors");
    if (version == root.end()
        || descriptors == root.end() || ! descriptors->is_array())
        return false;
    const bool versionMatches = version->is_number_unsigned()
        ? version->get<std::uint64_t>() == static_cast<std::uint64_t> (kCacheVersion)
        : version->is_number_integer()
            && version->get<std::int64_t>() == static_cast<std::int64_t> (kCacheVersion);
    if (! versionMatches)
        return false;

    std::vector<PluginDescriptor> parsed;
    parsed.reserve (descriptors->size());
    for (const auto& value : *descriptors)
    {
        PluginDescriptor descriptor;
        if (! PluginDescriptor::fromJson (value.dump(), descriptor))
            continue;
        if (locationExists && ! locationExists (descriptor.location))
            continue;
        parsed.push_back (std::move (descriptor));
    }

    out = std::move (parsed);
    return true;
}
} // namespace duskstudio::nativecache
