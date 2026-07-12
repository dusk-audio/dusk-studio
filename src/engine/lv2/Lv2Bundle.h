#pragma once

#include <string>
#include <vector>

namespace duskstudio::lv2
{
// One plugin advertised by an LV2 bundle's manifest.
struct PluginDesc
{
    std::string uri;    // the stable plugin URI - the id persisted in sessions
    std::string name;
    int  audioInputs  = 0;
    int  audioOutputs = 0;
    bool isInstrument = false;   // atom/MIDI input, no audio input, has audio output
};

// Owns a private LilvWorld, loads a single .lv2 bundle directory into it, and
// enumerates the plugins that bundle advertises (URI, name, audio-port counts).
// RAII: lilv_world_free on destruction. Message thread only - not real-time safe.
//
// LV2's analog of ClapBundle. lilv types stay out of this header (opaque void*
// handles); the instance layer includes lilv and casts. This stage only loads +
// enumerates; instance / process / editor land in later increments.
class Lv2Bundle
{
public:
    Lv2Bundle() = default;
    ~Lv2Bundle();
    Lv2Bundle (const Lv2Bundle&)            = delete;
    Lv2Bundle& operator= (const Lv2Bundle&) = delete;

    // Load the .lv2 bundle directory at `bundlePath`. False (+ errorOut) on failure;
    // the bundle is left unloaded. A successful load leaves the world alive.
    bool load (const std::string& bundlePath, std::string& errorOut);
    void unload();

    bool isLoaded() const noexcept { return worldHandle != nullptr; }

    const std::vector<PluginDesc>& plugins() const noexcept { return descriptors; }
    const std::string&             getPath() const noexcept { return bundlePath; }

    // Opaque accessors for the instance layer (keeps lilv out of this header):
    // world() is a LilvWorld*, pluginByUri() a const LilvPlugin* (null if absent).
    void*       world() const noexcept { return worldHandle; }
    const void* pluginByUri (const std::string& uri) const;

    // Build a PluginDesc from a lilv plugin (walks its ports once). Shared by the
    // bundle enumeration and the Lv2Scanner so the audio-effect / instrument
    // classification lives in one place. `world` is a LilvWorld*, `plugin` a
    // const LilvPlugin*.
    static PluginDesc describePlugin (void* world, const void* plugin);

private:
    void* worldHandle = nullptr;   // LilvWorld*
    std::vector<PluginDesc> descriptors;
    std::string bundlePath;
};
} // namespace duskstudio::lv2
