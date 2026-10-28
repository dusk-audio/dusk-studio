#include "UnreferencedAudio.h"

#include "Session.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <sys/stat.h>
#endif

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

// Two names for one file (a symbolic or hard link, a case variant on a
// case-insensitive volume) share this. One lookup per path keeps the scan
// linear; std::filesystem::equivalent opens both files of every pair it is given.
struct FileIdentity
{
    std::uint64_t volume = 0;
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    bool operator== (const FileIdentity& other) const noexcept
    {
        return volume == other.volume && high == other.high && low == other.low;
    }
};

enum class Lookup { found, missing, failed };

Lookup identify (const std::filesystem::path& path, FileIdentity& identity)
{
   #if defined (_WIN32)
    const HANDLE handle = CreateFileW (path.c_str(), 0,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        switch (GetLastError())
        {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
            case ERROR_INVALID_NAME:
            case ERROR_INVALID_DRIVE:
            case ERROR_BAD_NETPATH:
            case ERROR_BAD_NET_NAME:
                return Lookup::missing;
            default:
                return Lookup::failed;
        }
    }
    // The 128-bit id is the only unique one on ReFS. A volume that does not
    // report it (FAT) reports it for none of its files, so ids never mix kinds.
    bool okay = false;
    FILE_ID_INFO id {};
    if (GetFileInformationByHandleEx (handle, FileIdInfo, &id, sizeof (id)))
    {
        identity.volume = id.VolumeSerialNumber;
        std::memcpy (&identity.high, id.FileId.Identifier, sizeof (identity.high));
        std::memcpy (&identity.low, id.FileId.Identifier + sizeof (identity.high), sizeof (identity.low));
        okay = true;
    }
    else if (GetLastError() == ERROR_INVALID_PARAMETER)
    {
        BY_HANDLE_FILE_INFORMATION info {};
        if (GetFileInformationByHandle (handle, &info))
        {
            identity.volume = info.dwVolumeSerialNumber;
            identity.high = 0;
            identity.low = ((std::uint64_t) info.nFileIndexHigh << 32) | info.nFileIndexLow;
            okay = true;
        }
    }
    CloseHandle (handle);
    return okay ? Lookup::found : Lookup::failed;
   #else
    struct stat info {};
    if (::stat (path.c_str(), &info) == 0)
    {
        identity.volume = (std::uint64_t) info.st_dev;
        identity.high = 0;
        identity.low = (std::uint64_t) info.st_ino;
        return Lookup::found;
    }
    return errno == ENOENT || errno == ENOTDIR ? Lookup::missing : Lookup::failed;
   #endif
}
} // namespace

UnreferencedAudio findUnreferencedAudio (const Session& session, const AudioRegion* heldRegion)
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
    std::vector<FileIdentity> referencedFiles;
    const auto remember = [&] (const std::filesystem::path& path)
    {
        if (path.empty()
            || std::find (referenced.begin(), referenced.end(), path) != referenced.end())
            return;
        referenced.push_back (path);
        FileIdentity identity;
        const auto lookup = identify (path, identity);
        if (lookup == Lookup::found)
            referencedFiles.push_back (identity);
        else if (lookup == Lookup::failed)
            found.referenceUnreadable = true;
    };
    const auto rememberRegion = [&] (const AudioRegion& region)
    {
        remember (pathOf (region.file));
        // Reversing the region again plays its source forward.
        if (region.reversedFrom)
            remember (pathOf (region.reversedFrom->file));
    };
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto& track = session.track (t);
        for (const auto& region : track.regions)
            rememberRegion (region);
        for (const auto& take : track.takes)
            remember (pathOf (take.file));
        remember (std::filesystem::u8path (track.frozenAudioPath.toStdString()).lexically_normal());
        remember (pathOf (track.frozenRegion.file));
    }
    if (heldRegion != nullptr)
        rememberRegion (*heldRegion);
    // A bounce loaded into the Mastering stage may live in audio/ too.
    remember (pathOf (session.mastering().sourceFile));
    if (found.referenceUnreadable) return found;

    for (std::filesystem::directory_iterator it (audioDir, error), end; ! error && it != end;
         it.increment (error))
    {
        const auto path = it->path().lexically_normal();
        std::error_code entryError;
        const bool regular = it->is_regular_file (entryError);
        if (entryError) { found.scanFailed = true; continue; }
        if (! regular || ! isWav (path)) continue;
        if (std::find (referenced.begin(), referenced.end(), path) != referenced.end()) continue;
        FileIdentity identity;
        const auto lookup = identify (path, identity);
        if (lookup == Lookup::missing) continue;
        if (lookup == Lookup::failed) { found.scanFailed = true; continue; }
        if (std::find (referencedFiles.begin(), referencedFiles.end(), identity) != referencedFiles.end())
            continue;
        const auto size = std::filesystem::file_size (path, entryError);
        if (entryError) { found.scanFailed = true; continue; }
        found.files.push_back (path);
        found.totalBytes += (std::int64_t) size;
    }
    if (error) found.scanFailed = true;
    return found;
}
} // namespace duskstudio
