#include "ClapBundle.h"

#include <clap/clap.h>

#include <dlfcn.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>

#include <climits>
#include <filesystem>
#endif

namespace duskstudio::clap
{
#if defined(__APPLE__)
namespace
{
bool resolveBundleExecutable (const std::string& path,
                              std::string& executablePath,
                              std::string& errorOut)
{
    std::error_code ec;
    if (! std::filesystem::is_directory (std::filesystem::u8path (path), ec))
    {
        executablePath = path;
        return true;
    }

    const auto* bytes = reinterpret_cast<const UInt8*> (path.data());
    auto* bundleUrl = CFURLCreateFromFileSystemRepresentation (
        kCFAllocatorDefault, bytes, static_cast<CFIndex> (path.size()), true);
    if (bundleUrl == nullptr)
    {
        errorOut = "could not create a URL for the CLAP bundle";
        return false;
    }

    auto* bundle = CFBundleCreate (kCFAllocatorDefault, bundleUrl);
    CFRelease (bundleUrl);
    if (bundle == nullptr)
    {
        errorOut = "could not open the CLAP bundle";
        return false;
    }

    auto* executableUrl = CFBundleCopyExecutableURL (bundle);
    CFRelease (bundle);
    if (executableUrl == nullptr)
    {
        errorOut = "CLAP bundle has no executable";
        return false;
    }

    // Against the base: CFBundleCopyExecutableURL hands back a URL relative to the
    // bundle, so a plain CFURLCopyFileSystemPath yields the bare executable NAME -
    // dlopen would then search the dyld paths instead of the bundle.
    UInt8 executableBuffer[PATH_MAX] {};
    const bool resolved = CFURLGetFileSystemRepresentation (
        executableUrl, true, executableBuffer, static_cast<CFIndex> (sizeof executableBuffer));
    CFRelease (executableUrl);
    if (! resolved)
    {
        errorOut = "could not resolve the CLAP bundle executable path";
        return false;
    }

    executablePath = reinterpret_cast<const char*> (executableBuffer);
    return true;
}
} // namespace
#endif

bool PluginDesc::isInstrument() const
{
    for (const auto& f : features)
        if (f == CLAP_PLUGIN_FEATURE_INSTRUMENT) return true;
    return false;
}

ClapBundle::~ClapBundle() { unload(); }

bool ClapBundle::load (const std::string& path, std::string& errorOut)
{
    unload();
    bundlePath = path;

#if defined(__APPLE__)
    std::string executablePath;
    if (! resolveBundleExecutable (path, executablePath, errorOut))
    {
        unload();
        return false;
    }
    const char* libraryPath = executablePath.c_str();
#else
    const char* libraryPath = path.c_str();
#endif

    // RTLD_LOCAL so a plugin's symbols don't leak into our (or another plugin's)
    // global namespace; RTLD_NOW so missing symbols surface here, not mid-process.
    handle = dlopen (libraryPath, RTLD_LOCAL | RTLD_NOW);
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
    initialised = true;   // only now is it valid to call entry->deinit()

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
        if (d->features != nullptr)
            for (const char* const* f = d->features; *f != nullptr; ++f)
                pd.features.emplace_back (*f);
        descriptors.push_back (std::move (pd));
    }
    return true;
}

void ClapBundle::unload()
{
    descriptors.clear();
    factory = nullptr;
    // deinit() is only valid after a successful init() - failure paths that set
    // `entry` from dlsym but never initialised it must not call it.
    if (entry != nullptr && initialised && entry->deinit != nullptr)
        entry->deinit();
    initialised = false;
    entry = nullptr;
    if (handle != nullptr)
    {
        dlclose (handle);
        handle = nullptr;
    }
    bundlePath.clear();
}
} // namespace duskstudio::clap
