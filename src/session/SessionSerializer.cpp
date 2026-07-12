#include "SessionSerializer.h"
#include "../foundation/Json.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <limits>

#if JUCE_LINUX || JUCE_MAC
 #include <fcntl.h>
 #include <unistd.h>
#elif JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace duskstudio
{
namespace json = ::dusk::json;

namespace
{
// Save path builds ordered_json so key order follows insertion order, keeping
// session.json diffs stable across saves.
using JObj = nlohmann::ordered_json;

inline std::string toStd (const juce::String& s) { return s.toStdString(); }

// Bump whenever the JSON shape gains a NEW required field or changes
// the meaning of an existing one. Adding optional fields that default
// sensibly when absent does NOT require a bump - the load path
// gracefully ignores unknown keys.
// Loader rejects sessions with version > kFormatVersion (newer Dusk Studio
// can read older files via migrateSession; older Dusk Studio refusing
// newer files is safer than silently dropping fields).
constexpr int kFormatVersion = 3;

// Store a float from JSON only when it's finite, so a corrupt or hand-edited
// session.json can't push NaN / inf into a DSP parameter - the in-memory
// default is kept instead. Mirrors the master Pultec EQ's loadMasterFloat.
inline void storeFiniteFloat (std::atomic<float>& dst, const nlohmann::json& src) noexcept
{
    // Range-check the ORIGINAL double before narrowing: casting an out-of-float-range
    // double to float is UB, so reject non-finite / oversized values up front rather
    // than relying on the post-cast isfinite catching an inf the narrowing produced.
    if (! src.is_number()) return;
    const double d = src.get<double>();
    if (std::isfinite (d) && std::abs (d) <= (double) std::numeric_limits<float>::max())
        dst.store ((float) d);
}

// Scalar sibling for plain-float fields (region gain, etc.) that aren't atomics.
inline float finiteFloatOr (const nlohmann::json& src, float fallback) noexcept
{
    if (src.is_number())
    {
        const double d = src.get<double>();
        if (std::isfinite (d) && std::abs (d) <= (double) std::numeric_limits<float>::max())
            return (float) d;
    }
    return fallback;
}

// Parse one automation point from JSON, hardening every field against a
// hand-edited or truncated file: timeSamples >= 0 and recordedAtBPM finite.
// The lane evaluator's binary search assumes non-negative, sorted times, and
// no downstream retime math should ever see a non-finite anchor tempo. value
// is range-limited to [0, 1].
inline AutomationPoint parseAutomationPoint (const nlohmann::json& pv, double fallbackBpm) noexcept
{
    // The fallback itself must be strictly positive + finite - a 0/negative session
    // tempo would otherwise pass straight through as recordedAtBPM and break retime math.
    const float safeFallback = (std::isfinite (fallbackBpm) && fallbackBpm > 0.0) ? (float) fallbackBpm : 120.0f;
    AutomationPoint pt;
    // Validate finiteness AND int64 range BEFORE narrowing - casting a +inf or
    // out-of-range double (a hand-edited 1e40) to int64 is UB. 2^63 is exactly
    // representable as double; the strict upper bound rejects it. Non-finite /
    // negative / oversized time -> 0.
    const double rawT = json::getDouble (pv, "t", 0.0);
    pt.timeSamples   = (std::isfinite (rawT) && rawT > 0.0 && rawT < 9223372036854775808.0)
                           ? (std::int64_t) rawT : (std::int64_t) 0;
    // NaN slips through jlimit unchanged (NaN compares false both ways), so it
    // would otherwise poison the lane's binary search / a DSP param. Map it to a
    // safe default; ±inf is already clamped to the [0,1] range by jlimit.
    float rawV = json::getFloat (pv, "v", 0.0f);
    if (std::isnan (rawV)) rawV = 0.0f;
    pt.value         = juce::jlimit (0.0f, 1.0f, rawV);
    const float bpm  = json::getFloat (pv, "bpm", safeFallback);
    // A zero / negative bpm would break tempo-retime math (division by it),
    // so accept only a finite, strictly-positive value; else fall back.
    pt.recordedAtBPM = (std::isfinite (bpm) && bpm > 0.0f) ? bpm : safeFallback;
    return pt;
}
} // namespace

// Forward-migrate `root` from a known older version to kFormatVersion
// by mutating in place. Add cases as the format evolves:
//   case 1: do_v1_to_v2 (root); ++v; break;
//   case 2: do_v2_to_v3 (root); ++v; break;
// Each case MUST do its own ++v before break - the loop relies on the
// case body advancing the version. Without it the loop would spin
// forever on the same version.
// Returns true on success, false if a step refuses to migrate.
//
// Non-static + lives in namespace duskstudio so the Catch2 suite can
// forward-declare + exercise the migration loop directly (see
// tests/session_schema_migration.cpp). H1 schema-test hook.
bool migrateSession (nlohmann::json& root, int from)
{
    int v = from;
    while (v < kFormatVersion)
    {
        const int before = v;
        switch (v)
        {
            case 1:
                // v1 -> v2: no field changes. v1 sessions are
                // forward-compatible with v2 readers because v2 didn't
                // add or remove any keys - it just claimed the version
                // marker as a forward-compat anchor so the migrator
                // loop has an actual case to exercise. Bumping the
                // version field is the only mutation; the rest of the
                // tree is identical. Future schema changes get their
                // case here.
                if (root.is_object())
                    root["version"] = 2;
                ++v;
                break;

            case 2:
                // v2 -> v3: region "file" / mastering "source_file" may now
                // be SESSION-RELATIVE (portable sessions). v2 files only
                // ever stored absolute paths, which resolvePortablePath
                // still handles, so the version stamp is the only mutation.
                // The bump exists so v2 readers (0.10.x) REFUSE v3 files
                // loudly instead of silently failing to resolve relative
                // paths they predate.
                if (root.is_object())
                    root["version"] = 3;
                ++v;
                break;

            default:
                std::fprintf (stderr,
                              "[Dusk Studio/SessionSerializer] no migrator from v%d to v%d\n",
                              v, v + 1);
                return false;
        }
        // Belt-and-suspenders against a future migrator that forgets
        // its ++v - would otherwise spin the loop forever.
        if (v == before)
        {
            std::fprintf (stderr,
                          "[Dusk Studio/SessionSerializer] migrator at v%d failed to advance "
                          "version - aborting to avoid infinite loop\n", v);
            return false;
        }
    }
    juce::ignoreUnused (root);
    return true;
}

namespace
{

// Force the kernel page cache for `path` out to physical storage. Without
// this, a system crash between the temp-file write and the rename below
// can leave a renamed-but-empty file: the rename metadata reaches disk
// before the data does.
//
//   Linux/macOS : open + fsync + close.
//   Windows     : FlushFileBuffers requires GENERIC_WRITE per MSDN
//                 ("the file handle must have the GENERIC_WRITE access
//                 right"). For the regular-file case we open WRITE;
//                 for directories (parent-dir fsync below) we open with
//                 GENERIC_READ + FILE_FLAG_BACKUP_SEMANTICS because
//                 GENERIC_WRITE on a directory handle fails ACCESS
//                 DENIED on NTFS. Directory flush is a best-effort no-op
//                 on NTFS - rename+file-flush carries the data-
//                 durability invariant.
void fsyncFile (const juce::File& path)
{
   #if JUCE_LINUX || JUCE_MAC
    const int fd = ::open (path.getFullPathName().toRawUTF8(), O_RDONLY);
    if (fd < 0) return;
    (void) ::fsync (fd);
    ::close (fd);
   #elif JUCE_WINDOWS
    const auto wide = path.getFullPathName().toWideCharPointer();
    const bool isDir = path.isDirectory();
    const DWORD access = isDir ? GENERIC_READ : GENERIC_WRITE;
    const DWORD flags  = isDir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    HANDLE h = ::CreateFileW (wide,
                                access,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                flags,
                                nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    (void) ::FlushFileBuffers (h);
    ::CloseHandle (h);
   #else
    (void) path;
   #endif
}

// Resolve a MIDI device identifier (saved with a prior session) to its
// current index in juce::MidiInput::getAvailableDevices(). Returns -1 if
// the identifier doesn't match any currently-available device. The lookup
// is O(N) in the device list but called at most once per track on load,
// which is negligible compared to the JSON parse.
int resolveMidiInputIndexByIdentifier (const juce::String& identifier)
{
    if (identifier.isEmpty()) return -1;
    const auto devices = juce::MidiInput::getAvailableDevices();
    for (int i = 0; i < devices.size(); ++i)
        if (devices[i].identifier == identifier)
            return i;
    return -1;
}

int resolveMidiOutputIndexByIdentifier (const juce::String& identifier)
{
    if (identifier.isEmpty()) return -1;
    const auto devices = juce::MidiOutput::getAvailableDevices();
    for (int i = 0; i < devices.size(); ++i)
        if (devices[i].identifier == identifier)
            return i;
    return -1;
}

juce::String colourToHex (juce::Colour c)
{
    return juce::String::toHexString ((int) c.getARGB()).paddedLeft ('0', 8);
}

juce::Colour hexToColour (const juce::String& s, juce::Colour fallback)
{
    if (s.isEmpty()) return fallback;
    auto v = (std::uint32_t) s.getHexValue64();
    return juce::Colour (v);
}

// JSON key for each automation parameter. Stable across spec evolution -
// renames break sessions on round-trip. Add new params at the end of the
// enum AND here in the same order; never reuse a retired key.
static const char* automationParamKey (AutomationParam p) noexcept
{
    switch (p)
    {
        case AutomationParam::FaderDb:   return "fader_db";
        case AutomationParam::Pan:       return "pan";
        case AutomationParam::Mute:      return "mute";
        case AutomationParam::Solo:      return "solo";
        case AutomationParam::AuxSend1:  return "aux_send_1";
        case AutomationParam::AuxSend2:  return "aux_send_2";
        case AutomationParam::AuxSend3:  return "aux_send_3";
        case AutomationParam::AuxSend4:  return "aux_send_4";
        case AutomationParam::kCount:    break;
    }
    return "";
}

// Audio file paths are stored relative to the session directory whenever
// the file lives inside it (the normal case - RecordManager writes into
// <sessionDir>/audio). Absolute paths are kept for files referenced from
// outside the session folder. This keeps sessions portable across a
// rename, a copy to another machine, or the macOS<->Linux flow where the
// home prefix differs.
juce::String portablePath (const juce::File& f, const juce::File& sessionDir)
{
    if (f == juce::File()) return {};
    if (sessionDir != juce::File() && f.isAChildOf (sessionDir))
        // Canonical separator is '/' regardless of OS - Windows'
        // getRelativePathFrom returns backslashes, which a POSIX load
        // would treat as literal filename characters.
        return f.getRelativePathFrom (sessionDir).replaceCharacter ('\\', '/');
    return f.getFullPathName();
}

// Inverse of portablePath, with a recovery step: when the stored path
// (typically a stale absolute path from a moved/renamed session) doesn't
// exist, look for the same file name under <sessionDir>/audio before
// giving up. Unresolved paths are appended to `missing` so the UI can
// tell the user which files the session expected.
juce::File resolvePortablePath (const juce::String& stored,
                                const juce::File& sessionDir,
                                std::vector<juce::String>& missing)
{
    if (stored.isEmpty()) return {};

    // Recognise absolute paths cross-platform, not just for the running OS:
    // a leading '/' (POSIX absolute or UNC //server/...) and a Windows
    // drive-letter (C:/...) are all absolute even when juce::File::isAbsolutePath
    // reports false for the foreign form (e.g. "/home/..." on Windows, "C:/..."
    // on POSIX). This keeps re-root-by-name working for sessions moved between
    // operating systems instead of mis-joining the path under sessionDir.
    const auto normalised = stored.replaceCharacter ('\\', '/');
    const bool driveLetter = normalised.length() >= 3
                          && juce::CharacterFunctions::isLetter (normalised[0])
                          && normalised[1] == ':' && normalised[2] == '/';
    const bool isAbsolute = juce::File::isAbsolutePath (stored)
                         || driveLetter
                         || normalised.startsWith ("/");

    juce::File f;
    if (isAbsolute)
        f = juce::File (stored);
    else if (sessionDir != juce::File())
        // Normalise to '/' before resolving: a session saved by an older
        // Windows build stored backslash-relative paths, and getChildFile
        // on POSIX would treat those as part of the file name. Windows
        // accepts '/' natively, so this is a no-op there.
        f = sessionDir.getChildFile (stored.replaceCharacter ('\\', '/'));
    else
        return {};

    if (f.existsAsFile()) return f;

    // Re-root by file name, but only for paths that actually lived in a
    // session's audio folder: a relative "audio/..." (the canonical region
    // path) or an absolute path whose IMMEDIATE parent is audio/ (a moved/
    // renamed session). A relative path NOT under audio/ (e.g. a mastering
    // mixdown at the session root), and any absolute path where audio/ is not
    // the direct parent (external media that merely contains an audio/ segment),
    // must not silently remap to an unrelated same-named take - report missing.
    const auto fileName = normalised.fromLastOccurrenceOf ("/", false, false);
    const bool sessionLocal = normalised.startsWith ("audio/")
                           || (isAbsolute
                               && normalised.endsWith ("/audio/" + fileName));
    if (sessionLocal && sessionDir != juce::File() && fileName.isNotEmpty())
    {
        const auto byName = sessionDir.getChildFile ("audio").getChildFile (fileName);
        if (byName.existsAsFile()) return byName;
    }

    if (std::find (missing.begin(), missing.end(), stored) == missing.end())
        missing.push_back (stored);
    return f;
}

JObj trackToObject (const Track& t, const juce::File& sessionDir)
{
    JObj obj;
    obj["name"]   = toStd (t.name);
    obj["colour"] = toStd (colourToHex (t.colour));

    // Plugin-slot persistence. Empty strings (no plugin loaded) are written
    // verbatim - round-trip restoreFromSavedState treats empty as "no
    // plugin", which is the correct steady state for unused slots.
    if (t.pluginDescriptionXml.isNotEmpty())
        obj["plugin_desc_xml"] = toStd (t.pluginDescriptionXml);
    if (t.pluginStateBase64.isNotEmpty())
        obj["plugin_state"] = toStd (t.pluginStateBase64);
    if (t.nativeClapPath.isNotEmpty())
    {
        obj["native_clap_path"]   = toStd (t.nativeClapPath);
        obj["native_clap_plugin"] = toStd (t.nativeClapPluginId);
        obj["native_clap_state"]  = toStd (t.nativeClapStateBase64);
    }
    if (t.nativeLv2Path.isNotEmpty())
    {
        obj["native_lv2_path"]   = toStd (t.nativeLv2Path);
        obj["native_lv2_plugin"] = toStd (t.nativeLv2PluginId);
        obj["native_lv2_state"]  = toStd (t.nativeLv2StateBase64);
    }
    if (t.nativeVst3Path.isNotEmpty())
    {
        obj["native_vst3_path"]   = toStd (t.nativeVst3Path);
        obj["native_vst3_plugin"] = toStd (t.nativeVst3PluginId);
        obj["native_vst3_state"]  = toStd (t.nativeVst3StateBase64);
    }

    obj["fader_db"]     = t.strip.faderDb.load();
    obj["pan"]          = t.strip.pan.load();
    obj["mute"]         = t.strip.mute.load();
    obj["solo"]         = t.strip.solo.load();
    obj["phase_invert"] = t.strip.phaseInvert.load();
    if (t.strip.insertBypassed.load())
        obj["insert_bypassed"] = true;
    obj["fader_group"]    = t.strip.faderGroupId.load();
    obj["input_monitor"]  = t.inputMonitor.load();
    obj["print_effects"]  = t.printEffects.load();
    obj["input_source"]   = t.inputSource.load();
    obj["input_source_r"] = t.inputSourceR.load();
    // midi_input_idx is the legacy raw-int form (kept for back-compat
    // reading); midi_input_id is the stable identifier we resolve back to
    // an index on load. Older sessions without the id field fall through
    // to the int.
    obj["midi_input_idx"]  = t.midiInputIndex.load();
    obj["midi_input_id"]   = toStd (t.midiInputIdentifier);
    // External-MIDI-output side. Same shape as the input fields above.
    obj["midi_output_idx"] = t.midiOutputIndex.load();
    obj["midi_output_id"]  = toStd (t.midiOutputIdentifier);
    obj["midi_channel"]    = t.midiChannel.load();
    obj["track_mode"]      = t.mode.load();
    // Never write frozen=true without a restorable capture - a frozen flag with no
    // path would load as a locked track with no audio. Gate the flag on both, same
    // as the freeze fields below.
    const bool emitFrozen = t.frozen.load() && ! t.frozenAudioPath.isEmpty();
    obj["frozen"] = emitFrozen;
    // Only emit freeze fields when the track is actually frozen AND has a path -
    // a stale path left on a since-unfrozen (or reused) Session must not write
    // phantom freeze state that load() would then resurrect.
    if (emitFrozen)
    {
        obj["frozen_audio_path"] = toStd (portablePath (juce::File (t.frozenAudioPath), sessionDir));
        // Length + channels so load rebuilds frozenRegion without opening the
        // WAV (PlaybackEngine needs frozenRegion populated to play it back).
        obj["frozen_len"]      = (std::int64_t) t.frozenRegion.lengthInSamples;
        obj["frozen_channels"] = t.frozenRegion.numChannels;
        // Pre-freeze bypass so unfreeze-after-reload restores the user's setting.
        obj["frozen_plugin_bypass"] = t.frozenPluginBypass.load();
    }

    JObj buses = JObj::array();
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
        buses.push_back (t.strip.busAssign[(size_t) i].load());
    obj["bus_assign"] = std::move (buses);

    // Aux sends (continuous send level + pre/post-fader tap) - distinct
    // from busAssign which is the post-fader on/off routing toggle.
    JObj auxLevels = JObj::array(), auxPrePost = JObj::array();
    for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
    {
        auxLevels .push_back ((double) t.strip.auxSendDb[(size_t) i].load());
        auxPrePost.push_back (         t.strip.auxSendPreFader[(size_t) i].load());
    }
    obj["aux_send_db"]        = std::move (auxLevels);
    obj["aux_send_pre_fader"] = std::move (auxPrePost);

    JObj hpf;
    hpf["enabled"] = t.strip.hpfEnabled.load();
    hpf["freq"]    = t.strip.hpfFreq.load();
    obj["hpf"] = std::move (hpf);

    JObj lpf;
    lpf["enabled"] = t.strip.lpfEnabled.load();
    lpf["freq"]    = t.strip.lpfFreq.load();
    obj["lpf"] = std::move (lpf);

    JObj eq;
    eq["enabled"] = t.strip.eqEnabled.load();
    eq["type"]    = t.strip.eqBlackMode.load() ? "black" : "brown";
    auto bandObj = [] (float gain, float freq, float q = -1.0f)
    {
        JObj b;
        b["gain"] = gain;
        b["freq"] = freq;
        if (q >= 0.0f) b["q"] = q;
        return b;
    };
    eq["lf"] = bandObj (t.strip.lfGainDb.load(), t.strip.lfFreq.load());
    eq["lm"] = bandObj (t.strip.lmGainDb.load(), t.strip.lmFreq.load(), t.strip.lmQ.load());
    eq["hm"] = bandObj (t.strip.hmGainDb.load(), t.strip.hmFreq.load(), t.strip.hmQ.load());
    eq["hf"] = bandObj (t.strip.hfGainDb.load(), t.strip.hfFreq.load());
    obj["eq"] = std::move (eq);

    JObj comp;
    comp["enabled"]      = t.strip.compEnabled.load();
    comp["mode_picked"]  = t.strip.compModePicked.load();
    comp["mode"]         = t.strip.compMode.load();
    comp["threshold_db"] = t.strip.compThresholdDb.load();  // legacy meter-strip drag
    // Per-mode parameters - UniversalCompressor's native shape.
    comp["opto_peak_red"] = t.strip.compOptoPeakRed.load();
    comp["opto_gain"]     = t.strip.compOptoGain.load();
    comp["opto_limit"]    = t.strip.compOptoLimit.load();
    comp["fet_input"]        = t.strip.compFetInput.load();
    comp["fet_output"]       = t.strip.compFetOutput.load();
    comp["fet_attack"]       = t.strip.compFetAttack.load();
    comp["fet_release"]      = t.strip.compFetRelease.load();
    comp["fet_ratio"]        = t.strip.compFetRatio.load();
    comp["fet_threshold_db"] = t.strip.compFetThresholdDb.load();
    comp["vca_thresh_db"] = t.strip.compVcaThreshDb.load();
    comp["vca_ratio"]     = t.strip.compVcaRatio.load();
    comp["vca_attack"]    = t.strip.compVcaAttack.load();
    comp["vca_release"]   = t.strip.compVcaRelease.load();
    comp["vca_output"]    = t.strip.compVcaOutput.load();
    comp["vca_overeasy"]  = t.strip.compVcaOverEasy.load();
    comp["vca_detector_classic"] = t.strip.compVcaDetectorClassic.load();
    obj["comp"] = std::move (comp);

    // External hardware-insert state. The routing snapshot is read via
    // AtomicSnapshot::current() (message-thread side), the scalar knobs
    // via plain atomic loads. Persisting the fields here lets a saved
    // configuration survive a session reload regardless of whether the
    // strip's insertMode (Phase 3) is currently set to Hardware - if
    // the user flips back to Hardware later, the settings are restored.
    {
        JObj hwi;
        hwi["enabled"] = t.hardwareInsert.enabled.load();
        const auto& routing = t.hardwareInsert.routing.current();
        hwi["output_ch_l"]     = routing.outputChL;
        hwi["output_ch_r"]     = routing.outputChR;
        hwi["input_ch_l"]      = routing.inputChL;
        hwi["input_ch_r"]      = routing.inputChR;
        hwi["latency_samples"] = routing.latencySamples;
        hwi["format"]          = routing.format;
        hwi["output_gain_db"]  = t.hardwareInsert.outputGainDb.load();
        hwi["input_gain_db"]   = t.hardwareInsert.inputGainDb .load();
        hwi["dry_wet"]         = t.hardwareInsert.dryWet      .load();
        obj["hardware_insert"] = std::move (hwi);
    }

    JObj regions = JObj::array();
    for (auto& r : t.regions)
    {
        JObj rObj;
        rObj["file"]           = toStd (portablePath (r.file, sessionDir));
        rObj["timeline_start"] = (std::int64_t) r.timelineStart;
        rObj["length"]         = (std::int64_t) r.lengthInSamples;
        rObj["source_offset"]  = (std::int64_t) r.sourceOffset;
        if (r.numChannels != 1)
            rObj["num_channels"] = r.numChannels;
        // Fade samples emitted only when non-zero so existing sessions
        // don't gain noise. PlaybackEngine treats absent fields as 0.
        if (r.fadeInSamples  > 0) rObj["fade_in"]  = (std::int64_t) r.fadeInSamples;
        if (r.fadeOutSamples > 0) rObj["fade_out"] = (std::int64_t) r.fadeOutSamples;
        // Fade shapes emit only when non-Linear so older sessions stay
        // diff-clean. Stored as the int enum value (0..4) matching FadeShape.
        if (r.fadeInShape  != FadeShape::Linear) rObj["fade_in_shape"]  = (int) r.fadeInShape;
        if (r.fadeOutShape != FadeShape::Linear) rObj["fade_out_shape"] = (int) r.fadeOutShape;
        if (r.fadeInAuto)  rObj["fade_in_auto"]  = true;
        if (r.fadeOutAuto) rObj["fade_out_auto"] = true;
        // Skip gain_db when at unity to keep older sessions diff-clean
        // and avoid bloating the JSON for unedited regions. Float
        // exact-zero comparison is fine because the field is set
        // either by a default-construct (0.0f) or by an explicit user
        // drag - no float-arithmetic accumulation path exists.
        if (r.gainDb != 0.0f) rObj["gain_db"] = (double) r.gainDb;
        // Custom colour - only when the user explicitly set one
        // (default-constructed is transparent = "use track colour").
        // Stored as an 8-digit ARGB hex string via Colour::toString().
        if (! r.customColour.isTransparent())
            rObj["custom_colour"] = toStd (r.customColour.toString());
        // Label - skip when empty so unedited regions stay diff-clean.
        if (r.label.isNotEmpty())
            rObj["label"] = toStd (r.label);
        if (r.muted)  rObj["muted"]  = true;
        if (r.locked) rObj["locked"] = true;

        // Take history. Empty array on the common case (no overdubs); only
        // serialised when at least one prior take has been captured to keep
        // session.json compact.
        if (! r.previousTakes.empty())
        {
            JObj prior = JObj::array();
            for (auto& take : r.previousTakes)
            {
                JObj tObj;
                tObj["file"]          = toStd (portablePath (take.file, sessionDir));
                tObj["source_offset"] = (std::int64_t) take.sourceOffset;
                tObj["length"]        = (std::int64_t) take.lengthInSamples;
                prior.push_back (std::move (tObj));
            }
            rObj["previous_takes"] = std::move (prior);
        }

        regions.push_back (std::move (rObj));
    }
    obj["regions"] = std::move (regions);

    // MIDI regions. Same shape as audio regions (timelineStart + length)
    // but holds events in tick time instead of a WAV file path. Notes and
    // CCs are flat arrays so the JSON stays compact even on dense regions.
    // Only serialised when the track actually has MIDI data; absent for
    // audio tracks so existing sessions don't gain noise.
    if (! t.midiRegions.current().empty())
    {
        JObj midiRegions = JObj::array();
        for (const auto& r : t.midiRegions.current())
        {
            JObj rObj;
            rObj["timeline_start"] = (std::int64_t) r.timelineStart;
            rObj["length_samples"] = (std::int64_t) r.lengthInSamples;
            rObj["length_ticks"]   = (std::int64_t) r.lengthInTicks;

            JObj notes = JObj::array();
            for (const auto& n : r.notes)
            {
                JObj nObj;
                nObj["ch"]    = n.channel;
                nObj["note"]  = n.noteNumber;
                nObj["vel"]   = n.velocity;
                nObj["start"] = (std::int64_t) n.startTick;
                nObj["len"]   = (std::int64_t) n.lengthInTicks;
                notes.push_back (std::move (nObj));
            }
            rObj["notes"] = std::move (notes);

            if (! r.ccs.empty())
            {
                JObj ccs = JObj::array();
                for (const auto& c : r.ccs)
                {
                    JObj cObj;
                    cObj["ch"]   = c.channel;
                    cObj["ctrl"] = c.controller;
                    cObj["val"]  = c.value;
                    cObj["at"]   = (std::int64_t) c.atTick;
                    ccs.push_back (std::move (cObj));
                }
                rObj["ccs"] = std::move (ccs);
            }

            // Same custom-colour / label fields as AudioRegion -
            // skipped when unset so older sessions stay diff-clean.
            if (! r.customColour.isTransparent())
                rObj["custom_colour"] = toStd (r.customColour.toString());
            if (r.label.isNotEmpty())
                rObj["label"] = toStd (r.label);
            if (r.muted)  rObj["muted"]  = true;
            if (r.locked) rObj["locked"] = true;

            // BPM-change semantics (DuskStudio.md §5b). Default is locked,
            // so emit only when the user has explicitly unlocked. recorded_at_bpm
            // is always emitted so legacy sessions can be anchored deterministically
            // on first BPM change.
            if (! r.tempoLock) rObj["tempo_lock"] = false;
            rObj["recorded_at_bpm"] = r.recordedAtBPM;

            // MIDI take history mirrors audio: previously-recorded versions
            // of the same range stack here when an overdub fully overlaps
            // an existing region.
            if (! r.previousTakes.empty())
            {
                JObj prior = JObj::array();
                for (const auto& take : r.previousTakes)
                {
                    JObj tObj;
                    tObj["length_ticks"] = (std::int64_t) take.lengthInTicks;
                    JObj tnotes = JObj::array();
                    for (const auto& n : take.notes)
                    {
                        JObj nObj;
                        nObj["ch"]    = n.channel;
                        nObj["note"]  = n.noteNumber;
                        nObj["vel"]   = n.velocity;
                        nObj["start"] = (std::int64_t) n.startTick;
                        nObj["len"]   = (std::int64_t) n.lengthInTicks;
                        tnotes.push_back (std::move (nObj));
                    }
                    tObj["notes"] = std::move (tnotes);
                    if (! take.ccs.empty())
                    {
                        JObj tccs = JObj::array();
                        for (const auto& c : take.ccs)
                        {
                            JObj cObj;
                            cObj["ch"]   = c.channel;
                            cObj["ctrl"] = c.controller;
                            cObj["val"]  = c.value;
                            cObj["at"]   = (std::int64_t) c.atTick;
                            tccs.push_back (std::move (cObj));
                        }
                        tObj["ccs"] = std::move (tccs);
                    }
                    prior.push_back (std::move (tObj));
                }
                rObj["previous_takes"] = std::move (prior);
            }

            midiRegions.push_back (std::move (rObj));
        }
        obj["midi_regions"] = std::move (midiRegions);
    }

    // Automation: per-strip mode + one array per non-empty lane. Empty
    // lanes are omitted to keep session.json compact for the common case
    // (no automation recorded yet). The "automation" object is omitted
    // entirely when nothing has been recorded.
    obj["automation_mode"] = t.automationMode.load (std::memory_order_relaxed);
    JObj autoObj;
    bool anyLane = false;
    for (int p = 0; p < kNumAutomationParams; ++p)
    {
        const auto& lane = t.automationLanes[(size_t) p];
        if (lane.pointsConst().empty()) continue;
        JObj pts = JObj::array();
        for (const auto& pt : lane.pointsConst())
        {
            JObj pObj;
            pObj["t"]   = (std::int64_t) pt.timeSamples;
            pObj["v"]   = (double) pt.value;
            pObj["bpm"] = (double) pt.recordedAtBPM;
            pts.push_back (std::move (pObj));
        }
        autoObj[automationParamKey ((AutomationParam) p)] = std::move (pts);
        anyLane = true;
    }
    if (anyLane)
        obj["automation"] = std::move (autoObj);

    return obj;
}

JObj busToObject (const Bus& a)
{
    JObj obj;
    obj["name"]     = toStd (a.name);
    obj["colour"]   = toStd (colourToHex (a.colour));
    obj["fader_db"] = a.strip.faderDb.load();
    obj["pan"]      = a.strip.pan.load();
    obj["mute"]     = a.strip.mute.load();
    obj["solo"]     = a.strip.solo.load();

    obj["eq_enabled"] = a.strip.eqEnabled.load();
    obj["eq_lf_db"]   = a.strip.eqLfGainDb.load();
    obj["eq_mid_db"]  = a.strip.eqMidGainDb.load();
    obj["eq_hf_db"]   = a.strip.eqHfGainDb.load();

    obj["comp_enabled"]      = a.strip.compEnabled.load();
    obj["comp_thresh_db"]    = a.strip.compThreshDb.load();
    obj["comp_ratio"]        = a.strip.compRatio.load();
    obj["comp_attack_ms"]    = a.strip.compAttackMs.load();
    obj["comp_release_ms"]   = a.strip.compReleaseMs.load();
    obj["comp_release_auto"] = a.strip.compReleaseAuto.load();
    obj["comp_makeup_db"]    = a.strip.compMakeupDb.load();

    // Automation: same shape as tracks - mode + one array per non-empty lane.
    // Only FaderDb / Pan / Mute are populated for a bus, but we iterate all
    // params for symmetry (empty lanes are skipped).
    obj["automation_mode"] = a.strip.automationMode.load (std::memory_order_relaxed);
    JObj autoObj;
    bool anyLane = false;
    for (int p = 0; p < kNumAutomationParams; ++p)
    {
        const auto& lane = a.strip.automationLanes[(size_t) p];
        if (lane.pointsConst().empty()) continue;
        JObj pts = JObj::array();
        for (const auto& pt : lane.pointsConst())
        {
            JObj pObj;
            pObj["t"]   = (std::int64_t) pt.timeSamples;
            pObj["v"]   = (double) pt.value;
            pObj["bpm"] = (double) pt.recordedAtBPM;
            pts.push_back (std::move (pObj));
        }
        autoObj[automationParamKey ((AutomationParam) p)] = std::move (pts);
        anyLane = true;
    }
    if (anyLane)
        obj["automation"] = std::move (autoObj);

    return obj;
}

void restoreTrack (Track& t, const nlohmann::json& v, double defaultRecordBpm,
                   const juce::File& sessionDir,
                   std::vector<juce::String>& missingFiles)
{
    if (! v.is_object()) return;
    if (auto s = json::getString (v, "name");   ! s.empty()) t.name = s;
    if (auto s = json::getString (v, "colour"); ! s.empty()) t.colour = hexToColour (s, t.colour);

    // Plugin slot - strings remain empty when the property is absent (older
    // sessions or unused slots). AudioEngine::consumePluginStateAfterLoad
    // reads these back and asks each PluginSlot to reinstantiate.
    t.pluginDescriptionXml = json::getString (v, "plugin_desc_xml");
    t.pluginStateBase64    = json::getString (v, "plugin_state");
    t.nativeClapPath        = json::getString (v, "native_clap_path");
    t.nativeClapPluginId    = json::getString (v, "native_clap_plugin");
    t.nativeClapStateBase64 = json::getString (v, "native_clap_state");
    t.nativeLv2Path         = json::getString (v, "native_lv2_path");
    t.nativeLv2PluginId     = json::getString (v, "native_lv2_plugin");
    t.nativeLv2StateBase64  = json::getString (v, "native_lv2_state");
    t.nativeVst3Path        = json::getString (v, "native_vst3_path");
    t.nativeVst3PluginId    = json::getString (v, "native_vst3_plugin");
    t.nativeVst3StateBase64 = json::getString (v, "native_vst3_state");

    auto setFloat = [&v] (std::atomic<float>& a, const char* key)
    {
        if (json::has (v, key)) a.store (json::getFloat (v, key, 0.0f), std::memory_order_relaxed);
    };
    auto setBool = [&v] (std::atomic<bool>& a, const char* key)
    {
        if (json::has (v, key)) a.store (json::getBool (v, key, false), std::memory_order_relaxed);
    };
    auto setInt = [&v] (std::atomic<int>& a, const char* key)
    {
        if (json::has (v, key)) a.store (json::getInt (v, key, 0), std::memory_order_relaxed);
    };

    setFloat (t.strip.faderDb,      "fader_db");
    setFloat (t.strip.pan,          "pan");
    setBool  (t.strip.mute,         "mute");
    setBool  (t.strip.solo,         "solo");
    setBool  (t.strip.phaseInvert,  "phase_invert");
    t.strip.insertBypassed.store (json::getBool (v, "insert_bypassed", false));
    setInt   (t.strip.faderGroupId, "fader_group");
    setBool  (t.inputMonitor,       "input_monitor");
    setBool  (t.printEffects,       "print_effects");
    setInt   (t.inputSource,        "input_source");
    setInt   (t.inputSourceR,       "input_source_r");
    // MIDI input: prefer the stable identifier when present (resolved to
    // the current device list's index). Fall back to the legacy raw int
    // for sessions saved before identifiers existed, OR when the saved
    // identifier doesn't match any currently-available device (USB MIDI
    // gear unplugged, different machine, etc.) so the user can re-pick
    // without the index pointing at a wrong device.
    if (json::has (v, "midi_input_id"))
    {
        t.midiInputIdentifier = json::getString (v, "midi_input_id");
        const int resolved = resolveMidiInputIndexByIdentifier (t.midiInputIdentifier);
        if (resolved >= 0)
            t.midiInputIndex.store (resolved, std::memory_order_relaxed);
        else
            t.midiInputIndex.store (-1, std::memory_order_relaxed);
    }
    else
    {
        setInt (t.midiInputIndex, "midi_input_idx");
        t.midiInputIdentifier = juce::String();
    }
    // Same shape on the external-MIDI-output side.
    if (json::has (v, "midi_output_id"))
    {
        t.midiOutputIdentifier = json::getString (v, "midi_output_id");
        const int resolved = resolveMidiOutputIndexByIdentifier (t.midiOutputIdentifier);
        t.midiOutputIndex.store (resolved >= 0 ? resolved : -1,
                                  std::memory_order_relaxed);
    }
    else
    {
        setInt (t.midiOutputIndex, "midi_output_idx");
        t.midiOutputIdentifier = juce::String();
    }
    setInt   (t.midiChannel,        "midi_channel");
    setInt   (t.mode,               "track_mode");
    setBool  (t.frozen,             "frozen");
    if (auto fp = json::getString (v, "frozen_audio_path"); ! fp.empty())
    {
        // Engine-owned freeze WAV: a missing/zero-length one is dropped + recovered
        // transparently below, so it must NOT surface as a user-facing missing file.
        std::vector<juce::String> freezeIgnored;
        const auto wav = resolvePortablePath (fp, sessionDir, freezeIgnored);
        t.frozenAudioPath = wav.getFullPathName();
        // Rebuild frozenRegion so PlaybackEngine can open the baked WAV. The
        // instrument re-bypass is engine state, re-applied after load by
        // AudioEngine::reapplyFreezeState (the serializer can't reach strips).
        auto& fr = t.frozenRegion;
        fr = AudioRegion {};
        fr.file            = wav;
        fr.timelineStart   = 0;
        fr.lengthInSamples = json::getInt64 (v, "frozen_len", 0);
        fr.sourceOffset    = 0;
        fr.numChannels     = juce::jlimit (1, 2, json::getInt (v, "frozen_channels", 2));
        fr.gainDb          = 0.0f;
        t.frozenPluginBypass.store (json::getBool (v, "frozen_plugin_bypass", false));
        // A frozen flag with no usable WAV (missing file, zero length) is
        // meaningless - drop the whole freeze (flag + path + region) so it can't
        // be re-saved as phantom state, and the track falls back to live.
        if (! wav.existsAsFile() || fr.lengthInSamples <= 0)
        {
            t.frozen.store (false);
            t.frozenAudioPath.clear();
            t.frozenRegion = AudioRegion {};
            t.frozenPluginBypass.store (false);
        }
    }
    else
    {
        // frozen_audio_path absent => not frozen, regardless of a stale flag.
        // Fully reset freeze state (not just the flag) so a reused Session can't
        // carry a stale path / region forward and re-save it as phantom freeze.
        t.frozen.store (false);
        t.frozenAudioPath.clear();
        t.frozenRegion = AudioRegion {};
        t.frozenPluginBypass.store (false);
    }

    {
        const auto& buses = json::array (v, "bus_assign");
        const int n = juce::jmin (ChannelStripParams::kNumBuses, (int) buses.size());
        for (int i = 0; i < n; ++i)
            t.strip.busAssign[(size_t) i].store (buses[(size_t) i].is_boolean() && buses[(size_t) i].get<bool>(),
                                                 std::memory_order_relaxed);
    }

    {
        const auto& auxLevels = json::array (v, "aux_send_db");
        const int n = juce::jmin (ChannelStripParams::kNumAuxSends, (int) auxLevels.size());
        for (int i = 0; i < n; ++i)
            t.strip.auxSendDb[(size_t) i].store (auxLevels[(size_t) i].is_number()
                                                     ? (float) auxLevels[(size_t) i].get<double>() : 0.0f,
                                                 std::memory_order_relaxed);
    }
    {
        const auto& auxPrePost = json::array (v, "aux_send_pre_fader");
        const int n = juce::jmin (ChannelStripParams::kNumAuxSends, (int) auxPrePost.size());
        for (int i = 0; i < n; ++i)
            t.strip.auxSendPreFader[(size_t) i].store (auxPrePost[(size_t) i].is_boolean()
                                                           && auxPrePost[(size_t) i].get<bool>(),
                                                       std::memory_order_relaxed);
    }

    {
        const auto& hpf = json::child (v, "hpf");
        if (json::has (hpf, "enabled")) t.strip.hpfEnabled.store (json::getBool (hpf, "enabled", false));
        if (json::has (hpf, "freq"))    t.strip.hpfFreq.store (json::getFloat (hpf, "freq", 0.0f));
    }

    {
        const auto& lpf = json::child (v, "lpf");
        if (json::has (lpf, "enabled")) t.strip.lpfEnabled.store (json::getBool (lpf, "enabled", false));
        if (json::has (lpf, "freq"))    t.strip.lpfFreq.store (json::getFloat (lpf, "freq", 0.0f));
    }

    {
        const auto& eq = json::child (v, "eq");
        if (json::has (eq, "enabled")) t.strip.eqEnabled.store (json::getBool (eq, "enabled", false));
        if (auto type = json::getString (eq, "type"); ! type.empty())
            t.strip.eqBlackMode.store (type == "black");

        auto restoreBand = [&eq] (const char* key, std::atomic<float>* gain,
                                   std::atomic<float>* freq, std::atomic<float>* q)
        {
            const auto& b = json::child (eq, key);
            if (gain && json::has (b, "gain")) storeFiniteFloat (*gain, b["gain"]);
            if (freq && json::has (b, "freq")) storeFiniteFloat (*freq, b["freq"]);
            if (q    && json::has (b, "q"))    storeFiniteFloat (*q,    b["q"]);
        };
        restoreBand ("lf", &t.strip.lfGainDb, &t.strip.lfFreq, nullptr);
        restoreBand ("lm", &t.strip.lmGainDb, &t.strip.lmFreq, &t.strip.lmQ);
        restoreBand ("hm", &t.strip.hmGainDb, &t.strip.hmFreq, &t.strip.hmQ);
        restoreBand ("hf", &t.strip.hfGainDb, &t.strip.hfFreq, nullptr);
    }

    {
        const auto& comp = json::child (v, "comp");
        auto loadF = [&] (const char* key, std::atomic<float>& dst)
        {
            if (json::has (comp, key)) dst.store (json::getFloat (comp, key, 0.0f));
        };
        auto loadI = [&] (const char* key, std::atomic<int>& dst)
        {
            if (json::has (comp, key)) dst.store (json::getInt (comp, key, 0));
        };
        auto loadB = [&] (const char* key, std::atomic<bool>& dst)
        {
            if (json::has (comp, key)) dst.store (json::getBool (comp, key, false));
        };
        loadB ("enabled",     t.strip.compEnabled);
        loadB ("mode_picked", t.strip.compModePicked);
        loadI ("mode",        t.strip.compMode);
        loadF ("threshold_db", t.strip.compThresholdDb);
        loadF ("opto_peak_red", t.strip.compOptoPeakRed);
        loadF ("opto_gain",     t.strip.compOptoGain);
        loadB ("opto_limit",    t.strip.compOptoLimit);
        loadF ("fet_input",        t.strip.compFetInput);
        loadF ("fet_output",       t.strip.compFetOutput);
        loadF ("fet_attack",       t.strip.compFetAttack);
        loadF ("fet_release",      t.strip.compFetRelease);
        loadI ("fet_ratio",        t.strip.compFetRatio);
        loadF ("fet_threshold_db", t.strip.compFetThresholdDb);
        loadF ("vca_thresh_db", t.strip.compVcaThreshDb);
        loadF ("vca_ratio",     t.strip.compVcaRatio);
        loadF ("vca_attack",    t.strip.compVcaAttack);
        loadF ("vca_release",   t.strip.compVcaRelease);
        loadF ("vca_output",    t.strip.compVcaOutput);
        loadB ("vca_overeasy",  t.strip.compVcaOverEasy);
        loadB ("vca_detector_classic", t.strip.compVcaDetectorClassic);
    }

    // Enabled is stored unconditionally: load() reuses the live Session,
    // so a key absent from the file (pre-HW-insert session) must reset
    // the flag - inheriting the prior session's enabled insert would
    // route audio out to hardware this session never asked for.
    t.hardwareInsert.enabled.store (json::getBool (json::child (v, "hardware_insert"), "enabled", false));
    if (json::has (v, "hardware_insert"))
    {
        const auto& hwi = json::child (v, "hardware_insert");
        auto fresh = std::make_unique<HardwareInsertRouting>();
        // current() returns the existing snapshot - seeds any field that
        // the JSON doesn't carry (forward-compat with older sessions).
        *fresh = t.hardwareInsert.routing.current();
        if (json::has (hwi, "output_ch_l"))     fresh->outputChL      = json::getInt (hwi, "output_ch_l", 0);
        if (json::has (hwi, "output_ch_r"))     fresh->outputChR      = json::getInt (hwi, "output_ch_r", 0);
        if (json::has (hwi, "input_ch_l"))      fresh->inputChL       = json::getInt (hwi, "input_ch_l", 0);
        if (json::has (hwi, "input_ch_r"))      fresh->inputChR       = json::getInt (hwi, "input_ch_r", 0);
        if (json::has (hwi, "latency_samples")) fresh->latencySamples = json::getInt (hwi, "latency_samples", 0);
        if (json::has (hwi, "format"))          fresh->format         = json::getInt (hwi, "format", 0);
        t.hardwareInsert.routing.publish (std::move (fresh));

        if (json::has (hwi, "output_gain_db")) t.hardwareInsert.outputGainDb.store (json::getFloat (hwi, "output_gain_db", 0.0f));
        if (json::has (hwi, "input_gain_db"))  t.hardwareInsert.inputGainDb .store (json::getFloat (hwi, "input_gain_db", 0.0f));
        if (json::has (hwi, "dry_wet"))        t.hardwareInsert.dryWet      .store (json::getFloat (hwi, "dry_wet", 0.0f));
    }

    // Automation - per-strip mode + per-param point arrays. Lanes not in
    // the JSON stay empty (default-constructed). 3c-i loads only; 3c-ii
    // adds Write which mutates lanes mid-play via an atomic-swap pattern.
    //
    // Note: we mutate automationLanes BEFORE publishing the new
    // automationMode. Publishing the mode first would let the audio
    // thread observe Read/Touch and pull from a half-rebuilt lane
    // vector (mid-clear / mid-push_back). Release-store of the mode
    // after all lane mutations pairs with the engine's acquire-load
    // gating lane reads.
    for (auto& lane : t.automationLanes)
        lane.publishPoints ({});
    {
        const auto& autoVar = json::child (v, "automation");
        for (int p = 0; p < kNumAutomationParams; ++p)
        {
            const char* key = automationParamKey ((AutomationParam) p);
            const auto& pts = json::array (autoVar, key);
            if (pts.empty()) continue;
            std::vector<AutomationPoint> tmp;
            tmp.reserve (pts.size());
            for (const auto& pv : pts)
            {
                if (! pv.is_object()) continue;
                // Legacy / hand-edited points with no bpm anchor to the
                // session's load-time tempo (not a hard-coded 120) so a later
                // tempo change retimes them against the right reference.
                tmp.push_back (parseAutomationPoint (pv, defaultRecordBpm));
            }
            // Belt-and-braces sort - hand-edited JSON or out-of-order
            // writers can't violate the binary-search invariant in
            // evaluateLane().
            std::sort (tmp.begin(), tmp.end(),
                [] (const AutomationPoint& a, const AutomationPoint& b)
                { return a.timeSamples < b.timeSamples; });
            t.automationLanes[(size_t) p].publishPoints (std::move (tmp));
        }
    }
    // Unconditional store (default Off when absent) - same hazard as
    // restoreBus's automation_mode: a track left in Write by the prior
    // session must not keep writing over the loaded session's lanes.
    t.automationMode.store (json::getInt (v, "automation_mode", 0), std::memory_order_release);

    t.regions.clear();
    {
        const auto& regions = json::array (v, "regions");
        for (const auto& rv : regions)
        {
            if (! rv.is_object()) continue;
            AudioRegion r;
            r.file            = resolvePortablePath (json::getString (rv, "file"),
                                                      sessionDir, missingFiles);
            // Clamp every sample-domain field to >= 0 (mirrors the MIDI-note
            // loader below) so a hand-edited or truncated session.json can't
            // seed negative values that underflow PlaybackEngine's read-pointer
            // math (readStart = sourceOffset + (firstWithin - timelineStart)).
            r.timelineStart   = juce::jmax ((std::int64_t) 0, (std::int64_t) json::getInt64 (rv, "timeline_start", 0));
            r.lengthInSamples = juce::jmax ((std::int64_t) 0, (std::int64_t) json::getInt64 (rv, "length", 0));
            r.sourceOffset    = juce::jmax ((std::int64_t) 0, (std::int64_t) json::getInt64 (rv, "source_offset", 0));
            r.fadeInSamples   = juce::jmax ((std::int64_t) 0, (std::int64_t) json::getInt64 (rv, "fade_in", 0));
            r.fadeOutSamples  = juce::jmax ((std::int64_t) 0, (std::int64_t) json::getInt64 (rv, "fade_out", 0));
            auto loadShape = [] (int i) -> FadeShape
            {
                if (i >= 0 && i <= (int) FadeShape::RaisedCosine) return (FadeShape) i;
                return FadeShape::Linear;
            };
            r.fadeInShape     = loadShape (json::getInt (rv, "fade_in_shape", 0));
            r.fadeOutShape    = loadShape (json::getInt (rv, "fade_out_shape", 0));
            r.fadeInAuto      = json::getBool (rv, "fade_in_auto", false);
            r.fadeOutAuto     = json::getBool (rv, "fade_out_auto", false);
            r.numChannels     = juce::jlimit (1, 2, json::getInt (rv, "num_channels", 1));
            r.gainDb          = json::has (rv, "gain_db") ? finiteFloatOr (rv["gain_db"], 0.0f) : 0.0f;
            r.customColour    = json::has (rv, "custom_colour")
                                 ? juce::Colour::fromString (juce::String (json::getString (rv, "custom_colour")))
                                 : juce::Colour();
            r.label           = json::getString (rv, "label");
            r.muted           = json::getBool (rv, "muted", false);
            r.locked          = json::getBool (rv, "locked", false);

            {
                const auto& prior = json::array (rv, "previous_takes");
                for (const auto& tv : prior)
                {
                    if (! tv.is_object()) continue;
                    TakeRef take;
                    take.file            = resolvePortablePath (json::getString (tv, "file"),
                                                                 sessionDir, missingFiles);
                    take.sourceOffset    = juce::jmax ((std::int64_t) 0, (std::int64_t) json::getInt64 (tv, "source_offset", 0));
                    take.lengthInSamples = juce::jmax ((std::int64_t) 0, (std::int64_t) json::getInt64 (tv, "length", 0));
                    r.previousTakes.push_back (std::move (take));
                }
            }

            t.regions.push_back (std::move (r));
        }
    }

    // MIDI regions. Symmetric with the writer above; absent for audio
    // tracks. Helpers parse note + cc arrays out of a JSON value so the
    // top-level region and each take share the same code path.
    // Clamp every parsed field to its MIDI-spec range so a hand-edited or
    // truncated session.json can't seed out-of-range values into the model.
    auto parseNotes = [] (const nlohmann::json& notesVar, std::vector<MidiNote>& dst)
    {
        if (! notesVar.is_array()) return;
        dst.reserve (notesVar.size());
        for (const auto& nv : notesVar)
        {
            if (! nv.is_object()) continue;
            MidiNote n;
            n.channel       = juce::jlimit (1, 16,  json::getInt (nv, "ch", 1));
            n.noteNumber    = juce::jlimit (0, 127, json::getInt (nv, "note", 60));
            n.velocity      = juce::jlimit (1, 127, json::getInt (nv, "vel", 100));
            n.startTick     = juce::jmax ((std::int64_t) 0, (std::int64_t) json::getInt64 (nv, "start", 0));
            n.lengthInTicks = juce::jmax ((std::int64_t) 0, (std::int64_t) json::getInt64 (nv, "len", 0));
            dst.push_back (n);
        }
    };
    auto parseCcs = [] (const nlohmann::json& ccsVar, std::vector<MidiCc>& dst)
    {
        if (! ccsVar.is_array()) return;
        dst.reserve (ccsVar.size());
        for (const auto& cv : ccsVar)
        {
            if (! cv.is_object()) continue;
            MidiCc c;
            c.channel    = juce::jlimit (1, 16,  json::getInt (cv, "ch", 1));
            c.controller = juce::jlimit (0, 127, json::getInt (cv, "ctrl", 0));
            c.value      = juce::jlimit (0, 127, json::getInt (cv, "val", 0));
            c.atTick     = juce::jmax ((std::int64_t) 0, (std::int64_t) json::getInt64 (cv, "at", 0));
            dst.push_back (c);
        }
    };

    // Build the regions list off-snapshot, then publish atomically so the
    // audio thread either sees the prior set or the new one - never a
    // half-loaded state.
    auto freshMidi = std::make_unique<std::vector<MidiRegion>>();
    {
        const auto& midiRegions = json::array (v, "midi_regions");
        for (const auto& rv : midiRegions)
        {
            if (! rv.is_object()) continue;
            MidiRegion r;
            r.timelineStart   = juce::jmax ((std::int64_t) 0, (std::int64_t) json::getInt64 (rv, "timeline_start", 0));
            r.lengthInSamples = juce::jmax ((std::int64_t) 0, (std::int64_t) json::getInt64 (rv, "length_samples", 0));
            r.lengthInTicks   = juce::jmax ((std::int64_t) 0, (std::int64_t) json::getInt64 (rv, "length_ticks", 0));
            r.customColour    = json::has (rv, "custom_colour")
                                 ? juce::Colour::fromString (juce::String (json::getString (rv, "custom_colour")))
                                 : juce::Colour();
            r.label           = json::getString (rv, "label");
            r.muted           = json::getBool (rv, "muted", false);
            r.locked          = json::getBool (rv, "locked", false);

            // tempo_lock defaults true (spec §5b: locked is the default).
            // recorded_at_bpm defaults to the session's tempo at load time,
            // so legacy sessions that didn't record this field are
            // anchored to their own saved BPM rather than 120 - the first
            // BPM change after load won't silently mis-retime.
            r.tempoLock       = ! json::has (rv, "tempo_lock") || json::getBool (rv, "tempo_lock", true);
            // Reject a non-finite / non-positive bpm (would break tempo-retime
            // division), mirroring parseAutomationPoint's guard.
            const double rawBpm = json::getDouble (rv, "recorded_at_bpm", defaultRecordBpm);
            const double safeBpm = (std::isfinite (defaultRecordBpm) && defaultRecordBpm > 0.0) ? defaultRecordBpm : 120.0;
            r.recordedAtBPM   = (std::isfinite (rawBpm) && rawBpm > 0.0) ? rawBpm : safeBpm;

            parseNotes (json::array (rv, "notes"), r.notes);
            parseCcs   (json::array (rv, "ccs"),   r.ccs);

            {
                const auto& prior = json::array (rv, "previous_takes");
                for (const auto& tv : prior)
                {
                    if (! tv.is_object()) continue;
                    MidiTakeRef take;
                    take.lengthInTicks = juce::jmax ((std::int64_t) 0, (std::int64_t) json::getInt64 (tv, "length_ticks", 0));
                    parseNotes (json::array (tv, "notes"), take.notes);
                    parseCcs   (json::array (tv, "ccs"),   take.ccs);
                    r.previousTakes.push_back (std::move (take));
                }
            }

            freshMidi->push_back (std::move (r));
        }
    }
    t.midiRegions.publish (std::move (freshMidi));
}

void restoreBus (Bus& a, const nlohmann::json& v, double defaultRecordBpm)
{
    if (! v.is_object()) return;
    if (auto s = json::getString (v, "name");   ! s.empty()) a.name = s;
    if (auto s = json::getString (v, "colour"); ! s.empty()) a.colour = hexToColour (s, a.colour);
    if (json::has (v, "fader_db")) a.strip.faderDb.store (json::getFloat (v, "fader_db", 0.0f));
    if (json::has (v, "pan"))      a.strip.pan.store     (json::getFloat (v, "pan", 0.0f));
    if (json::has (v, "mute"))     a.strip.mute.store (json::getBool (v, "mute", false));
    if (json::has (v, "solo"))     a.strip.solo.store (json::getBool (v, "solo", false));

    if (json::has (v, "eq_enabled")) a.strip.eqEnabled.store (json::getBool (v, "eq_enabled", false));
    if (json::has (v, "eq_lf_db"))   storeFiniteFloat (a.strip.eqLfGainDb,  v["eq_lf_db"]);
    if (json::has (v, "eq_mid_db"))  storeFiniteFloat (a.strip.eqMidGainDb, v["eq_mid_db"]);
    if (json::has (v, "eq_hf_db"))   storeFiniteFloat (a.strip.eqHfGainDb,  v["eq_hf_db"]);

    if (json::has (v, "comp_enabled"))     a.strip.compEnabled  .store (json::getBool (v, "comp_enabled", false));
    if (json::has (v, "comp_thresh_db"))   a.strip.compThreshDb .store (json::getFloat (v, "comp_thresh_db", 0.0f));
    if (json::has (v, "comp_ratio"))       a.strip.compRatio    .store (json::getFloat (v, "comp_ratio", 0.0f));
    if (json::has (v, "comp_attack_ms"))   a.strip.compAttackMs .store (json::getFloat (v, "comp_attack_ms", 0.0f));
    if (json::has (v, "comp_release_ms"))  a.strip.compReleaseMs.store (json::getFloat (v, "comp_release_ms", 0.0f));
    if (json::has (v, "comp_release_auto")) a.strip.compReleaseAuto.store (json::getBool (v, "comp_release_auto", false));
    if (json::has (v, "comp_makeup_db"))   a.strip.compMakeupDb .store (json::getFloat (v, "comp_makeup_db", 0.0f));

    // Automation - mirror restoreTrack: clear lanes, rebuild from JSON, then
    // release-store the mode so the audio thread never reads a half-rebuilt
    // lane vector. Only FaderDb / Pan / Mute lanes are ever populated.
    for (auto& lane : a.strip.automationLanes)
        lane.publishPoints ({});
    {
        const auto& autoVar = json::child (v, "automation");
        for (int p = 0; p < kNumAutomationParams; ++p)
        {
            const char* key = automationParamKey ((AutomationParam) p);
            const auto& pts = json::array (autoVar, key);
            if (pts.empty()) continue;
            std::vector<AutomationPoint> tmp;
            tmp.reserve (pts.size());
            for (const auto& pv : pts)
            {
                if (! pv.is_object()) continue;
                tmp.push_back (parseAutomationPoint (pv, defaultRecordBpm));
            }
            std::sort (tmp.begin(), tmp.end(),
                [] (const AutomationPoint& x, const AutomationPoint& y)
                { return x.timeSamples < y.timeSamples; });
            a.strip.automationLanes[(size_t) p].publishPoints (std::move (tmp));
        }
    }
    // Always write the mode so a re-load into a reused strip can't inherit a
    // stale value: an absent key means the saved session predates automation -> Off.
    a.strip.automationMode.store (json::getInt (v, "automation_mode", 0), std::memory_order_release);
}
} // namespace

juce::String SessionSerializer::serialize (const Session& s)
{
    JObj root;
    root["version"] = kFormatVersion;
    if (s.sessionSampleRate > 0.0)
        root["session_sample_rate"] = s.sessionSampleRate;

    JObj tracks = JObj::array();
    for (int i = 0; i < Session::kNumTracks; ++i)
        tracks.push_back (trackToObject (s.track (i), s.getSessionDirectory()));
    root["tracks"] = std::move (tracks);

    JObj busesArr = JObj::array();
    for (int i = 0; i < Session::kNumBuses; ++i)
        busesArr.push_back (busToObject (s.bus (i)));
    root["buses"] = std::move (busesArr);

    JObj auxLanesArr = JObj::array();
    for (int i = 0; i < Session::kNumAuxLanes; ++i)
    {
        const auto& lane = s.auxLane (i);
        JObj obj;
        obj["name"]   = toStd (lane.name);
        obj["colour"] = toStd (colourToHex (lane.colour));
        obj["return_level_db"] = lane.params.returnLevelDb.load();
        obj["mute"]            = lane.params.mute.load();
        obj["output_pair"]     = lane.params.outputPair.load();
        // Per-slot plugin state. Empty strings serialise as empty - same
        // pattern as Track.
        JObj slots = JObj::array();
        for (int p = 0; p < AuxLaneParams::kMaxLanePlugins; ++p)
        {
            JObj slot;
            slot["plugin_desc_xml"] = toStd (lane.pluginDescriptionXml[(size_t) p]);
            slot["plugin_state"]    = toStd (lane.pluginStateBase64[(size_t) p]);

            // Native CLAP slot (mutually exclusive with the JUCE plugin). Only emit
            // when set, so JUCE-only sessions are unchanged.
            if (lane.nativeClapPath[(size_t) p].isNotEmpty())
            {
                slot["native_clap_path"]   = toStd (lane.nativeClapPath[(size_t) p]);
                slot["native_clap_plugin"] = toStd (lane.nativeClapPluginId[(size_t) p]);
                slot["native_clap_state"]  = toStd (lane.nativeClapStateBase64[(size_t) p]);
            }
            if (lane.nativeLv2Path[(size_t) p].isNotEmpty())
            {
                slot["native_lv2_path"]   = toStd (lane.nativeLv2Path[(size_t) p]);
                slot["native_lv2_plugin"] = toStd (lane.nativeLv2PluginId[(size_t) p]);
                slot["native_lv2_state"]  = toStd (lane.nativeLv2StateBase64[(size_t) p]);
            }
            if (lane.nativeVst3Path[(size_t) p].isNotEmpty())
            {
                slot["native_vst3_path"]   = toStd (lane.nativeVst3Path[(size_t) p]);
                slot["native_vst3_plugin"] = toStd (lane.nativeVst3PluginId[(size_t) p]);
                slot["native_vst3_state"]  = toStd (lane.nativeVst3StateBase64[(size_t) p]);
            }

            // Hardware-insert side of this slot. Same shape as the
            // Track::hardwareInsert block above.
            JObj hwi;
            const auto& hw = lane.hardwareInserts[(size_t) p];
            hwi["enabled"] = hw.enabled.load();
            const auto& routing = hw.routing.current();
            hwi["output_ch_l"]     = routing.outputChL;
            hwi["output_ch_r"]     = routing.outputChR;
            hwi["input_ch_l"]      = routing.inputChL;
            hwi["input_ch_r"]      = routing.inputChR;
            hwi["latency_samples"] = routing.latencySamples;
            hwi["format"]          = routing.format;
            hwi["output_gain_db"]  = hw.outputGainDb.load();
            hwi["input_gain_db"]   = hw.inputGainDb .load();
            hwi["dry_wet"]         = hw.dryWet      .load();
            slot["hardware_insert"] = std::move (hwi);

            slots.push_back (std::move (slot));
        }
        obj["plugin_slots"] = std::move (slots);

        // Automation: aux fader + mute lanes. Same shape as per-track.
        obj["automation_mode"] = lane.params.automationMode.load (std::memory_order_relaxed);
        JObj autoObj;
        bool anyLane = false;
        for (int p = 0; p < kNumAutomationParams; ++p)
        {
            const auto& al = lane.params.automationLanes[(size_t) p];
            if (al.pointsConst().empty()) continue;
            JObj pts = JObj::array();
            for (const auto& pt : al.pointsConst())
            {
                JObj pObj;
                pObj["t"]   = (std::int64_t) pt.timeSamples;
                pObj["v"]   = (double) pt.value;
                pObj["bpm"] = (double) pt.recordedAtBPM;
                pts.push_back (std::move (pObj));
            }
            autoObj[automationParamKey ((AutomationParam) p)] = std::move (pts);
            anyLane = true;
        }
        if (anyLane)
            obj["automation"] = std::move (autoObj);

        auxLanesArr.push_back (std::move (obj));
    }
    root["aux_lanes"] = std::move (auxLanesArr);

    JObj markersArr = JObj::array();
    for (const auto& m : s.getMarkers())
    {
        JObj obj;
        obj["name"]   = toStd (m.name);
        obj["time"]   = (std::int64_t) m.timelineSamples;
        obj["colour"] = toStd (colourToHex (m.colour));
        markersArr.push_back (std::move (obj));
    }
    root["markers"] = std::move (markersArr);

    JObj master;
    master["fader_db"]    = s.master().faderDb.load();
    master["output_pair"] = s.master().outputPair.load();
    if (s.master().mute.load (std::memory_order_relaxed))
        master["mute"] = true;
    if (s.master().monoSum.load (std::memory_order_relaxed))
        master["mono_sum"] = true;
    master["tape_enabled"] = s.master().tapeEnabled.load();
    master["tape_hq"]      = s.master().tapeHQ.load();

    // Pultec EQ (all atoms - every knob the user can move).
    master["eq_enabled"]            = s.master().eqEnabled.load();
    master["eq_lf_boost"]           = s.master().eqLfBoost.load();
    master["eq_lf_atten"]           = s.master().eqLfAtten.load();
    master["eq_lf_freq"]            = s.master().eqLfFreq.load();
    master["eq_hf_boost"]           = s.master().eqHfBoost.load();
    master["eq_hf_boost_freq"]      = s.master().eqHfBoostFreq.load();
    master["eq_hf_boost_bandwidth"] = s.master().eqHfBoostBandwidth.load();
    master["eq_hf_atten"]           = s.master().eqHfAtten.load();
    master["eq_hf_atten_freq"]      = s.master().eqHfAttenFreq.load();
    master["eq_output_gain_db"]     = s.master().eqOutputGainDb.load();

    // Bus comp (SSL-style).
    master["comp_enabled"]      = s.master().compEnabled.load();
    master["comp_thresh_db"]    = s.master().compThreshDb.load();
    master["comp_ratio"]        = s.master().compRatio.load();
    master["comp_attack_ms"]    = s.master().compAttackMs.load();
    master["comp_release_ms"]   = s.master().compReleaseMs.load();
    master["comp_release_auto"] = s.master().compReleaseAuto.load();
    master["comp_makeup_db"]    = s.master().compMakeupDb.load();
    // TapeMachine APVTS state (base64). Skipped when empty so existing
    // sessions don't gain a noisy field they don't need.
    if (s.master().tapeStateBase64.isNotEmpty())
        master["tape_state"] = toStd (s.master().tapeStateBase64);

    // Master automation: only FaderDb is automatable per spec. Reuses
    // the same shape as track / aux serialization for symmetry.
    master["automation_mode"] = s.master().automationMode.load (std::memory_order_relaxed);
    {
        JObj autoObj;
        bool anyLane = false;
        for (int p = 0; p < kNumAutomationParams; ++p)
        {
            const auto& al = s.master().automationLanes[(size_t) p];
            if (al.pointsConst().empty()) continue;
            JObj pts = JObj::array();
            for (const auto& pt : al.pointsConst())
            {
                JObj pObj;
                pObj["t"]   = (std::int64_t) pt.timeSamples;
                pObj["v"]   = (double) pt.value;
                pObj["bpm"] = (double) pt.recordedAtBPM;
                pts.push_back (std::move (pObj));
            }
            autoObj[automationParamKey ((AutomationParam) p)] = std::move (pts);
            anyLane = true;
        }
        if (anyLane)
            master["automation"] = std::move (autoObj);
    }

    root["master"] = std::move (master);

    // Mastering chain - separate from the master strip so its EQ/comp/limiter
    // settings can diverge from the in-mix master DSP.
    JObj mast;
    mast["source_file"] = toStd (portablePath (s.mastering().sourceFile, s.getSessionDirectory()));
    mast["eq_enabled"]  = s.mastering().eqEnabled.load();
    for (int b = 0; b < MasteringParams::kNumEqBands; ++b)
    {
        const auto idx = std::string ("eq_band_") + std::to_string (b);
        mast[idx + "_freq"]    = s.mastering().eqBandFreq[b].load();
        mast[idx + "_gain_db"] = s.mastering().eqBandGainDb[b].load();
        mast[idx + "_q"]       = s.mastering().eqBandQ[b].load();
    }
    mast["eq_lf_boost"]       = s.mastering().eqLfBoost.load();
    mast["eq_hf_boost"]       = s.mastering().eqHfBoost.load();
    mast["eq_hf_atten"]       = s.mastering().eqHfAtten.load();
    mast["eq_tube_drive"]     = s.mastering().eqTubeDrive.load();
    mast["eq_output_gain_db"] = s.mastering().eqOutputGainDb.load();
    mast["comp_enabled"]      = s.mastering().compEnabled.load();
    mast["comp_thresh_db"]    = s.mastering().compThreshDb.load();
    mast["comp_ratio"]        = s.mastering().compRatio.load();
    mast["comp_attack_ms"]    = s.mastering().compAttackMs.load();
    mast["comp_release_ms"]   = s.mastering().compReleaseMs.load();
    mast["comp_release_auto"] = s.mastering().compReleaseAuto.load();
    mast["comp_makeup_db"]    = s.mastering().compMakeupDb.load();
    mast["limiter_enabled"]      = s.mastering().limiterEnabled.load();
    mast["limiter_drive_db"]     = s.mastering().limiterDriveDb.load();
    mast["limiter_ceiling_db"]   = s.mastering().limiterCeilingDb.load();
    mast["limiter_release_ms"]   = s.mastering().limiterReleaseMs.load();
    mast["limiter_lookahead_ms"] = s.mastering().limiterLookaheadMs.load();
    mast["limiter_mode"]         = s.mastering().limiterMode.load();
    mast["limiter_stereo_link"]  = s.mastering().limiterStereoLink.load();
    mast["target_preset"]        = s.mastering().targetPresetIndex.load();
    root["mastering"] = std::move (mast);

    // Transport (loop + punch). Mirrored onto Session by
    // AudioEngine::publishTransportStateForSave before this call runs.
    JObj tport;
    tport["loop_enabled"]  = s.savedLoopEnabled;
    tport["loop_start"]    = (std::int64_t) s.savedLoopStart;
    tport["loop_end"]      = (std::int64_t) s.savedLoopEnd;
    tport["punch_enabled"] = s.savedPunchEnabled;
    tport["punch_in"]      = (std::int64_t) s.savedPunchIn;
    tport["punch_out"]     = (std::int64_t) s.savedPunchOut;
    tport["snap_to_grid"]      = s.snapToGrid;
    tport["audio_editor_snap"] = s.audioEditorSnap;
    tport["midi_editor_snap"]  = s.midiEditorSnap;
    tport["snap_resolution"]   = (int) s.snapResolution;
    tport["piano_roll_key_snap"] = s.pianoRollKeySnap;
    tport["tempo_bpm"]         = s.tempoBpm.load();
    if (! s.tempoMap.empty())
    {
        JObj tpArr = JObj::array();
        for (const auto& p : s.tempoMap.points())
        {
            JObj o;
            o["sample"] = p.timelineSamples;
            o["bpm"]    = (double) p.bpm;
            tpArr.push_back (std::move (o));
        }
        tport["tempo_points"] = std::move (tpArr);
    }
    tport["ui_stage"]         = s.uiStage.load();
    tport["sync_source_input"] = toStd (s.syncSourceInputIdentifier);
    tport["sync_follow_tempo"]    = s.externalSyncFollowsTempo.load();
    tport["sync_chase_transport"] = s.externalSyncChasesTransport.load();
    tport["sync_output"]         = toStd (s.syncOutputIdentifier);
    tport["sync_emit_clock"]     = s.syncOutputEmitClock.load();

    // Mackie Control Universal device pair + last-used assign mode.
    // Bank + selectedChannel are session-runtime state and intentionally
    // NOT persisted -- a fresh launch always starts on bank 0 / ch 0.
    tport["mcu_input_id"]   = toStd (s.mcu.inputIdentifier);
    tport["mcu_output_id"]  = toStd (s.mcu.outputIdentifier);
    tport["mcu_assign_mode"] = s.mcu.assignMode.load (std::memory_order_relaxed);
    tport["beats_per_bar"]     = s.beatsPerBar.load();
    tport["beat_unit"]         = s.beatUnit.load();
    tport["metronome_enabled"]          = s.metronomeEnabled.load();
    tport["metronome_vol_db"]           = s.metronomeVolDb.load();
    tport["metronome_click_recording"]  = s.metronomeClickWhileRecording.load();
    tport["metronome_click_playing"]    = s.metronomeClickWhilePlaying.load();
    tport["metronome_only_countin"]     = s.metronomeOnlyDuringCountIn.load();
    tport["metronome_polyphonic"]       = s.metronomePolyphonic.load();
    tport["count_in_enabled"]  = s.countInEnabled.load();
    tport["time_display_mode"] = s.timeDisplayMode.load();
    // Tascam-style transport-cluster state. The last-record point is
    // what the FFWD-while-stopped tap (= TO LAST REC) snaps to.
    tport["last_record_point"]  = (std::int64_t) s.lastRecordPointSamples.load();
    tport["pre_roll_seconds"]   = (double) s.preRollSeconds.load();
    tport["post_roll_seconds"]  = (double) s.postRollSeconds.load();
    tport["pre_roll_enabled"]   = s.preRollEnabled.load();
    tport["post_roll_enabled"]  = s.postRollEnabled.load();

    // MIDI controller bindings. Each entry stamps a (channel, dataNumber,
    // trigger) source onto a target enum + per-strip index. Only emit
    // when at least one binding exists so the JSON stays compact for
    // sessions that never wire a controller.
    if (! s.midiBindings.current().empty())
    {
        JObj arr = JObj::array();
        for (const auto& b : s.midiBindings.current())
        {
            JObj o;
            o["channel"]     = b.channel;
            o["data"]        = b.dataNumber;
            o["trigger"]     = (int) b.trigger;
            o["target"]      = (int) b.target;
            o["target_idx"]  = b.targetIndex;
            o["param_idx"]   = b.paramIndex;
            o["button_mode"] = (int) b.buttonMode;
            arr.push_back (std::move (o));
        }
        tport["midi_bindings"] = std::move (arr);
    }
    tport["oversampling_factor"] = s.oversamplingFactor.load();
    root["transport"] = std::move (tport);

    return juce::String (root.dump (2));
}

bool SessionSerializer::writeAtomic (const juce::File& target, const juce::String& json)
{
    // Atomic write: temp file + atomic replace. replaceFileIn (NOT
    // moveFileTo, which deletes the target first and leaves a window with
    // neither file on disk) is a bare rename() on POSIX / ReplaceFileW on
    // Windows, so a partial or missing session.json never appears even if
    // we crash mid-save.
    const auto parent = target.getParentDirectory();
    parent.createDirectory();
    auto tmp = parent.getChildFile (target.getFileName() + ".tmp");
    // Cleanup-on-failure: any early-return below leaves no junk *.tmp behind.
    auto cleanupTmp = [&tmp] { if (tmp.existsAsFile()) tmp.deleteFile(); };
    {
        juce::FileOutputStream out (tmp);
        if (! out.openedOk())                          { cleanupTmp(); return false; }
        out.setPosition (0);
        if (! out.truncate().wasOk())                  { cleanupTmp(); return false; }
        if (! out.writeText (json, false, false, "\n")) { cleanupTmp(); return false; }
        out.flush();
        if (out.getStatus().failed())                  { cleanupTmp(); return false; }
    }
    // Push the temp-file contents to physical storage BEFORE the
    // rename. Without this, a power loss / kernel oops between the
    // rename and the next page-cache flush can leave the canonical
    // session.json renamed but with empty / partial content.
    fsyncFile (tmp);
    // On replace failure, KEEP the tmp: it holds a complete, fsynced copy
    // of the state being saved - the only one if the target was lost.
    // The next successful save overwrites it.
    if (! tmp.replaceFileIn (target)) return false;
    // POSIX rename() is atomic in-memory only; durability of the rename
    // itself requires fsync on the PARENT DIRECTORY. Without this a power
    // loss after moveFileTo can vanish the rename and leave neither tmp
    // nor target on disk.
    fsyncFile (parent);
    return true;
}

bool SessionSerializer::save (const Session& s, const juce::File& target)
{
    return writeAtomic (target, serialize (s));
}

bool SessionSerializer::load (Session& s, const juce::File& source)
{
    if (! source.existsAsFile()) return false;
    nlohmann::json root = nlohmann::json::parse (source.loadFileAsString().toStdString(), nullptr, false);
    if (! root.is_object()) return false;

    s.missingAudioFilesAfterLoad.clear();

    // Format version gate. Missing key (pre-versioning sessions) is
    // treated as v1 - the format was effectively stable at v1 when the
    // version field landed. Future-versioned sessions are rejected up
    // front rather than partial-loaded; downgrading Dusk Studio to read a
    // session saved by a newer build silently dropping new state is
    // the worst-case bug class.
    const int fileVersion = json::getInt (root, "version", 1);
    if (fileVersion > kFormatVersion)
    {
        std::fprintf (stderr,
                      "[Dusk Studio/SessionSerializer] session.json version %d is newer "
                      "than this build's max supported version %d - refusing to "
                      "load. Upgrade Dusk Studio.\n",
                      fileVersion, kFormatVersion);
        return false;
    }
    if (fileVersion < kFormatVersion && ! migrateSession (root, fileVersion))
        return false;

    // Unconditional (reset-when-absent): a pre-SR-aware file must not inherit
    // the previous session's rate - 0 tells the load UI to adopt the device's.
    {
        const double sr = json::getDouble (root, "session_sample_rate", 0.0);
        s.sessionSampleRate = (std::isfinite (sr) && sr >= 8000.0 && sr <= 384000.0)
                                  ? sr : 0.0;
    }

    // Peek at transport.tempo_bpm BEFORE the track loop so legacy sessions
    // (no recorded_at_bpm field on MidiRegion) get anchored to their own
    // saved tempo rather than the struct default of 120. The transport
    // block is parsed in full further down; this is a read-only peek.
    double sessionLoadBpm = (double) s.tempoBpm.load (std::memory_order_relaxed);
    {
        const auto& tportPeek = json::child (root, "transport");
        if (json::has (tportPeek, "tempo_bpm"))
        {
            const double peeked = json::getDouble (tportPeek, "tempo_bpm", 0.0);
            // Clamp to the same 30-300 range the final tempoBpm store uses, so
            // the automation / MIDI fallback tempo stays aligned with the loaded
            // session tempo instead of using a raw out-of-range value. Reject a
            // value past float range before it narrows (UB) - keep the seeded tempo.
            if (std::isfinite (peeked) && std::abs (peeked) <= (double) std::numeric_limits<float>::max())
                sessionLoadBpm = juce::jlimit (30.0, 300.0, peeked);
        }
    }

    // A truncated / hand-edited file may lack whole section keys, or list
    // fewer buses / aux lanes than this build has. Every slot must still be
    // driven through restore - skipping any leaves the PREVIOUS session's
    // content (regions, plugins, mixer state) alive under the new session's
    // name. restoreBus and the aux-lane path inherit any absent property, so
    // an empty object is not enough to blank them: substitute the serialized
    // sections of a default Session, which carry every key at its model
    // default.
    nlohmann::json sectionDefaults;
    if (json::array (root, "tracks").empty()
        || (int) json::array (root, "buses").size() < Session::kNumBuses
        || (int) json::array (root, "aux_lanes").size() < Session::kNumAuxLanes)
        sectionDefaults = nlohmann::json::parse (serialize (Session{}).toStdString(), nullptr, false);

    {
        const auto& tracks = ! json::array (root, "tracks").empty()
                                 ? json::array (root, "tracks") : json::array (sectionDefaults, "tracks");
        // Restore EVERY track slot, not just the ones the JSON lists. A session
        // with fewer tracks than this build (hand-edited, or written by a tool
        // like the DP importer) must blank the surplus slots - otherwise they
        // keep the PREVIOUS session's regions / MIDI / automation / plugin, i.e.
        // ghost content that still plays back. Driving an absent slot through
        // restoreTrack with an empty object runs the same unconditional clears
        // the present-track path uses (regions, midiRegions, automation lanes,
        // automation mode, plugin state).
        const nlohmann::json emptyTrack = nlohmann::json::object();
        for (int i = 0; i < Session::kNumTracks; ++i)
            restoreTrack (s.track (i),
                          i < (int) tracks.size() ? tracks[(size_t) i] : emptyTrack,
                          sessionLoadBpm, s.getSessionDirectory(),
                          s.missingAudioFilesAfterLoad);
    }
    {
        const auto& busesArr    = ! json::array (root, "buses").empty()
                                     ? json::array (root, "buses") : json::array (sectionDefaults, "buses");
        const auto& busDefaults = json::array (sectionDefaults, "buses");
        for (int i = 0; i < Session::kNumBuses; ++i)
            restoreBus (s.bus (i),
                        i < (int) busesArr.size() ? busesArr[(size_t) i] : busDefaults[(size_t) i],
                        sessionLoadBpm);
    }
    {
        const auto& auxLanesArr  = ! json::array (root, "aux_lanes").empty()
                                      ? json::array (root, "aux_lanes") : json::array (sectionDefaults, "aux_lanes");
        const auto& auxDefaults  = json::array (sectionDefaults, "aux_lanes");
        for (int i = 0; i < Session::kNumAuxLanes; ++i)
        {
            const auto& v = i < (int) auxLanesArr.size() ? auxLanesArr[(size_t) i] : auxDefaults[(size_t) i];
            if (! v.is_object()) continue;
            auto& lane = s.auxLane (i);
            if (auto str = json::getString (v, "name");   ! str.empty()) lane.name   = str;
            if (auto str = json::getString (v, "colour"); ! str.empty()) lane.colour = hexToColour (str, lane.colour);
            if (json::has (v, "return_level_db"))
                lane.params.returnLevelDb.store (juce::jlimit (-100.0f, 12.0f, json::getFloat (v, "return_level_db", 0.0f)));
            if (json::has (v, "mute"))
                lane.params.mute.store (json::getBool (v, "mute", false));
            if (json::has (v, "output_pair"))
                lane.params.outputPair.store (json::getInt (v, "output_pair", -1));
            else
                lane.params.outputPair.store (-1);   // model default: Master only
            {
                const auto& slots = json::array (v, "plugin_slots");
                const int sn = juce::jmin (AuxLaneParams::kMaxLanePlugins, (int) slots.size());
                for (int p = 0; p < sn; ++p)
                {
                    const auto& sv = slots[(size_t) p];
                    if (! sv.is_object()) continue;
                    lane.pluginDescriptionXml[(size_t) p] = json::getString (sv, "plugin_desc_xml");
                    lane.pluginStateBase64[(size_t) p]    = json::getString (sv, "plugin_state");
                    lane.nativeClapPath[(size_t) p]        = json::getString (sv, "native_clap_path");
                    lane.nativeClapPluginId[(size_t) p]    = json::getString (sv, "native_clap_plugin");
                    lane.nativeClapStateBase64[(size_t) p] = json::getString (sv, "native_clap_state");
                    lane.nativeLv2Path[(size_t) p]         = json::getString (sv, "native_lv2_path");
                    lane.nativeLv2PluginId[(size_t) p]     = json::getString (sv, "native_lv2_plugin");
                    lane.nativeLv2StateBase64[(size_t) p]  = json::getString (sv, "native_lv2_state");
                    lane.nativeVst3Path[(size_t) p]        = json::getString (sv, "native_vst3_path");
                    lane.nativeVst3PluginId[(size_t) p]    = json::getString (sv, "native_vst3_plugin");
                    lane.nativeVst3StateBase64[(size_t) p] = json::getString (sv, "native_vst3_state");

                    // Same default-off rationale as the track hardware_insert.
                    lane.hardwareInserts[(size_t) p].enabled.store (
                        json::getBool (json::child (sv, "hardware_insert"), "enabled", false));
                    if (json::has (sv, "hardware_insert"))
                    {
                        const auto& hwi = json::child (sv, "hardware_insert");
                        auto& hw = lane.hardwareInserts[(size_t) p];
                        auto fresh = std::make_unique<HardwareInsertRouting>();
                        *fresh = hw.routing.current();
                        if (json::has (hwi, "output_ch_l"))     fresh->outputChL      = json::getInt (hwi, "output_ch_l", 0);
                        if (json::has (hwi, "output_ch_r"))     fresh->outputChR      = json::getInt (hwi, "output_ch_r", 0);
                        if (json::has (hwi, "input_ch_l"))      fresh->inputChL       = json::getInt (hwi, "input_ch_l", 0);
                        if (json::has (hwi, "input_ch_r"))      fresh->inputChR       = json::getInt (hwi, "input_ch_r", 0);
                        if (json::has (hwi, "latency_samples")) fresh->latencySamples = json::getInt (hwi, "latency_samples", 0);
                        if (json::has (hwi, "format"))          fresh->format         = json::getInt (hwi, "format", 0);
                        hw.routing.publish (std::move (fresh));

                        if (json::has (hwi, "output_gain_db")) hw.outputGainDb.store (json::getFloat (hwi, "output_gain_db", 0.0f));
                        if (json::has (hwi, "input_gain_db"))  hw.inputGainDb .store (json::getFloat (hwi, "input_gain_db", 0.0f));
                        if (json::has (hwi, "dry_wet"))        hw.dryWet      .store (json::getFloat (hwi, "dry_wet", 0.0f));
                    }
                }
            }

            // Mode publish happens AFTER lane mutations below - same
            // ordering rationale as the track-load block: avoid the
            // audio thread reading half-rebuilt lane vectors.
            for (auto& al : lane.params.automationLanes)
                al.publishPoints ({});
            {
                const auto& autoObj = json::child (v, "automation");
                for (int p = 0; p < kNumAutomationParams; ++p)
                {
                    const char* key = automationParamKey ((AutomationParam) p);
                    const auto& pts = json::array (autoObj, key);
                    if (pts.empty()) continue;
                    std::vector<AutomationPoint> tmp;
                    tmp.reserve (pts.size());
                    for (const auto& pv : pts)
                    {
                        if (! pv.is_object()) continue;
                        tmp.push_back (parseAutomationPoint (pv, sessionLoadBpm));
                    }
                    // Sort by time so the lane evaluator's binary search holds,
                    // matching the track / bus / master restore paths (a
                    // hand-edited or out-of-order JSON would otherwise break it).
                    std::sort (tmp.begin(), tmp.end(),
                               [] (const AutomationPoint& a, const AutomationPoint& b)
                               { return a.timeSamples < b.timeSamples; });
                    lane.params.automationLanes[(size_t) p].publishPoints (std::move (tmp));
                }
            }
            lane.params.automationMode.store (json::getInt (v, "automation_mode", 0), std::memory_order_release);
        }
    }
    // Clear unconditionally - a session without the key must not keep the
    // prior session's markers.
    s.getMarkers().clear();
    {
        const auto& markersArr = json::array (root, "markers");
        for (const auto& v : markersArr)
        {
            if (! v.is_object()) continue;
            // Use the public addMarker so the inserted-sorted invariant
            // holds even if the JSON happened to be out of order.
            const auto idx = s.addMarker (json::getInt64 (v, "time", 0),
                                          juce::String (json::getString (v, "name")).substring (0, 256));
            if (auto col = json::getString (v, "colour"); ! col.empty())
                s.getMarkers()[(size_t) idx].colour = hexToColour (col, juce::Colour (0xffe0a050));
        }
    }
    if (json::has (root, "master"))
    {
        const auto& master = json::child (root, "master");
        // Reset to struct defaults when a key is absent - load() reuses the live
        // session (no pre-load reset), so a conditional store would inherit the
        // previously-loaded session's value. (Audible master state: level /
        // routing / mute.)
        s.master().faderDb.store    (master.contains ("fader_db")    ? juce::jlimit (-100.0f, 12.0f, json::getFloat (master, "fader_db", 0.0f)) : 0.0f);
        s.master().outputPair.store (json::getInt  (master, "output_pair", -1));
        s.master().mute.store       (json::getBool (master, "mute", false));
        // Reset-when-absent too (same rationale as the level/routing/mute lines
        // above): a conditional store would inherit the previously-loaded
        // session's tape / mono-sum state.
        s.master().monoSum.store     (json::getBool (master, "mono_sum", false));
        s.master().tapeEnabled.store (json::getBool (master, "tape_enabled", false));
        s.master().tapeHQ.store      (json::getBool (master, "tape_hq", false));
        s.master().tapeStateBase64 = json::getString (master, "tape_state");

        // Pultec EQ. Missing keys keep the in-memory default (matches
        // the per-track / per-bus pattern).
        auto loadMasterFloat = [&master] (std::atomic<float>& dst, const char* key)
        {
            if (json::has (master, key))
            {
                const float v = json::getFloat (master, key, 0.0f);
                if (std::isfinite (v)) dst.store (v);   // reject NaN/inf from a corrupt file - keep the default
            }
        };
        auto loadMasterBool = [&master] (std::atomic<bool>& dst, const char* key)
        {
            if (json::has (master, key)) dst.store (json::getBool (master, key, false));
        };
        loadMasterBool  (s.master().eqEnabled,           "eq_enabled");
        loadMasterFloat (s.master().eqLfBoost,           "eq_lf_boost");
        loadMasterFloat (s.master().eqLfAtten,           "eq_lf_atten");
        loadMasterFloat (s.master().eqLfFreq,            "eq_lf_freq");
        loadMasterFloat (s.master().eqHfBoost,           "eq_hf_boost");
        loadMasterFloat (s.master().eqHfBoostFreq,       "eq_hf_boost_freq");
        loadMasterFloat (s.master().eqHfBoostBandwidth,  "eq_hf_boost_bandwidth");
        loadMasterFloat (s.master().eqHfAtten,           "eq_hf_atten");
        loadMasterFloat (s.master().eqHfAttenFreq,       "eq_hf_atten_freq");
        loadMasterFloat (s.master().eqOutputGainDb,      "eq_output_gain_db");

        // Bus comp.
        loadMasterBool  (s.master().compEnabled,      "comp_enabled");
        loadMasterFloat (s.master().compThreshDb,     "comp_thresh_db");
        loadMasterFloat (s.master().compRatio,        "comp_ratio");
        loadMasterFloat (s.master().compAttackMs,     "comp_attack_ms");
        loadMasterFloat (s.master().compReleaseMs,    "comp_release_ms");
        loadMasterBool  (s.master().compReleaseAuto,  "comp_release_auto");
        loadMasterFloat (s.master().compMakeupDb,     "comp_makeup_db");
        // Mode publish happens AFTER lane mutations below - same
        // ordering rationale as the track-load + aux-load blocks.
        for (auto& al : s.master().automationLanes)
            al.publishPoints ({});
        {
            const auto& autoObj = json::child (master, "automation");
            for (int p = 0; p < kNumAutomationParams; ++p)
            {
                const char* key = automationParamKey ((AutomationParam) p);
                const auto& pts = json::array (autoObj, key);
                if (pts.empty()) continue;
                std::vector<AutomationPoint> tmp;
                tmp.reserve (pts.size());
                for (const auto& pv : pts)
                {
                    if (! pv.is_object()) continue;
                    tmp.push_back (parseAutomationPoint (pv, sessionLoadBpm));
                }
                // Sort by time so the lane evaluator's binary search holds,
                // matching the track / bus / aux restore paths.
                std::sort (tmp.begin(), tmp.end(),
                    [] (const AutomationPoint& a, const AutomationPoint& b)
                    { return a.timeSamples < b.timeSamples; });
                s.master().automationLanes[(size_t) p].publishPoints (std::move (tmp));
            }
        }
        if (json::has (master, "automation_mode"))
            s.master().automationMode.store (json::getInt (master, "automation_mode", 0),
                                              std::memory_order_release);
    }
    // Always reset: a session without a saved mastering source must not
    // inherit the previously loaded session's file.
    s.mastering().sourceFile = juce::File();
    if (json::has (root, "mastering"))
    {
        const auto& mast = json::child (root, "mastering");
        auto& m = s.mastering();
        if (json::has (mast, "source_file"))
            m.sourceFile = resolvePortablePath (json::getString (mast, "source_file"),
                                                s.getSessionDirectory(),
                                                s.missingAudioFilesAfterLoad);
        auto loadF = [&] (const char* k, std::atomic<float>& dst)
            { if (json::has (mast, k)) dst.store (json::getFloat (mast, k, 0.0f)); };
        auto loadB = [&] (const char* k, std::atomic<bool>& dst)
            { if (json::has (mast, k)) dst.store (json::getBool (mast, k, false)); };
        loadB ("eq_enabled",        m.eqEnabled);
        for (int b = 0; b < MasteringParams::kNumEqBands; ++b)
        {
            const auto idx = std::string ("eq_band_") + std::to_string (b);
            loadF ((idx + "_freq").c_str(),    m.eqBandFreq[b]);
            loadF ((idx + "_gain_db").c_str(), m.eqBandGainDb[b]);
            loadF ((idx + "_q").c_str(),       m.eqBandQ[b]);
        }
        loadF ("eq_lf_boost",       m.eqLfBoost);
        loadF ("eq_hf_boost",       m.eqHfBoost);
        loadF ("eq_hf_atten",       m.eqHfAtten);
        loadF ("eq_tube_drive",     m.eqTubeDrive);
        loadF ("eq_output_gain_db", m.eqOutputGainDb);
        loadB ("comp_enabled",      m.compEnabled);
        loadF ("comp_thresh_db",    m.compThreshDb);
        loadF ("comp_ratio",        m.compRatio);
        loadF ("comp_attack_ms",    m.compAttackMs);
        loadF ("comp_release_ms",   m.compReleaseMs);
        loadB ("comp_release_auto", m.compReleaseAuto);
        loadF ("comp_makeup_db",    m.compMakeupDb);
        // Limiter fields are newly persisted, so old sessions lack them - reset
        // to the struct defaults when absent instead of inheriting the prior
        // session's limiter state (loadB/loadF conditional-store, no reset).
        m.limiterEnabled.store    (json::getBool  (mast, "limiter_enabled", true));
        m.limiterDriveDb.store    (json::getFloat (mast, "limiter_drive_db", 0.0f));
        m.limiterCeilingDb.store  (json::getFloat (mast, "limiter_ceiling_db", -0.3f));
        m.limiterReleaseMs.store  (json::getFloat (mast, "limiter_release_ms", 100.0f));
        {
            // Validate before storing: a NaN/±inf here would otherwise sit in the
            // limiter param (and the UI knob) until the next set; fall back to the
            // default so a corrupt session can't strand non-finite lookahead.
            const float la = json::getFloat (mast, "limiter_lookahead_ms", 2.0f);
            m.limiterLookaheadMs.store (std::isfinite (la) ? la : 2.0f);
        }
        m.limiterMode.store       (json::getInt  (mast, "limiter_mode", 0));
        m.limiterStereoLink.store (json::getBool (mast, "limiter_stereo_link", true));
        if (json::has (mast, "target_preset"))
            m.targetPresetIndex.store (json::getInt (mast, "target_preset", 0));
    }
    if (json::has (root, "transport"))
    {
        const auto& tport = json::child (root, "transport");
        if (json::has (tport, "loop_enabled"))  s.savedLoopEnabled  = json::getBool  (tport, "loop_enabled", false);
        if (json::has (tport, "loop_start"))    s.savedLoopStart    = json::getInt64 (tport, "loop_start", 0);
        if (json::has (tport, "loop_end"))      s.savedLoopEnd      = json::getInt64 (tport, "loop_end", 0);
        if (json::has (tport, "punch_enabled")) s.savedPunchEnabled = json::getBool  (tport, "punch_enabled", false);
        if (json::has (tport, "punch_in"))      s.savedPunchIn      = json::getInt64 (tport, "punch_in", 0);
        if (json::has (tport, "punch_out"))     s.savedPunchOut     = json::getInt64 (tport, "punch_out", 0);
        // Assign unconditionally (default false when absent) so a session
        // missing the key doesn't inherit a stale snapToGrid from a prior load -
        // the per-editor fallback below reads this value.
        s.snapToGrid = json::getBool (tport, "snap_to_grid", false);
        // Per-editor snap is newer than snap_to_grid. Always assign (don't leave
        // a stale value from a prior session when loading an older one): a
        // pre-feature session followed one global snap, so migrate the audio
        // editor to snap_to_grid; the piano roll historically always snapped, so
        // default the MIDI editor on.
        s.audioEditorSnap = json::has (tport, "audio_editor_snap")
                              ? json::getBool (tport, "audio_editor_snap", false) : s.snapToGrid;
        s.midiEditorSnap  = json::has (tport, "midi_editor_snap")
                              ? json::getBool (tport, "midi_editor_snap", true)   : true;
        if (json::has (tport, "piano_roll_key_snap"))
            s.pianoRollKeySnap = json::getBool (tport, "piano_roll_key_snap", false);
        if (json::has (tport, "snap_resolution"))
        {
            const int i = json::getInt (tport, "snap_resolution", 0);
            if (i >= 0 && i <= (int) SnapResolution::CDFrames) s.snapResolution = (SnapResolution) i;
        }
        // edit_mode intentionally not restored - the edit tool is transient UI
        // state, not session content. A persisted Cut/Range would reload as the
        // tapestrip's tool with no on-screen selector to change it back. Always
        // start in the Session.h default (Grab).
        if (json::has (tport, "tempo_bpm"))
        {
            // Guard the value before narrowing to float: a value past float range
            // (e.g. a hand-edited 1e40) would be UB to cast and would otherwise
            // clamp to 300, silently overwriting the session's real tempo. Reject
            // it and keep the existing tempo instead.
            const double bpm = json::getDouble (tport, "tempo_bpm", 0.0);
            if (std::isfinite (bpm) && std::abs (bpm) <= (double) std::numeric_limits<float>::max())
                s.tempoBpm.store (juce::jlimit (30.0f, 300.0f, (float) bpm));
        }
        if (json::has (tport, "tempo_points"))
        {
            std::vector<TempoPoint> pts;
            for (const auto& o : json::array (tport, "tempo_points"))
                if (o.is_object() && json::has (o, "sample") && json::has (o, "bpm"))
                {
                    // Guard finiteness before jlimit (NaN/inf slip through it);
                    // skip a corrupt point rather than poison the tempo map.
                    const double bpm = json::getDouble (o, "bpm", 0.0);
                    if (! std::isfinite (bpm)) continue;
                    pts.push_back ({ json::getInt64 (o, "sample", 0),
                                     juce::jlimit (30.0f, 300.0f, (float) bpm) });
                }
            s.tempoMap.setPoints (std::move (pts));
        }
        else
            s.tempoMap.setPoints ({});   // no map in the file -> clear any stale map from a prior load
        if (json::has (tport, "ui_stage"))          s.uiStage.store          (juce::jlimit (0, 3, json::getInt (tport, "ui_stage", 0)));
        if (json::has (tport, "sync_source_input"))
            s.syncSourceInputIdentifier = json::getString (tport, "sync_source_input");
        if (json::has (tport, "sync_follow_tempo"))
            s.externalSyncFollowsTempo.store (json::getBool (tport, "sync_follow_tempo", false));
        if (json::has (tport, "sync_chase_transport"))
            s.externalSyncChasesTransport.store (json::getBool (tport, "sync_chase_transport", false));
        if (json::has (tport, "sync_output"))
            s.syncOutputIdentifier = json::getString (tport, "sync_output");
        if (json::has (tport, "sync_emit_clock"))
            s.syncOutputEmitClock.store (json::getBool (tport, "sync_emit_clock", false));
        if (json::has (tport, "mcu_input_id"))
            s.mcu.inputIdentifier = json::getString (tport, "mcu_input_id");
        if (json::has (tport, "mcu_output_id"))
            s.mcu.outputIdentifier = json::getString (tport, "mcu_output_id");
        if (json::has (tport, "mcu_assign_mode"))
        {
            const int m = juce::jlimit (0, 6, json::getInt (tport, "mcu_assign_mode", 0));
            s.mcu.assignMode.store (m, std::memory_order_relaxed);
        }
        if (json::has (tport, "beats_per_bar"))     s.beatsPerBar.store      (juce::jlimit (1, 32, json::getInt (tport, "beats_per_bar", 4)));
        if (json::has (tport, "beat_unit"))         s.beatUnit.store         (juce::jlimit (1, 32, json::getInt (tport, "beat_unit", 4)));
        if (json::has (tport, "metronome_enabled")) s.metronomeEnabled.store (json::getBool (tport, "metronome_enabled", false));
        if (json::has (tport, "metronome_vol_db"))  s.metronomeVolDb.store   (juce::jlimit (-60.0f, 12.0f, json::getFloat (tport, "metronome_vol_db", 0.0f)));
        if (json::has (tport, "metronome_click_recording"))
            s.metronomeClickWhileRecording.store (json::getBool (tport, "metronome_click_recording", false));
        if (json::has (tport, "metronome_click_playing"))
            s.metronomeClickWhilePlaying  .store (json::getBool (tport, "metronome_click_playing", false));
        if (json::has (tport, "metronome_only_countin"))
            s.metronomeOnlyDuringCountIn  .store (json::getBool (tport, "metronome_only_countin", false));
        if (json::has (tport, "metronome_polyphonic"))
            s.metronomePolyphonic         .store (json::getBool (tport, "metronome_polyphonic", false));
        if (json::has (tport, "count_in_enabled"))  s.countInEnabled.store   (json::getBool (tport, "count_in_enabled", false));
        if (json::has (tport, "time_display_mode")) s.timeDisplayMode.store  (juce::jlimit (0, 1, json::getInt (tport, "time_display_mode", 0)));
        // jumpback_seconds was a previous-version Session field powering
        // the standalone "« 5s" jumpback button; the button has been
        // removed in favor of the DP-24SD-style multi-action REW. We
        // silently ignore the legacy field on load so older session.json
        // files still parse cleanly.
        if (json::has (tport, "last_record_point")) s.lastRecordPointSamples.store (json::getInt64 (tport, "last_record_point", 0));
        if (json::has (tport, "pre_roll_seconds"))  s.preRollSeconds.store   (juce::jlimit (0.0f, 300.0f, json::getFloat (tport, "pre_roll_seconds", 0.0f)));
        if (json::has (tport, "post_roll_seconds")) s.postRollSeconds.store  (juce::jlimit (0.0f, 300.0f, json::getFloat (tport, "post_roll_seconds", 0.0f)));
        // Default true when absent (Session.h) so an older file lacking the key
        // doesn't inherit a disabled flag from a previously-loaded session.
        s.preRollEnabled.store  (json::getBool (tport, "pre_roll_enabled", true));
        s.postRollEnabled.store (json::getBool (tport, "post_roll_enabled", true));

        // Build the bindings list off-snapshot, then publish atomically so
        // the audio thread either sees the prior set or the new one - never
        // a half-loaded state.
        auto fresh = std::make_unique<std::vector<MidiBinding>>();
        {
            const auto& arr = json::array (tport, "midi_bindings");
            for (const auto& v : arr)
            {
                if (! v.is_object()) continue;
                MidiBinding b;
                b.channel     = juce::jlimit (0, 16,  json::getInt (v, "channel", 0));
                b.dataNumber  = juce::jlimit (0, 127, json::getInt (v, "data", 0));
                const int rawTrig = json::getInt (v, "trigger", (int) MidiBindingTrigger::CC);
                switch (rawTrig)
                {
                    case (int) MidiBindingTrigger::Note:       b.trigger = MidiBindingTrigger::Note;       break;
                    case (int) MidiBindingTrigger::PitchBend:  b.trigger = MidiBindingTrigger::PitchBend;  break;
                    case (int) MidiBindingTrigger::MmcCommand: b.trigger = MidiBindingTrigger::MmcCommand; break;
                    default:                                   b.trigger = MidiBindingTrigger::CC;         break;
                }
                const int rawTgt = json::getInt (v, "target", (int) MidiBindingTarget::None);
                // Map every legal enumerator; everything else falls back to
                // None so isValid() drops the binding.
                switch (rawTgt)
                {
                    case (int) MidiBindingTarget::TransportPlay:
                    case (int) MidiBindingTarget::TransportStop:
                    case (int) MidiBindingTarget::TransportRecord:
                    case (int) MidiBindingTarget::TransportToggle:
                    case (int) MidiBindingTarget::TrackFader:
                    case (int) MidiBindingTarget::TrackPan:
                    case (int) MidiBindingTarget::TrackMute:
                    case (int) MidiBindingTarget::TrackSolo:
                    case (int) MidiBindingTarget::TrackArm:
                    case (int) MidiBindingTarget::TrackAuxSend:
                    case (int) MidiBindingTarget::TrackHpfFreq:
                    case (int) MidiBindingTarget::TrackEqGain:
                    case (int) MidiBindingTarget::TrackEqFreq:
                    case (int) MidiBindingTarget::TrackEqQ:
                    case (int) MidiBindingTarget::TrackCompThresh:
                    case (int) MidiBindingTarget::TrackCompMakeup:
                    case (int) MidiBindingTarget::TrackPluginParam:
                    case (int) MidiBindingTarget::TrackFaderBank:
                    case (int) MidiBindingTarget::TrackPanBank:
                    case (int) MidiBindingTarget::TrackMuteBank:
                    case (int) MidiBindingTarget::TrackSoloBank:
                    case (int) MidiBindingTarget::TrackArmBank:
                    case (int) MidiBindingTarget::TrackAuxSendBank:
                    case (int) MidiBindingTarget::TrackHpfFreqBank:
                    case (int) MidiBindingTarget::TrackEqGainBank:
                    case (int) MidiBindingTarget::TrackEqFreqBank:
                    case (int) MidiBindingTarget::TrackEqQBank:
                    case (int) MidiBindingTarget::TrackCompThreshBank:
                    case (int) MidiBindingTarget::TrackCompMakeupBank:
                    case (int) MidiBindingTarget::TrackPluginParamBank:
                    case (int) MidiBindingTarget::BusFader:
                    case (int) MidiBindingTarget::BusPan:
                    case (int) MidiBindingTarget::BusMute:
                    case (int) MidiBindingTarget::BusSolo:
                    case (int) MidiBindingTarget::AuxLaneFader:
                    case (int) MidiBindingTarget::AuxLaneMute:
                    case (int) MidiBindingTarget::AuxPluginParam:
                    case (int) MidiBindingTarget::MasterFader:
                    // Were silently coerced to None on load, dropping the
                    // binding from a saved session.
                    case (int) MidiBindingTarget::TrackEqEnabled:
                    case (int) MidiBindingTarget::TrackCompEnabled:
                    case (int) MidiBindingTarget::TrackInsertBypass:
                    case (int) MidiBindingTarget::TrackAuxSendPrePost:
                    case (int) MidiBindingTarget::BusEqGain:
                    case (int) MidiBindingTarget::MasterEqLfBoost:
                    case (int) MidiBindingTarget::MasterEqHfBoost:
                    case (int) MidiBindingTarget::MasterCompThresh:
                    case (int) MidiBindingTarget::MasterCompMakeup:
                    case (int) MidiBindingTarget::MasterCompRatio:
                        b.target = (MidiBindingTarget) rawTgt; break;
                    default:
                        b.target = MidiBindingTarget::None; break;
                }
                // Bus targets index 0..kNumBuses-1; aux-send targets pack
                // (track, aux) so range is 0..(kNumTracks*kNumAuxSends-1);
                // bank-relative variants use position 0..kBankSize-1 (or a
                // packed pos*N+sub range for the AuxSend/EqGain bank
                // variants); everything else uses 0..kNumTracks-1 (global
                // targets ignore the field).
                const int rawIdx = json::getInt (v, "target_idx", 0);
                const bool bankRelative = isBankRelativeTarget (b.target);
                const int maxIdx = needsBusIndex (b.target)
                    ? Session::kNumBuses - 1
                    : (needsPackedBusEqIndex (b.target)
                        ? Session::kNumBuses * kBusEqBands - 1
                    : (needsAuxLaneIndex (b.target)
                        ? Session::kNumAuxLanes - 1
                        : (needsPackedTrackAuxIndex (b.target)
                            ? Session::kNumTracks * ChannelStripParams::kNumAuxSends - 1
                            : (needsPackedTrackEqIndex (b.target)
                                ? Session::kNumTracks * kPackedEqBands - 1
                                : (bankRelative
                                    ? (b.target == MidiBindingTarget::TrackAuxSendBank
                                        ? Session::kBankSize * kPackedAuxLanes - 1
                                        : ((b.target == MidiBindingTarget::TrackEqGainBank
                                            || b.target == MidiBindingTarget::TrackEqFreqBank
                                            || b.target == MidiBindingTarget::TrackEqQBank)
                                            ? Session::kBankSize * kPackedEqBands - 1
                                            : Session::kBankSize - 1))
                                    : Session::kNumTracks - 1)))));
                b.targetIndex = juce::jlimit (0, maxIdx, rawIdx);
                // paramIndex only meaningful for TrackPluginParam, but
                // round-trip unconditionally for forward-compat. Clamp
                // wide so future plugins with hundreds of params
                // round-trip cleanly.
                if (json::has (v, "param_idx"))
                    b.paramIndex = juce::jlimit (0, 65535, json::getInt (v, "param_idx", 0));
                if (json::has (v, "button_mode"))
                    b.buttonMode = (MidiButtonMode) juce::jlimit (0, 1, json::getInt (v, "button_mode", 0));
                if (b.isValid())
                    fresh->push_back (b);
            }
        }
        s.midiBindings.publish (std::move (fresh));
        if (json::has (tport, "oversampling_factor"))
        {
            const int f = json::getInt (tport, "oversampling_factor", 1);
            s.oversamplingFactor.store ((f == 2 || f == 4) ? f : 1, std::memory_order_relaxed);
        }
    }
    // Bulk load wrote solo / armed atoms directly - resync the RT counters
    // so the audio thread's any-X-soloed reads are correct on first callback.
    s.recomputeRtCounters();
    return true;
}

SessionSerializer::ConsolidationResult
SessionSerializer::consolidateInto (Session& s, const juce::File& newSessionDir)
{
    ConsolidationResult res;
    const auto oldDir = s.getSessionDirectory();
    if (newSessionDir == juce::File() || newSessionDir == oldDir)
        return res;

    // Phase A - plan. Map each unique source to its destination without
    // touching the model. Files under the old session dir keep their relative
    // subpath (audio/, audio/freeze/, a root mixdown.wav); anything else is
    // flattened into audio/ with a numeric suffix on basename collisions.
    std::map<juce::String, juce::File> remap;
    std::set<juce::String> usedTargets;

    auto plan = [&] (const juce::File& f)
    {
        if (f == juce::File()) return;
        const auto key = f.getFullPathName();
        if (remap.count (key) != 0) return;
        if (! f.existsAsFile())
        {
            if (std::find (res.missingSources.begin(), res.missingSources.end(), key)
                    == res.missingSources.end())
                res.missingSources.push_back (key);
            return;
        }

        // Both branches go through the same collision suffixing: a flattened
        // external can claim a name a later session-local file also maps to
        // (external loop.wav planned before audio/loop.wav), so the
        // preserve-relative branch is not collision-free either.
        juce::File target = (oldDir != juce::File() && f.isAChildOf (oldDir))
            ? newSessionDir.getChildFile (f.getRelativePathFrom (oldDir))
            : newSessionDir.getChildFile ("audio").getChildFile (f.getFileName());
        const auto targetDir = target.getParentDirectory();
        for (int n = 2; usedTargets.count (target.getFullPathName()) != 0; ++n)
            target = targetDir.getChildFile (f.getFileNameWithoutExtension()
                                              + "_" + juce::String (n)
                                              + f.getFileExtension());
        usedTargets.insert (target.getFullPathName());
        remap[key] = target;
    };

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& track = s.track (t);
        for (auto& r : track.regions)
        {
            plan (r.file);
            for (auto& take : r.previousTakes)
                plan (take.file);
        }
        if (track.frozenAudioPath.isNotEmpty())
            plan (juce::File (track.frozenAudioPath));
    }
    // Session-local mastering source moves with the session; a genuinely
    // external one (a mix from elsewhere on disk) stays where the user put it.
    if (auto& src = s.mastering().sourceFile;
        src != juce::File() && oldDir != juce::File() && src.isAChildOf (oldDir))
        plan (src);

    // Native plugin file-backed state (state/lv2/<slot>/...) travels with the
    // session as a whole tree: the serialized blobs reference it by
    // cur/-relative abstract paths, so no model repoint is needed - the copy
    // just has to exist under the new root before the post-swap re-save
    // refreshes it.
    juce::File copiedStateDir;
    if (oldDir != juce::File())
    {
        const auto oldStateDir = oldDir.getChildFile ("state");
        if (oldStateDir.isDirectory())
        {
            const auto newStateDir = newSessionDir.getChildFile ("state");
            if (! oldStateDir.copyDirectoryTo (newStateDir))
            {
                newStateDir.deleteRecursively();
                res.ok = false;
                res.errorMessage = "Could not copy the plugin state folder to \""
                                 + newStateDir.getFullPathName() + "\"";
                return res;
            }
            copiedStateDir = newStateDir;
        }
    }

    // Phase B - copy. Any failure rolls back the files copied so far (including
    // the state tree above) and returns with the model untouched.
    std::vector<juce::File> copied;
    for (const auto& [srcPath, target] : remap)
    {
        const juce::File src (srcPath);
        if (! target.getParentDirectory().createDirectory().wasOk()
            || ! src.copyFileTo (target))
        {
            for (auto& c : copied) c.deleteFile();
            if (copiedStateDir != juce::File()) copiedStateDir.deleteRecursively();
            res.ok = false;
            res.errorMessage = "Could not copy \"" + src.getFileName() + "\" to \""
                             + target.getParentDirectory().getFullPathName() + "\"";
            return res;
        }
        copied.push_back (target);
    }
    res.filesCopied = (int) copied.size();

    // Phase C - repoint the model.
    auto repoint = [&remap] (juce::File& f)
    {
        auto it = remap.find (f.getFullPathName());
        if (it != remap.end()) f = it->second;
    };
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& track = s.track (t);
        for (auto& r : track.regions)
        {
            repoint (r.file);
            for (auto& take : r.previousTakes)
                repoint (take.file);
        }
        if (track.frozenAudioPath.isNotEmpty())
        {
            juce::File fz (track.frozenAudioPath);
            repoint (fz);
            track.frozenAudioPath = fz.getFullPathName();
            repoint (track.frozenRegion.file);
        }
    }
    repoint (s.mastering().sourceFile);

    return res;
}
} // namespace duskstudio
