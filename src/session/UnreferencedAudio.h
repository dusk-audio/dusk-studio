#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace duskstudio
{
class Session;
struct AudioRegion;

// What Clean out would delete: the .wav files sitting in the session's audio
// directory that no region, take, freeze render, loaded mastering source or
// held region points at, including through a filesystem alias. Subdirectories
// are left alone.
struct UnreferencedAudio
{
    std::vector<std::filesystem::path> files;
    std::int64_t totalBytes = 0;
    // The directory could not be read (permissions, a broken mount). An empty
    // list then means "could not tell", not "nothing to clean", and the files
    // listed are only what was reached before the walk stopped.
    bool scanFailed = false;
    // A file the session uses could not be looked up, so any candidate might be
    // an alias of it; nothing is listed.
    bool referenceUnreadable = false;
};

// heldRegion is a region kept off the timeline, such as the region clipboard's.
UnreferencedAudio findUnreferencedAudio (const Session& session,
                                         const AudioRegion* heldRegion = nullptr);
} // namespace duskstudio
