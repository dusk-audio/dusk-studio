#include "SfzLibraryLocations.h"
#include "SfzCatalogJson.h"
#include "../../foundation/AppConfigDir.h"
#include "../../foundation/Fs.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <exception>
#include <fstream>
#include <set>
#include <system_error>
#if defined(_WIN32)
 #include <windows.h>
#else
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

namespace duskstudio::sfz
{
namespace
{
namespace fs = std::filesystem;
using Json = dusk::json::Json;
constexpr const char* kMarker = ".dusk-sfz-library";

fs::path internalRoot()
{
    const auto data = dusk::fs::appDataDir();
    return data.empty() ? fs::path {} : data / "sfz-library";
}

std::vector<LibraryLocation> roots (const LibraryLocations& locations)
{
    auto result = locations.previous;
    result.insert (result.begin(), locations.active);
    return result;
}

bool plainDirectory (const fs::path& path)
{
    std::error_code error;
    return fs::is_directory (fs::symlink_status (dusk::fs::nativeIOPath (path), error)) && ! error;
}

using DirectoryIdentity = std::array<std::uint64_t, 2>;

std::optional<DirectoryIdentity> directoryIdentity (const fs::path& path)
{
#if defined(_WIN32)
    const auto handle = CreateFileW (dusk::fs::nativeIOPath (path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info {};
    const bool read = GetFileInformationByHandle (handle, &info) != 0;
    const bool closed = CloseHandle (handle) != 0;
    if (! read || ! closed || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
        || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return {};
    return DirectoryIdentity { info.dwVolumeSerialNumber,
        (std::uint64_t (info.nFileIndexHigh) << 32) | info.nFileIndexLow };
#else
    struct stat info {};
    if (::lstat (path.c_str(), &info) != 0 || ! S_ISDIR (info.st_mode)) return {};
    return DirectoryIdentity { static_cast<std::uint64_t> (info.st_dev), static_cast<std::uint64_t> (info.st_ino) };
#endif
}

struct DirectoryGuard
{
    fs::path path;
    DirectoryIdentity identity {};
    bool matches() const { return directoryIdentity (path) == identity; }
};

std::optional<DirectoryGuard> guardDirectory (const fs::path& path)
{
    if (const auto identity = directoryIdentity (path)) return DirectoryGuard { path, *identity };
    return {};
}

struct LibraryGuard
{
    LibraryLocation location;
    DirectoryGuard root, packs;
    bool matches() const
    { return root.matches() && packs.matches() && libraryLocationAvailable (location); }
};

std::optional<LibraryGuard> guardLibrary (const LibraryLocation& location)
{
    const auto root = guardDirectory (location.root), packs = guardDirectory (location.root / "packs");
    if (root && packs && libraryLocationAvailable (location)) return LibraryGuard { location, *root, *packs };
    return {};
}

bool syncPath (const fs::path& path, bool directory)
{
#if defined(_WIN32)
    // Directory publication uses MoveFileEx's write-through flag on Windows.
    if (directory) return directoryIdentity (path).has_value();
    const auto handle = CreateFileW (dusk::fs::nativeIOPath (path).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info {};
    const bool ordinary = GetFileInformationByHandle (handle, &info) != 0
        && (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
    const bool flushed = ordinary && FlushFileBuffers (handle) != 0;
    const bool closed = CloseHandle (handle) != 0;
    return flushed && closed;
#else
    const auto handle = ::open (path.c_str(), O_NOFOLLOW | (directory ? O_RDONLY | O_DIRECTORY : O_RDWR));
    if (handle < 0) return false;
    struct stat info {};
    const bool ordinary = ::fstat (handle, &info) == 0
        && (directory ? S_ISDIR (info.st_mode) : S_ISREG (info.st_mode));
    int flushed = -1;
    if (ordinary) do { flushed = ::fsync (handle); } while (flushed != 0 && errno == EINTR);
    const bool closed = ::close (handle) == 0;
    return flushed == 0 && closed;
#endif
}

bool renamePath (const fs::path& source, const fs::path& target)
{
#if defined(_WIN32)
    return MoveFileExW (dusk::fs::nativeIOPath (source).c_str(), dusk::fs::nativeIOPath (target).c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    fs::rename (source, target, error);
    return ! error;
#endif
}

bool createDirectoriesDurably (const fs::path& path)
{
    std::vector<fs::path> missing;
    auto parent = path;
    while (! plainDirectory (parent))
    {
        if (parent.empty() || parent == parent.parent_path()) return false;
        missing.push_back (parent);
        parent = parent.parent_path();
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it)
    {
        std::error_code error;
        if (! fs::create_directory (dusk::fs::nativeIOPath (*it), error) || error || ! syncPath (it->parent_path(), true)) return false;
    }
    return true;
}

std::string readSmallFile (const fs::path& path, std::uintmax_t limit)
{
    std::error_code error;
    const auto ioPath = dusk::fs::nativeIOPath (path);
    if (! fs::is_regular_file (fs::symlink_status (ioPath, error)) || error
        || fs::file_size (ioPath, error) > limit || error) return {};
    std::ifstream input (ioPath, std::ios::binary);
    if (! input) return {};
    std::string text (static_cast<std::size_t> (limit) + 1, '\0');
    input.read (text.data(), static_cast<std::streamsize> (text.size()));
    if (input.bad() || input.gcount() > static_cast<std::streamsize> (limit)) return {};
    text.resize (static_cast<std::size_t> (input.gcount()));
    return text;
}

std::string newMarker()
{
    if (sodium_init() < 0) return {};
    std::array<unsigned char, 16> bytes {};
    std::array<char, 33> hex {};
    randombytes_buf (bytes.data(), bytes.size());
    sodium_bin2hex (hex.data(), hex.size(), bytes.data(), bytes.size());
    return hex.data();
}

bool validMarker (const std::string& text)
{
    return text.size() == 32 && std::all_of (text.begin(), text.end(), [] (char c)
    { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

enum class ConfigurationWrite { Failed, Visible, Durable };

ConfigurationWrite writeConfiguration (const fs::path& file, const LibraryLocations& locations)
{
    if (file.empty()) return ConfigurationWrite::Durable;
    const auto token = newMarker();
    if (token.empty()) return ConfigurationWrite::Failed;
    const auto encode = [] (const LibraryLocation& location)
    { return Json { { "path", location.root.u8string() }, { "marker", location.marker } }; };
    Json previous = Json::array();
    for (const auto& location : locations.previous) previous.push_back (encode (location));
    const Json value { { "schema", 1 }, { "active", encode (locations.active) }, { "previous", previous } };
    std::error_code error;
    if (! createDirectoriesDurably (file.parent_path())) return ConfigurationWrite::Failed;
    const auto temporary = file.parent_path() / (".sfz-location-" + token);
    if (! dusk::fs::writeStringToFile (temporary, value.dump (2)) || ! syncPath (temporary, false)
        || ! syncPath (file.parent_path(), true))
    {
        fs::remove (dusk::fs::nativeIOPath (temporary), error);
        return ConfigurationWrite::Failed;
    }
    if (! renamePath (temporary, file))
    {
        if (! fs::exists (dusk::fs::nativeIOPath (temporary), error) && ! error) return ConfigurationWrite::Visible;
        fs::remove (dusk::fs::nativeIOPath (temporary), error);
        return ConfigurationWrite::Failed;
    }
    return syncPath (file.parent_path(), true) ? ConfigurationWrite::Durable : ConfigurationWrite::Visible;
}

bool nested (const fs::path& child, const fs::path& parent)
{
    const auto relative = child.lexically_relative (parent);
    return ! relative.empty() && *relative.begin() != "..";
}

bool hasAncestorIdentity (fs::path path, const DirectoryIdentity& identity)
{
    while (! path.empty())
    {
        if (directoryIdentity (path) == identity) return true;
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    return false;
}

struct FileEntry
{
    fs::path relative;
    std::uintmax_t bytes = 0;
};

bool fileTree (const fs::path& root, std::vector<FileEntry>& files, std::string& error)
{
    std::error_code ec;
    std::size_t entries = 0;
    const auto ioRoot = dusk::fs::nativeIOPath (root);
    for (fs::recursive_directory_iterator it (ioRoot, ec), end; ! ec && it != end; it.increment (ec))
    {
        if (++entries > 100000 || it.depth() > 64) { error = "The SFZ pack file tree exceeds its limits"; return false; }
        const auto status = it->symlink_status (ec);
        if (ec || (! fs::is_directory (status) && ! fs::is_regular_file (status)))
        { error = "The SFZ pack contains a link or unsupported file"; return false; }
        if (fs::is_regular_file (status))
        {
            const auto size = it->file_size (ec);
            if (ec) break;
            files.push_back ({ it->path().lexically_relative (ioRoot), size });
        }
    }
    if (ec) { error = "Cannot read the SFZ pack: " + ec.message(); return false; }
    std::sort (files.begin(), files.end(), [] (const auto& a, const auto& b) { return a.relative < b.relative; });
    return true;
}

OperationResult interrupted (const OperationControl& control)
{
    return { control.status(), "SFZ library location change interrupted" };
}

bool createPackParents (const DirectoryGuard& destination, const fs::path& relative,
                        const LibraryGuard& library)
{
    auto current = destination.path;
    for (const auto& component : relative)
    {
        if (! library.matches() || ! destination.matches()) return false;
        const auto parent = current;
        current /= component;
        std::error_code error;
        const auto status = fs::symlink_status (dusk::fs::nativeIOPath (current), error);
        if (error && error != std::errc::no_such_file_or_directory) return false;
        if (! fs::exists (status))
        {
            error.clear();
            if (! fs::create_directory (dusk::fs::nativeIOPath (current), error) || error || ! syncPath (parent, true)) return false;
        }
        else if (! fs::is_directory (status)) return false;
    }
    return library.matches() && destination.matches();
}

bool syncPackDirectories (const DirectoryGuard& pack, const LibraryGuard& library)
{
    std::vector<fs::path> directories { pack.path };
    std::error_code error;
    for (fs::recursive_directory_iterator it (dusk::fs::nativeIOPath (pack.path), error), end; ! error && it != end; it.increment (error))
    {
        if (! library.matches() || ! pack.matches()) return false;
        const auto status = it->symlink_status (error);
        if (error || (! fs::is_regular_file (status) && ! fs::is_directory (status))) return false;
        if (fs::is_directory (status)) directories.push_back (it->path());
    }
    if (error) return false;
    for (auto it = directories.rbegin(); it != directories.rend(); ++it)
        if (! library.matches() || ! pack.matches() || ! syncPath (*it, true)) return false;
    return true;
}

OperationResult transferPack (const fs::path& source, const DirectoryGuard& destination,
                              const LibraryGuard& library, bool copy,
                              const OperationControl& control, std::uint64_t& completed)
{
    const auto available = [&] { return library.matches() && destination.matches(); };
    const OperationResult disconnected { OperationStatus::Failed, "The destination SFZ drive disconnected or changed during the move" };
    if (! available()) return disconnected;
    std::string error;
    std::vector<FileEntry> files;
    if (! fileTree (source, files, error)) return { OperationStatus::Failed, error };
    std::array<char, 65536> buffer {}, verify {};
    for (const auto& file : files)
    {
        if (control.status() != OperationStatus::Complete) return interrupted (control);
        if (! available()) return disconnected;
        const auto target = destination.path / file.relative;
        std::error_code ec;
        if (copy)
        {
            if (! createPackParents (destination, file.relative.parent_path(), library))
                return { OperationStatus::Failed, "Cannot create folders under the verified SFZ staging directory" };
        }
        else if (! fs::is_regular_file (fs::symlink_status (dusk::fs::nativeIOPath (target), ec)) || ec
                 || fs::file_size (dusk::fs::nativeIOPath (target), ec) != file.bytes || ec)
            return { OperationStatus::Failed, "An existing destination pack differs from the installed pack" };
        std::ifstream input (dusk::fs::nativeIOPath (source / file.relative), std::ios::binary);
        std::ofstream output;
        if (copy) output.open (dusk::fs::nativeIOPath (target), std::ios::binary | std::ios::trunc);
        if (! input || (copy && ! output)) return { OperationStatus::Failed, "Cannot open an SFZ file for relocation" };
        crypto_hash_sha256_state expected {};
        crypto_hash_sha256_init (&expected);
        std::uintmax_t bytes = 0;
        while (input)
        {
            if (control.status() != OperationStatus::Complete) return interrupted (control);
            if (! available()) return disconnected;
            input.read (buffer.data(), static_cast<std::streamsize> (buffer.size()));
            const auto count = input.gcount();
            if (count <= 0) break;
            bytes += static_cast<std::uintmax_t> (count);
            crypto_hash_sha256_update (&expected, reinterpret_cast<const unsigned char*> (buffer.data()), static_cast<std::size_t> (count));
            if (copy)
            {
                output.write (buffer.data(), count);
                if (! output) return { OperationStatus::Failed, "Cannot write the destination SFZ file" };
            }
            completed += static_cast<std::uint64_t> (count);
            if (control.progress) control.progress ({ completed, 0, 0 });
            if (! available()) return disconnected;
        }
        if (input.bad() || bytes != file.bytes) return { OperationStatus::Failed, "The source SFZ file changed or could not be read" };
        if (copy)
        {
            output.close();
            if (! output) return { OperationStatus::Failed, "Cannot flush the destination SFZ file" };
        }
        if (! available()) return disconnected;
        if (! syncPath (target, false)) return { OperationStatus::Failed, "Cannot durably flush the destination SFZ file; originals were kept" };
        std::ifstream check (dusk::fs::nativeIOPath (target), std::ios::binary);
        if (! check) return { OperationStatus::Failed, "Cannot read back the destination SFZ file" };
        crypto_hash_sha256_state actual {};
        crypto_hash_sha256_init (&actual);
        std::uintmax_t verified = 0;
        while (check)
        {
            if (control.status() != OperationStatus::Complete) return interrupted (control);
            if (! available()) return disconnected;
            check.read (verify.data(), static_cast<std::streamsize> (verify.size()));
            const auto count = check.gcount();
            if (count <= 0) break;
            verified += static_cast<std::uintmax_t> (count);
            crypto_hash_sha256_update (&actual, reinterpret_cast<const unsigned char*> (verify.data()), static_cast<std::size_t> (count));
        }
        std::array<unsigned char, crypto_hash_sha256_BYTES> a {}, b {};
        crypto_hash_sha256_final (&expected, a.data());
        crypto_hash_sha256_final (&actual, b.data());
        if (check.bad() || verified != file.bytes || a != b)
            return { OperationStatus::Failed, "The destination SFZ file failed verification; originals were kept" };
    }
    std::vector<FileEntry> copied;
    if (! available()) return disconnected;
    if (! fileTree (destination.path, copied, error) || copied.size() != files.size())
        return { OperationStatus::Failed, "The destination SFZ pack has a different file set" };
    for (std::size_t i = 0; i < files.size(); ++i)
        if (files[i].relative != copied[i].relative || files[i].bytes != copied[i].bytes)
            return { OperationStatus::Failed, "The destination SFZ pack has a different file set" };
    if (! syncPackDirectories (destination, library))
        return { OperationStatus::Failed, "Cannot durably flush the destination SFZ folders; originals were kept" };
    return { OperationStatus::Complete, {} };
}

bool emptyLibrary (const LibraryLocation& location)
{
    const auto library = guardLibrary (location);
    if (! library) return false;
    std::error_code error;
    return fs::is_empty (dusk::fs::nativeIOPath (library->packs.path), error) && ! error && library->matches();
}
} // namespace

fs::path libraryLocationsFile()
{
    const auto config = dusk::fs::appConfigDir();
    return config.empty() ? fs::path {} : config / "sfz-library-locations.json";
}

LibraryLocations readLibraryLocations (const fs::path& file)
{
    LibraryLocations locations;
    std::error_code error;
    const auto status = fs::symlink_status (dusk::fs::nativeIOPath (file), error);
    if (! fs::exists (status) && (error == std::errc::no_such_file_or_directory || ! error))
    {
        locations.active.root = internalRoot();
        return locations;
    }
    const auto text = readSmallFile (file, 131072);
    const auto fail = [&]
    {
        locations = {};
        locations.error = "The SFZ library location settings are invalid. Choose a library folder in Settings.";
        return locations;
    };
    const auto parsed = detail::parseJsonRejectingDuplicateKeys (text, "SFZ library locations");
    if (! parsed) return fail();
    const auto& value = *parsed.root;
    if (! value.is_object() || ! value.contains ("schema") || ! value["schema"].is_number_integer()
        || value["schema"] != 1 || ! value.contains ("active")
        || ! value.contains ("previous") || ! value["previous"].is_array() || value["previous"].size() > 32) return fail();
    const auto decode = [] (const Json& item, LibraryLocation& location)
    {
        if (! item.is_object() || ! item.contains ("path") || ! item["path"].is_string()
            || ! item.contains ("marker") || ! item["marker"].is_string()) return false;
        const auto path = item["path"].get<std::string>();
        location.marker = item["marker"].get<std::string>();
        if (path.empty() || path.size() > 32768 || path.find ('\0') != std::string::npos) return false;
        try
        {
            location.root = fs::u8path (path);
            return location.root.is_absolute() && (validMarker (location.marker)
                || (location.marker.empty() && location.root == internalRoot()));
        }
        catch (const std::exception&) { return false; }
    };
    if (! decode (value["active"], locations.active)) return fail();
    std::set<fs::path> seen { locations.active.root };
    for (const auto& item : value["previous"])
    {
        LibraryLocation location;
        if (! decode (item, location) || ! seen.insert (location.root).second) return fail();
        locations.previous.push_back (std::move (location));
    }
    return locations;
}

bool libraryLocationAvailable (const LibraryLocation& location)
{
    if (location.root.empty() || ! plainDirectory (location.root) || ! plainDirectory (location.root / "packs")) return false;
    return location.marker.empty() || (validMarker (location.marker) && readSmallFile (location.root / kMarker, 64) == location.marker);
}

std::vector<InstalledPack> installedLibraryPacks (const LibraryLocations& locations)
{
    std::vector<InstalledPack> installed;
    std::set<std::string> seen;
    for (const auto& root : roots (locations))
        if (libraryLocationAvailable (root))
            for (auto& pack : PackStore (root.root, root.marker).installed())
                if (! pack.instruments.empty() && seen.insert (PackStore::identity (pack.locatorFor (pack.instruments.front()))).second)
                    installed.push_back (std::move (pack));
    return installed;
}

std::optional<fs::path> resolveLibraryInstrument (const LibraryLocations& locations, const ManagedSfzLocator& locator)
{
    for (const auto& root : roots (locations))
        if (libraryLocationAvailable (root))
            if (auto path = PackStore (root.root, root.marker).resolve (locator)) return path;
    return {};
}

std::optional<ManagedSfzLocator> libraryLocatorForFile (const LibraryLocations& locations, const fs::path& file)
{
    for (const auto& root : roots (locations))
        if (libraryLocationAvailable (root))
            if (auto locator = PackStore (root.root, root.marker).locatorForFile (file)) return locator;
    return {};
}

LocationChangeResult changeLibraryLocation (const LibraryLocations& before, const fs::path& destination,
                                           bool moveExisting, const OperationControl& control, const fs::path& configurationFile)
{
    LocationChangeResult result;
    const auto fail = [&] (const std::string& message) { result.error = message; return result; };
    if (sodium_init() < 0) return fail ("Cannot initialize SFZ library verification");
    if (control.status() != OperationStatus::Complete) { result.status = control.status(); return result; }
    std::error_code error;
    const auto target = fs::canonical (dusk::fs::nativeIOPath (destination), error);
    const auto chosenDirectory = error ? std::optional<DirectoryGuard> {} : guardDirectory (target);
    if (! chosenDirectory) return fail ("Choose an existing folder on the connected SFZ library drive");
    auto after = before;
    after.error.clear();
    const auto sourceRoots = roots (before);
    for (const auto& root : sourceRoots)
    {
        if (root.root.empty()) continue;
        const auto canonical = fs::weakly_canonical (dusk::fs::nativeIOPath (root.root), error);
        if (error) return fail ("Cannot identify the existing SFZ library folder");
        const auto sourceIdentity = directoryIdentity (root.root);
        if (sourceIdentity != chosenDirectory->identity && canonical != target
            && (nested (target, canonical) || nested (canonical, target)
                || (sourceIdentity && hasAncestorIdentity (target, *sourceIdentity))
                || hasAncestorIdentity (canonical, chosenDirectory->identity)))
            return fail ("Choose an SFZ library folder outside the existing library folders");
    }
    auto marker = readSmallFile (target / kMarker, 64);
    if (fs::exists (dusk::fs::nativeIOPath (target / kMarker), error) && ! validMarker (marker))
        return fail ("The selected folder has an invalid SFZ library marker");
    if (marker.empty())
    {
        marker = newMarker();
        const auto temporary = target / (".sfz-marker-" + marker);
        if (marker.empty() || ! chosenDirectory->matches()
            || ! dusk::fs::writeStringToFile (temporary, marker) || ! syncPath (temporary, false)
            || ! chosenDirectory->matches() || ! renamePath (temporary, target / kMarker))
            return fail ("Cannot mark the selected SFZ library folder");
    }
    if (! chosenDirectory->matches() || ! syncPath (target / kMarker, false) || ! syncPath (target, true))
        return fail ("Cannot durably save the SFZ library marker");
    const auto packsPath = target / "packs";
    if (! fs::exists (fs::symlink_status (dusk::fs::nativeIOPath (packsPath), error)))
    {
        error.clear();
        const auto temporary = target / (".sfz-packs-" + newMarker());
        if (! chosenDirectory->matches() || ! fs::create_directory (dusk::fs::nativeIOPath (temporary), error) || error
            || ! chosenDirectory->matches() || ! renamePath (temporary, packsPath) || ! syncPath (target, true))
            return fail ("Cannot create the SFZ packs directory on the selected drive");
    }
    const LibraryLocation active { target, marker };
    const auto destinationLibrary = guardLibrary (active);
    if (! destinationLibrary || ! chosenDirectory->matches()) return fail ("The destination SFZ library drive is unavailable");
    PackStore destinationStore (target, marker);
    std::string initializationError;
    if (! destinationStore.initialize (initializationError)) return fail (initializationError);
    after.active = active;
    after.previous.clear();
    std::set<fs::path> seen { target };
    std::set<DirectoryIdentity> identities { chosenDirectory->identity };
    for (const auto& root : sourceRoots)
        if (! root.root.empty())
        {
            const auto canonical = fs::weakly_canonical (dusk::fs::nativeIOPath (root.root), error);
            if (error) return fail ("Cannot identify the previous SFZ library folder");
            const auto identity = directoryIdentity (root.root);
            if (identity && ! identities.insert (*identity).second) continue;
            if (seen.insert (canonical).second) after.previous.push_back (root);
        }
    if (after.previous.size() > 32) return fail ("Too many previous SFZ library folders are registered");

    struct Rollback
    {
        LibraryGuard library;
        std::vector<DirectoryGuard> paths;
        bool committed = false;
        ~Rollback()
        {
            try
            {
                if (! committed)
                    for (const auto& path : paths)
                        if (library.matches() && path.matches())
                        { std::error_code ignored; fs::remove_all (dusk::fs::nativeIOPath (path.path), ignored); }
            }
            catch (...) {}
        }
    } rollback { *destinationLibrary, {}, false };
    struct Original
    {
        LibraryGuard library;
        DirectoryGuard directory, published;
        InstalledPack pack;
    };
    std::vector<Original> originals;
    std::uint64_t completed = 0;
    if (moveExisting)
        for (const auto& root : sourceRoots)
        {
            if (root.root.empty()) continue;
            if (directoryIdentity (root.root) == chosenDirectory->identity) continue;
            const auto sourceLibrary = guardLibrary (root);
            if (! sourceLibrary)
                return fail ("Reconnect the existing SFZ library drive before moving its packs");
            for (const auto& pack : PackStore (root.root, root.marker).installed())
            {
                const auto sourcePack = guardDirectory (pack.directory);
                if (! sourcePack || ! sourceLibrary->matches() || ! destinationLibrary->matches())
                    return fail ("An SFZ library drive disconnected or changed during the move");
                const auto published = target / "packs" / pack.directory.filename();
                if (fs::exists (dusk::fs::nativeIOPath (published), error))
                {
                    const auto publishedDirectory = guardDirectory (published);
                    if (! publishedDirectory) return fail ("The destination SFZ version is not a plain directory");
                    if (publishedDirectory->identity == sourcePack->identity) continue;
                    const auto verified = transferPack (pack.directory, *publishedDirectory, *destinationLibrary, false, control, completed);
                    if (! verified) { result.status = verified.status; return fail (verified.error); }
                }
                else
                {
                    auto staging = destinationStore.createStaging (initializationError);
                    if (staging.empty()) return fail (initializationError);
                    const auto stagingDirectory = guardDirectory (staging);
                    if (! stagingDirectory || ! destinationLibrary->matches())
                        return fail ("The destination SFZ drive disconnected while preparing the move");
                    rollback.paths.push_back (*stagingDirectory);
                    const auto verified = transferPack (pack.directory, *stagingDirectory, *destinationLibrary, true, control, completed);
                    if (! verified) { result.status = verified.status; return fail (verified.error); }
                    if (! destinationLibrary->matches() || ! stagingDirectory->matches())
                        return fail ("The destination SFZ drive disconnected during the move");
                    if (! renamePath (staging, published)) return fail ("Cannot publish the relocated SFZ pack");
                    rollback.paths.back().path = published;
                    if (! destinationLibrary->matches() || ! rollback.paths.back().matches()
                        || ! syncPath (packsPath, true) || ! syncPath (target, true))
                    {
                        rollback.committed = true;
                        return fail ("Cannot confirm durable publication of the SFZ pack; copies and originals were kept");
                    }
                }
                const auto publishedDirectory = guardDirectory (published);
                if (! publishedDirectory) return fail ("The relocated SFZ pack is no longer available; originals were kept");
                originals.push_back ({ *sourceLibrary, *sourcePack, *publishedDirectory, pack });
            }
        }
    if (control.status() != OperationStatus::Complete) { result.status = control.status(); return result; }
    if (! destinationLibrary->matches() || ! syncPath (packsPath, true) || ! syncPath (target, true))
        return fail ("The destination SFZ library drive is unavailable or could not be flushed");
    const auto configured = writeConfiguration (configurationFile, after);
    if (configured == ConfigurationWrite::Failed) return fail ("Cannot save the SFZ library location; originals were kept");
    rollback.committed = true;
    if (configured == ConfigurationWrite::Visible)
    {
        result.status = OperationStatus::Complete;
        result.error = "The new library is configured, but settings durability could not be confirmed. Copies and originals were kept.";
        result.locations = std::move (after);
        return result;
    }
    for (const auto& original : originals)
    {
        if (! destinationLibrary->matches() || ! original.library.matches()
            || ! original.directory.matches() || ! original.published.matches())
        {
            result.error = "The new library is active. Reconnect the previous drive to remove its retained copies.";
            continue;
        }
        const auto destinationIdentity = directoryIdentity (packsPath / original.pack.directory.filename());
        if (destinationIdentity == original.directory.identity) continue;
        const auto removed = PackStore (original.library.location.root, original.library.location.marker).remove (original.pack, {});
        if (! removed) result.error = "The new library is active. Some original copies could not be removed: " + removed.error;
    }
    if (moveExisting)
    {
        auto pruned = after;
        pruned.previous.erase (std::remove_if (pruned.previous.begin(), pruned.previous.end(), [] (const auto& location)
        {
            return emptyLibrary (location) && syncPath (location.root / "packs", true) && syncPath (location.root, true);
        }), pruned.previous.end());
        if (pruned.previous.size() != after.previous.size())
        {
            const auto saved = writeConfiguration (configurationFile, pruned);
            if (saved != ConfigurationWrite::Failed) after = std::move (pruned);
            if (saved != ConfigurationWrite::Durable)
                result.error = "The new library is active. Cleanup of previous library settings could not be durably saved.";
        }
    }
    result.status = OperationStatus::Complete;
    result.locations = std::move (after);
    return result;
}
} // namespace duskstudio::sfz
