#pragma once

#include "Vst3Bundle.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace duskstudio::vst3
{
// One discoverable VST3 plugin: the module it lives in plus its descriptor.
struct ScannedVst3
{
    juce::String bundlePath;   // absolute path to the .vst3 bundle
    PluginDesc   desc;
};

// Discovers installed VST3 plugins. Message thread only (each module is dlopen'd
// to read its factory, same trade-off as ClapScanner). Linux search paths follow
// the VST3 spec: $VST3_PATH (':'-separated), ~/.vst3, /usr/lib/vst3,
// /usr/local/lib/vst3.
class Vst3Scanner
{
public:
    // Existing default search directories for this platform (skips missing ones).
    static std::vector<juce::File> defaultSearchPaths();

    // Collect *.vst3 bundles under each directory. On Linux a .vst3 is a bundle
    // DIRECTORY (Contents/<arch>-linux/*.so) or, for old builds, a bare .so.
    static std::vector<juce::File> findVst3Bundles (const std::vector<juce::File>& dirs);

    // Load every discovered module and gather its advertised effect classes.
    // Modules that fail to load are skipped silently — a broken .vst3 must not
    // abort the scan.
    static std::vector<ScannedVst3> scan (const std::vector<juce::File>& dirs);
    static std::vector<ScannedVst3> scan() { return scan (defaultSearchPaths()); }
};
} // namespace duskstudio::vst3
