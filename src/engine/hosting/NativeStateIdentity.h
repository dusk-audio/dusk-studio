#pragma once

#include <string>

namespace duskstudio::hosting
{
// Identity of the native plug-in instance that produced a carried state blob.
// `format` prevents cross-host reuse, `location` identifies the bundle or AU
// component, and `pluginId` distinguishes multiple plug-ins in one bundle.
struct NativeStateIdentity
{
    std::string format;
    std::string location;
    std::string pluginId;

    bool operator== (const NativeStateIdentity& other) const noexcept
    {
        return format == other.format
            && location == other.location
            && pluginId == other.pluginId;
    }
};

// A failed capture may retain the last good blob only when the live plug-in is
// exactly the owner that produced it. Call this before publishing the live
// identity so a replacement cannot inherit its predecessor's opaque bytes.
template <typename State>
bool retainStateForLiveIdentity (const NativeStateIdentity& carried,
                                 const NativeStateIdentity& live,
                                 State& state)
{
    if (carried == live) return true;
    state.clear();
    return false;
}
}
