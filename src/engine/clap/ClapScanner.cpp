#include "ClapScanner.h"

#include "../../foundation/Fs.h"
#include "../../foundation/Text.h"

#include <cstdlib>
#include <system_error>

namespace duskstudio::clap
{
namespace stdfs = std::filesystem;

std::vector<stdfs::path> ClapScanner::defaultSearchPaths()
{
    std::vector<stdfs::path> dirs;
    std::error_code ec;
    auto add = [&] (const stdfs::path& d)
    {
        if (! stdfs::is_directory (d, ec)) return;
        for (const auto& existing : dirs) if (existing == d) return;
        dirs.push_back (d);
    };

    // $CLAP_PATH overrides / extends the defaults, separated like $PATH
    // (':' on POSIX, ';' on Windows).
#if defined(_WIN32)
    constexpr char kListSep = ';';
    // _wgetenv, not getenv: the value carries user-profile paths, which the
    // ANSI environment mangles for non-ASCII user names. ';' is one byte in
    // UTF-8, so splitting after the conversion is safe.
    const std::string envStorage = [] {
        const wchar_t* w = ::_wgetenv (L"CLAP_PATH");
        return w != nullptr ? stdfs::path (w).u8string() : std::string();
    }();
    const char* env = envStorage.empty() ? nullptr : envStorage.c_str();
#else
    constexpr char kListSep = ':';
    const char* env = std::getenv ("CLAP_PATH");
#endif
    if (env != nullptr)
        for (const auto& tok : dusk::text::split (env, kListSep))
        {
            const auto trimmed = dusk::text::trim (tok);
            if (! trimmed.empty()) add (stdfs::u8path (trimmed));
        }

#if defined(__APPLE__)
    add (dusk::fs::userHomeDir() / "Library/Audio/Plug-Ins/CLAP");
    add ("/Library/Audio/Plug-Ins/CLAP");
#elif defined(_WIN32)
    // CLAP's documented Windows install dirs (clap/entry.h).
    if (const wchar_t* common = ::_wgetenv (L"COMMONPROGRAMFILES"))
        add (stdfs::path (common) / L"CLAP");
    if (const wchar_t* local = ::_wgetenv (L"LOCALAPPDATA"))
        add (stdfs::path (local) / L"Programs" / L"Common" / L"CLAP");
#else
    add (dusk::fs::userHomeDir() / ".clap");
    add ("/usr/lib/clap");
    add ("/usr/local/lib/clap");
#endif
    return dirs;
}

std::vector<stdfs::path> ClapScanner::findClapFiles (const std::vector<stdfs::path>& dirs)
{
    std::vector<stdfs::path> files;
    std::error_code ec;
    auto add = [&] (const stdfs::path& bundle)
    {
        for (const auto& existing : files) if (existing == bundle) return;
        files.push_back (bundle);
    };

    for (const auto& dirIn : dirs)
    {
        // Absolute so a cached bundle path (and its native identifier) stays
        // stable regardless of cwd, even for a relative $CLAP_PATH entry.
        const auto dir = stdfs::absolute (dirIn, ec);
        if (ec || ! stdfs::is_directory (dir, ec)) continue;
#if defined(__APPLE__)
        std::error_code walkEc, entryEc;
        for (auto it = stdfs::recursive_directory_iterator (dir, walkEc);
             ! walkEc && it != stdfs::recursive_directory_iterator(); it.increment (walkEc))
        {
            if (! it->is_directory (entryEc) || ! dusk::fs::hasExtension (it->path(), "clap"))
                continue;

            add (it->path());
            it.disable_recursion_pending();
        }
#else
        for (const auto& f : dusk::fs::findChildFiles (dir, "*.clap", true))
            add (f);
#endif
    }
    return files;
}

std::vector<ScannedClap> ClapScanner::scan (const std::vector<stdfs::path>& dirs)
{
    std::vector<ScannedClap> found;
    for (const auto& file : findClapFiles (dirs))
    {
        ClapBundle bundle;
        std::string err;
        if (! bundle.load (file.u8string(), err))
            continue;
        for (const auto& d : bundle.plugins())
            found.push_back ({ file.u8string(), d });
    }
    return found;
}
} // namespace duskstudio::clap
