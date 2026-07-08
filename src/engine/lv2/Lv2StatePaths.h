#pragma once

#include <filesystem>
#include <string>

// LV2 state:mapPath path translation for file-backed plugin state. Pure path
// logic, split out so it can be unit-tested without a lilv world or a real
// file-writing plugin (the live path is otherwise only exercised by a gated
// integration test). stateDir empty => paths pass through unchanged, matching
// the blob-only (in-memory) save behaviour.
namespace duskstudio::lv2::statepaths
{
// Restore side: an abstract path from a state blob resolves against <dir>/cur.
// Already-absolute abstract paths (and an empty stateDir) pass through.
inline std::string toAbsolute (const std::filesystem::path& stateDir,
                               const std::string& abstractPath)
{
    if (stateDir.empty() || std::filesystem::path (abstractPath).is_absolute())
        return abstractPath;
    return (stateDir / "cur" / std::filesystem::u8path (abstractPath)).u8string();
}

// Save side: an absolute path lilv hands us becomes a cur/-relative abstract
// path when the file lives under <dir>/cur; otherwise it passes through.
inline std::string toAbstract (const std::filesystem::path& stateDir,
                               const std::string& absolutePath)
{
    if (stateDir.empty()) return absolutePath;
    const auto cur = stateDir / "cur";
    const auto rel = std::filesystem::u8path (absolutePath).lexically_relative (cur);
    // lexically_relative yields "" for unrelated roots, "." for the dir itself,
    // and a "../"-leading path for siblings — none of which are children.
    if (! rel.empty() && rel != std::filesystem::path (".")
        && *rel.begin() != std::filesystem::path (".."))
        return rel.generic_string();
    return absolutePath;
}
} // namespace duskstudio::lv2::statepaths
