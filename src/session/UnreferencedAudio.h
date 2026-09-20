#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace duskstudio
{
class Session;

// What Clean out would delete: the .wav files sitting in the session's audio
// directory that no region, no take under a region and no loaded mastering
// source points at. Subdirectories are left alone, so freeze renders and
// anything the user dropped in by hand are never candidates.
struct UnreferencedAudio
{
    std::vector<std::filesystem::path> files;
    std::int64_t totalBytes = 0;
    // The directory could not be read (permissions, a broken mount). An empty
    // list then means "could not tell", not "nothing to clean", and the files
    // listed are only what was reached before the walk stopped.
    bool scanFailed = false;
};

UnreferencedAudio findUnreferencedAudio (const Session& session);
} // namespace duskstudio
