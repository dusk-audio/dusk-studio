#pragma once

#include "../PluginDescriptor.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace duskstudio::scanproto
{
inline constexpr std::string_view kPayloadBegin = "==DUSK_SCAN_BEGIN==";
inline constexpr std::string_view kPayloadEnd   = "==DUSK_SCAN_END==";

inline bool formatRequiresSandbox (std::string_view formatName)
{
    return formatName != "DuskMultisample";
}

inline std::string makePayload (const std::vector<PluginDescriptor>& found)
{
    nlohmann::ordered_json root {
        { "version", 1 },
        { "descriptors", nlohmann::ordered_json::array() }
    };
    for (const auto& descriptor : found)
        root["descriptors"].push_back (
            nlohmann::ordered_json::parse (descriptor.toJson()));

    return std::string (kPayloadBegin) + "\n" + root.dump() + "\n"
        + std::string (kPayloadEnd) + "\n";
}

inline std::string extractPayload (std::string_view childStdout)
{
    const auto begin = childStdout.find (kPayloadBegin);
    if (begin == std::string_view::npos)
        return {};
    const auto from = begin + kPayloadBegin.size();
    const auto end = childStdout.find (kPayloadEnd, from);
    if (end == std::string_view::npos)
        return {};

    auto payload = childStdout.substr (from, end - from);
    while (! payload.empty() && (payload.front() == '\n' || payload.front() == '\r'
                                  || payload.front() == ' ' || payload.front() == '\t'))
        payload.remove_prefix (1);
    while (! payload.empty() && (payload.back() == '\n' || payload.back() == '\r'
                                  || payload.back() == ' ' || payload.back() == '\t'))
        payload.remove_suffix (1);
    return std::string (payload);
}

inline std::optional<std::vector<PluginDescriptor>> parsePayload (std::string_view payload)
{
    const auto root = nlohmann::json::parse (payload, nullptr, false);
    if (! root.is_object())
        return std::nullopt;
    const auto version = root.find ("version");
    const auto descriptors = root.find ("descriptors");
    if (version == root.end()
        || descriptors == root.end() || ! descriptors->is_array())
        return std::nullopt;
    const bool versionMatches = version->is_number_unsigned()
        ? version->get<std::uint64_t>() == 1u
        : version->is_number_integer()
            && version->get<std::int64_t>() == 1;
    if (! versionMatches)
        return std::nullopt;

    std::vector<PluginDescriptor> parsed;
    parsed.reserve (descriptors->size());
    for (const auto& value : *descriptors)
    {
        PluginDescriptor descriptor;
        if (! PluginDescriptor::fromJson (value.dump(), descriptor))
            return std::nullopt;
        parsed.push_back (std::move (descriptor));
    }
    return parsed;
}
} // namespace duskstudio::scanproto
