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

inline Roots normalize (const Roots& roots)
{
    Roots result;
    for (const auto& root : roots)
    {
        if (root.empty()) continue;
        std::error_code error;
        auto absolute = std::filesystem::absolute (root, error);
        if (error) continue;
        auto path = std::filesystem::weakly_canonical (absolute, error);
        if (error) path = absolute.lexically_normal();
        const auto duplicate = std::any_of (result.begin(), result.end(), [&] (const auto& existing)
        {
            if (existing == path) return true;
           #if defined(_WIN32)
            if (CompareStringOrdinal (existing.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL) return true;
           #endif
            std::error_code sameError;
            return std::filesystem::equivalent (existing, path, sameError) && ! sameError;
        });
        if (! duplicate) result.push_back (std::move (path));
    }
    return result;
}

inline std::filesystem::path settingsFile()
{
    const auto directory = dusk::fs::appConfigDir();
    return directory.empty() ? std::filesystem::path {} : directory / "plugin-search-paths.json";
}

inline Roots load (const std::filesystem::path& file = settingsFile())
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
    return normalize (paths);
}

inline bool save (const Roots& roots, std::string& error, const std::filesystem::path& file = settingsFile())
{
    error.clear();
    if (file.empty()) { error = "The settings folder is unavailable."; return false; }
    dusk::json::Json json = dusk::json::Json::array();
    for (const auto& path : normalize (roots)) json.push_back (path.u8string());
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

inline Roots findBundles (const Roots& roots, std::string_view wantedExtension,
                          const std::atomic<bool>* abort = nullptr)
{
    const auto cancelled = [&] { return abort != nullptr && abort->load (std::memory_order_relaxed); };
    const auto extension = [] (const auto& path)
    {
        auto value = path.extension().u8string();
        std::transform (value.begin(), value.end(), value.begin(), [] (unsigned char c)
        { return c >= 'A' && c <= 'Z' ? static_cast<char> (c - 'A' + 'a') : static_cast<char> (c); });
        return value;
    };
    Roots found;
    std::set<std::filesystem::path> visited;
    if (cancelled()) return {};
    for (const auto& root : normalize (roots))
    {
        if (cancelled()) return {};
        std::error_code error;
        if (! std::filesystem::is_directory (root, error)) continue;
        const auto rootKind = extension (root);
        if (rootKind == ".clap" || rootKind == ".vst3" || rootKind == ".lv2" || rootKind == ".component")
        {
            if (rootKind == wantedExtension) found.push_back (root);
            continue;
        }
        if (! visited.insert (root).second) continue;
        const auto options = std::filesystem::directory_options::skip_permission_denied
                           | std::filesystem::directory_options::follow_directory_symlink;
        for (std::filesystem::recursive_directory_iterator it (root, options, error), end;
             ! error && it != end; it.increment (error))
        {
            if (cancelled()) return {};
            const auto kind = extension (it->path());
            const bool bundle = kind == ".clap" || kind == ".vst3" || kind == ".lv2" || kind == ".component";
            std::error_code entryError;
            const bool directory = it->is_directory (entryError);
            if (bundle)
            {
                if (directory) it.disable_recursion_pending();
                if (kind == wantedExtension && (directory || it->is_regular_file (entryError))) found.push_back (it->path());
            }
            else if (directory)
            {
                const auto identity = std::filesystem::canonical (it->path(), entryError);
                if (entryError || ! visited.insert (identity).second) it.disable_recursion_pending();
            }
        }
    }
    if (cancelled()) return {};
    auto result = normalize (found);
    std::sort (result.begin(), result.end());
    return result;
}
} // namespace duskstudio::pluginpaths
