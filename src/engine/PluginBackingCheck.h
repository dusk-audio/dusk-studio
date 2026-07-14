#pragma once

#include "../foundation/Fs.h"

#include <filesystem>
#include <string>

namespace duskstudio
{
namespace detail
{
// A leading separator, a Windows drive colon or forward slash, or a leading
// '~' on POSIX. std::filesystem::path::is_absolute would reject the tilde
// form, so classify by hand.
inline bool pathLooksAbsolute (const std::string& path) noexcept
{
    if (path.empty()) return false;
    const char first = path[0];
#if defined(_WIN32)
    return first == '\\' || first == '/' || (path.size() > 1 && path[1] == ':');
#else
    return first == '/' || first == '~';
#endif
}

// JUCE's File(String) expanded a leading tilde; std::filesystem takes it
// literally, which would misread every home-relative entry as dead.
inline std::filesystem::path expandedPath (const std::string& p)
{
    if (! p.empty() && p[0] == '~' && (p.size() == 1 || p[1] == '/'))
        return dusk::fs::userHomeDir() / std::filesystem::u8path (p.substr (p.size() == 1 ? 1 : 2));
    return std::filesystem::u8path (p);
}
} // namespace detail

// True when a cached plugin entry's backing is gone or hollowed out (a broken /
// half-removed install) and should be pruned so the picker never offers an entry
// that can't load. Pure filesystem heuristic - never instantiates a plugin:
//   - a URI identifier (LV2) isn't a filesystem path, so it's left alone (can't
//     cheaply validate; never risk dropping a valid one).
//   - a present single-file backing (.so / .sf2 / single-file plugin) -> alive.
//   - a bundle directory (.vst3 / .component / .clap-as-folder) is dead only if
//     it holds no files at all - an empty shell, the classic broken-install
//     symptom. Any file inside (the binary on any platform - no extension on
//     macOS - moduleinfo.json, resources) counts as intact enough to attempt.
//   - a path that no longer exists -> dead.
inline bool pluginBackingLooksDead (const std::string& fileOrIdentifier)
{
    if (! detail::pathLooksAbsolute (fileOrIdentifier))
        return false;

    namespace stdfs = std::filesystem;
    const auto path = detail::expandedPath (fileOrIdentifier);

    std::error_code ec;
    if (! stdfs::exists (path, ec))         return true;
    if (stdfs::is_regular_file (path, ec))  return false;
    return dusk::fs::findChildFiles (path, "*", true).empty();
}
} // namespace duskstudio
