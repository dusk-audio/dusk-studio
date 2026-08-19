#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "ClapBundle.h"

#include <clap/clap.h>

#if ! defined(_WIN32)
#include <dlfcn.h>
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>

#include <climits>
#include <filesystem>
#endif

namespace duskstudio::clap
{
#if defined(_WIN32)
namespace
{
// Identity and error text stay UTF-8; only the loader call itself goes UTF-16.
std::wstring widenUtf8 (const std::string& utf8)
{
    if (utf8.empty()) return {};
    const int n = ::MultiByteToWideChar (CP_UTF8, 0, utf8.data(),
                                         (int) utf8.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring wide ((size_t) n, L'\0');
    ::MultiByteToWideChar (CP_UTF8, 0, utf8.data(), (int) utf8.size(),
                           wide.data(), n);
    return wide;
}

std::string lastErrorUtf8()
{
    const DWORD code = ::GetLastError();
    wchar_t* buffer = nullptr;
    const DWORD len = ::FormatMessageW (
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*> (&buffer), 0, nullptr);
    std::string text = "error " + std::to_string (code);
    if (len > 0 && buffer != nullptr)
    {
        const int n = ::WideCharToMultiByte (CP_UTF8, 0, buffer, (int) len,
                                             nullptr, 0, nullptr, nullptr);
        if (n > 0)
        {
            std::string message ((size_t) n, '\0');
            ::WideCharToMultiByte (CP_UTF8, 0, buffer, (int) len,
                                   message.data(), n, nullptr, nullptr);
            while (! message.empty()
                   && (message.back() == '\n' || message.back() == '\r'
                       || message.back() == '\0'))
                message.pop_back();
            if (! message.empty()) text += ": " + message;
        }
        ::LocalFree (buffer);
    }
    return text;
}
} // namespace
#endif
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

#if defined(_WIN32)
    // A Windows .clap is a renamed DLL. The scanner and session store absolute
    // paths, which LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR requires; it lets the
    // plugin's own directory resolve its dependent DLLs.
    handle = ::LoadLibraryExW (widenUtf8 (path).c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                                   | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (handle == nullptr)
    {
        errorOut = "LoadLibrary failed: " + lastErrorUtf8();
        bundlePath.clear();
        return false;
    }

    // A .clap exports `const clap_plugin_entry_t clap_entry`.
    entry = reinterpret_cast<const ::clap_plugin_entry*> (
        ::GetProcAddress (static_cast<HMODULE> (handle), "clap_entry"));
#else
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
#endif
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
#if defined(_WIN32)
        ::FreeLibrary (static_cast<HMODULE> (handle));
#else
        dlclose (handle);
#endif
        handle = nullptr;
    }
    bundlePath.clear();
}
} // namespace duskstudio::clap
