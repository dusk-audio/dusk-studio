#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace duskstudio
{
enum class PluginBackend
{
    Native,
    JuceLegacy
};

struct PluginDescriptor
{
    std::string name;
    std::string descriptiveName;
    std::string manufacturer;
    std::string category;
    std::string version;
    std::string formatName;
    PluginBackend backend { PluginBackend::JuceLegacy };
    std::string location;
    std::string pluginId;
    std::int32_t uniqueId { 0 };
    std::int32_t deprecatedUid { 0 };
    std::int32_t numInputChannels { 0 };
    std::int32_t numOutputChannels { 0 };
    std::int64_t lastFileModificationMs { 0 };
    std::int64_t lastInfoUpdateMs { 0 };
    bool isInstrument { false };
    bool hasSharedContainer { false };
    bool hasAraExtension { false };

    nlohmann::ordered_json toJsonObject() const;
    static bool fromJsonObject (const nlohmann::json& object,
                                PluginDescriptor& out) noexcept;
    std::string toJson() const;
    static bool fromJson (const std::string& source, PluginDescriptor& out);

    bool operator== (const PluginDescriptor& other) const noexcept
    {
        return name == other.name
            && descriptiveName == other.descriptiveName
            && manufacturer == other.manufacturer
            && category == other.category
            && version == other.version
            && formatName == other.formatName
            && backend == other.backend
            && location == other.location
            && pluginId == other.pluginId
            && uniqueId == other.uniqueId
            && deprecatedUid == other.deprecatedUid
            && numInputChannels == other.numInputChannels
            && numOutputChannels == other.numOutputChannels
            && lastFileModificationMs == other.lastFileModificationMs
            && lastInfoUpdateMs == other.lastInfoUpdateMs
            && isInstrument == other.isInstrument
            && hasSharedContainer == other.hasSharedContainer
            && hasAraExtension == other.hasAraExtension;
    }
};

// The instantiated plugin is authoritative for metadata/classification. The
// reload descriptor remains authoritative only for identity fields that may
// have been normalised before JUCE instantiation.
PluginDescriptor mergeLoadedPluginDescriptor (
    const PluginDescriptor& reloadDescriptor,
    PluginDescriptor instantiatedDescriptor);
} // namespace duskstudio
