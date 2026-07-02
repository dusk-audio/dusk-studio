#pragma once

#include "Lv2Bundle.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace duskstudio::lv2
{
// One discoverable LV2 plugin: the bundle directory it lives in plus its descriptor.
struct ScannedLv2
{
    juce::String bundlePath;   // absolute path to the .lv2 bundle directory
    PluginDesc   desc;
};

// Discovers installed LV2 plugins via lilv, honouring $LV2_PATH or the spec
// defaults (~/.lv2, /usr/lib/lv2, /usr/local/lib/lv2, distro multiarch dirs).
// Message thread only. Discovery is manifest-only: lilv parses the Turtle
// metadata and never dlopens the plugin binary, so a broken .so cannot crash
// the scan — no sandbox child needed (unlike binary-loading formats).
class Lv2Scanner
{
public:
    static std::vector<ScannedLv2> scan();
};
} // namespace duskstudio::lv2
