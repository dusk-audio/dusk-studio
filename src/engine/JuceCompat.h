#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginSearchPaths.h"

#include <map>
#include <mutex>

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

inline auto pluginSearchLocations (juce::AudioPluginFormat& format,
                                   const pluginpaths::Roots& custom)
{
    auto locations = format.getDefaultLocationsToSearch();
    if (format.getName() == "AudioUnit") return locations;
    const auto add = [&] (const auto& path) { locations.add (juce::File (path.u8string())); };
    for (const auto& path : custom) add (path);
    locations.removeRedundantPaths();
    return locations;
}

inline void primeCustomLv2 (juce::AudioPluginFormat& format, const pluginpaths::Roots& custom,
                            const std::atomic<bool>* abort = nullptr)
{
    if (format.getName() != "LV2") return;
    for (const auto& bundle : pluginpaths::findBundles (custom, ".lv2", abort))
    {
        if (abort != nullptr && abort->load (std::memory_order_relaxed)) return;
        juce::OwnedArray<juce::PluginDescription> descriptions;
        // Explicit bundles avoid nonrecursive LV2_PATH lookup and separator ambiguity.
        format.findAllTypesForFile (descriptions, bundle.u8string());
    }
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
    friend void primeCustomLv2 (FormatManager&, const pluginpaths::Roots&, bool, const std::atomic<bool>*);
};

inline void primeCustomLv2 (FormatManager& manager, const pluginpaths::Roots& custom,
                            bool refresh = false, const std::atomic<bool>* abort = nullptr)
{
    const std::lock_guard<std::mutex> lock (manager.lv2PrimingMutex);
    const auto cancelled = [&] { return abort != nullptr && abort->load (std::memory_order_relaxed); };
    if (cancelled()) return;
    FormatManager::PrimedRoots state { pluginpaths::normalize (custom), {} };
    for (const auto& root : state.roots)
    {
        std::error_code error;
        state.available.push_back (std::filesystem::is_directory (root, error));
    }
    for (auto* format : manager.getFormats())
        if (format != nullptr && format->getName() == "LV2")
        {
            const auto primed = manager.primedLv2Roots.find (format);
            if (! refresh && primed != manager.primedLv2Roots.end()
                && primed->second.roots == state.roots && primed->second.available == state.available) continue;
            manager.primedLv2Roots.erase (format);
            primeCustomLv2 (*format, state.roots, abort);
            if (cancelled()) return;
            manager.primedLv2Roots.emplace (format, state);
        }
}

inline void primeCustomLv2 (FormatManager& manager)
{
    primeCustomLv2 (manager, pluginpaths::load());
}
} // namespace duskstudio::juce_compat
