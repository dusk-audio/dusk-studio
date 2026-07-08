#include "RecentSessions.h"

#include "../foundation/Fs.h"
#include "../foundation/Text.h"

#include <algorithm>
#include <fstream>
#include <string>

namespace duskstudio
{
namespace stdfs = std::filesystem;

static stdfs::path configRoot()
{
    const auto cfg = dusk::fs::userConfigDir();
    return cfg.empty() ? stdfs::path {} : cfg / "Dusk Studio";
}

stdfs::path RecentSessions::getStoreFile()
{
    const auto cfgDir = configRoot();
    if (cfgDir.empty()) return {};

    std::error_code ec;
    if (! stdfs::is_directory (cfgDir, ec) && ! stdfs::create_directories (cfgDir, ec))
        return {};   // empty path - load() returns empty, add() drops the write
    return cfgDir / "recent.txt";
}

std::vector<stdfs::path> RecentSessions::load()
{
    std::vector<stdfs::path> result;
    const auto store = getStoreFile();
    std::error_code ec;
    if (store.empty() || ! stdfs::is_regular_file (store, ec)) return result;

    std::ifstream in (store);
    std::string line;
    while (std::getline (in, line))
    {
        const auto trimmed = dusk::text::trim (line);
        if (trimmed.empty()) continue;

        const stdfs::path f (trimmed);
        // Prune stale entries - a removed session directory is worse than
        // useless in the picker UI. Also dedupe (load can be called after a
        // partial write).
        if (stdfs::is_directory (f, ec)
            && std::find (result.begin(), result.end(), f) == result.end())
            result.push_back (f);
    }
    return result;
}

void RecentSessions::add (const stdfs::path& sessionDirectory)
{
    if (sessionDirectory.empty()) return;

    auto entries = load();

    // Newest goes to the front; remove any existing copy first so we don't
    // promote a duplicate.
    entries.erase (std::remove (entries.begin(), entries.end(), sessionDirectory), entries.end());
    entries.insert (entries.begin(), sessionDirectory);

    if ((int) entries.size() > kMaxEntries)
        entries.resize (kMaxEntries);

    const auto store = getStoreFile();
    if (store.empty()) return;

    std::vector<std::string> lines;
    lines.reserve (entries.size());
    for (auto& f : entries) lines.push_back (f.string());
    dusk::fs::writeStringToFile (store, dusk::text::joinIntoString (lines, "\n"));
}

void RecentSessions::clear()
{
    // Compute the path WITHOUT getStoreFile() - that creates the config dir as
    // a side effect, so clearing a never-used recents list would spawn an empty
    // "Dusk Studio" config dir. If the dir isn't there, there's nothing to clear.
    const auto cfgDir = configRoot();
    std::error_code ec;
    if (cfgDir.empty() || ! stdfs::is_directory (cfgDir, ec)) return;
    const auto store = cfgDir / "recent.txt";
    if (stdfs::is_regular_file (store, ec))
        stdfs::remove (store, ec);
}
} // namespace duskstudio
