#pragma once

#include "SfzPackStore.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace duskstudio::sfz
{
struct LibraryLocation
{
    std::filesystem::path root;
    std::string marker;
};

struct LibraryLocations
{
    LibraryLocation active;
    std::vector<LibraryLocation> previous;
    std::string error;
};

struct LocationChangeResult : OperationResult
{
    std::optional<LibraryLocations> locations;
};

std::filesystem::path libraryLocationsFile();
LibraryLocations readLibraryLocations (const std::filesystem::path& file = libraryLocationsFile());
bool libraryLocationAvailable (const LibraryLocation&);
std::vector<InstalledPack> installedLibraryPacks (const LibraryLocations&);
std::optional<std::filesystem::path> resolveLibraryInstrument (const LibraryLocations&, const ManagedSfzLocator&);
std::optional<ManagedSfzLocator> libraryLocatorForFile (const LibraryLocations&, const std::filesystem::path&);
LocationChangeResult changeLibraryLocation (const LibraryLocations&, const std::filesystem::path& destination,
                                           bool moveExisting, const OperationControl&,
                                           const std::filesystem::path& configurationFile = libraryLocationsFile());
} // namespace duskstudio::sfz
