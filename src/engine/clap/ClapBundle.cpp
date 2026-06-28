#include "ClapBundle.h"

#include <clap/clap.h>

#include <dlfcn.h>

namespace duskstudio::clap
{
ClapBundle::~ClapBundle() { unload(); }

bool ClapBundle::load (const std::string& path, std::string& errorOut)
{
    unload();
    bundlePath = path;

    // RTLD_LOCAL so a plugin's symbols don't leak into our (or another plugin's)
    // global namespace; RTLD_NOW so missing symbols surface here, not mid-process.
    handle = dlopen (path.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (handle == nullptr)
    {
        const char* e = dlerror();
        errorOut = std::string ("dlopen failed: ") + (e != nullptr ? e : "unknown");
        bundlePath.clear();
        return false;
    }

    // A .clap exports `const clap_plugin_entry_t clap_entry`.
    entry = reinterpret_cast<const ::clap_plugin_entry*> (dlsym (handle, "clap_entry"));
    if (entry == nullptr)              { errorOut = "no clap_entry symbol";        unload(); return false; }
    if (! clap_version_is_compatible (entry->clap_version))
                                       { errorOut = "incompatible CLAP version";   unload(); return false; }
    if (entry->init == nullptr || ! entry->init (path.c_str()))
                                       { errorOut = "clap entry init() failed";    unload(); return false; }

    factory = reinterpret_cast<const ::clap_plugin_factory*> (
        entry->get_factory != nullptr ? entry->get_factory (CLAP_PLUGIN_FACTORY_ID) : nullptr);
    if (factory == nullptr)            { errorOut = "no plugin factory";           unload(); return false; }

    const uint32_t count = factory->get_plugin_count != nullptr
                             ? factory->get_plugin_count (factory) : 0;
    descriptors.reserve (count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto* d = factory->get_plugin_descriptor != nullptr
                          ? factory->get_plugin_descriptor (factory, i) : nullptr;
        if (d == nullptr) continue;
        PluginDesc pd;
        pd.id          = d->id          != nullptr ? d->id          : "";
        pd.name        = d->name        != nullptr ? d->name        : "";
        pd.vendor      = d->vendor      != nullptr ? d->vendor      : "";
        pd.version     = d->version     != nullptr ? d->version     : "";
        pd.description = d->description != nullptr ? d->description : "";
        descriptors.push_back (std::move (pd));
    }
    return true;
}

void ClapBundle::unload()
{
    descriptors.clear();
    factory = nullptr;
    if (entry != nullptr && entry->deinit != nullptr)
        entry->deinit();
    entry = nullptr;
    if (handle != nullptr)
    {
        dlclose (handle);
        handle = nullptr;
    }
    bundlePath.clear();
}
} // namespace duskstudio::clap
