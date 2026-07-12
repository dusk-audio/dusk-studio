#include "Vst3Bundle.h"

#include <public.sdk/source/vst/hosting/module.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>

#include <algorithm>

namespace duskstudio::vst3
{
struct Vst3Bundle::Impl
{
    VST3::Hosting::Module::Ptr module;
};

Vst3Bundle::Vst3Bundle() : impl (std::make_unique<Impl>()) {}
Vst3Bundle::~Vst3Bundle() { unload(); }

bool Vst3Bundle::isLoaded() const noexcept { return impl->module != nullptr; }
void* Vst3Bundle::module() const noexcept { return impl->module.get(); }

bool Vst3Bundle::load (const std::string& path, std::string& errorOut)
{
    unload();

    std::string err;
    auto mod = VST3::Hosting::Module::create (path, err);
    if (! mod)
    {
        errorOut = err.empty() ? ("failed to load module: " + path) : err;
        return false;
    }

    descriptors.clear();
    for (const auto& ci : mod->getFactory().classInfos())
    {
        // Only audio-effect classes are hostable plugins; the factory also lists
        // controller / service classes that don't stand alone.
        if (ci.category() != kVstAudioEffectClass)
            continue;

        PluginDesc d;
        d.id            = ci.ID().toString();
        d.name          = ci.name();
        d.vendor        = ci.vendor();
        d.version       = ci.version();
        d.subCategories = ci.subCategoriesString();
        const auto& subs = ci.subCategories();
        d.isInstrument = std::find (subs.begin(), subs.end(), "Instrument") != subs.end();
        descriptors.push_back (std::move (d));
    }

    if (descriptors.empty())
    {
        errorOut = "no audio-effect classes in module: " + path;
        return false;   // module Ptr goes out of scope -> unloaded
    }

    impl->module = std::move (mod);
    modulePath   = path;
    return true;
}

void Vst3Bundle::unload()
{
    impl->module.reset();
    descriptors.clear();
    modulePath.clear();
}
} // namespace duskstudio::vst3
