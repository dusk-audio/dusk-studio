#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace duskstudio
{
// Per-app plugin host infrastructure. Owns the AudioPluginFormatManager and
// the shared KnownPluginList (the "what's installed" cache). Channel strips
// hold their own PluginSlot which calls back here to instantiate plugins.
//
// MVP: registers JUCE's default formats (VST3 and any platform-natives that
// happen to compile in). NO automatic scan-on-startup - that's slow and
// unbounded; deferred to a later stage that runs scanning on a background
// thread with progress UI. For now, plugins are loaded by file path via
// `createPluginInstance(File)` and we add to KnownPluginList as we go so
// session save/restore can re-instantiate by description.
//
// Lifetime: a single PluginManager instance lives in AudioEngine for now
// (so the audio engine owns plugin infrastructure alongside the device
// manager). Could move to DuskStudioApp scope later if non-engine code needs
// access.
//
// Threading: ALL methods on this class are message-thread-only. The audio
// thread never touches the manager - plugin instances live in PluginSlot
// after construction and the audio thread only reads the slot's atomic
// `currentInstance` pointer. KnownPluginList is mutated in-place by
// scanInstalledPlugins / createPluginInstance and read by
// getInstrumentDescriptions / getKnownPluginList; concurrent access from
// other threads would race. Calling any method here from the audio thread
// is a bug.
class PluginManager
{
public:
    PluginManager();
    ~PluginManager();

    juce::AudioPluginFormatManager& getFormatManager() noexcept { return formatManager; }
    juce::KnownPluginList&          getKnownPluginList() noexcept { return knownPluginList; }

    // Out-of-process plugin hosting toggle. Off by default. When on,
    // PluginSlot::loadFromDescription forks the dusk-studio-plugin-host child
    // and routes processBlock / state I/O through RemotePluginConnection
    // instead of holding the AudioPluginInstance in-process. Off-platform
    // (Mac / Windows) the flag is harmless — PluginSlot's #if branches
    // compile out and we always take the in-process path.
    //
    // Read by PluginSlot at load time only; flipping mid-session affects
    // the next load, not currently-loaded plugins. Set via UI (later
    // phase) or the DUSKSTUDIO_USE_OOP_PLUGINS env var on app startup.
    void setOopEnabled (bool enable) noexcept { oopEnabled = enable; }
    bool isOopEnabled() const noexcept       { return oopEnabled; }

    // Path to the dusk-studio-plugin-host child binary. Computed lazily from
    // the running Focal executable's own location: the binary lives at
    // `<bundle dir>/dusk-studio-plugin-host` on Linux (CMake's
    // RUNTIME_OUTPUT_DIRECTORY = $<TARGET_FILE_DIR:Focal>). Returns an
    // empty string when OOP is unavailable on this platform.
    juce::String getHostExecutablePath() const;

    // Message-thread only. Reads knownPluginList; would race a concurrent
    // scan or createPluginInstance.
    //
    // Subset of getKnownPluginList()::getTypes() filtered by the donor-side
    // `isInstrument` flag (set by JUCE from the plugin's own self-report).
    // Used by the per-channel picker on MIDI tracks so the user only sees
    // synths / samplers, not effects. Computed on demand - the known list
    // changes only on user-triggered scans, so re-filtering each open is
    // cheap.
    juce::Array<juce::PluginDescription> getInstrumentDescriptions() const;

    // Mirror of getInstrumentDescriptions for the inverse case: effect
    // plugins (anything with isInstrument == false). Used by the per-
    // channel picker on Mono / Stereo tracks and by the aux send-FX
    // picker so instrument plugins (which need MIDI input to produce
    // sound) don't clutter the list of effect choices.
    juce::Array<juce::PluginDescription> getEffectDescriptions() const;

    // Message-thread only. Mutates knownPluginList and runs synchronous
    // plugin loading (file I/O, JUCE format-manager calls); never call
    // from the audio thread or any RT-sensitive context.
    //
    // Instantiate a plugin from a file path (e.g. a .vst3 bundle directory
    // on Linux). Returns nullptr on failure (file missing, format not
    // supported, plugin failed to load). Synchronous - may take 100s of ms
    // for a slow plugin's instantiation. Caller is responsible for calling
    // prepareToPlay before processing audio through the returned instance.
    //
    // On success the discovered PluginDescription is added to
    // knownPluginList so future session loads can resolve by description.
    std::unique_ptr<juce::AudioPluginInstance>
    createPluginInstance (const juce::File& pluginFile,
                           double sampleRate, int blockSize,
                           juce::String& errorMessage);

    // Message-thread only. Same threading constraints as the file overload.
    //
    // Same shape but resolve by a previously-discovered PluginDescription
    // (used when restoring a session - the plugin file path may have moved
    // but the description's uid + format matches).
    std::unique_ptr<juce::AudioPluginInstance>
    createPluginInstance (const juce::PluginDescription& desc,
                           double sampleRate, int blockSize,
                           juce::String& errorMessage);

    // Cache file lives under juce::File::userApplicationDataDirectory:
    //   ~/.config/Focal/plugin-cache.xml on Linux,
    //   ~/Library/Application Support/Focal/plugin-cache.xml on macOS.
    // Saved every time a plugin is successfully added to knownPluginList;
    // loaded (best-effort) at construction. Invalid entries are tolerated -
    // JUCE's KnownPluginList::recreateFromXml silently skips bad ones.
    juce::File getCacheFile() const;

    // Message-thread only. Mutates knownPluginList; UI should also block
    // any other call into this manager for the duration so getInstrument-
    // Descriptions / createPluginInstance don't observe a half-populated
    // list.
    //
    // Scans every supported plugin format's default install locations and
    // populates knownPluginList. Synchronous; can take 10-30 seconds the
    // first time on a system with many plugins. Returns the number of
    // plugins added (existing entries are not re-added). UI should run
    // this from a "Scanning..." modal so the user knows the app isn't
    // hung. Cache is auto-saved on success.
    int scanInstalledPlugins();

private:
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList          knownPluginList;
    bool                           oopEnabled { false };

    void loadCache();
    void saveCache() const;
};

inline juce::String PluginManager::getHostExecutablePath() const
{
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    return exe.getParentDirectory().getChildFile ("dusk-studio-plugin-host").getFullPathName();
   #else
    return {};
   #endif
}
} // namespace duskstudio
