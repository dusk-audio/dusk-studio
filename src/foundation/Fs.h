#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Text.h"

// Filesystem helpers over std::filesystem for the JUCE File operations that
// don't map 1:1 (whole-file read/write, wildcard child search, size, mtime).
// The 1:1 operations (getChildFile, exists, getFileName, extension, ...) are
// used inline as std::filesystem at call sites, so they live no helper here.
namespace dusk::fs
{
inline std::string loadFileAsString (const std::filesystem::path& path)
{
    std::ifstream in (path, std::ios::binary);
    if (! in) return {};
    return std::string (std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>());
}

inline bool writeStringToFile (const std::filesystem::path& path, std::string_view text)
{
    std::ofstream out (path, std::ios::binary | std::ios::trunc);
    if (! out) return false;
    out.write (text.data(), (std::streamsize) text.size());
    out.flush();   // surface disk-full / write errors that only appear at flush
    return (bool) out;
}

inline std::int64_t fileSize (const std::filesystem::path& path) noexcept
{
    std::error_code ec;
    const auto sz = std::filesystem::file_size (path, ec);
    return ec ? 0 : (std::int64_t) sz;
}

// glob with '*' (any run) and '?' (one char), case-insensitive, matching JUCE.
inline bool matchesWildcard (std::string_view name, std::string_view pattern)
{
    const std::string n = text::toLowerCase (std::string (name));
    const std::string p = text::toLowerCase (std::string (pattern));
    size_t ni = 0, pi = 0, star = std::string::npos, mark = 0;
    while (ni < n.size())
    {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == n[ni])) { ++ni; ++pi; }
        else if (pi < p.size() && p[pi] == '*')                { star = pi++; mark = ni; }
        else if (star != std::string::npos)                    { pi = star + 1; ni = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

inline std::vector<std::filesystem::path> findChildFiles (const std::filesystem::path& dir,
                                                          std::string_view wildcard,
                                                          bool recursive)
{
    namespace stdfs = std::filesystem;
    std::vector<stdfs::path> out;
    std::error_code ec;

    auto take = [&] (const stdfs::directory_entry& e)
    {
        if (e.is_regular_file (ec) && matchesWildcard (e.path().filename().string(), wildcard))
            out.push_back (e.path());
    };

    if (recursive)
        for (auto it = stdfs::recursive_directory_iterator (dir, ec);
             ! ec && it != stdfs::recursive_directory_iterator(); it.increment (ec))
            take (*it);
    else
        for (auto it = stdfs::directory_iterator (dir, ec);
             ! ec && it != stdfs::directory_iterator(); it.increment (ec))
            take (*it);

    return out;
}

// std::filesystem::file_time_type has no C++17 clock_cast; shift by the offset
// between the file clock's now and the system clock's now.
inline std::int64_t lastModificationTimeSecs (const std::filesystem::path& path) noexcept
{
    std::error_code ec;
    const auto ft = std::filesystem::last_write_time (path, ec);
    if (ec) return 0;
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration> (
        ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return (std::int64_t) std::chrono::system_clock::to_time_t (sys);
}

// JUCE hasFileExtension: case-insensitive, tolerant of a leading dot on either side.
inline bool hasExtension (const std::filesystem::path& path, std::string_view ext)
{
    std::string a = text::toLowerCase (path.extension().string());   // ".wav"
    std::string b = text::toLowerCase (std::string (ext));           // "wav" or ".wav"
    if (! a.empty() && a[0] == '.') a.erase (0, 1);
    if (! b.empty() && b[0] == '.') b.erase (0, 1);
    return a == b;
}
} // namespace dusk::fs
