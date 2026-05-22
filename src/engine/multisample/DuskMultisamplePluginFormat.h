#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace duskstudio
{
// juce::AudioPluginFormat that makes .sfz (and, in 1.0, .sf2) files
// look like instrument plugins to the rest of the host. Registered
// in PluginManager alongside VST3 / LV2 / AU; the picker calls
// `getInstrumentDescriptions()` -> KnownPluginList::getTypes(),
// which now includes one PluginDescription per loaded .sfz file the
// user has touched, plus a "blank" entry that opens a file chooser
// when picked.
//
// File scanning is intentionally OFF (canScanForPlugins() = false):
// .sfz files live anywhere on disk + a single soundfont pack can
// be many hundreds of MB. Cluttering the picker with a recursive
// scan of the user's drive would be hostile. Discovery happens
// per-user-action via the file chooser or drag-drop.
class DuskMultisamplePluginFormat final : public juce::AudioPluginFormat
{
public:
    DuskMultisamplePluginFormat() = default;

    juce::String getName() const override                          { return "DuskMultisample"; }
    bool fileMightContainThisPluginType (const juce::String& path) override;
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results,
                              const juce::String& path) override;
    juce::String getNameOfPluginFromIdentifier (const juce::String& path) override;
    bool pluginNeedsRescanning (const juce::PluginDescription&) override    { return false; }
    bool doesPluginStillExist (const juce::PluginDescription& desc) override;
    bool canScanForPlugins() const override                        { return false; }
    bool isTrivialToScan() const override                          { return true; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&,
                                              bool, bool) override         { return {}; }
    juce::FileSearchPath getDefaultLocationsToSearch() override    { return {}; }
    bool requiresUnblockedMessageThreadDuringCreation (
        const juce::PluginDescription&) const override             { return false; }

    void createPluginInstance (const juce::PluginDescription& desc,
                               double initialSampleRate,
                               int initialBufferSize,
                               PluginCreationCallback callback) override;

    // Helper used by the picker shim - returns true for any path
    // ending in .sfz (or .sf2 once Phase 2 lands).
    static bool isSoundfontExtension (const juce::String& path) noexcept;
};
} // namespace duskstudio
