#include "Vst3Scanner.h"

#include "../../foundation/Fs.h"
#include "../../foundation/Text.h"

#include <cstdlib>
#include <system_error>

namespace duskstudio::vst3
{
namespace stdfs = std::filesystem;

std::vector<stdfs::path> Vst3Scanner::defaultSearchPaths()
{
    std::vector<stdfs::path> dirs;
    std::error_code ec;
    auto add = [&] (const stdfs::path& d)
    {
        if (! stdfs::is_directory (d, ec)) return;
        for (const auto& existing : dirs) if (existing == d) return;
        dirs.push_back (d);
    };

    // $VST3_PATH overrides / extends the defaults, separated like $PATH
    // (':' on POSIX, ';' on Windows).
#if defined(_WIN32)
    constexpr char kListSep = ';';
    // _wgetenv, not getenv: the value carries user-profile paths, which the
    // ANSI environment mangles for non-ASCII user names. ';' is one byte in
    // UTF-8, so splitting after the conversion is safe.
    const std::string envStorage = [] {
        const wchar_t* w = ::_wgetenv (L"VST3_PATH");
        return w != nullptr ? stdfs::path (w).u8string() : std::string();
    }();
    const char* env = envStorage.empty() ? nullptr : envStorage.c_str();
#else
    constexpr char kListSep = ':';
    const char* env = std::getenv ("VST3_PATH");
#endif
    if (env != nullptr)
        for (const auto& tok : dusk::text::split (env, kListSep))
        {
            const auto trimmed = dusk::text::trim (tok);
            if (! trimmed.empty()) add (stdfs::u8path (trimmed));
        }

#if defined(__APPLE__)
    add (dusk::fs::userHomeDir() / "Library/Audio/Plug-Ins/VST3");
    add ("/Library/Audio/Plug-Ins/VST3");
#elif defined(_WIN32)
    // The VST3 spec's Windows install dirs.
    if (const wchar_t* local = ::_wgetenv (L"LOCALAPPDATA"))
        add (stdfs::path (local) / L"Programs" / L"Common" / L"VST3");
    if (const wchar_t* common = ::_wgetenv (L"COMMONPROGRAMFILES"))
        add (stdfs::path (common) / L"VST3");
#else
    add (dusk::fs::userHomeDir() / ".vst3");
    add ("/usr/lib/vst3");
    add ("/usr/local/lib/vst3");
#endif
    return dirs;
}

std::vector<stdfs::path> Vst3Scanner::findVst3Bundles (const std::vector<stdfs::path>& dirs)
{
    std::vector<stdfs::path> files;
    std::error_code ec;
    auto add = [&] (const stdfs::path& f)
    {
        for (const auto& g : files) if (g == f) return;
        files.push_back (f);
    };

    // ec-aware increment (like dusk::fs::findChildFiles): a directory mutated
    // mid-scan stops the walk cleanly instead of throwing from operator++.
    for (const auto& dirIn : dirs)
    {
        std::error_code walkEc, entryEc;
        // Absolute so a cached bundle path stays stable regardless of cwd, even
        // for a relative $VST3_PATH entry.
        const auto dir = stdfs::absolute (dirIn, ec);
        if (ec || ! stdfs::is_directory (dir, ec)) continue;
        // Bundle entries (file or directory) with a .vst3 extension, not descending
        // into them (their inner .so is not a module path the SDK loader takes).
        for (auto it = stdfs::directory_iterator (dir, walkEc);
             ! walkEc && it != stdfs::directory_iterator(); it.increment (walkEc))
            if (dusk::fs::hasExtension (it->path(), "vst3"))
                add (it->path());
        // Plain subdirectories (vendor folders) one level down.
        walkEc.clear();
        for (auto it = stdfs::directory_iterator (dir, walkEc);
             ! walkEc && it != stdfs::directory_iterator(); it.increment (walkEc))
        {
            if (! it->is_directory (entryEc) || dusk::fs::hasExtension (it->path(), "vst3"))
                continue;
            std::error_code subEc;
            for (auto sub = stdfs::directory_iterator (it->path(), subEc);
                 ! subEc && sub != stdfs::directory_iterator(); sub.increment (subEc))
                if (dusk::fs::hasExtension (sub->path(), "vst3"))
                    add (sub->path());
        }
    }
    return files;
}

std::vector<ScannedVst3> Vst3Scanner::scan (const std::vector<stdfs::path>& dirs)
{
    std::vector<ScannedVst3> found;
    for (const auto& file : findVst3Bundles (dirs))
    {
        Vst3Bundle bundle;
        std::string err;
        if (! bundle.load (file.u8string(), err))
            continue;
        for (const auto& d : bundle.plugins())
            found.push_back ({ file.u8string(), d });
    }
    return found;
}
} // namespace duskstudio::vst3
