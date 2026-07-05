#include "AppConfig.h"
#include "../engine/AudioEngine.h"   // AudioEngine::getMaxWorkerCount()

namespace duskstudio::appconfig
{
namespace
{
constexpr const char* kKeyUiScale            = "ui_scale";
constexpr const char* kKeyScanOnStartup      = "scan_plugins_on_startup";
constexpr const char* kKeyTapeStripExpanded  = "tape_strip_expanded_default";
constexpr const char* kKeyFollowPlayhead     = "follow_playhead_default";
constexpr const char* kKeyStopBehavior       = "stop_behavior";
constexpr const char* kKeyVkbCentreNote      = "vkb_centre_note";
constexpr const char* kKeyMulticoreMode      = "multicore_dsp_mode";
constexpr const char* kKeyMulticoreManual    = "multicore_dsp_workers";
constexpr const char* kKeyMidiSoftTakeover   = "midi_soft_takeover";
constexpr const char* kKeyAutosaveInterval   = "autosave_interval_sec";

juce::File getStorePath()
{
    auto cfgDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                      .getChildFile ("Dusk Studio");
    if (! cfgDir.exists()) cfgDir.createDirectory();
    return cfgDir.getChildFile ("app-config.properties");
}

// Tiny key=value parser. The data is one or two scalar entries - a real
// PropertiesFile is overkill, and JUCE's PropertiesFile doesn't honour
// XDG paths on Linux (it stores under ~/<folderName>/, not ~/.config/),
// which makes it the wrong tool for a per-machine config we want
// alongside window-state.txt and recent.txt.
juce::String readKey (const juce::String& key)
{
    const auto file = getStorePath();
    if (! file.existsAsFile()) return {};

    juce::StringArray lines;
    file.readLines (lines);
    for (auto& raw : lines)
    {
        const auto line = raw.trim();
        if (line.isEmpty() || line.startsWithChar ('#')) continue;
        const auto eq = line.indexOfChar ('=');
        if (eq <= 0) continue;
        if (line.substring (0, eq).trim() == key)
            return line.substring (eq + 1).trim();
    }
    return {};
}

void writeKey (const juce::String& key, const juce::String& value)
{
    const auto file = getStorePath();
    juce::StringArray lines;
    if (file.existsAsFile()) file.readLines (lines);

    bool replaced = false;
    for (auto& raw : lines)
    {
        const auto trimmed = raw.trim();
        if (trimmed.isEmpty() || trimmed.startsWithChar ('#')) continue;
        const auto eq = trimmed.indexOfChar ('=');
        if (eq <= 0) continue;
        if (trimmed.substring (0, eq).trim() == key)
        {
            raw = key + "=" + value;
            replaced = true;
            break;
        }
    }
    if (! replaced) lines.add (key + "=" + value);

    file.replaceWithText (lines.joinIntoString ("\n"));
}
} // namespace

float getUiScaleOverride()
{
    const auto raw = readKey (kKeyUiScale);
    if (raw.isEmpty()) return kUiScaleDefault;
    const float value = raw.getFloatValue();
    return juce::jlimit (kUiScaleMin, kUiScaleMax,
                          value > 0.0f ? value : kUiScaleDefault);
}

void setUiScaleOverride (float scale)
{
    const float clamped = juce::jlimit (kUiScaleMin, kUiScaleMax, scale);
    writeKey (kKeyUiScale, juce::String (clamped));
}

bool getScanPluginsOnStartup()
{
    const auto raw = readKey (kKeyScanOnStartup);
    if (raw.isEmpty()) return false;   // default off — caches are cheap, scans aren't
    return raw == "1" || raw.equalsIgnoreCase ("true")
        || raw.equalsIgnoreCase ("yes");
}

void setScanPluginsOnStartup (bool scan)
{
    writeKey (kKeyScanOnStartup, scan ? "1" : "0");
}

bool getTapeStripExpandedDefault()
{
    const auto raw = readKey (kKeyTapeStripExpanded);
    if (raw.isEmpty()) return false;   // default collapsed — strips get full body
    return raw == "1" || raw.equalsIgnoreCase ("true")
        || raw.equalsIgnoreCase ("yes");
}

void setTapeStripExpandedDefault (bool expanded)
{
    writeKey (kKeyTapeStripExpanded, expanded ? "1" : "0");
}

bool getFollowPlayheadDefault()
{
    const auto raw = readKey (kKeyFollowPlayhead);
    if (raw.isEmpty()) return false;   // default off — view stays put unless asked
    return raw == "1" || raw.equalsIgnoreCase ("true")
        || raw.equalsIgnoreCase ("yes");
}

void setFollowPlayheadDefault (bool follow)
{
    writeKey (kKeyFollowPlayhead, follow ? "1" : "0");
}

int getAutosaveIntervalSeconds()
{
    const auto raw = readKey (kKeyAutosaveInterval);
    if (raw.isEmpty()) return kAutosaveIntervalDefaultSec;
    const int v = raw.getIntValue();
    return (v >= 10 && v <= 600) ? v : kAutosaveIntervalDefaultSec;
}

void setAutosaveIntervalSeconds (int seconds)
{
    writeKey (kKeyAutosaveInterval, juce::String (juce::jlimit (10, 600, seconds)));
}

bool getMidiSoftTakeover()
{
    const auto raw = readKey (kKeyMidiSoftTakeover);
    if (raw.isEmpty()) return false;   // default off — controllers track 1:1
    return raw == "1" || raw.equalsIgnoreCase ("true")
        || raw.equalsIgnoreCase ("yes");
}

void setMidiSoftTakeover (bool on)
{
    writeKey (kKeyMidiSoftTakeover, on ? "1" : "0");
}

StopBehavior getStopBehavior()
{
    const auto raw = readKey (kKeyStopBehavior);
    if (raw.isEmpty()) return StopBehavior::PauseInPlace;
    const int v = raw.getIntValue();
    if (v >= 0 && v <= 2) return (StopBehavior) v;
    return StopBehavior::PauseInPlace;
}

void setStopBehavior (StopBehavior b)
{
    writeKey (kKeyStopBehavior, juce::String ((int) b));
}

MulticoreDspMode getMulticoreDspMode()
{
    const auto raw = readKey (kKeyMulticoreMode);
    // Reject empty or non-numeric: getIntValue() coerces "abc" to 0, which would
    // silently pass the range check and flip behaviour instead of defaulting.
    if (raw.isEmpty() || ! raw.containsOnly ("0123456789"))
        return MulticoreDspMode::Auto;   // default: use spare cores
    const int v = raw.getIntValue();
    if (v >= 0 && v <= 2) return (MulticoreDspMode) v;
    return MulticoreDspMode::Auto;
}

void setMulticoreDspMode (MulticoreDspMode m)
{
    writeKey (kKeyMulticoreMode, juce::String ((int) m));
}

int maxMulticoreWorkers()
{
    // Never advertise more than the engine will actually use, regardless of CPU
    // count — otherwise the settings UI / manual-worker cap drift above what
    // resolveWorkerCount can request. AudioEngine is the single source of truth.
    return juce::jlimit (0, AudioEngine::getMaxWorkerCount(),
                          juce::SystemStats::getNumCpus() - 2);
}

int getMulticoreManualWorkers()
{
    // On a host with no spare cores maxMulticoreWorkers() is 0, and runtime
    // clamps Manual to 0 too — so allow 0 here instead of forcing 1, otherwise
    // the stored/displayed count (1) disagrees with what actually runs (0).
    const int mx = maxMulticoreWorkers();
    const int lo = juce::jmin (1, mx);   // 1 when workers are possible, else 0
    const auto raw = readKey (kKeyMulticoreManual);
    if (raw.isEmpty()) return mx;        // default to the full available count
    return juce::jlimit (lo, mx, raw.getIntValue());
}

void setMulticoreManualWorkers (int n)
{
    const int mx = maxMulticoreWorkers();
    const int lo = juce::jmin (1, mx);
    writeKey (kKeyMulticoreManual, juce::String (juce::jlimit (lo, mx, n)));
}

int resolveWorkerCount()
{
    switch (getMulticoreDspMode())
    {
        case MulticoreDspMode::Off:    return 0;
        // Auto fans out only on >= 4-core hosts (cores - 2 workers); smaller
        // machines stay single-core, matching the manual's documented behaviour.
        case MulticoreDspMode::Auto:   return juce::SystemStats::getNumCpus() >= 4
                                                  ? maxMulticoreWorkers() : 0;
        case MulticoreDspMode::Manual: return juce::jmin (getMulticoreManualWorkers(),
                                                          maxMulticoreWorkers());
    }
    return 0;
}

int getVkbCentreNote()
{
    const auto raw = readKey (kKeyVkbCentreNote);
    if (raw.isEmpty()) return kVkbCentreDefault;
    return juce::jlimit (0, 120, raw.getIntValue());
}

void setVkbCentreNote (int midiNote)
{
    writeKey (kKeyVkbCentreNote, juce::String (juce::jlimit (0, 120, midiNote)));
}
} // namespace duskstudio::appconfig
