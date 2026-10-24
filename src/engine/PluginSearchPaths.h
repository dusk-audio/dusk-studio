#pragma once

#include "../foundation/AppConfigDir.h"
#include "../foundation/Json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace duskstudio::pluginpaths
{
using Roots = std::vector<std::filesystem::path>;

namespace detail
{
inline std::filesystem::path spelling (const std::filesystem::path& path, std::error_code& error)
{
    auto result = std::filesystem::absolute (path, error).lexically_normal();
    if (! result.has_filename() && result.has_relative_path()) result = result.parent_path();
    return result;
}

inline bool sameSpelling (const std::filesystem::path& a, const std::filesystem::path& b)
{
   #if defined(_WIN32)
    return CompareStringOrdinal (a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
   #else
    return a == b;
   #endif
}

inline bool missingOrDenied (const std::error_code& error)
{
    return error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory
        || error == std::errc::permission_denied || error == std::errc::too_many_symbolic_link_levels;
}
} // namespace detail

// Absolute spellings without touching the filesystem, so a hung mount cannot
// stall a caller that only shows or edits the list.
inline Roots lexical (const Roots& roots)
{
    Roots result;
    for (const auto& root : roots)
    {
        if (root.empty()) continue;
        std::error_code error;
        auto path = detail::spelling (root, error);
        if (error) continue;
        if (std::none_of (result.begin(), result.end(), [&] (const auto& existing)
                          { return detail::sameSpelling (existing, path); }))
            result.push_back (std::move (path));
    }
    return result;
}

// Drops aliases of an earlier entry but keeps each survivor as it was spelled:
// sessions and the picker store these, and a link to a versioned folder must
// still resolve after the target moves.
inline Roots normalize (const Roots& roots)
{
    Roots result, identities;
    for (auto& path : lexical (roots))
    {
        std::error_code error;
        auto identity = std::filesystem::weakly_canonical (path, error);
        if (error) identity = path;
        const auto duplicate = std::any_of (identities.begin(), identities.end(), [&] (const auto& existing)
        {
            if (detail::sameSpelling (existing, identity)) return true;
            std::error_code sameError;
            return std::filesystem::equivalent (existing, identity, sameError) && ! sameError;
        });
        if (duplicate) continue;
        identities.push_back (std::move (identity));
        result.push_back (std::move (path));
    }
    return result;
}

inline std::filesystem::path settingsFile()
{
    const auto directory = dusk::fs::appConfigDir();
    return directory.empty() ? std::filesystem::path {} : directory / "plugin-search-paths.json";
}

inline Roots stored (const std::filesystem::path& file = settingsFile())
{
    if (file.empty() || dusk::fs::fileSize (file) > 1024 * 1024) return {};
    const auto json = dusk::json::Json::parse (dusk::fs::loadFileAsString (file), nullptr, false);
    if (! json.is_array()) return {};
    Roots paths;
    for (const auto& value : json)
        if (value.is_string())
        {
            const auto text = value.get<std::string>();
            if (text.find ('\0') == std::string::npos) paths.push_back (std::filesystem::u8path (text));
        }
    return lexical (paths);
}

inline Roots load (const std::filesystem::path& file = settingsFile())
{
    return normalize (stored (file));
}

inline bool save (const Roots& roots, std::string& error, const std::filesystem::path& file = settingsFile())
{
    error.clear();
    if (file.empty()) { error = "The settings folder is unavailable."; return false; }
    dusk::json::Json json = dusk::json::Json::array();
    for (const auto& path : lexical (roots)) json.push_back (path.u8string());
    std::error_code nativeError;
    std::filesystem::create_directories (file.parent_path(), nativeError);
    if (nativeError) { error = nativeError.message(); return false; }
    auto temporary = file;
    temporary += "." + std::to_string (std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";
    if (! dusk::fs::writeStringToFile (temporary, json.dump (2) + "\n", &nativeError))
    {
        std::error_code ignored;
        std::filesystem::remove (temporary, ignored);
        error = nativeError.message();
        return false;
    }
   #if defined(_WIN32)
    if (! MoveFileExW (dusk::fs::nativeIOPath (temporary).c_str(), dusk::fs::nativeIOPath (file).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        nativeError = { static_cast<int> (GetLastError()), std::system_category() };
   #else
    std::filesystem::rename (temporary, file, nativeError);
   #endif
    if (! nativeError) return true;
    error = nativeError.message();
    std::error_code ignored;
    std::filesystem::remove (temporary, ignored);
    return false;
}

inline Roots extend (Roots defaults, const Roots& custom)
{
    defaults.insert (defaults.end(), custom.begin(), custom.end());
    return normalize (defaults);
}

// A walk error other than a missing or unreadable entry sets walkError and
// skips only the directory it hit, so a caller can keep its previous results
// instead of publishing a shortened list.
inline Roots findBundles (const Roots& roots, std::string_view wantedExtension,
                          const std::atomic<bool>* abort = nullptr, bool* walkError = nullptr)
{
    const auto cancelled = [&] { return abort != nullptr && abort->load (std::memory_order_relaxed); };
    const auto extension = [] (const auto& path)
    {
        auto value = path.extension().u8string();
        std::transform (value.begin(), value.end(), value.begin(), [] (unsigned char c)
        { return c >= 'A' && c <= 'Z' ? static_cast<char> (c - 'A' + 'a') : static_cast<char> (c); });
        return value;
    };
    const auto isBundle = [] (const std::string& kind)
    { return kind == ".clap" || kind == ".vst3" || kind == ".lv2" || kind == ".component"; };
    bool failed = false;
    const auto check = [&failed] (const std::error_code& error)
    {
        if (error && ! detail::missingOrDenied (error)) failed = true;
    };
    Roots found;
    std::set<std::filesystem::path> visited;
    if (cancelled()) return {};
    for (const auto& root : normalize (roots))
    {
        if (cancelled()) return {};
        std::error_code error;
        if (! std::filesystem::is_directory (dusk::fs::nativeIOPath (root), error)) { check (error); continue; }
        const auto rootKind = extension (root);
        if (isBundle (rootKind))
        {
            if (rootKind == wantedExtension) found.push_back (root);
            continue;
        }
        const auto rootIdentity = std::filesystem::canonical (dusk::fs::nativeIOPath (root), error);
        if (error) { check (error); continue; }
        if (! visited.insert (rootIdentity).second) continue;
        Roots fromRoot;
        std::vector<std::filesystem::path> pending { root };
        while (! pending.empty())
        {
            if (cancelled()) return {};
            const auto directory = std::move (pending.back());
            pending.pop_back();
            std::filesystem::directory_iterator it (dusk::fs::nativeIOPath (directory),
                                                    std::filesystem::directory_options::skip_permission_denied, error);
            if (error)
            {
                if (error != std::errc::permission_denied) failed = true;
                continue;
            }
            for (std::filesystem::directory_iterator end; ! error && it != end; it.increment (error))
            {
                if (cancelled()) return {};
                const auto path = directory / it->path().filename();
                const auto kind = extension (path);
                std::error_code entryError;
                const bool isDirectory = it->is_directory (entryError);
                check (entryError);
                if (isBundle (kind))
                {
                    entryError.clear();
                    if (kind == wantedExtension && (isDirectory || it->is_regular_file (entryError)))
                        fromRoot.push_back (path);
                }
                else if (isDirectory)
                {
                    const auto identity = std::filesystem::canonical (it->path(), entryError);
                    check (entryError);
                    if (! entryError && visited.insert (identity).second) pending.push_back (path);
                }
            }
            if (error) failed = true;
        }
        std::sort (fromRoot.begin(), fromRoot.end());
        found.insert (found.end(), fromRoot.begin(), fromRoot.end());
    }
    if (cancelled()) return {};
    if (walkError != nullptr) *walkError = failed;
    auto result = normalize (found);
    std::sort (result.begin(), result.end());
    return result;
}
} // namespace duskstudio::pluginpaths
