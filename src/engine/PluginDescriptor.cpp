#include "PluginDescriptor.h"

#include <nlohmann/json.hpp>
#include <limits>
#include <type_traits>

namespace duskstudio
{
namespace
{
constexpr int kDescriptorVersion = 1;

bool readOptionalString (const nlohmann::json& object, const char* key,
                         std::string& value)
{
    const auto it = object.find (key);
    if (it == object.end())
        return true;
    if (! it->is_string())
        return false;
    value = it->get<std::string>();
    return true;
}

bool readOptionalBool (const nlohmann::json& object, const char* key, bool& value)
{
    const auto it = object.find (key);
    if (it == object.end())
        return true;
    if (! it->is_boolean())
        return false;
    value = it->get<bool>();
    return true;
}

template <typename T>
bool readOptionalInteger (const nlohmann::json& object, const char* key, T& value)
{
    static_assert (std::is_integral_v<T> && std::is_signed_v<T>);

    const auto it = object.find (key);
    if (it == object.end())
        return true;
    if (! it->is_number_integer() && ! it->is_number_unsigned())
        return false;

    if (it->is_number_unsigned())
    {
        const auto candidate = it->get<std::uint64_t>();
        if (candidate > static_cast<std::uint64_t> (std::numeric_limits<T>::max()))
            return false;
        value = static_cast<T> (candidate);
        return true;
    }

    const auto candidate = it->get<std::int64_t>();
    if (candidate < static_cast<std::int64_t> (std::numeric_limits<T>::min())
        || candidate > static_cast<std::int64_t> (std::numeric_limits<T>::max()))
        return false;
    value = static_cast<T> (candidate);
    return true;
}
} // namespace

std::string PluginDescriptor::toJson() const
{
    const auto backendName = backend == PluginBackend::Native ? "native" : "juce_legacy";
    const nlohmann::ordered_json value {
        { "version", kDescriptorVersion },
        { "name", name },
        { "descriptive_name", descriptiveName },
        { "manufacturer", manufacturer },
        { "category", category },
        { "plugin_version", version },
        { "format_name", formatName },
        { "backend", backendName },
        { "location", location },
        { "plugin_id", pluginId },
        { "unique_id", uniqueId },
        { "deprecated_uid", deprecatedUid },
        { "num_input_channels", numInputChannels },
        { "num_output_channels", numOutputChannels },
        { "last_file_modification_ms", lastFileModificationMs },
        { "last_info_update_ms", lastInfoUpdateMs },
        { "is_instrument", isInstrument },
        { "has_shared_container", hasSharedContainer },
        { "has_ara_extension", hasAraExtension }
    };
    return value.dump();
}

bool PluginDescriptor::fromJson (const std::string& source, PluginDescriptor& out)
{
    const auto value = nlohmann::json::parse (source, nullptr, false);
    if (! value.is_object())
        return false;

    const auto versionIt = value.find ("version");
    const auto backendIt = value.find ("backend");
    if (versionIt == value.end()
        || (! versionIt->is_number_integer() && ! versionIt->is_number_unsigned())
        || backendIt == value.end() || ! backendIt->is_string())
        return false;

    PluginDescriptor parsed;
    int descriptorVersion = 0;
    if (! readOptionalInteger (value, "version", descriptorVersion)
        || descriptorVersion != kDescriptorVersion)
        return false;

    const auto backendName = backendIt->get<std::string>();
    if (backendName == "native")
        parsed.backend = PluginBackend::Native;
    else if (backendName == "juce_legacy")
        parsed.backend = PluginBackend::JuceLegacy;
    else
        return false;

    if (! readOptionalString (value, "name", parsed.name)
        || ! readOptionalString (value, "descriptive_name", parsed.descriptiveName)
        || ! readOptionalString (value, "manufacturer", parsed.manufacturer)
        || ! readOptionalString (value, "category", parsed.category)
        || ! readOptionalString (value, "plugin_version", parsed.version)
        || ! readOptionalString (value, "format_name", parsed.formatName)
        || ! readOptionalString (value, "location", parsed.location)
        || ! readOptionalString (value, "plugin_id", parsed.pluginId)
        || ! readOptionalInteger (value, "unique_id", parsed.uniqueId)
        || ! readOptionalInteger (value, "deprecated_uid", parsed.deprecatedUid)
        || ! readOptionalInteger (value, "num_input_channels", parsed.numInputChannels)
        || ! readOptionalInteger (value, "num_output_channels", parsed.numOutputChannels)
        || ! readOptionalInteger (value, "last_file_modification_ms", parsed.lastFileModificationMs)
        || ! readOptionalInteger (value, "last_info_update_ms", parsed.lastInfoUpdateMs)
        || ! readOptionalBool (value, "is_instrument", parsed.isInstrument)
        || ! readOptionalBool (value, "has_shared_container", parsed.hasSharedContainer)
        || ! readOptionalBool (value, "has_ara_extension", parsed.hasAraExtension))
        return false;

    out = std::move (parsed);
    return true;
}

PluginDescriptor mergeLoadedPluginDescriptor (
    const PluginDescriptor& reloadDescriptor,
    PluginDescriptor instantiatedDescriptor)
{
    instantiatedDescriptor.backend = reloadDescriptor.backend;
    instantiatedDescriptor.location = reloadDescriptor.location;
    instantiatedDescriptor.pluginId = reloadDescriptor.pluginId;
    return instantiatedDescriptor;
}
} // namespace duskstudio
