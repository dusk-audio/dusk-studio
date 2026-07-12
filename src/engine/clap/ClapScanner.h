#pragma once

#include "ClapBundle.h"

#include <filesystem>
#include <string>
#include <vector>

namespace duskstudio::clap
{
// One discoverable CLAP plugin: the bundle file it lives in plus its descriptor.
struct ScannedClap
{
    std::string bundlePath;   // absolute path to the .clap file
    PluginDesc  desc;         // id / name / vendor / version / description
};

// Discovers installed CLAP plugins. Message thread only (each bundle is dlopen'd
// to read its factory). Linux search paths follow the CLAP spec:
//   $CLAP_PATH (':'-separated), ~/.clap, /usr/lib/clap, /usr/local/lib/clap.
// A .clap on Linux is a single shared object; bundle directories (macOS/Windows)
// are out of scope here.
class ClapScanner
{
public:
    // Existing default search directories for this platform (skips missing ones).
    static std::vector<std::filesystem::path> defaultSearchPaths();

    // Recursively collect *.clap files under each directory.
    static std::vector<std::filesystem::path> findClapFiles (const std::vector<std::filesystem::path>& dirs);

    // Load every discovered bundle and gather its advertised plugins. Bundles that
    // fail to load are skipped silently - a broken .clap must not abort the scan.
    static std::vector<ScannedClap> scan (const std::vector<std::filesystem::path>& dirs);
    static std::vector<ScannedClap> scan() { return scan (defaultSearchPaths()); }
};
} // namespace duskstudio::clap
