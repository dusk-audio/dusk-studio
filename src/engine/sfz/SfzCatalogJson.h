#pragma once

#include "../../foundation/Json.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace duskstudio::sfz::detail
{
using Json = dusk::json::Json;

inline std::string sanitizeJsonKeyForDiagnostic (std::string_view key)
{
    constexpr std::size_t maximumBytes = 64;
    const auto keptBytes = std::min (key.size(), maximumBytes);

    std::string sanitized;
    sanitized.reserve (keptBytes + 3);
    for (std::size_t i = 0; i < keptBytes; ++i)
    {
        const auto byte = static_cast<unsigned char> (key[i]);
        sanitized.push_back (byte >= 0x20 && byte <= 0x7e
                                 ? static_cast<char> (byte)
                                 : '?');
    }
    if (key.size() > keptBytes)
        sanitized += "...";
    return sanitized;
}

struct JsonParseResult
{
    std::optional<Json> root;
    std::string error;

    explicit operator bool() const noexcept { return root.has_value(); }
};

inline JsonParseResult parseJsonRejectingDuplicateKeys (
    std::string_view source, std::string_view errorPrefix)
{
    std::vector<std::unordered_set<std::string>> objectKeys;
    bool duplicateKeyFound = false;
    std::string duplicateKey;
    const auto callback = [&objectKeys, &duplicateKeyFound, &duplicateKey] (
        int, Json::parse_event_t event, Json& parsed)
    {
        if (event == Json::parse_event_t::object_start)
        {
            objectKeys.emplace_back();
        }
        else if (event == Json::parse_event_t::key && ! objectKeys.empty())
        {
            const auto& key = parsed.get_ref<const std::string&>();
            if (! objectKeys.back().insert (key).second && ! duplicateKeyFound)
            {
                duplicateKeyFound = true;
                duplicateKey = key;
            }
        }
        else if (event == Json::parse_event_t::object_end && ! objectKeys.empty())
        {
            objectKeys.pop_back();
        }
        return true;
    };

    auto root = Json::parse (source.begin(), source.end(), callback, false);
    if (root.is_discarded())
        return { std::nullopt, std::string (errorPrefix) + ": malformed JSON" };
    if (duplicateKeyFound)
        return { std::nullopt,
                 std::string (errorPrefix) + ": duplicate object key '"
                     + sanitizeJsonKeyForDiagnostic (duplicateKey) + "'" };
    return { std::move (root), {} };
}
} // namespace duskstudio::sfz::detail
