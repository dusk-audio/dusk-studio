#pragma once

#include <juce_core/juce_core.h>

namespace duskstudio
{
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
inline bool pluginBackingLooksDead (const juce::String& fileOrIdentifier)
{
    if (! juce::File::isAbsolutePath (fileOrIdentifier))
        return false;

    const juce::File f (fileOrIdentifier);
    if (! f.exists())     return true;
    if (f.existsAsFile()) return false;
    return f.findChildFiles (juce::File::findFiles, true).isEmpty();
}
} // namespace duskstudio
