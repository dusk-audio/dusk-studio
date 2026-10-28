#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginSearchPaths.h"

#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace duskstudio::juce_compat
{
// One-call shim hiding the AudioPluginFormatManager::addDefaultFormats
// vs the wayland fork's free addDefaultFormatsToManager() split.
// DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS is set by CMake at configure time
// (CMakeLists.txt scans the JUCE source for the free function symbol)
// so this picks the right API for any combination of platform + JUCE
// version. No __linux__ branch at the call site.
//
// Why CMake-time detection over SFINAE: the plugdata-team fork marks
// the member `= delete`, which passes SFINAE then fails at the call
// site. The upstream JUCE lacks the free function entirely, so
// `decltype(addDefaultFormatsToManager(...))` fails lookup at
// template definition time rather than substitution - also unusable
// for SFINAE. CMake-time text scan is the only clean cross-shape
// detection.
inline void addDefaultFormats (juce::AudioPluginFormatManager& fm)
{
   #if DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS
    juce::addDefaultFormatsToManager (fm);
   #else
    fm.addDefaultFormats();
   #endif
    for (auto* format : fm.getFormats())
        if (format != nullptr && format->getName() == "LV2")
        {
            // JUCE loads embedded LV2 specifications after its initial class table.
            format->searchPathsForPlugins (format->getDefaultLocationsToSearch(), true, false);
        }
}

// JUCE's VST3 and LV2 hosts cache a host-side parameter change and write it into
// the plug-in only inside the next process call, so state read before that call
// lacks it. Its AudioUnit host sets the parameter on the unit at once.
inline bool defersParameterChangesToProcess (const std::string& formatName)
{
    return formatName == "VST3" || formatName == "LV2";
}

// JUCE's VST3 host reports every restartComponent as a program change, whatever
// its flags - a latency re-announcement among them. A real program change there
// arrives as a change of the program parameter.
inline bool programChangeReportsMeanState (const std::string& formatName)
{
    return formatName != "VST3";
}

// The folders a format's own search may walk. Custom folders stay out of the
// VST3 search: JUCE's walk follows directory links with no record of where it
// has been and never sees a cancel request. The native VST3 scan, whose rows
// replace the JUCE ones, reaches them through the loop-safe findBundles.
inline auto pluginSearchLocations (juce::AudioPluginFormat& format,
                                   const pluginpaths::Roots& custom)
{
    auto locations = format.getDefaultLocationsToSearch();
    if (format.getName() == "AudioUnit" || format.getName() == "VST3") return locations;
    const auto add = [&] (const auto& path) { locations.add (juce::File (path.u8string())); };
    for (const auto& path : custom) add (path);
    locations.removeRedundantPaths();
    return locations;
}

// Returns how many descriptions the identifiers produced. A bundle path loads
// that bundle into the format's world; explicit bundles avoid nonrecursive
// LV2_PATH lookup and separator ambiguity.
inline int describeLv2 (juce::AudioPluginFormat& format, const std::vector<std::string>& identifiers,
                        const std::atomic<bool>* abort = nullptr)
{
    juce::OwnedArray<juce::PluginDescription> descriptions;
    for (const auto& identifier : identifiers)
    {
        if (abort != nullptr && abort->load (std::memory_order_relaxed)) break;
        format.findAllTypesForFile (descriptions, identifier);
    }
    return descriptions.size();
}

inline std::vector<std::string> bundleIdentifiers (const pluginpaths::Roots& bundles)
{
    std::vector<std::string> identifiers;
    for (const auto& bundle : bundles) identifiers.push_back (bundle.u8string());
    return identifiers;
}

class FormatManager : public juce::AudioPluginFormatManager
{
    struct PrimedRoots
    {
        pluginpaths::Roots roots;
        std::vector<bool> available;
    };
    std::mutex lv2PrimingMutex;
    std::map<const void*, PrimedRoots> primedLv2Roots;
    pluginpaths::Roots walkedLv2Bundles;
    std::map<const void*, std::set<std::filesystem::path>> loadedLv2Bundles;
    friend bool primeCustomLv2 (FormatManager&, const pluginpaths::Roots&, bool, const std::atomic<bool>*);
    friend void primeLv2Plugin (FormatManager&, const std::string&);
    friend pluginpaths::Roots customLv2Bundles (FormatManager&);
    friend void restoreCustomLv2Bundles (FormatManager&, pluginpaths::Roots);
};

// False when cancelled or when the folder walk hit an error. Neither records
// anything, so a later call retries and the last complete bundle list survives.
inline bool primeCustomLv2 (FormatManager& manager, const pluginpaths::Roots& custom,
                            bool refresh = false, const std::atomic<bool>* abort = nullptr)
{
    const std::lock_guard<std::mutex> lock (manager.lv2PrimingMutex);
    const auto cancelled = [&] { return abort != nullptr && abort->load (std::memory_order_relaxed); };
    if (cancelled()) return false;
    FormatManager::PrimedRoots state { pluginpaths::normalize (custom), {} };
    for (const auto& root : state.roots)
    {
        std::error_code error;
        state.available.push_back (std::filesystem::is_directory (root, error));
    }
    std::optional<pluginpaths::Roots> bundles;
    bool walkError = false;
    for (auto* format : manager.getFormats())
        if (format != nullptr && format->getName() == "LV2")
        {
            const auto primed = manager.primedLv2Roots.find (format);
            if (! refresh && primed != manager.primedLv2Roots.end()
                && primed->second.roots == state.roots && primed->second.available == state.available) continue;
            manager.primedLv2Roots.erase (format);
            if (! bundles) bundles = pluginpaths::findBundles (state.roots, ".lv2", abort, &walkError);
            describeLv2 (*format, bundleIdentifiers (*bundles), abort);
            if (cancelled()) return false;
            manager.loadedLv2Bundles[format].insert (bundles->begin(), bundles->end());
            if (! walkError) manager.primedLv2Roots.emplace (format, state);
        }
    if (bundles && ! walkError) manager.walkedLv2Bundles = std::move (*bundles);
    return ! walkError;
}

// Loading a plug-in never walks the custom folders: a URI the format already
// knows needs nothing, and an unknown one is looked for only in the bundles
// the last complete walk found.
inline void primeLv2Plugin (FormatManager& manager, const std::string& uri)
{
    const std::lock_guard<std::mutex> lock (manager.lv2PrimingMutex);
    for (auto* format : manager.getFormats())
        if (format != nullptr && format->getName() == "LV2")
        {
            if (describeLv2 (*format, { uri }) != 0) continue;
            auto& loaded = manager.loadedLv2Bundles[format];
            for (const auto& bundle : manager.walkedLv2Bundles)
            {
                if (loaded.count (bundle) != 0) continue;
                describeLv2 (*format, bundleIdentifiers ({ bundle }));
                loaded.insert (bundle);
                if (describeLv2 (*format, { uri }) != 0) break;
            }
        }
}

inline pluginpaths::Roots customLv2Bundles (FormatManager& manager)
{
    const std::lock_guard<std::mutex> lock (manager.lv2PrimingMutex);
    return manager.walkedLv2Bundles;
}

inline void restoreCustomLv2Bundles (FormatManager& manager, pluginpaths::Roots bundles)
{
    const std::lock_guard<std::mutex> lock (manager.lv2PrimingMutex);
    manager.walkedLv2Bundles = std::move (bundles);
}
} // namespace duskstudio::juce_compat
