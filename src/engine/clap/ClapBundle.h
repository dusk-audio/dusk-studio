#pragma once

#include <string>
#include <vector>

// CLAP C-API types live in the global namespace; forward-declare the two the
// public surface needs so this header pulls in no CLAP headers.
struct clap_plugin_entry;
struct clap_plugin_factory;

namespace duskstudio::clap
{
// One plugin advertised by a .clap bundle's factory.
struct PluginDesc
{
    std::string id, name, vendor, version, description;
};

// Loads a .clap shared object (dlopen), runs its entry init(), and exposes the
// plugin factory + descriptors. RAII: deinit() + dlclose() on destruction.
// Message thread only — loading/unloading is not real-time safe.
//
// Foundation of the native CLAP host (replaces JUCE plugin hosting, aux-first).
// This stage only loads + enumerates; instance/process/editor land in later
// increments. See docs/native-clap-host-plan.md.
class ClapBundle
{
public:
    ClapBundle() = default;
    ~ClapBundle();
    ClapBundle (const ClapBundle&)            = delete;
    ClapBundle& operator= (const ClapBundle&) = delete;

    // Load the bundle at `path`. On any failure returns false and sets errorOut;
    // the bundle is left unloaded. A successful load leaves the entry initialised.
    bool load (const std::string& path, std::string& errorOut);
    void unload();

    bool isLoaded() const noexcept { return entry != nullptr; }

    const std::vector<PluginDesc>&  plugins()    const noexcept { return descriptors; }
    const ::clap_plugin_factory*    getFactory() const noexcept { return factory; }
    const std::string&              getPath()    const noexcept { return bundlePath; }

private:
    void* handle = nullptr;                          // dlopen handle
    const ::clap_plugin_entry*   entry   = nullptr;
    bool  initialised = false;                       // entry->init() succeeded
    const ::clap_plugin_factory* factory = nullptr;
    std::vector<PluginDesc> descriptors;
    std::string bundlePath;
};
} // namespace duskstudio::clap
