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

    // $CLAP_PATH overrides / extends the defaults (':'-separated, like $PATH).
    if (const char* env = std::getenv ("CLAP_PATH"))
        for (const auto& tok : dusk::text::split (env, ':'))
        {
            const auto trimmed = dusk::text::trim (tok);
            if (! trimmed.empty()) add (stdfs::u8path (trimmed));
        }

#if defined(__APPLE__)
    add (dusk::fs::userHomeDir() / "Library/Audio/Plug-Ins/CLAP");
    add ("/Library/Audio/Plug-Ins/CLAP");
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
#if defined(__APPLE__)
    auto add = [&] (const stdfs::path& bundle)
    {
        for (const auto& existing : files) if (existing == bundle) return;
        files.push_back (bundle);
    };
#endif

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
        {
            bool seen = false;
            for (const auto& g : files) if (g == f) { seen = true; break; }
            if (! seen) files.push_back (f);
        }
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
