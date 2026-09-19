#include "UnreferencedAudio.h"

#include "Session.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>

namespace duskstudio
{
namespace
{
// The session stores its paths in the framework's file type; everything here
// works in std::filesystem terms.
template <typename File>
std::filesystem::path pathOf (const File& file)
{
    return std::filesystem::u8path (file.getFullPathName().toStdString()).lexically_normal();
}

bool isWav (const std::filesystem::path& path)
{
    auto extension = path.extension().u8string();
    std::transform (extension.begin(), extension.end(), extension.begin(),
                    [] (unsigned char c) { return (char) std::tolower (c); });
    return extension == ".wav";
}
} // namespace

UnreferencedAudio findUnreferencedAudio (const Session& session)
{
    UnreferencedAudio found;
    const auto audioDir = pathOf (session.getAudioDirectory());
    std::error_code error;
    if (! std::filesystem::is_directory (audioDir, error))
    {
        // A session that has recorded nothing yet has no audio directory, which
        // is not a failure; anything else is.
        found.scanFailed = error && error != std::errc::no_such_file_or_directory;
        return found;
    }

    std::vector<std::filesystem::path> referenced;
    const auto remember = [&referenced] (const std::filesystem::path& path)
    {
        if (! path.empty()
            && std::find (referenced.begin(), referenced.end(), path) == referenced.end())
            referenced.push_back (path);
    };
    for (int t = 0; t < Session::kNumTracks; ++t)
        for (const auto& region : session.track (t).regions)
        {
            remember (pathOf (region.file));
            for (const auto& take : region.previousTakes)
                remember (pathOf (take.file));
        }
    // A bounce loaded into the Mastering stage may live in audio/ too.
    remember (pathOf (session.mastering().sourceFile));

    for (std::filesystem::directory_iterator it (audioDir, error), end; ! error && it != end;
         it.increment (error))
    {
        const auto path = it->path().lexically_normal();
        std::error_code entryError;
        const bool regular = it->is_regular_file (entryError);
        if (entryError) { found.scanFailed = true; continue; }
        if (! regular || ! isWav (path)) continue;
        if (std::find (referenced.begin(), referenced.end(), path) != referenced.end()) continue;
        const auto size = std::filesystem::file_size (path, entryError);
        if (entryError) { found.scanFailed = true; continue; }
        found.files.push_back (path);
        found.totalBytes += (std::int64_t) size;
    }
    if (error) found.scanFailed = true;
    return found;
}
} // namespace duskstudio
