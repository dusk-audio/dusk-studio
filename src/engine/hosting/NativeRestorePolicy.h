#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace duskstudio::hosting
{
struct NativeRestoreFailure
{
    std::string format;
    std::string pluginName;
    std::string reason;
    int slotIndex = -1;
};

inline std::string nativePluginName (const std::string& path,
                                     const std::string& pluginId = {})
{
    try
    {
        const auto name = std::filesystem::u8path (path).stem().u8string();
        if (! name.empty()) return name;
    }
    catch (const std::filesystem::filesystem_error&)
    {
    }
    if (! pluginId.empty()) return pluginId;
    return path;
}

// A native plug-in that rejected its saved state must not remain live while
// the save path deliberately preserves (rather than refreshes) that state.
// Keeping the policy here makes the prepared and deferred restore paths use
// the same decision and gives every host format the same offline behaviour.
template <typename UnloadFn>
std::string enforceRestorePolicy (bool loadSucceeded,
                                  bool stateWasSupplied,
                                  bool stateWasAccepted,
                                  std::string_view loadError,
                                  std::size_t stateBytes,
                                  UnloadFn&& unload)
{
    if (loadSucceeded && (! stateWasSupplied || stateWasAccepted))
        return {};

    std::forward<UnloadFn> (unload)();

    if (! loadSucceeded)
    {
        std::string reason = "plug-in load failed";
        if (! loadError.empty())
        {
            reason += ": ";
            reason.append (loadError.data(), loadError.size());
        }
        reason += "; slot left offline";
        return reason;
    }

    std::string reason = "saved state was rejected (" + std::to_string (stateBytes)
                       + " bytes)";
    if (! loadError.empty())
    {
        reason += ": ";
        reason.append (loadError.data(), loadError.size());
    }
    reason += "; slot left offline to preserve the saved state";
    return reason;
}

// A reactivation failure is different from a failed session restore: an editor
// may still own native GUI handles into the existing instance. Silence it, but
// retain its identity until the UI can tear the editor down safely. A later
// successful reactivation can bring the same instance back online (notably
// after an offline bounce returns to the device sample rate).
template <typename QuarantineFn>
std::string enforceReactivationPolicy (bool succeeded,
                                       std::string_view error,
                                       QuarantineFn&& quarantine)
{
    if (succeeded)
        return {};

    std::forward<QuarantineFn> (quarantine)();
    std::string reason = "plug-in reactivation failed";
    if (! error.empty())
    {
        reason += ": ";
        reason.append (error.data(), error.size());
    }
    reason += "; slot left offline and instance retained for safe editor teardown";
    return reason;
}

// LV2 must re-instantiate at a new sample rate. A failure can therefore mean
// either that the replacement is alive but its carried state was rejected, or
// that there is no plug-in instance left at all. Only the first case is safe to
// retain for an attached editor; the second must invalidate the slot.
template <typename QuarantineFn, typename UnloadFn>
std::string enforceReactivationPolicy (bool succeeded,
                                       std::string_view error,
                                       bool hasLiveInstance,
                                       QuarantineFn&& quarantine,
                                       UnloadFn&& unload)
{
    if (succeeded)
        return {};

    if (hasLiveInstance)
        std::forward<QuarantineFn> (quarantine)();
    else
        std::forward<UnloadFn> (unload)();

    std::string reason = "plug-in reactivation failed";
    if (! error.empty())
    {
        reason += ": ";
        reason.append (error.data(), error.size());
    }
    reason += hasLiveInstance
        ? "; slot left offline and instance retained for safe editor teardown"
        : "; plug-in instance was lost and the slot was unloaded";
    return reason;
}
} // namespace duskstudio::hosting
