#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginSearchPaths.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace duskstudio::juce_compat
{
// JUCE's MidiBuffer::addEvent finds an event's place by walking the buffer from
// its start, so a block put in one event at a time costs the square of its
// events when they share a sample. OrderedMidiFill clears a JUCE MidiBuffer and
// fills it in one pass from events in time order: an event at or after the last
// one goes on the end of the buffer's public storage as the record addEvent
// writes there, an int32 sample position, a uint16 size and the bytes. Anything
// else takes addEvent: an event that sorts earlier, and a meta event, whose size
// JUCE reads from its length field. Either way the bytes are those addEvent
// alone would leave, whatever the order. No allocation once the buffer has its
// room (ensureSize).
template <typename Buffer>
class OrderedMidiFill
{
public:
    explicit OrderedMidiFill (Buffer& destination) noexcept : buffer (destination)
    {
        buffer.clear();
    }

    // As addEvent: false for an event the buffer cannot hold.
    bool add (const std::uint8_t* data, int maxBytes, int samplePosition)
    {
        const int numBytes = data != nullptr && maxBytes > 0 && samplePosition >= last
                               ? storedSize (data, maxBytes) : -1;
        if (numBytes < 0)
        {
            last = std::max (last, samplePosition);
            return buffer.addEvent (data, maxBytes, samplePosition);
        }
        if (numBytes == 0) return true;
        if (numBytes > 0xFFFF) return false;

        static constexpr int kHeaderBytes = (int) (sizeof (std::int32_t) + sizeof (std::uint16_t));
        auto& bytes = buffer.data;
        const int at = bytes.size();
        bytes.insertMultiple (at, 0, kHeaderBytes + numBytes);
        auto* record = bytes.begin() + at;
        const auto position = (std::int32_t) samplePosition;
        const auto size = (std::uint16_t) numBytes;
        std::memcpy (record, &position, sizeof position);
        std::memcpy (record + sizeof position, &size, sizeof size);
        std::memcpy (record + kHeaderBytes, data, (std::size_t) numBytes);
        last = samplePosition;
        return true;
    }

private:
    // The size addEvent stores for an event: 0 for one it drops, -1 for a meta
    // event, which it sizes itself.
    static int storedSize (const std::uint8_t* data, int maxBytes) noexcept
    {
        const int status = data[0];
        if (status == 0xF0 || status == 0xF7)
        {
            int size = 1;
            while (size < maxBytes)
                if (data[size++] == 0xF7) break;
            return size;
        }
        if (status == 0xFF) return -1;
        if (status < 0x80) return 0;
        int size = 1;
        if (status < 0xF0) size = (status & 0xE0) == 0xC0 ? 2 : 3;
        else if (status == 0xF2) size = 3;
        else if (status == 0xF1 || status == 0xF3) size = 2;
        return std::min (size, maxBytes);
    }

    Buffer& buffer;
    int last = std::numeric_limits<int>::min();
};

template <typename Source, typename Buffer>
void copyMidiInOrder (const Source& source, Buffer& destination)
{
    OrderedMidiFill<Buffer> fill (destination);
    for (const auto meta : source)
        fill.add (meta.data, meta.numBytes, meta.samplePosition);
}

// One-call shim hiding the AudioPluginFormatManager::addDefaultFormats
// vs the wayland fork's free addDefaultFormatsToManager() split.
// DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS is set by CMake at configure time
// (CMakeLists.txt scans the JUCE source for the free function symbol)
// so this picks the right API for any combination of platform + JUCE
// version. No __linux__ branch at the call site.
//
// Why CMake-time detection over SFINAE: the plugdata-team fork marks
// the member `= delete`, which passes SFINAE then fails at the call
// site. The upstream JUCE lacks the free function entirely, so
// `decltype(addDefaultFormatsToManager(...))` fails lookup at
// template definition time rather than substitution - also unusable
// for SFINAE. CMake-time text scan is the only clean cross-shape
// detection.
inline void addDefaultFormats (juce::AudioPluginFormatManager& fm)
{
   #if DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS
    // The fork's LV2 format rebuilds its class table itself, and only when its
    // first search found no specifications: a rebuild re-creates every class the
    // table holds, and the system lilv 0.24.12 its format runs leaks each duplicate.
    juce::addDefaultFormatsToManager (fm);
   #else
    fm.addDefaultFormats();
    for (auto* format : fm.getFormats())
        if (format != nullptr && format->getName() == "LV2")
        {
            // JUCE loads embedded LV2 specifications after its initial class table.
            // Its constructor already loaded the default folders, and searching
            // them again reloads every bundle; an empty search only rebuilds
            // the table.
            format->searchPathsForPlugins ({}, true, false);
        }
   #endif
}

// JUCE's VST3 and LV2 hosts cache a host-side parameter change and write it into
// the plug-in only inside the next process call, so state read before that call
// lacks it. Its AudioUnit host sets the parameter on the unit at once.
inline bool defersParameterChangesToProcess (const std::string& formatName)
{
    return formatName == "VST3" || formatName == "LV2";
}

// JUCE's VST3 host reports every restartComponent as a program change, whatever
// its flags: a latency re-announced from setActive as much as the restart that
// is a plug-in's only report of an edit made in its own editor.
inline bool reportsRestartsAsProgramChanges (const std::string& formatName)
{
    return formatName == "VST3";
}

// The folders a format's own search may walk. Custom folders stay out of the
// VST3 search: JUCE's walk follows directory links with no record of where it
// has been and never sees a cancel request. The native VST3 scan, whose rows
// replace the JUCE ones, reaches them through the loop-safe findBundles.
inline auto pluginSearchLocations (juce::AudioPluginFormat& format,
                                   const pluginpaths::Roots& custom)
{
    auto locations = format.getDefaultLocationsToSearch();
    if (format.getName() == "AudioUnit" || format.getName() == "VST3") return locations;
    // A custom folder a default already names, a tilde spelling included, would
    // reload every bundle in it. getChildFile resolves an absolute default to
    // its folder; a Windows default, spelled with environment variables, stays
    // relative and never matches, and no File is built from it.
    const auto add = [&] (const auto& path)
    {
        const juce::File folder (path.u8string());
        for (int i = 0; i < locations.getNumPaths(); ++i)
            if (folder.getChildFile (locations.getRawString (i)) == folder)
                return;
        locations.add (folder);
    };
    for (const auto& path : custom) add (path);
    locations.removeRedundantPaths();
    return locations;
}

// Returns how many descriptions the identifiers produced. A bundle path loads
// that bundle into the format's world; explicit bundles avoid nonrecursive
// LV2_PATH lookup and separator ambiguity.
inline int describeLv2 (juce::AudioPluginFormat& format, const std::vector<std::string>& identifiers,
                        const std::atomic<bool>* abort = nullptr)
{
    juce::OwnedArray<juce::PluginDescription> descriptions;
    for (const auto& identifier : identifiers)
    {
        if (abort != nullptr && abort->load (std::memory_order_relaxed)) break;
        format.findAllTypesForFile (descriptions, identifier);
    }
    return descriptions.size();
}

inline std::vector<std::string> bundleIdentifiers (const pluginpaths::Roots& bundles)
{
    std::vector<std::string> identifiers;
    for (const auto& bundle : bundles) identifiers.push_back (bundle.u8string());
    return identifiers;
}

class FormatManager : public juce::AudioPluginFormatManager
{
    struct PrimedRoots
    {
        pluginpaths::Roots roots;
        std::vector<bool> available;
    };
    std::mutex lv2PrimingMutex;
    std::map<const void*, PrimedRoots> primedLv2Roots;
    pluginpaths::Roots walkedLv2Bundles;
    std::map<const void*, std::set<std::filesystem::path>> loadedLv2Bundles;
    friend bool primeCustomLv2 (FormatManager&, const pluginpaths::Roots&, bool, const std::atomic<bool>*);
    friend void primeLv2Plugin (FormatManager&, const std::string&);
    friend pluginpaths::Roots customLv2Bundles (FormatManager&);
    friend void restoreCustomLv2Bundles (FormatManager&, pluginpaths::Roots);
};

// Adds one format, for a child that needs only that one: building the LV2
// format loads every installed LV2 bundle and writes JUCE's embedded LV2
// specifications to a folder in the temp folder. The fork's manager takes a
// unique_ptr, upstream's a raw pointer it then owns.
template <typename Format>
void addFormat (FormatManager& manager)
{
   #if DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS
    manager.addFormat (std::make_unique<Format>());
   #else
    auto* format = new Format();
    manager.addFormat (format);
    // As in addDefaultFormats: an empty search rebuilds the LV2 class table.
    if (format->getName() == "LV2") format->searchPathsForPlugins ({}, true, false);
   #endif
}

// False when cancelled or when the folder walk hit an error. Neither records
// anything, so a later call retries and the last complete bundle list survives.
inline bool primeCustomLv2 (FormatManager& manager, const pluginpaths::Roots& custom,
                            bool refresh = false, const std::atomic<bool>* abort = nullptr)
{
    const std::lock_guard<std::mutex> lock (manager.lv2PrimingMutex);
    const auto cancelled = [&] { return abort != nullptr && abort->load (std::memory_order_relaxed); };
    if (cancelled()) return false;
    FormatManager::PrimedRoots state { pluginpaths::normalize (custom), {} };
    for (const auto& root : state.roots)
    {
        std::error_code error;
        state.available.push_back (std::filesystem::is_directory (root, error));
    }
    std::optional<pluginpaths::Roots> bundles;
    bool walkError = false;
    for (auto* format : manager.getFormats())
        if (format != nullptr && format->getName() == "LV2")
        {
            const auto primed = manager.primedLv2Roots.find (format);
            if (! refresh && primed != manager.primedLv2Roots.end()
                && primed->second.roots == state.roots && primed->second.available == state.available) continue;
            manager.primedLv2Roots.erase (format);
            if (! bundles) bundles = pluginpaths::findBundles (state.roots, ".lv2", abort, &walkError);
            auto& loaded = manager.loadedLv2Bundles[format];
            // When it was built, the format loaded every bundle directly inside its
            // default locations, so a custom folder that is or reaches one leaves
            // those bundles alone. A Windows default is spelled with environment
            // variables and matches nothing.
            std::vector<std::filesystem::path> defaults;
            const auto locations = format->getDefaultLocationsToSearch();
            for (int i = 0; i < locations.getNumPaths(); ++i)
            {
                const auto raw = locations.getRawString (i);
                if (! raw.startsWithChar ('~') && ! std::filesystem::u8path (raw.toStdString()).is_absolute()) continue;
                std::error_code error;
                auto folder = std::filesystem::canonical (std::filesystem::u8path (locations[i].getFullPathName().toStdString()), error);
                if (! error) defaults.push_back (std::move (folder));
            }
            pluginpaths::Roots unseen;
            for (const auto& bundle : *bundles)
            {
                std::error_code error;
                const auto parent = std::filesystem::canonical (bundle, error).parent_path();
                const bool loadedBuilt = ! error && std::find (defaults.begin(), defaults.end(), parent) != defaults.end();
                if (loaded.count (bundle) == 0 && ! loadedBuilt) unseen.push_back (bundle);
            }
            describeLv2 (*format, bundleIdentifiers (unseen), abort);
            if (cancelled()) return false;
            loaded.insert (unseen.begin(), unseen.end());
            if (! walkError) manager.primedLv2Roots.emplace (format, state);
        }
    if (bundles && ! walkError) manager.walkedLv2Bundles = std::move (*bundles);
    return ! walkError;
}

// Loading a plug-in never walks the custom folders: a URI the format already
// knows needs nothing, and an unknown one is looked for only in the bundles
// the last complete walk found.
inline void primeLv2Plugin (FormatManager& manager, const std::string& uri)
{
    const std::lock_guard<std::mutex> lock (manager.lv2PrimingMutex);
    for (auto* format : manager.getFormats())
        if (format != nullptr && format->getName() == "LV2")
        {
            if (describeLv2 (*format, { uri }) != 0) continue;
            auto& loaded = manager.loadedLv2Bundles[format];
            for (const auto& bundle : manager.walkedLv2Bundles)
            {
                if (loaded.count (bundle) != 0) continue;
                describeLv2 (*format, bundleIdentifiers ({ bundle }));
                loaded.insert (bundle);
                if (describeLv2 (*format, { uri }) != 0) break;
            }
        }
}

inline pluginpaths::Roots customLv2Bundles (FormatManager& manager)
{
    const std::lock_guard<std::mutex> lock (manager.lv2PrimingMutex);
    return manager.walkedLv2Bundles;
}

inline void restoreCustomLv2Bundles (FormatManager& manager, pluginpaths::Roots bundles)
{
    const std::lock_guard<std::mutex> lock (manager.lv2PrimingMutex);
    manager.walkedLv2Bundles = std::move (bundles);
}
} // namespace duskstudio::juce_compat
