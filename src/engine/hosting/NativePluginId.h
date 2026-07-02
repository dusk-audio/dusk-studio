#pragma once

#include <juce_core/juce_core.h>

namespace duskstudio::hosting
{
// Native picker rows carry "which plugin inside the bundle" alongside the bundle
// path in juce::PluginDescription::fileOrIdentifier, newline-separated (a newline
// can't appear in a path or a class id). A bundle with one plugin still encodes
// its id so the decode is uniform; JUCE-format rows never pass through these.

inline juce::String joinNativeIdentifier (const juce::String& bundlePath,
                                          const juce::String& pluginId)
{
    return pluginId.isEmpty() ? bundlePath : bundlePath + "\n" + pluginId;
}

struct NativeIdentifier
{
    juce::String bundlePath;
    juce::String pluginId;   // empty = the format's default pick
};

inline NativeIdentifier splitNativeIdentifier (const juce::String& fileOrIdentifier)
{
    const int nl = fileOrIdentifier.indexOfChar ('\n');
    if (nl < 0) return { fileOrIdentifier, {} };
    return { fileOrIdentifier.substring (0, nl), fileOrIdentifier.substring (nl + 1) };
}
} // namespace duskstudio::hosting
