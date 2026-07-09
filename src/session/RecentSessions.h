#pragma once

#include <filesystem>
#include <vector>

namespace duskstudio
{
// Persistent list of session directories the user has recently saved/loaded.
// Stored as a newline-delimited file at <userConfigDir>/Dusk Studio/recent.txt
// (one absolute path per line, most recent first). Cap at kMaxEntries - older
// entries are evicted on overflow. Stale paths (directory removed) are pruned
// on read so the startup dialog never shows broken entries.
//
// Threading: read on the message thread (startup dialog, save/load callbacks);
// not safe to call concurrently. The file is small enough that this isn't a
// real constraint.
class RecentSessions
{
public:
    static constexpr int kMaxEntries = 10;

    static std::vector<std::filesystem::path> load();
    static void                               add (const std::filesystem::path& sessionDirectory);
    static void                               clear();

private:
    static std::filesystem::path getStoreFile();
};
} // namespace duskstudio
