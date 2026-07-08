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
    if (v.is_number())  { const double d = v.get<double>(); return d < 0.0 || d > 0.0; }
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
    // Unsigned first: is_number_integer() is also true for unsigned, so a
    // uint past INT_MAX would otherwise reach get<int64_t>() and wrap negative.
    if (v.is_number_unsigned())
    {
        const auto u = v.get<std::uint64_t>();
        return u <= (std::uint64_t) std::numeric_limits<int>::max() ? (int) u : def;
    }
    if (v.is_number_integer())  return (int) v.get<std::int64_t>();
    if (v.is_number_float())
    {
        // Range-check before narrowing: casting an out-of-int double is UB.
        const double d = v.get<double>();
        if (std::isfinite (d) && d >= (double) std::numeric_limits<int>::min()
                              && d <= (double) std::numeric_limits<int>::max())
            return (int) d;
    }
    return def;
}

inline std::int64_t getInt64 (const Json& j, const char* key, std::int64_t def)
{
    if (! has (j, key)) return def;
    const auto& v = j[key];
    // Unsigned first (is_number_integer() is also true for unsigned): a uint
    // past INT64_MAX would otherwise reach get<int64_t>() and wrap negative.
    if (v.is_number_unsigned())
    {
        const auto u = v.get<std::uint64_t>();
        return u <= (std::uint64_t) std::numeric_limits<std::int64_t>::max()
                   ? (std::int64_t) u : def;
    }
    if (v.is_number_integer())  return v.get<std::int64_t>();
    if (v.is_number_float())
    {
        // Range-check before narrowing: casting a double past int64 range is UB.
        // 2^63 (int64 max + 1) is exactly representable as double; use a strict
        // upper bound so 9223372036854775808.0 is rejected.
        const double d = v.get<double>();
        if (std::isfinite (d) && d >= -9223372036854775808.0 && d < 9223372036854775808.0)
            return (std::int64_t) d;
    }
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
