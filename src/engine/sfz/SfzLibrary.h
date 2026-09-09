#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace duskstudio::sfz
{
enum class LibraryFormat
{
    Sfz,
    Sf2,
};

std::string_view formatName (LibraryFormat format) noexcept;

struct LibraryEntry
{
    std::filesystem::path path;
    std::string           displayName;
    std::string           folder;
    LibraryFormat         format = LibraryFormat::Sfz;
    std::int64_t          sizeBytes = 0;
};

// A root the user configured that could not be read. Reported rather than
// dropped: a library that silently lists nothing is indistinguishable from a
// library that is genuinely empty, and the user cannot fix what they cannot see.
struct RootProblem
{
    std::filesystem::path root;
    std::string           reason;
};

struct ScanResult
{
    std::vector<LibraryEntry> entries;
    std::vector<RootProblem>  problems;
    bool                      cancelled = false;
};

// Depth is capped rather than unbounded because a library root is routinely a
// pack collection nested a few levels deep, while an accidental root like the
// home directory is not something to walk to the bottom of.
constexpr int kMaxScanDepth = 6;

// Where instruments land when something else installed them. Distro packages
// own the first two entries on Linux. Everything here is a first guess and is
// skipped without complaint when absent; the user's own roots do the real work.
std::vector<std::filesystem::path> defaultLibraryRoots();

// Walks `roots` for .sfz and .sf2. Runs on the calling thread and blocks: the
// caller owns the worker. Not for the audio thread and not for the message
// thread. `cancel`, when set, is polled per directory and per file. `progress`,
// when set, is called with the number of roots finished and the total.
ScanResult scanLibraryRoots (const std::vector<std::filesystem::path>& roots,
                             const std::atomic<bool>* cancel = nullptr,
                             std::function<void (std::size_t, std::size_t)> progress = {});

// Case-insensitive substring match over the display name and the containing
// folder. An empty query matches everything, so clearing the box restores the
// full list without a rescan.
std::vector<const LibraryEntry*> filterEntries (const std::vector<LibraryEntry>& entries,
                                                std::string_view query);
} // namespace duskstudio::sfz
