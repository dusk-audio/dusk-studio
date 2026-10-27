#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace duskstudio
{
class Session;

// What Clean out would delete: the .wav files sitting in the session's audio
// directory that no region, take, freeze render or loaded mastering source
// points at, including through a filesystem alias. Subdirectories are left alone.
struct UnreferencedAudio
{
    std::vector<std::filesystem::path> files;
    std::int64_t totalBytes = 0;
    // The directory or a reference could not be read (permissions, a broken mount).
    // An empty list then means "could not tell", not "nothing to clean", and the
    // files listed are only what was reached before the walk stopped.
    bool scanFailed = false;
};

UnreferencedAudio findUnreferencedAudio (const Session& session);
} // namespace duskstudio
