#include "AppConfig.h"
#include "../engine/AudioEngine.h"   // AudioEngine::getMaxWorkerCount()

#include "../foundation/Fs.h"
#include "../foundation/Text.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace duskstudio::appconfig
{
namespace stdfs = std::filesystem;

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

int numCpus()
{
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : (int) n;   // hardware_concurrency may report 0 (unknowable)
}

// Trailing-zero-trimmed decimal, matching JUCE String(float)'s readable form
// (1.5, not 1.500000) so the config file stays hand-editable.
std::string floatToString (float v)
{
    std::string s = std::to_string (v);
    if (s.find ('.') != std::string::npos)
    {
        size_t last = s.find_last_not_of ('0');
        if (s[last] == '.') --last;
        s.erase (last + 1);
    }
    return s;
}

bool isTruthy (const std::string& s)
{
    const auto low = dusk::text::toLowerCase (s);
    return s == "1" || low == "true" || low == "yes";
}

// getIntValue() coerces non-numeric strings to 0; require a well-formed
// non-negative integer so a garbage value falls back to the default instead
// of reading as a valid 0.
bool looksNumeric (const std::string& s)
{
    return ! s.empty() && s.find_first_not_of ("0123456789") == std::string::npos;
}

stdfs::path getStorePath()
{
    const auto cfg = dusk::fs::userConfigDir();
    if (cfg.empty()) return {};
    const auto cfgDir = cfg / "Dusk Studio";
    std::error_code ec;
    if (! stdfs::is_directory (cfgDir, ec) && ! stdfs::create_directories (cfgDir, ec))
        return {};   // config dir unusable (missing + uncreatable, or a non-dir file)
    return cfgDir / "app-config.properties";
}

std::vector<std::string> readLines (const stdfs::path& file)
{
    std::vector<std::string> out;
    std::ifstream in (file);
    std::string line;
    while (std::getline (in, line))
    {
        if (! line.empty() && line.back() == '\r') line.pop_back();
        out.push_back (line);
    }
    return out;
}

// Tiny key=value parser. The data is a handful of scalar entries - a real
// PropertiesFile is overkill, and JUCE's PropertiesFile doesn't honour XDG
// paths on Linux (it stores under ~/<folderName>/, not ~/.config/), which
// makes it the wrong tool for a per-machine config we want alongside
// window-state.txt and recent.txt.
std::string readKey (const std::string& key)
{
    const auto file = getStorePath();
    std::error_code ec;
    if (file.empty() || ! stdfs::is_regular_file (file, ec)) return {};

    for (const auto& raw : readLines (file))
    {
        const auto line = dusk::text::trim (raw);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find ('=');
        if (eq == std::string::npos || eq == 0) continue;
        if (dusk::text::trim (line.substr (0, eq)) == key)
            return dusk::text::trim (line.substr (eq + 1));
    }
    return {};
}

void writeKey (const std::string& key, const std::string& value)
{
    const auto file = getStorePath();
    if (file.empty()) return;

    std::error_code ec;
    auto lines = stdfs::is_regular_file (file, ec) ? readLines (file)
                                                   : std::vector<std::string> {};

    bool replaced = false;
    for (auto& raw : lines)
    {
        const auto trimmed = dusk::text::trim (raw);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        const auto eq = trimmed.find ('=');
        if (eq == std::string::npos || eq == 0) continue;
        if (dusk::text::trim (trimmed.substr (0, eq)) == key)
        {
            raw = key + "=" + value;
            replaced = true;
            break;
        }
    }
    if (! replaced) lines.push_back (key + "=" + value);

    dusk::fs::writeStringToFile (file, dusk::text::joinIntoString (lines, "\n"));
}
} // namespace

float getUiScaleOverride()
{
    const auto raw = readKey (kKeyUiScale);
    if (raw.empty()) return kUiScaleDefault;
    const float value = dusk::text::getFloatValue (raw);
    return std::clamp (value > 0.0f ? value : kUiScaleDefault, kUiScaleMin, kUiScaleMax);
}

void setUiScaleOverride (float scale)
{
    const float clamped = std::clamp (scale, kUiScaleMin, kUiScaleMax);
    writeKey (kKeyUiScale, floatToString (clamped));
}

bool getScanPluginsOnStartup()
{
    const auto raw = readKey (kKeyScanOnStartup);
    if (raw.empty()) return false;   // default off - caches are cheap, scans aren't
    return isTruthy (raw);
}

void setScanPluginsOnStartup (bool scan)
{
    writeKey (kKeyScanOnStartup, scan ? "1" : "0");
}

bool getTapeStripExpandedDefault()
{
    const auto raw = readKey (kKeyTapeStripExpanded);
    if (raw.empty()) return false;   // default collapsed - strips get full body
    return isTruthy (raw);
}

void setTapeStripExpandedDefault (bool expanded)
{
    writeKey (kKeyTapeStripExpanded, expanded ? "1" : "0");
}

bool getFollowPlayheadDefault()
{
    const auto raw = readKey (kKeyFollowPlayhead);
    if (raw.empty()) return false;   // default off - view stays put unless asked
    return isTruthy (raw);
}

void setFollowPlayheadDefault (bool follow)
{
    writeKey (kKeyFollowPlayhead, follow ? "1" : "0");
}

int getAutosaveIntervalSeconds()
{
    const auto raw = readKey (kKeyAutosaveInterval);
    if (! looksNumeric (raw)) return kAutosaveIntervalDefaultSec;
    const int v = dusk::text::getIntValue (raw);
    return (v >= 10 && v <= 600) ? v : kAutosaveIntervalDefaultSec;
}

void setAutosaveIntervalSeconds (int seconds)
{
    writeKey (kKeyAutosaveInterval, std::to_string (std::clamp (seconds, 10, 600)));
}

bool getMidiSoftTakeover()
{
    const auto raw = readKey (kKeyMidiSoftTakeover);
    if (raw.empty()) return false;   // default off - controllers track 1:1
    return isTruthy (raw);
}

void setMidiSoftTakeover (bool on)
{
    writeKey (kKeyMidiSoftTakeover, on ? "1" : "0");
}

StopBehavior getStopBehavior()
{
    const auto raw = readKey (kKeyStopBehavior);
    if (! looksNumeric (raw)) return StopBehavior::PauseInPlace;
    const int v = dusk::text::getIntValue (raw);
    if (v >= 0 && v <= 2) return (StopBehavior) v;
    return StopBehavior::PauseInPlace;
}

void setStopBehavior (StopBehavior b)
{
    writeKey (kKeyStopBehavior, std::to_string ((int) b));
}

MulticoreDspMode getMulticoreDspMode()
{
    const auto raw = readKey (kKeyMulticoreMode);
    if (! looksNumeric (raw)) return MulticoreDspMode::Auto;   // default: use spare cores
    const int v = dusk::text::getIntValue (raw);
    if (v >= 0 && v <= 2) return (MulticoreDspMode) v;
    return MulticoreDspMode::Auto;
}

void setMulticoreDspMode (MulticoreDspMode m)
{
    writeKey (kKeyMulticoreMode, std::to_string ((int) m));
}

int maxMulticoreWorkers()
{
    // Never advertise more than the engine will actually use, regardless of CPU
    // count - otherwise the settings UI / manual-worker cap drift above what
    // resolveWorkerCount can request. AudioEngine is the single source of truth.
    return std::clamp (numCpus() - 2, 0, AudioEngine::getMaxWorkerCount());
}

int getMulticoreManualWorkers()
{
    // On a host with no spare cores maxMulticoreWorkers() is 0, and runtime
    // clamps Manual to 0 too - so allow 0 here instead of forcing 1, otherwise
    // the stored/displayed count (1) disagrees with what actually runs (0).
    const int mx = maxMulticoreWorkers();
    const int lo = std::min (1, mx);   // 1 when workers are possible, else 0
    const auto raw = readKey (kKeyMulticoreManual);
    if (! looksNumeric (raw)) return mx;   // default to the full available count
    return std::clamp (dusk::text::getIntValue (raw), lo, mx);
}

void setMulticoreManualWorkers (int n)
{
    const int mx = maxMulticoreWorkers();
    const int lo = std::min (1, mx);
    writeKey (kKeyMulticoreManual, std::to_string (std::clamp (n, lo, mx)));
}

int resolveWorkerCount()
{
    switch (getMulticoreDspMode())
    {
        case MulticoreDspMode::Off:    return 0;
        // Auto fans out only on >= 4-core hosts (cores - 2 workers); smaller
        // machines stay single-core, matching the manual's documented behaviour.
        case MulticoreDspMode::Auto:   return numCpus() >= 4 ? maxMulticoreWorkers() : 0;
        case MulticoreDspMode::Manual: return std::min (getMulticoreManualWorkers(),
                                                        maxMulticoreWorkers());
    }
    return 0;
}

int getVkbCentreNote()
{
    const auto raw = readKey (kKeyVkbCentreNote);
    if (! looksNumeric (raw)) return kVkbCentreDefault;
    return std::clamp (dusk::text::getIntValue (raw), 0, 120);
}

void setVkbCentreNote (int midiNote)
{
    writeKey (kKeyVkbCentreNote, std::to_string (std::clamp (midiNote, 0, 120)));
}
} // namespace duskstudio::appconfig
