#pragma once

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

// Guarded read accessors over nlohmann::json, replicating JUCE's var lenient
// coerce-or-default contract. nlohmann's get<>() throws on a type mismatch and
// const operator[] on a missing key is undefined, so session load — which
// parses untrusted, possibly hand-edited files — must never touch the json
// directly. Every read goes through here.
namespace dusk::json
{
using Json = nlohmann::json;

inline bool has (const Json& j, const char* key)
{
    return j.is_object() && j.contains (key);
}

// A missing / non-object child resolves to a shared empty object, so chained
// reads on it fall through to their defaults instead of dereferencing nothing.
inline const Json& child (const Json& j, const char* key)
{
    static const Json empty = Json::object();
    return has (j, key) && j[key].is_object() ? j[key] : empty;
}

inline const Json& array (const Json& j, const char* key)
{
    static const Json empty = Json::array();
    return has (j, key) && j[key].is_array() ? j[key] : empty;
}

inline bool getBool (const Json& j, const char* key, bool def)
{
    if (! has (j, key)) return def;
    const auto& v = j[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number())  return v.get<double>() != 0.0;
    return def;
}

inline double getDouble (const Json& j, const char* key, double def)
{
    if (! has (j, key)) return def;
    const auto& v = j[key];
    return v.is_number() ? v.get<double>() : def;
}

inline float getFloat (const Json& j, const char* key, float def)
{
    return (float) getDouble (j, key, (double) def);
}

inline int getInt (const Json& j, const char* key, int def)
{
    if (! has (j, key)) return def;
    const auto& v = j[key];
    return v.is_number() ? (int) v.get<double>() : def;
}

inline std::int64_t getInt64 (const Json& j, const char* key, std::int64_t def)
{
    if (! has (j, key)) return def;
    const auto& v = j[key];
    if (v.is_number_integer())  return v.get<std::int64_t>();
    if (v.is_number_unsigned()) return (std::int64_t) v.get<std::uint64_t>();
    if (v.is_number())          return (std::int64_t) v.get<double>();
    return def;
}

inline std::string getString (const Json& j, const char* key, const std::string& def = {})
{
    if (! has (j, key)) return def;
    const auto& v = j[key];
    return v.is_string() ? v.get<std::string>() : def;
}

// Finite-guarded float: rejects NaN / inf / out-of-float-range doubles from a
// corrupt file before narrowing (the narrowing cast would be UB). Mirrors the
// former finiteFloatOr helper.
inline float getFiniteFloat (const Json& j, const char* key, float fallback)
{
    if (! has (j, key)) return fallback;
    const auto& v = j[key];
    if (! v.is_number()) return fallback;
    const double d = v.get<double>();
    if (std::isfinite (d) && std::abs (d) <= (double) std::numeric_limits<float>::max())
        return (float) d;
    return fallback;
}
} // namespace dusk::json
