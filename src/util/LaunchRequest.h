#pragma once

#include "SingleInstance.h"
#include "../foundation/Json.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace duskstudio
{
struct LaunchRequest
{
    std::string sessionPath;
    std::string activationToken;

    std::optional<std::string> encode() const
    {
        if (sessionPath.size() > single_instance::kMaxPayloadBytes
            || activationToken.size() > single_instance::kMaxPayloadBytes
            || sessionPath.find ('\0') != std::string::npos
            || activationToken.find ('\0') != std::string::npos)
            return {};
        try
        {
            auto payload = dusk::json::Json { { "version", 1 }, { "sessionPath", sessionPath },
                                             { "activationToken", activationToken } }.dump();
            if (payload.size() <= single_instance::kMaxPayloadBytes)
                return payload;
        }
        catch (const dusk::json::Json::exception&) {}
        return {};
    }

    // The launcher's token only lets the running window take focus; one that cannot
    // travel is dropped rather than failing a launch that has a session to hand over.
    std::optional<std::string> encodeForHandoff() const
    {
        if (auto payload = encode())
            return payload;
        return LaunchRequest { sessionPath, {} }.encode();
    }

    // A version before this request sent the resolved path, quoted, or nothing,
    // which `legacySessionPath` parses the way a command line is parsed.
    template <typename LegacySessionPath>
    static LaunchRequest receive (std::string_view payload, LegacySessionPath&& legacySessionPath)
    {
        if (auto request = decode (payload))
            return std::move (*request);
        return { legacySessionPath (payload), {} };
    }

    static std::optional<LaunchRequest> decode (std::string_view payload)
    {
        if (payload.size() > single_instance::kMaxPayloadBytes
            || payload.find ('\0') != std::string_view::npos)
            return {};
        const auto json = dusk::json::Json::parse (payload.begin(), payload.end(), nullptr, false);
        if (! json.is_object() || json.size() != 3
            || ! dusk::json::has (json, "version") || ! json["version"].is_number_integer()
            || json["version"] != 1
            || ! dusk::json::has (json, "sessionPath") || ! json["sessionPath"].is_string()
            || ! dusk::json::has (json, "activationToken") || ! json["activationToken"].is_string())
            return {};
        LaunchRequest request { dusk::json::getString (json, "sessionPath"),
                                dusk::json::getString (json, "activationToken") };
        if (request.sessionPath.find ('\0') != std::string::npos
            || request.activationToken.find ('\0') != std::string::npos)
            return {};
        return request;
    }
};

inline std::string takeLaunchActivationToken()
{
   #if defined (__linux__)
    const auto* const value = std::getenv ("XDG_ACTIVATION_TOKEN");
    const std::string token = value != nullptr ? value : "";
    ::unsetenv ("XDG_ACTIVATION_TOKEN");
    return token;
   #else
    return {};
   #endif
}
}
