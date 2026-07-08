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

    // $VST3_PATH overrides / extends the defaults (':'-separated, like $PATH).
    if (const char* env = std::getenv ("VST3_PATH"))
        for (const auto& tok : dusk::text::split (env, ':'))
        {
            const auto trimmed = dusk::text::trim (tok);
            if (! trimmed.empty()) add (stdfs::u8path (trimmed));
        }

    add (dusk::fs::userHomeDir() / ".vst3");
    add ("/usr/lib/vst3");
    add ("/usr/local/lib/vst3");
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

    for (const auto& dir : dirs)
    {
        if (! stdfs::is_directory (dir, ec)) continue;
        // Bundle entries (file or directory) with a .vst3 extension, not descending
        // into them (their inner .so is not a module path the SDK loader takes).
        for (const auto& e : stdfs::directory_iterator (dir, ec))
            if (dusk::fs::hasExtension (e.path(), "vst3"))
                add (e.path());
        // Plain subdirectories (vendor folders) one level down.
        for (const auto& e : stdfs::directory_iterator (dir, ec))
            if (e.is_directory (ec) && ! dusk::fs::hasExtension (e.path(), "vst3"))
                for (const auto& f : stdfs::directory_iterator (e.path(), ec))
                    if (dusk::fs::hasExtension (f.path(), "vst3"))
                        add (f.path());
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
