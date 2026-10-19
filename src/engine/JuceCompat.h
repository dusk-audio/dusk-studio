#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginSearchPaths.h"

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

inline void primeCustomLv2 (juce::AudioPluginFormatManager& manager)
{
    const auto custom = pluginpaths::load();
    if (custom.empty()) return;
    for (auto* format : manager.getFormats())
        if (format != nullptr && format->getName() == "LV2")
        {
            // URI creation uses this format's world, including in fresh sandbox children.
            primeCustomLv2 (*format, custom);
            break;
        }
}
} // namespace duskstudio::juce_compat
