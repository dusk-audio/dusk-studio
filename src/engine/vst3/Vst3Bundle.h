#pragma once

#include <memory>
#include <string>
#include <vector>

namespace duskstudio::vst3
{
// One plugin class advertised by a .vst3 module's factory.
struct PluginDesc
{
    std::string id;      // class UID as a hex string — the stable id persisted in sessions
    std::string name;
    std::string vendor;
    std::string version;
    std::string subCategories;   // pipe-separated, e.g. "Fx|EQ" or "Instrument|Synth"
    bool isInstrument = false;   // subCategories carries "Instrument"
};

// Loads a .vst3 module (bundle directory or bare .so) through the Steinberg SDK's
// hosting Module and enumerates its audio-effect classes. VST3's analog of
// ClapBundle / Lv2Bundle: the module must stay loaded for as long as any instance
// created from it lives. Message thread only.
//
// SDK types stay out of this header (pImpl); the instance layer reaches the live
// module through the opaque accessor and casts.
class Vst3Bundle
{
public:
    Vst3Bundle();
    ~Vst3Bundle();
    Vst3Bundle (const Vst3Bundle&)            = delete;
    Vst3Bundle& operator= (const Vst3Bundle&) = delete;

    // Load the module at `modulePath`. False (+ errorOut) on failure; the bundle
    // is left unloaded. A successful load keeps the module resident.
    bool load (const std::string& modulePath, std::string& errorOut);
    void unload();

    bool isLoaded() const noexcept;

    const std::vector<PluginDesc>& plugins() const noexcept { return descriptors; }
    const std::string&             getPath() const noexcept { return modulePath; }

    // Opaque VST3::Hosting::Module* for the instance layer (null when unloaded).
    void* module() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::vector<PluginDesc> descriptors;
    std::string modulePath;
};
} // namespace duskstudio::vst3
