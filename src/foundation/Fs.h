#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Text.h"

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <pwd.h>
 #include <unistd.h>
#endif
#if defined(__APPLE__)
 #include <mach-o/dyld.h>
#endif

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

// ---- Special-location helpers (JUCE File::getSpecialLocation equivalents) ----
// These map to no std::filesystem primitive. Paths mirror JUCE exactly so a
// migrated build still finds the same config/recent/session files JUCE wrote.
// Verified against juce_core/native/juce_Files_{linux,mac,windows}.
namespace detail
{
inline bool isXdgWhitespace (char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

inline std::string trimBoth (std::string s)
{
    size_t b = 0, e = s.size();
    while (b < e && isXdgWhitespace (s[b])) ++b;
    while (e > b && isXdgWhitespace (s[e - 1])) --e;
    return s.substr (b, e - b);
}

inline std::string trimStart (std::string s)
{
    size_t b = 0;
    while (b < s.size() && isXdgWhitespace (s[b])) ++b;
    return s.substr (b);
}

// JUCE String::unquoted: drops one leading and one trailing quote independently.
inline std::string unquote (std::string s)
{
    if (s.empty()) return s;
    const size_t dropStart = (s.front() == '"' || s.front() == '\'') ? 1 : 0;
    const size_t dropEnd   = (s.back()  == '"' || s.back()  == '\'') ? 1 : 0;
    if (dropStart + dropEnd > s.size()) return {};
    return s.substr (dropStart, s.size() - dropStart - dropEnd);
}

inline std::string replaceAll (std::string s, const std::string& from, const std::string& to)
{
    if (from.empty()) return s;
    for (size_t pos = 0; (pos = s.find (from, pos)) != std::string::npos; pos += to.size())
        s.replace (pos, from.size(), to);
    return s;
}
} // namespace detail

inline std::filesystem::path userHomeDir()
{
#if defined(_WIN32)
    if (const char* profile = std::getenv ("USERPROFILE"))
        return std::filesystem::path (profile);
    return {};
#else
    if (const char* home = std::getenv ("HOME"))
        return std::filesystem::path (home);
    if (auto* pw = ::getpwuid (::getuid()))
        return std::filesystem::path (pw->pw_dir);
    return {};
#endif
}

#if ! defined(_WIN32) && ! defined(__APPLE__)
// Replicates juce_core's resolveXDGFolder: parses ~/.config/user-dirs.dirs for a
// KEY=... line (KEY is the freedesktop name, NOT an env var), returns its dir if
// it exists, else the tilde-expanded fallback.
inline std::filesystem::path resolveXdgFolder (const std::string& key,
                                               const std::filesystem::path& fallback)
{
    const auto home = userHomeDir();
    if (home.empty()) return fallback;   // never open/build a CWD-relative path

    std::ifstream in (home / ".config" / "user-dirs.dirs");
    std::string line;
    while (std::getline (in, line))
    {
        const std::string trimmed = detail::trimStart (line);
        if (trimmed.rfind (key, 0) != 0) continue;

        const auto eq = trimmed.find ('=');
        if (eq == std::string::npos) continue;

        std::string value = detail::replaceAll (trimmed.substr (eq + 1), "$HOME", home.string());
        value = detail::unquote (detail::trimBoth (value));

        std::error_code ec;
        if (std::filesystem::is_directory (value, ec))
            return std::filesystem::path (value);
    }
    return fallback;
}
#endif

// JUCE File::userApplicationDataDirectory.
inline std::filesystem::path userConfigDir()
{
#if defined(_WIN32)
    if (const char* appdata = std::getenv ("APPDATA"))
        return std::filesystem::path (appdata);   // %APPDATA%, matches CSIDL_APPDATA
    return {};
#else
    const auto home = userHomeDir();
    if (home.empty()) return {};   // never build a CWD-relative config path
 #if defined(__APPLE__)
    return home / "Library";
 #else
    return resolveXdgFolder ("XDG_CONFIG_HOME", home / ".config");
 #endif
#endif
}

// JUCE File::userMusicDirectory.
inline std::filesystem::path userMusicDir()
{
#if defined(_WIN32)
    if (const char* profile = std::getenv ("USERPROFILE"))
        return std::filesystem::path (profile) / "Music";
    return {};
#else
    const auto home = userHomeDir();
    if (home.empty()) return {};   // never build a CWD-relative music path
 #if defined(__APPLE__)
    return home / "Music";
 #else
    return resolveXdgFolder ("XDG_MUSIC_DIR", home / "Music");
 #endif
#endif
}

// JUCE File::tempDirectory.
inline std::filesystem::path tempDir()
{
#if defined(_WIN32)
    if (const char* t = std::getenv ("TEMP")) return std::filesystem::path (t);
    if (const char* t = std::getenv ("TMP"))  return std::filesystem::path (t);
    return std::filesystem::path ("C:\\Windows\\Temp");
#else
    if (const char* t = std::getenv ("TMPDIR")) return std::filesystem::path (t);
    return std::filesystem::path ("/tmp");
#endif
}

// JUCE File::currentExecutableFile.
inline std::filesystem::path currentExecutablePath()
{
#if defined(_WIN32)
    std::vector<wchar_t> buf (MAX_PATH);
    for (;;)
    {
        const DWORD n = GetModuleFileNameW (nullptr, buf.data(), (DWORD) buf.size());
        if (n == 0) return {};
        if (n < buf.size()) return std::filesystem::path (std::wstring (buf.data(), n));
        buf.resize (buf.size() * 2);   // truncated (n == size) - grow and retry
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath (nullptr, &size);   // sets size to the required length
    std::vector<char> buf (size + 1, '\0');
    if (_NSGetExecutablePath (buf.data(), &size) != 0) return {};
    return std::filesystem::u8path (std::string (buf.data()));
#else
    std::vector<char> buf (4096);
    for (;;)
    {
        const auto n = ::readlink ("/proc/self/exe", buf.data(), buf.size());
        if (n < 0) return {};
        if ((std::size_t) n < buf.size())
            return std::filesystem::u8path (std::string (buf.data(), (std::size_t) n));
        buf.resize (buf.size() * 2);   // possibly truncated - grow and retry
    }
#endif
}
} // namespace dusk::fs
