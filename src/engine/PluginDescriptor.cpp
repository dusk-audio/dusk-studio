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

nlohmann::ordered_json PluginDescriptor::toJsonObject() const
{
    const auto backendName = backend == PluginBackend::Native ? "native" : "juce_legacy";
    return {
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
}

std::string PluginDescriptor::toJson() const
{
    return toJsonObject().dump (
        -1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

bool PluginDescriptor::fromJsonObject (
    const nlohmann::json& object, PluginDescriptor& out) noexcept
{
    try
    {
        if (! object.is_object())
            return false;

        const auto versionIt = object.find ("version");
        const auto backendIt = object.find ("backend");
        if (versionIt == object.end()
            || (! versionIt->is_number_integer()
                && ! versionIt->is_number_unsigned())
            || backendIt == object.end() || ! backendIt->is_string())
            return false;

        PluginDescriptor parsed;
        int descriptorVersion = 0;
        if (! readOptionalInteger (object, "version", descriptorVersion)
            || descriptorVersion != kDescriptorVersion)
            return false;

        const auto backendName = backendIt->get<std::string>();
        if (backendName == "native")
            parsed.backend = PluginBackend::Native;
        else if (backendName == "juce_legacy")
            parsed.backend = PluginBackend::JuceLegacy;
        else
            return false;

        if (! readOptionalString (object, "name", parsed.name)
            || ! readOptionalString (
                object, "descriptive_name", parsed.descriptiveName)
            || ! readOptionalString (
                object, "manufacturer", parsed.manufacturer)
            || ! readOptionalString (object, "category", parsed.category)
            || ! readOptionalString (
                object, "plugin_version", parsed.version)
            || ! readOptionalString (
                object, "format_name", parsed.formatName)
            || ! readOptionalString (object, "location", parsed.location)
            || ! readOptionalString (object, "plugin_id", parsed.pluginId)
            || ! readOptionalInteger (object, "unique_id", parsed.uniqueId)
            || ! readOptionalInteger (
                object, "deprecated_uid", parsed.deprecatedUid)
            || ! readOptionalInteger (
                object, "num_input_channels", parsed.numInputChannels)
            || ! readOptionalInteger (
                object, "num_output_channels", parsed.numOutputChannels)
            || ! readOptionalInteger (
                object, "last_file_modification_ms",
                parsed.lastFileModificationMs)
            || ! readOptionalInteger (
                object, "last_info_update_ms", parsed.lastInfoUpdateMs)
            || ! readOptionalBool (
                object, "is_instrument", parsed.isInstrument)
            || ! readOptionalBool (
                object, "has_shared_container", parsed.hasSharedContainer)
            || ! readOptionalBool (
                object, "has_ara_extension", parsed.hasAraExtension))
            return false;

        out = std::move (parsed);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool PluginDescriptor::fromJson (
    const std::string& source, PluginDescriptor& out)
{
    const auto object = nlohmann::json::parse (source, nullptr, false);
    return fromJsonObject (object, out);
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
