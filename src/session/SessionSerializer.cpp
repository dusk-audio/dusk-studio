#include "SessionSerializer.h"
#include <juce_audio_devices/juce_audio_devices.h>

#if JUCE_LINUX || JUCE_MAC
 #include <fcntl.h>
 #include <unistd.h>
#elif JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace duskstudio
{
namespace
{
// Bump whenever the JSON shape gains a NEW required field or changes
// the meaning of an existing one. Adding optional fields that default
// sensibly when absent does NOT require a bump — the load path
// gracefully ignores unknown keys.
// Loader rejects sessions with version > kFormatVersion (newer Dusk Studio
// can read older files via migrateSession; older Dusk Studio refusing
// newer files is safer than silently dropping fields).
constexpr int kFormatVersion = 2;
} // namespace

// Forward-migrate `root` from a known older version to kFormatVersion
// by mutating in place. Add cases as the format evolves:
//   case 1: do_v1_to_v2 (root); ++v; break;
//   case 2: do_v2_to_v3 (root); ++v; break;
// Each case MUST do its own ++v before break — the loop relies on the
// case body advancing the version. Without it the loop would spin
// forever on the same version.
// Returns true on success, false if a step refuses to migrate.
//
// Non-static + lives in namespace duskstudio so the Catch2 suite can
// forward-declare + exercise the migration loop directly (see
// tests/session_schema_migration.cpp). H1 schema-test hook.
bool migrateSession (juce::var& root, int from)
{
    int v = from;
    while (v < kFormatVersion)
    {
        const int before = v;
        switch (v)
        {
            case 1:
                // v1 → v2: no field changes. v1 sessions are
                // forward-compatible with v2 readers because v2 didn't
                // add or remove any keys — it just claimed the version
                // marker as a forward-compat anchor so the migrator
                // loop has an actual case to exercise. Bumping the
                // version field is the only mutation; the rest of the
                // tree is identical. Future schema changes get their
                // case here.
                if (root.isObject())
                    root.getDynamicObject()->setProperty ("version", 2);
                ++v;
                break;

            default:
                std::fprintf (stderr,
                              "[Dusk Studio/SessionSerializer] no migrator from v%d to v%d\n",
                              v, v + 1);
                return false;
        }
        // Belt-and-suspenders against a future migrator that forgets
        // its ++v — would otherwise spin the loop forever.
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
//                 on NTFS — rename+file-flush carries the data-
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
    auto v = (juce::uint32) s.getHexValue64();
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
// the file lives inside it (the normal case — RecordManager writes into
// <sessionDir>/audio). Absolute paths are kept for files referenced from
// outside the session folder. This keeps sessions portable across a
// rename, a copy to another machine, or the macOS<->Linux flow where the
// home prefix differs.
juce::String portablePath (const juce::File& f, const juce::File& sessionDir)
{
    if (f == juce::File()) return {};
    if (sessionDir != juce::File() && f.isAChildOf (sessionDir))
        return f.getRelativePathFrom (sessionDir);
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

    juce::File f;
    if (juce::File::isAbsolutePath (stored))
        f = juce::File (stored);
    else if (sessionDir != juce::File())
        f = sessionDir.getChildFile (stored);
    else
        return {};

    if (f.existsAsFile()) return f;

    const auto fileName = stored.replaceCharacter ('\\', '/')
                                .fromLastOccurrenceOf ("/", false, false);
    if (sessionDir != juce::File() && fileName.isNotEmpty())
    {
        const auto byName = sessionDir.getChildFile ("audio").getChildFile (fileName);
        if (byName.existsAsFile()) return byName;
    }

    missing.push_back (stored);
    return f;
}

juce::DynamicObject::Ptr trackToObject (const Track& t, const juce::File& sessionDir)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("name",   t.name);
    obj->setProperty ("colour", colourToHex (t.colour));

    // Plugin-slot persistence. Empty strings (no plugin loaded) are written
    // verbatim - round-trip restoreFromSavedState treats empty as "no
    // plugin", which is the correct steady state for unused slots.
    if (t.pluginDescriptionXml.isNotEmpty())
        obj->setProperty ("plugin_desc_xml", t.pluginDescriptionXml);
    if (t.pluginStateBase64.isNotEmpty())
        obj->setProperty ("plugin_state",    t.pluginStateBase64);

    obj->setProperty ("fader_db",       t.strip.faderDb.load());
    obj->setProperty ("pan",            t.strip.pan.load());
    obj->setProperty ("mute",           t.strip.mute.load());
    obj->setProperty ("solo",           t.strip.solo.load());
    obj->setProperty ("phase_invert",   t.strip.phaseInvert.load());
    obj->setProperty ("fader_group",    t.strip.faderGroupId.load());
    obj->setProperty ("input_monitor",  t.inputMonitor.load());
    obj->setProperty ("print_effects",  t.printEffects.load());
    obj->setProperty ("input_source",   t.inputSource.load());
    obj->setProperty ("input_source_r", t.inputSourceR.load());
    // midi_input_idx is the legacy raw-int form (kept for back-compat
    // reading); midi_input_id is the stable identifier we resolve back to
    // an index on load. Older sessions without the id field fall through
    // to the int.
    obj->setProperty ("midi_input_idx",  t.midiInputIndex.load());
    obj->setProperty ("midi_input_id",   t.midiInputIdentifier);
    // External-MIDI-output side. Same shape as the input fields above.
    obj->setProperty ("midi_output_idx", t.midiOutputIndex.load());
    obj->setProperty ("midi_output_id",  t.midiOutputIdentifier);
    obj->setProperty ("midi_channel",    t.midiChannel.load());
    obj->setProperty ("track_mode",     t.mode.load());

    juce::Array<juce::var> buses;
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
        buses.add (t.strip.busAssign[(size_t) i].load());
    obj->setProperty ("bus_assign", buses);

    // Aux sends (continuous send level + pre/post-fader tap) - distinct
    // from busAssign which is the post-fader on/off routing toggle.
    juce::Array<juce::var> auxLevels, auxPrePost;
    for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
    {
        auxLevels .add ((double) t.strip.auxSendDb[(size_t) i].load());
        auxPrePost.add (         t.strip.auxSendPreFader[(size_t) i].load());
    }
    obj->setProperty ("aux_send_db",        auxLevels);
    obj->setProperty ("aux_send_pre_fader", auxPrePost);

    auto* hpf = new juce::DynamicObject();
    hpf->setProperty ("enabled", t.strip.hpfEnabled.load());
    hpf->setProperty ("freq",    t.strip.hpfFreq.load());
    obj->setProperty ("hpf", juce::var (hpf));

    auto* lpf = new juce::DynamicObject();
    lpf->setProperty ("enabled", t.strip.lpfEnabled.load());
    lpf->setProperty ("freq",    t.strip.lpfFreq.load());
    obj->setProperty ("lpf", juce::var (lpf));

    auto* eq = new juce::DynamicObject();
    eq->setProperty ("enabled", t.strip.eqEnabled.load());
    eq->setProperty ("type", t.strip.eqBlackMode.load() ? "black" : "brown");
    auto bandObj = [] (float gain, float freq, float q = -1.0f)
    {
        auto* b = new juce::DynamicObject();
        b->setProperty ("gain", gain);
        b->setProperty ("freq", freq);
        if (q >= 0.0f) b->setProperty ("q", q);
        return juce::var (b);
    };
    eq->setProperty ("lf", bandObj (t.strip.lfGainDb.load(), t.strip.lfFreq.load()));
    eq->setProperty ("lm", bandObj (t.strip.lmGainDb.load(), t.strip.lmFreq.load(), t.strip.lmQ.load()));
    eq->setProperty ("hm", bandObj (t.strip.hmGainDb.load(), t.strip.hmFreq.load(), t.strip.hmQ.load()));
    eq->setProperty ("hf", bandObj (t.strip.hfGainDb.load(), t.strip.hfFreq.load()));
    obj->setProperty ("eq", juce::var (eq));

    auto* comp = new juce::DynamicObject();
    comp->setProperty ("enabled",     t.strip.compEnabled.load());
    comp->setProperty ("mode_picked", t.strip.compModePicked.load());
    comp->setProperty ("mode",        t.strip.compMode.load());
    comp->setProperty ("threshold_db", t.strip.compThresholdDb.load());  // legacy meter-strip drag
    // Per-mode parameters - UniversalCompressor's native shape.
    comp->setProperty ("opto_peak_red", t.strip.compOptoPeakRed.load());
    comp->setProperty ("opto_gain",     t.strip.compOptoGain.load());
    comp->setProperty ("opto_limit",    t.strip.compOptoLimit.load());
    comp->setProperty ("fet_input",       t.strip.compFetInput.load());
    comp->setProperty ("fet_output",      t.strip.compFetOutput.load());
    comp->setProperty ("fet_attack",      t.strip.compFetAttack.load());
    comp->setProperty ("fet_release",     t.strip.compFetRelease.load());
    comp->setProperty ("fet_ratio",       t.strip.compFetRatio.load());
    comp->setProperty ("fet_threshold_db", t.strip.compFetThresholdDb.load());
    comp->setProperty ("vca_thresh_db", t.strip.compVcaThreshDb.load());
    comp->setProperty ("vca_ratio",     t.strip.compVcaRatio.load());
    comp->setProperty ("vca_attack",    t.strip.compVcaAttack.load());
    comp->setProperty ("vca_release",   t.strip.compVcaRelease.load());
    comp->setProperty ("vca_output",    t.strip.compVcaOutput.load());
    comp->setProperty ("vca_overeasy",  t.strip.compVcaOverEasy.load());
    comp->setProperty ("vca_detector_classic", t.strip.compVcaDetectorClassic.load());
    obj->setProperty ("comp", juce::var (comp));

    // External hardware-insert state. The routing snapshot is read via
    // AtomicSnapshot::current() (message-thread side), the scalar knobs
    // via plain atomic loads. Persisting the fields here lets a saved
    // configuration survive a session reload regardless of whether the
    // strip's insertMode (Phase 3) is currently set to Hardware - if
    // the user flips back to Hardware later, the settings are restored.
    {
        auto* hwi = new juce::DynamicObject();
        hwi->setProperty ("enabled",         t.hardwareInsert.enabled.load());
        const auto& routing = t.hardwareInsert.routing.current();
        hwi->setProperty ("output_ch_l",     routing.outputChL);
        hwi->setProperty ("output_ch_r",     routing.outputChR);
        hwi->setProperty ("input_ch_l",      routing.inputChL);
        hwi->setProperty ("input_ch_r",      routing.inputChR);
        hwi->setProperty ("latency_samples", routing.latencySamples);
        hwi->setProperty ("format",          routing.format);
        hwi->setProperty ("output_gain_db",  t.hardwareInsert.outputGainDb.load());
        hwi->setProperty ("input_gain_db",   t.hardwareInsert.inputGainDb .load());
        hwi->setProperty ("dry_wet",         t.hardwareInsert.dryWet      .load());
        obj->setProperty ("hardware_insert", juce::var (hwi));
    }

    juce::Array<juce::var> regions;
    for (auto& r : t.regions)
    {
        auto* rObj = new juce::DynamicObject();
        rObj->setProperty ("file",            portablePath (r.file, sessionDir));
        rObj->setProperty ("timeline_start",  (juce::int64) r.timelineStart);
        rObj->setProperty ("length",          (juce::int64) r.lengthInSamples);
        rObj->setProperty ("source_offset",   (juce::int64) r.sourceOffset);
        if (r.numChannels != 1)
            rObj->setProperty ("num_channels", r.numChannels);
        // Fade samples emitted only when non-zero so existing sessions
        // don't gain noise. PlaybackEngine treats absent fields as 0.
        if (r.fadeInSamples  > 0) rObj->setProperty ("fade_in",  (juce::int64) r.fadeInSamples);
        if (r.fadeOutSamples > 0) rObj->setProperty ("fade_out", (juce::int64) r.fadeOutSamples);
        // Fade shapes emit only when non-Linear so older sessions stay
        // diff-clean. Stored as the int enum value (0..4) matching FadeShape.
        if (r.fadeInShape  != FadeShape::Linear) rObj->setProperty ("fade_in_shape",  (int) r.fadeInShape);
        if (r.fadeOutShape != FadeShape::Linear) rObj->setProperty ("fade_out_shape", (int) r.fadeOutShape);
        if (r.fadeInAuto)  rObj->setProperty ("fade_in_auto",  true);
        if (r.fadeOutAuto) rObj->setProperty ("fade_out_auto", true);
        // Skip gain_db when at unity to keep older sessions diff-clean
        // and avoid bloating the JSON for unedited regions. Float
        // exact-zero comparison is fine because the field is set
        // either by a default-construct (0.0f) or by an explicit user
        // drag - no float-arithmetic accumulation path exists.
        if (r.gainDb != 0.0f) rObj->setProperty ("gain_db", (double) r.gainDb);
        // Custom colour - only when the user explicitly set one
        // (default-constructed is transparent = "use track colour").
        // Stored as an 8-digit ARGB hex string via Colour::toString().
        if (! r.customColour.isTransparent())
            rObj->setProperty ("custom_colour", r.customColour.toString());
        // Label - skip when empty so unedited regions stay diff-clean.
        if (r.label.isNotEmpty())
            rObj->setProperty ("label", r.label);
        if (r.muted)  rObj->setProperty ("muted",  true);
        if (r.locked) rObj->setProperty ("locked", true);

        // Take history. Empty array on the common case (no overdubs); only
        // serialised when at least one prior take has been captured to keep
        // session.json compact.
        if (! r.previousTakes.empty())
        {
            juce::Array<juce::var> prior;
            for (auto& take : r.previousTakes)
            {
                auto* tObj = new juce::DynamicObject();
                tObj->setProperty ("file",          portablePath (take.file, sessionDir));
                tObj->setProperty ("source_offset", (juce::int64) take.sourceOffset);
                tObj->setProperty ("length",        (juce::int64) take.lengthInSamples);
                prior.add (juce::var (tObj));
            }
            rObj->setProperty ("previous_takes", prior);
        }

        regions.add (juce::var (rObj));
    }
    obj->setProperty ("regions", regions);

    // MIDI regions. Same shape as audio regions (timelineStart + length)
    // but holds events in tick time instead of a WAV file path. Notes and
    // CCs are flat arrays so the JSON stays compact even on dense regions.
    // Only serialised when the track actually has MIDI data; absent for
    // audio tracks so existing sessions don't gain noise.
    if (! t.midiRegions.current().empty())
    {
        juce::Array<juce::var> midiRegions;
        for (const auto& r : t.midiRegions.current())
        {
            auto* rObj = new juce::DynamicObject();
            rObj->setProperty ("timeline_start",   (juce::int64) r.timelineStart);
            rObj->setProperty ("length_samples",   (juce::int64) r.lengthInSamples);
            rObj->setProperty ("length_ticks",     (juce::int64) r.lengthInTicks);

            juce::Array<juce::var> notes;
            notes.ensureStorageAllocated ((int) r.notes.size());
            for (const auto& n : r.notes)
            {
                auto* nObj = new juce::DynamicObject();
                nObj->setProperty ("ch",    n.channel);
                nObj->setProperty ("note",  n.noteNumber);
                nObj->setProperty ("vel",   n.velocity);
                nObj->setProperty ("start", (juce::int64) n.startTick);
                nObj->setProperty ("len",   (juce::int64) n.lengthInTicks);
                notes.add (juce::var (nObj));
            }
            rObj->setProperty ("notes", notes);

            if (! r.ccs.empty())
            {
                juce::Array<juce::var> ccs;
                ccs.ensureStorageAllocated ((int) r.ccs.size());
                for (const auto& c : r.ccs)
                {
                    auto* cObj = new juce::DynamicObject();
                    cObj->setProperty ("ch",   c.channel);
                    cObj->setProperty ("ctrl", c.controller);
                    cObj->setProperty ("val",  c.value);
                    cObj->setProperty ("at",   (juce::int64) c.atTick);
                    ccs.add (juce::var (cObj));
                }
                rObj->setProperty ("ccs", ccs);
            }

            // Same custom-colour / label fields as AudioRegion -
            // skipped when unset so older sessions stay diff-clean.
            if (! r.customColour.isTransparent())
                rObj->setProperty ("custom_colour", r.customColour.toString());
            if (r.label.isNotEmpty())
                rObj->setProperty ("label", r.label);
            if (r.muted)  rObj->setProperty ("muted",  true);
        if (r.locked) rObj->setProperty ("locked", true);

            // BPM-change semantics (DuskStudio.md §5b). Default is locked,
            // so emit only when the user has explicitly unlocked. recorded_at_bpm
            // is always emitted so legacy sessions can be anchored deterministically
            // on first BPM change.
            if (! r.tempoLock) rObj->setProperty ("tempo_lock", false);
            rObj->setProperty ("recorded_at_bpm", r.recordedAtBPM);

            // MIDI take history mirrors audio: previously-recorded versions
            // of the same range stack here when an overdub fully overlaps
            // an existing region.
            if (! r.previousTakes.empty())
            {
                juce::Array<juce::var> prior;
                for (const auto& take : r.previousTakes)
                {
                    auto* tObj = new juce::DynamicObject();
                    tObj->setProperty ("length_ticks", (juce::int64) take.lengthInTicks);
                    juce::Array<juce::var> tnotes;
                    for (const auto& n : take.notes)
                    {
                        auto* nObj = new juce::DynamicObject();
                        nObj->setProperty ("ch",    n.channel);
                        nObj->setProperty ("note",  n.noteNumber);
                        nObj->setProperty ("vel",   n.velocity);
                        nObj->setProperty ("start", (juce::int64) n.startTick);
                        nObj->setProperty ("len",   (juce::int64) n.lengthInTicks);
                        tnotes.add (juce::var (nObj));
                    }
                    tObj->setProperty ("notes", tnotes);
                    if (! take.ccs.empty())
                    {
                        juce::Array<juce::var> tccs;
                        for (const auto& c : take.ccs)
                        {
                            auto* cObj = new juce::DynamicObject();
                            cObj->setProperty ("ch",   c.channel);
                            cObj->setProperty ("ctrl", c.controller);
                            cObj->setProperty ("val",  c.value);
                            cObj->setProperty ("at",   (juce::int64) c.atTick);
                            tccs.add (juce::var (cObj));
                        }
                        tObj->setProperty ("ccs", tccs);
                    }
                    prior.add (juce::var (tObj));
                }
                rObj->setProperty ("previous_takes", prior);
            }

            midiRegions.add (juce::var (rObj));
        }
        obj->setProperty ("midi_regions", midiRegions);
    }

    // Automation: per-strip mode + one array per non-empty lane. Empty
    // lanes are omitted to keep session.json compact for the common case
    // (no automation recorded yet). The "automation" object is omitted
    // entirely when nothing has been recorded.
    obj->setProperty ("automation_mode", t.automationMode.load (std::memory_order_relaxed));
    juce::DynamicObject::Ptr autoObj = new juce::DynamicObject();
    bool anyLane = false;
    for (int p = 0; p < kNumAutomationParams; ++p)
    {
        const auto& lane = t.automationLanes[(size_t) p];
        if (lane.pointsConst().empty()) continue;
        juce::Array<juce::var> pts;
        pts.ensureStorageAllocated ((int) lane.pointsConst().size());
        for (const auto& pt : lane.pointsConst())
        {
            auto* pObj = new juce::DynamicObject();
            pObj->setProperty ("t",   (juce::int64) pt.timeSamples);
            pObj->setProperty ("v",   (double) pt.value);
            pObj->setProperty ("bpm", (double) pt.recordedAtBPM);
            pts.add (juce::var (pObj));
        }
        autoObj->setProperty (automationParamKey ((AutomationParam) p), pts);
        anyLane = true;
    }
    if (anyLane)
        obj->setProperty ("automation", juce::var (autoObj.get()));

    return obj;
}

juce::DynamicObject::Ptr busToObject (const Bus& a)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("name",     a.name);
    obj->setProperty ("colour",   colourToHex (a.colour));
    obj->setProperty ("fader_db", a.strip.faderDb.load());
    obj->setProperty ("pan",      a.strip.pan.load());
    obj->setProperty ("mute",     a.strip.mute.load());
    obj->setProperty ("solo",     a.strip.solo.load());

    obj->setProperty ("eq_enabled",   a.strip.eqEnabled.load());
    obj->setProperty ("eq_lf_db",     a.strip.eqLfGainDb.load());
    obj->setProperty ("eq_mid_db",    a.strip.eqMidGainDb.load());
    obj->setProperty ("eq_hf_db",     a.strip.eqHfGainDb.load());

    obj->setProperty ("comp_enabled",     a.strip.compEnabled.load());
    obj->setProperty ("comp_thresh_db",   a.strip.compThreshDb.load());
    obj->setProperty ("comp_ratio",       a.strip.compRatio.load());
    obj->setProperty ("comp_attack_ms",   a.strip.compAttackMs.load());
    obj->setProperty ("comp_release_ms",  a.strip.compReleaseMs.load());
    obj->setProperty ("comp_release_auto", a.strip.compReleaseAuto.load());
    obj->setProperty ("comp_makeup_db",   a.strip.compMakeupDb.load());

    // Automation: same shape as tracks — mode + one array per non-empty lane.
    // Only FaderDb / Pan / Mute are populated for a bus, but we iterate all
    // params for symmetry (empty lanes are skipped).
    obj->setProperty ("automation_mode", a.strip.automationMode.load (std::memory_order_relaxed));
    juce::DynamicObject::Ptr autoObj = new juce::DynamicObject();
    bool anyLane = false;
    for (int p = 0; p < kNumAutomationParams; ++p)
    {
        const auto& lane = a.strip.automationLanes[(size_t) p];
        if (lane.pointsConst().empty()) continue;
        juce::Array<juce::var> pts;
        pts.ensureStorageAllocated ((int) lane.pointsConst().size());
        for (const auto& pt : lane.pointsConst())
        {
            auto* pObj = new juce::DynamicObject();
            pObj->setProperty ("t",   (juce::int64) pt.timeSamples);
            pObj->setProperty ("v",   (double) pt.value);
            pObj->setProperty ("bpm", (double) pt.recordedAtBPM);
            pts.add (juce::var (pObj));
        }
        autoObj->setProperty (automationParamKey ((AutomationParam) p), pts);
        anyLane = true;
    }
    if (anyLane)
        obj->setProperty ("automation", juce::var (autoObj.get()));

    return obj;
}

void restoreTrack (Track& t, const juce::var& v, double defaultRecordBpm,
                   const juce::File& sessionDir,
                   std::vector<juce::String>& missingFiles)
{
    if (! v.isObject()) return;
    if (auto s = v["name"].toString();           s.isNotEmpty()) t.name = s;
    if (auto s = v["colour"].toString();         s.isNotEmpty()) t.colour = hexToColour (s, t.colour);

    // Plugin slot - strings remain empty when the property is absent (older
    // sessions or unused slots). AudioEngine::consumePluginStateAfterLoad
    // reads these back and asks each PluginSlot to reinstantiate.
    t.pluginDescriptionXml = v["plugin_desc_xml"].toString();
    t.pluginStateBase64    = v["plugin_state"]   .toString();

    auto setFloat = [&v] (std::atomic<float>& a, const char* key)
    {
        if (v.hasProperty (key)) a.store ((float) (double) v[key], std::memory_order_relaxed);
    };
    auto setBool = [&v] (std::atomic<bool>& a, const char* key)
    {
        if (v.hasProperty (key)) a.store ((bool) v[key], std::memory_order_relaxed);
    };
    auto setInt = [&v] (std::atomic<int>& a, const char* key)
    {
        if (v.hasProperty (key)) a.store ((int) v[key], std::memory_order_relaxed);
    };

    setFloat (t.strip.faderDb,      "fader_db");
    setFloat (t.strip.pan,          "pan");
    setBool  (t.strip.mute,         "mute");
    setBool  (t.strip.solo,         "solo");
    setBool  (t.strip.phaseInvert,  "phase_invert");
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
    if (v.hasProperty ("midi_input_id"))
    {
        t.midiInputIdentifier = v["midi_input_id"].toString();
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
    if (v.hasProperty ("midi_output_id"))
    {
        t.midiOutputIdentifier = v["midi_output_id"].toString();
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

    if (auto buses = v["bus_assign"]; buses.isArray())
    {
        const int n = juce::jmin (ChannelStripParams::kNumBuses, buses.size());
        for (int i = 0; i < n; ++i)
            t.strip.busAssign[(size_t) i].store ((bool) buses[i], std::memory_order_relaxed);
    }

    if (auto auxLevels = v["aux_send_db"]; auxLevels.isArray())
    {
        const int n = juce::jmin (ChannelStripParams::kNumAuxSends, auxLevels.size());
        for (int i = 0; i < n; ++i)
            t.strip.auxSendDb[(size_t) i].store ((float) (double) auxLevels[i],
                                                   std::memory_order_relaxed);
    }
    if (auto auxPrePost = v["aux_send_pre_fader"]; auxPrePost.isArray())
    {
        const int n = juce::jmin (ChannelStripParams::kNumAuxSends, auxPrePost.size());
        for (int i = 0; i < n; ++i)
            t.strip.auxSendPreFader[(size_t) i].store ((bool) auxPrePost[i],
                                                          std::memory_order_relaxed);
    }

    if (auto hpf = v["hpf"]; hpf.isObject())
    {
        if (hpf.hasProperty ("enabled")) t.strip.hpfEnabled.store ((bool) hpf["enabled"]);
        if (hpf.hasProperty ("freq"))    t.strip.hpfFreq.store ((float) (double) hpf["freq"]);
    }

    if (auto lpf = v["lpf"]; lpf.isObject())
    {
        if (lpf.hasProperty ("enabled")) t.strip.lpfEnabled.store ((bool) lpf["enabled"]);
        if (lpf.hasProperty ("freq"))    t.strip.lpfFreq.store ((float) (double) lpf["freq"]);
    }

    if (auto eq = v["eq"]; eq.isObject())
    {
        if (eq.hasProperty ("enabled")) t.strip.eqEnabled.store ((bool) eq["enabled"]);
        if (auto type = eq["type"].toString(); type.isNotEmpty())
            t.strip.eqBlackMode.store (type == "black");

        auto restoreBand = [&eq] (const char* key, std::atomic<float>* gain,
                                   std::atomic<float>* freq, std::atomic<float>* q)
        {
            auto b = eq[key];
            if (! b.isObject()) return;
            if (gain && b.hasProperty ("gain")) gain->store ((float) (double) b["gain"]);
            if (freq && b.hasProperty ("freq")) freq->store ((float) (double) b["freq"]);
            if (q    && b.hasProperty ("q"))    q->store    ((float) (double) b["q"]);
        };
        restoreBand ("lf", &t.strip.lfGainDb, &t.strip.lfFreq, nullptr);
        restoreBand ("lm", &t.strip.lmGainDb, &t.strip.lmFreq, &t.strip.lmQ);
        restoreBand ("hm", &t.strip.hmGainDb, &t.strip.hmFreq, &t.strip.hmQ);
        restoreBand ("hf", &t.strip.hfGainDb, &t.strip.hfFreq, nullptr);
    }

    if (auto comp = v["comp"]; comp.isObject())
    {
        auto loadF = [&] (const char* key, std::atomic<float>& dst)
        {
            if (comp.hasProperty (key)) dst.store ((float) (double) comp[key]);
        };
        auto loadI = [&] (const char* key, std::atomic<int>& dst)
        {
            if (comp.hasProperty (key)) dst.store ((int) comp[key]);
        };
        auto loadB = [&] (const char* key, std::atomic<bool>& dst)
        {
            if (comp.hasProperty (key)) dst.store ((bool) comp[key]);
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

    if (auto hwi = v["hardware_insert"]; hwi.isObject())
    {
        if (hwi.hasProperty ("enabled"))
            t.hardwareInsert.enabled.store ((bool) hwi["enabled"]);

        auto fresh = std::make_unique<HardwareInsertRouting>();
        // current() returns the existing snapshot - seeds any field that
        // the JSON doesn't carry (forward-compat with older sessions).
        *fresh = t.hardwareInsert.routing.current();
        if (hwi.hasProperty ("output_ch_l"))     fresh->outputChL      = (int) hwi["output_ch_l"];
        if (hwi.hasProperty ("output_ch_r"))     fresh->outputChR      = (int) hwi["output_ch_r"];
        if (hwi.hasProperty ("input_ch_l"))      fresh->inputChL       = (int) hwi["input_ch_l"];
        if (hwi.hasProperty ("input_ch_r"))      fresh->inputChR       = (int) hwi["input_ch_r"];
        if (hwi.hasProperty ("latency_samples")) fresh->latencySamples = (int) hwi["latency_samples"];
        if (hwi.hasProperty ("format"))          fresh->format         = (int) hwi["format"];
        t.hardwareInsert.routing.publish (std::move (fresh));

        if (hwi.hasProperty ("output_gain_db"))
            t.hardwareInsert.outputGainDb.store ((float) (double) hwi["output_gain_db"]);
        if (hwi.hasProperty ("input_gain_db"))
            t.hardwareInsert.inputGainDb .store ((float) (double) hwi["input_gain_db"]);
        if (hwi.hasProperty ("dry_wet"))
            t.hardwareInsert.dryWet      .store ((float) (double) hwi["dry_wet"]);
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
    if (auto autoVar = v["automation"]; autoVar.isObject())
    {
        for (int p = 0; p < kNumAutomationParams; ++p)
        {
            const char* key = automationParamKey ((AutomationParam) p);
            auto pts = autoVar[key];
            if (! pts.isArray()) continue;
            std::vector<AutomationPoint> tmp;
            tmp.reserve ((size_t) pts.size());
            for (int k = 0; k < pts.size(); ++k)
            {
                auto pv = pts[k];
                if (! pv.isObject()) continue;
                AutomationPoint pt;
                pt.timeSamples   = (juce::int64) pv["t"];
                pt.value         = juce::jlimit (0.0f, 1.0f, (float) (double) pv["v"]);
                // Legacy / hand-edited points with no bpm anchor to the
                // session's load-time tempo (not a hard-coded 120) so a later
                // tempo change retimes them against the right reference.
                pt.recordedAtBPM = pv.hasProperty ("bpm")
                    ? (float) (double) pv["bpm"]
                    : (float) defaultRecordBpm;
                tmp.push_back (pt);
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
    if (v.hasProperty ("automation_mode"))
        t.automationMode.store ((int) v["automation_mode"], std::memory_order_release);

    t.regions.clear();
    if (auto regions = v["regions"]; regions.isArray())
    {
        for (int i = 0; i < regions.size(); ++i)
        {
            auto rv = regions[i];
            if (! rv.isObject()) continue;
            AudioRegion r;
            r.file            = resolvePortablePath (rv["file"].toString(),
                                                      sessionDir, missingFiles);
            r.timelineStart   = (juce::int64) rv["timeline_start"];
            r.lengthInSamples = (juce::int64) rv["length"];
            r.sourceOffset    = (juce::int64) rv["source_offset"];
            r.fadeInSamples   = rv.hasProperty ("fade_in")  ? (juce::int64) rv["fade_in"]  : 0;
            r.fadeOutSamples  = rv.hasProperty ("fade_out") ? (juce::int64) rv["fade_out"] : 0;
            auto loadShape = [] (const juce::var& v) -> FadeShape
            {
                const int i = (int) v;
                if (i >= 0 && i <= (int) FadeShape::RaisedCosine) return (FadeShape) i;
                return FadeShape::Linear;
            };
            r.fadeInShape     = rv.hasProperty ("fade_in_shape")
                                 ? loadShape (rv["fade_in_shape"])  : FadeShape::Linear;
            r.fadeOutShape    = rv.hasProperty ("fade_out_shape")
                                 ? loadShape (rv["fade_out_shape"]) : FadeShape::Linear;
            r.fadeInAuto      = rv.hasProperty ("fade_in_auto")  && (bool) rv["fade_in_auto"];
            r.fadeOutAuto     = rv.hasProperty ("fade_out_auto") && (bool) rv["fade_out_auto"];
            r.numChannels     = rv.hasProperty ("num_channels") ? (int) rv["num_channels"] : 1;
            r.gainDb          = rv.hasProperty ("gain_db")  ? (float) (double) rv["gain_db"] : 0.0f;
            r.customColour    = rv.hasProperty ("custom_colour")
                                 ? juce::Colour::fromString (rv["custom_colour"].toString())
                                 : juce::Colour();
            r.label           = rv.hasProperty ("label") ? rv["label"].toString()
                                                          : juce::String();
            r.muted           = rv.hasProperty ("muted")  && (bool) rv["muted"];
            r.locked          = rv.hasProperty ("locked") && (bool) rv["locked"];

            if (auto prior = rv["previous_takes"]; prior.isArray())
            {
                for (int k = 0; k < prior.size(); ++k)
                {
                    auto tv = prior[k];
                    if (! tv.isObject()) continue;
                    TakeRef take;
                    take.file            = resolvePortablePath (tv["file"].toString(),
                                                                 sessionDir, missingFiles);
                    take.sourceOffset    = (juce::int64) tv["source_offset"];
                    take.lengthInSamples = (juce::int64) tv["length"];
                    r.previousTakes.push_back (std::move (take));
                }
            }

            t.regions.push_back (std::move (r));
        }
    }

    // MIDI regions. Symmetric with the writer above; absent for audio
    // tracks. Helpers parse note + cc arrays out of a juce::var so the
    // top-level region and each take share the same code path.
    // Clamp every parsed field to its MIDI-spec range so a hand-edited or
    // truncated session.json can't seed out-of-range values into the model.
    auto parseNotes = [] (const juce::var& notesVar, std::vector<MidiNote>& dst)
    {
        if (! notesVar.isArray()) return;
        dst.reserve ((size_t) notesVar.size());
        for (int k = 0; k < notesVar.size(); ++k)
        {
            auto nv = notesVar[k];
            if (! nv.isObject()) continue;
            MidiNote n;
            n.channel       = juce::jlimit (1, 16,  nv.hasProperty ("ch")    ? (int) nv["ch"]    : 1);
            n.noteNumber    = juce::jlimit (0, 127, nv.hasProperty ("note")  ? (int) nv["note"]  : 60);
            n.velocity      = juce::jlimit (1, 127, nv.hasProperty ("vel")   ? (int) nv["vel"]   : 100);
            n.startTick     = juce::jmax ((juce::int64) 0, nv.hasProperty ("start") ? (juce::int64) nv["start"] : 0);
            n.lengthInTicks = juce::jmax ((juce::int64) 0, nv.hasProperty ("len")   ? (juce::int64) nv["len"]   : 0);
            dst.push_back (n);
        }
    };
    auto parseCcs = [] (const juce::var& ccsVar, std::vector<MidiCc>& dst)
    {
        if (! ccsVar.isArray()) return;
        dst.reserve ((size_t) ccsVar.size());
        for (int k = 0; k < ccsVar.size(); ++k)
        {
            auto cv = ccsVar[k];
            if (! cv.isObject()) continue;
            MidiCc c;
            c.channel    = juce::jlimit (1, 16,  cv.hasProperty ("ch")   ? (int) cv["ch"]   : 1);
            c.controller = juce::jlimit (0, 127, cv.hasProperty ("ctrl") ? (int) cv["ctrl"] : 0);
            c.value      = juce::jlimit (0, 127, cv.hasProperty ("val")  ? (int) cv["val"]  : 0);
            c.atTick     = juce::jmax ((juce::int64) 0, cv.hasProperty ("at")   ? (juce::int64) cv["at"] : 0);
            dst.push_back (c);
        }
    };

    // Build the regions list off-snapshot, then publish atomically so the
    // audio thread either sees the prior set or the new one - never a
    // half-loaded state.
    auto freshMidi = std::make_unique<std::vector<MidiRegion>>();
    if (auto midiRegions = v["midi_regions"]; midiRegions.isArray())
    {
        for (int i = 0; i < midiRegions.size(); ++i)
        {
            auto rv = midiRegions[i];
            if (! rv.isObject()) continue;
            MidiRegion r;
            r.timelineStart   = (juce::int64) rv["timeline_start"];
            r.lengthInSamples = (juce::int64) rv["length_samples"];
            r.lengthInTicks   = (juce::int64) rv["length_ticks"];
            r.customColour    = rv.hasProperty ("custom_colour")
                                 ? juce::Colour::fromString (rv["custom_colour"].toString())
                                 : juce::Colour();
            r.label           = rv.hasProperty ("label") ? rv["label"].toString()
                                                          : juce::String();
            r.muted           = rv.hasProperty ("muted")  && (bool) rv["muted"];
            r.locked          = rv.hasProperty ("locked") && (bool) rv["locked"];

            // tempo_lock defaults true (spec §5b: locked is the default).
            // recorded_at_bpm defaults to the session's tempo at load time,
            // so legacy sessions that didn't record this field are
            // anchored to their own saved BPM rather than 120 - the first
            // BPM change after load won't silently mis-retime.
            r.tempoLock       = ! rv.hasProperty ("tempo_lock")
                                  || (bool) rv["tempo_lock"];
            r.recordedAtBPM   = rv.hasProperty ("recorded_at_bpm")
                                  ? (double) rv["recorded_at_bpm"]
                                  : defaultRecordBpm;

            parseNotes (rv["notes"], r.notes);
            parseCcs   (rv["ccs"],   r.ccs);

            if (auto prior = rv["previous_takes"]; prior.isArray())
            {
                for (int k = 0; k < prior.size(); ++k)
                {
                    auto tv = prior[k];
                    if (! tv.isObject()) continue;
                    MidiTakeRef take;
                    take.lengthInTicks = (juce::int64) tv["length_ticks"];
                    parseNotes (tv["notes"], take.notes);
                    parseCcs   (tv["ccs"],   take.ccs);
                    r.previousTakes.push_back (std::move (take));
                }
            }

            freshMidi->push_back (std::move (r));
        }
    }
    t.midiRegions.publish (std::move (freshMidi));
}

void restoreBus (Bus& a, const juce::var& v, double defaultRecordBpm)
{
    if (! v.isObject()) return;
    if (auto s = v["name"].toString();   s.isNotEmpty()) a.name = s;
    if (auto s = v["colour"].toString(); s.isNotEmpty()) a.colour = hexToColour (s, a.colour);
    if (v.hasProperty ("fader_db"))                       a.strip.faderDb.store ((float) (double) v["fader_db"]);
    if (v.hasProperty ("pan"))                            a.strip.pan.store     ((float) (double) v["pan"]);
    if (v.hasProperty ("mute"))                           a.strip.mute.store ((bool) v["mute"]);
    if (v.hasProperty ("solo"))                           a.strip.solo.store ((bool) v["solo"]);

    if (v.hasProperty ("eq_enabled"))   a.strip.eqEnabled  .store ((bool)  v["eq_enabled"]);
    if (v.hasProperty ("eq_lf_db"))     a.strip.eqLfGainDb .store ((float) (double) v["eq_lf_db"]);
    if (v.hasProperty ("eq_mid_db"))    a.strip.eqMidGainDb.store ((float) (double) v["eq_mid_db"]);
    if (v.hasProperty ("eq_hf_db"))     a.strip.eqHfGainDb .store ((float) (double) v["eq_hf_db"]);

    if (v.hasProperty ("comp_enabled"))    a.strip.compEnabled  .store ((bool)  v["comp_enabled"]);
    if (v.hasProperty ("comp_thresh_db"))  a.strip.compThreshDb .store ((float) (double) v["comp_thresh_db"]);
    if (v.hasProperty ("comp_ratio"))      a.strip.compRatio    .store ((float) (double) v["comp_ratio"]);
    if (v.hasProperty ("comp_attack_ms"))  a.strip.compAttackMs .store ((float) (double) v["comp_attack_ms"]);
    if (v.hasProperty ("comp_release_ms")) a.strip.compReleaseMs.store ((float) (double) v["comp_release_ms"]);
    if (v.hasProperty ("comp_release_auto")) a.strip.compReleaseAuto.store ((bool) v["comp_release_auto"]);
    if (v.hasProperty ("comp_makeup_db"))  a.strip.compMakeupDb .store ((float) (double) v["comp_makeup_db"]);

    // Automation — mirror restoreTrack: clear lanes, rebuild from JSON, then
    // release-store the mode so the audio thread never reads a half-rebuilt
    // lane vector. Only FaderDb / Pan / Mute lanes are ever populated.
    for (auto& lane : a.strip.automationLanes)
        lane.publishPoints ({});
    if (auto autoVar = v["automation"]; autoVar.isObject())
    {
        for (int p = 0; p < kNumAutomationParams; ++p)
        {
            const char* key = automationParamKey ((AutomationParam) p);
            auto pts = autoVar[key];
            if (! pts.isArray()) continue;
            std::vector<AutomationPoint> tmp;
            tmp.reserve ((size_t) pts.size());
            for (int k = 0; k < pts.size(); ++k)
            {
                auto pv = pts[k];
                if (! pv.isObject()) continue;
                AutomationPoint pt;
                pt.timeSamples   = (juce::int64) pv["t"];
                pt.value         = juce::jlimit (0.0f, 1.0f, (float) (double) pv["v"]);
                pt.recordedAtBPM = pv.hasProperty ("bpm")
                    ? (float) (double) pv["bpm"]
                    : (float) defaultRecordBpm;
                tmp.push_back (pt);
            }
            std::sort (tmp.begin(), tmp.end(),
                [] (const AutomationPoint& x, const AutomationPoint& y)
                { return x.timeSamples < y.timeSamples; });
            a.strip.automationLanes[(size_t) p].publishPoints (std::move (tmp));
        }
    }
    // Always write the mode so a re-load into a reused strip can't inherit a
    // stale value: an absent key means the saved session predates automation → Off.
    a.strip.automationMode.store (
        v.hasProperty ("automation_mode") ? (int) v["automation_mode"] : 0,
        std::memory_order_release);
}
} // namespace

juce::String SessionSerializer::serialize (const Session& s)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("version", kFormatVersion);

    juce::Array<juce::var> tracks;
    for (int i = 0; i < Session::kNumTracks; ++i)
        tracks.add (juce::var (trackToObject (s.track (i), s.getSessionDirectory()).get()));
    root->setProperty ("tracks", tracks);

    juce::Array<juce::var> busesArr;
    for (int i = 0; i < Session::kNumBuses; ++i)
        busesArr.add (juce::var (busToObject (s.bus (i)).get()));
    root->setProperty ("buses", busesArr);

    juce::Array<juce::var> auxLanesArr;
    for (int i = 0; i < Session::kNumAuxLanes; ++i)
    {
        const auto& lane = s.auxLane (i);
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("name",   lane.name);
        obj->setProperty ("colour", colourToHex (lane.colour));
        obj->setProperty ("return_level_db", lane.params.returnLevelDb.load());
        obj->setProperty ("mute",            lane.params.mute.load());
        obj->setProperty ("output_pair",     lane.params.outputPair.load());
        // Per-slot plugin state. Empty strings serialise as empty - same
        // pattern as Track.
        juce::Array<juce::var> slots;
        for (int p = 0; p < AuxLaneParams::kMaxLanePlugins; ++p)
        {
            auto* slot = new juce::DynamicObject();
            slot->setProperty ("plugin_desc_xml", lane.pluginDescriptionXml[(size_t) p]);
            slot->setProperty ("plugin_state",    lane.pluginStateBase64[(size_t) p]);

            // Hardware-insert side of this slot. Same shape as the
            // Track::hardwareInsert block above.
            auto* hwi = new juce::DynamicObject();
            const auto& hw = lane.hardwareInserts[(size_t) p];
            hwi->setProperty ("enabled",         hw.enabled.load());
            const auto& routing = hw.routing.current();
            hwi->setProperty ("output_ch_l",     routing.outputChL);
            hwi->setProperty ("output_ch_r",     routing.outputChR);
            hwi->setProperty ("input_ch_l",      routing.inputChL);
            hwi->setProperty ("input_ch_r",      routing.inputChR);
            hwi->setProperty ("latency_samples", routing.latencySamples);
            hwi->setProperty ("format",          routing.format);
            hwi->setProperty ("output_gain_db",  hw.outputGainDb.load());
            hwi->setProperty ("input_gain_db",   hw.inputGainDb .load());
            hwi->setProperty ("dry_wet",         hw.dryWet      .load());
            slot->setProperty ("hardware_insert", juce::var (hwi));

            slots.add (juce::var (slot));
        }
        obj->setProperty ("plugin_slots", slots);

        // Automation: aux fader + mute lanes. Same shape as per-track.
        obj->setProperty ("automation_mode",
                           lane.params.automationMode.load (std::memory_order_relaxed));
        juce::DynamicObject::Ptr autoObj = new juce::DynamicObject();
        bool anyLane = false;
        for (int p = 0; p < kNumAutomationParams; ++p)
        {
            const auto& al = lane.params.automationLanes[(size_t) p];
            if (al.pointsConst().empty()) continue;
            juce::Array<juce::var> pts;
            pts.ensureStorageAllocated ((int) al.pointsConst().size());
            for (const auto& pt : al.pointsConst())
            {
                auto* pObj = new juce::DynamicObject();
                pObj->setProperty ("t",   (juce::int64) pt.timeSamples);
                pObj->setProperty ("v",   (double) pt.value);
                pObj->setProperty ("bpm", (double) pt.recordedAtBPM);
                pts.add (juce::var (pObj));
            }
            autoObj->setProperty (automationParamKey ((AutomationParam) p), pts);
            anyLane = true;
        }
        if (anyLane)
            obj->setProperty ("automation", juce::var (autoObj.get()));

        auxLanesArr.add (juce::var (obj));
    }
    root->setProperty ("aux_lanes", auxLanesArr);

    juce::Array<juce::var> markersArr;
    for (const auto& m : s.getMarkers())
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("name",   m.name);
        obj->setProperty ("time",   (juce::int64) m.timelineSamples);
        obj->setProperty ("colour", colourToHex (m.colour));
        markersArr.add (juce::var (obj));
    }
    root->setProperty ("markers", markersArr);

    auto* master = new juce::DynamicObject();
    master->setProperty ("fader_db",     s.master().faderDb.load());
    master->setProperty ("output_pair",  s.master().outputPair.load());
    if (s.master().mute.load (std::memory_order_relaxed))
        master->setProperty ("mute", true);
    if (s.master().monoSum.load (std::memory_order_relaxed))
        master->setProperty ("mono_sum", true);
    master->setProperty ("tape_enabled", s.master().tapeEnabled.load());
    master->setProperty ("tape_hq",      s.master().tapeHQ.load());

    // Pultec EQ (all atoms — every knob the user can move).
    master->setProperty ("eq_enabled",            s.master().eqEnabled.load());
    master->setProperty ("eq_lf_boost",           s.master().eqLfBoost.load());
    master->setProperty ("eq_lf_atten",           s.master().eqLfAtten.load());
    master->setProperty ("eq_lf_freq",            s.master().eqLfFreq.load());
    master->setProperty ("eq_hf_boost",           s.master().eqHfBoost.load());
    master->setProperty ("eq_hf_boost_freq",      s.master().eqHfBoostFreq.load());
    master->setProperty ("eq_hf_boost_bandwidth", s.master().eqHfBoostBandwidth.load());
    master->setProperty ("eq_hf_atten",           s.master().eqHfAtten.load());
    master->setProperty ("eq_hf_atten_freq",      s.master().eqHfAttenFreq.load());
    master->setProperty ("eq_output_gain_db",     s.master().eqOutputGainDb.load());

    // Bus comp (SSL-style).
    master->setProperty ("comp_enabled",      s.master().compEnabled.load());
    master->setProperty ("comp_thresh_db",    s.master().compThreshDb.load());
    master->setProperty ("comp_ratio",        s.master().compRatio.load());
    master->setProperty ("comp_attack_ms",    s.master().compAttackMs.load());
    master->setProperty ("comp_release_ms",   s.master().compReleaseMs.load());
    master->setProperty ("comp_release_auto", s.master().compReleaseAuto.load());
    master->setProperty ("comp_makeup_db",    s.master().compMakeupDb.load());
    // TapeMachine APVTS state (base64). Skipped when empty so existing
    // sessions don't gain a noisy field they don't need.
    if (s.master().tapeStateBase64.isNotEmpty())
        master->setProperty ("tape_state", s.master().tapeStateBase64);

    // Master automation: only FaderDb is automatable per spec. Reuses
    // the same shape as track / aux serialization for symmetry.
    master->setProperty ("automation_mode",
                          s.master().automationMode.load (std::memory_order_relaxed));
    {
        juce::DynamicObject::Ptr autoObj = new juce::DynamicObject();
        bool anyLane = false;
        for (int p = 0; p < kNumAutomationParams; ++p)
        {
            const auto& al = s.master().automationLanes[(size_t) p];
            if (al.pointsConst().empty()) continue;
            juce::Array<juce::var> pts;
            pts.ensureStorageAllocated ((int) al.pointsConst().size());
            for (const auto& pt : al.pointsConst())
            {
                auto* pObj = new juce::DynamicObject();
                pObj->setProperty ("t",   (juce::int64) pt.timeSamples);
                pObj->setProperty ("v",   (double) pt.value);
                pObj->setProperty ("bpm", (double) pt.recordedAtBPM);
                pts.add (juce::var (pObj));
            }
            autoObj->setProperty (automationParamKey ((AutomationParam) p), pts);
            anyLane = true;
        }
        if (anyLane)
            master->setProperty ("automation", juce::var (autoObj.get()));
    }

    root->setProperty ("master", juce::var (master));

    // Mastering chain - separate from the master strip so its EQ/comp/limiter
    // settings can diverge from the in-mix master DSP.
    auto* mast = new juce::DynamicObject();
    mast->setProperty ("source_file",       portablePath (s.mastering().sourceFile,
                                                           s.getSessionDirectory()));
    mast->setProperty ("eq_enabled",        s.mastering().eqEnabled.load());
    for (int b = 0; b < MasteringParams::kNumEqBands; ++b)
    {
        const auto idx = juce::String (b);
        mast->setProperty ("eq_band_" + idx + "_freq",    s.mastering().eqBandFreq[b].load());
        mast->setProperty ("eq_band_" + idx + "_gain_db", s.mastering().eqBandGainDb[b].load());
        mast->setProperty ("eq_band_" + idx + "_q",       s.mastering().eqBandQ[b].load());
    }
    mast->setProperty ("eq_lf_boost",       s.mastering().eqLfBoost.load());
    mast->setProperty ("eq_hf_boost",       s.mastering().eqHfBoost.load());
    mast->setProperty ("eq_hf_atten",       s.mastering().eqHfAtten.load());
    mast->setProperty ("eq_tube_drive",     s.mastering().eqTubeDrive.load());
    mast->setProperty ("eq_output_gain_db", s.mastering().eqOutputGainDb.load());
    mast->setProperty ("comp_enabled",      s.mastering().compEnabled.load());
    mast->setProperty ("comp_thresh_db",    s.mastering().compThreshDb.load());
    mast->setProperty ("comp_ratio",        s.mastering().compRatio.load());
    mast->setProperty ("comp_attack_ms",    s.mastering().compAttackMs.load());
    mast->setProperty ("comp_release_ms",   s.mastering().compReleaseMs.load());
    mast->setProperty ("comp_release_auto", s.mastering().compReleaseAuto.load());
    mast->setProperty ("comp_makeup_db",    s.mastering().compMakeupDb.load());
    mast->setProperty ("limiter_enabled",     s.mastering().limiterEnabled.load());
    mast->setProperty ("limiter_drive_db",    s.mastering().limiterDriveDb.load());
    mast->setProperty ("limiter_ceiling_db",  s.mastering().limiterCeilingDb.load());
    mast->setProperty ("limiter_release_ms",  s.mastering().limiterReleaseMs.load());
    mast->setProperty ("limiter_mode",        s.mastering().limiterMode.load());
    mast->setProperty ("limiter_stereo_link", s.mastering().limiterStereoLink.load());
    mast->setProperty ("target_preset",     s.mastering().targetPresetIndex.load());
    root->setProperty ("mastering", juce::var (mast));

    // Transport (loop + punch). Mirrored onto Session by
    // AudioEngine::publishTransportStateForSave before this call runs.
    auto* tport = new juce::DynamicObject();
    tport->setProperty ("loop_enabled",  s.savedLoopEnabled);
    tport->setProperty ("loop_start",    (juce::int64) s.savedLoopStart);
    tport->setProperty ("loop_end",      (juce::int64) s.savedLoopEnd);
    tport->setProperty ("punch_enabled", s.savedPunchEnabled);
    tport->setProperty ("punch_in",      (juce::int64) s.savedPunchIn);
    tport->setProperty ("punch_out",     (juce::int64) s.savedPunchOut);
    tport->setProperty ("snap_to_grid",      s.snapToGrid);
    tport->setProperty ("snap_resolution",   (int) s.snapResolution);
    tport->setProperty ("piano_roll_key_snap", s.pianoRollKeySnap);
    tport->setProperty ("tempo_bpm",         s.tempoBpm.load());
    if (! s.tempoMap.empty())
    {
        juce::Array<juce::var> tpArr;
        for (const auto& p : s.tempoMap.points())
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("sample", p.timelineSamples);
            o->setProperty ("bpm",    (double) p.bpm);
            tpArr.add (juce::var (o));
        }
        tport->setProperty ("tempo_points", tpArr);
    }
    tport->setProperty ("ui_stage",          s.uiStage.load());
    tport->setProperty ("sync_source_input",  s.syncSourceInputIdentifier);
    tport->setProperty ("sync_follow_tempo",
                          s.externalSyncFollowsTempo.load());
    tport->setProperty ("sync_chase_transport",
                          s.externalSyncChasesTransport.load());
    tport->setProperty ("sync_output",         s.syncOutputIdentifier);
    tport->setProperty ("sync_emit_clock",     s.syncOutputEmitClock.load());

    // Mackie Control Universal device pair + last-used assign mode.
    // Bank + selectedChannel are session-runtime state and intentionally
    // NOT persisted -- a fresh launch always starts on bank 0 / ch 0.
    tport->setProperty ("mcu_input_id",   s.mcu.inputIdentifier);
    tport->setProperty ("mcu_output_id",  s.mcu.outputIdentifier);
    tport->setProperty ("mcu_assign_mode", s.mcu.assignMode.load (std::memory_order_relaxed));
    tport->setProperty ("beats_per_bar",     s.beatsPerBar.load());
    tport->setProperty ("beat_unit",         s.beatUnit.load());
    tport->setProperty ("metronome_enabled",          s.metronomeEnabled.load());
    tport->setProperty ("metronome_vol_db",           s.metronomeVolDb.load());
    tport->setProperty ("metronome_click_recording",  s.metronomeClickWhileRecording.load());
    tport->setProperty ("metronome_click_playing",    s.metronomeClickWhilePlaying.load());
    tport->setProperty ("metronome_only_countin",     s.metronomeOnlyDuringCountIn.load());
    tport->setProperty ("metronome_polyphonic",       s.metronomePolyphonic.load());
    tport->setProperty ("count_in_enabled",  s.countInEnabled.load());
    tport->setProperty ("time_display_mode", s.timeDisplayMode.load());
    // Tascam-style transport-cluster state. The last-record point is
    // what the FFWD-while-stopped tap (= TO LAST REC) snaps to.
    tport->setProperty ("last_record_point",  (juce::int64) s.lastRecordPointSamples.load());
    tport->setProperty ("pre_roll_seconds",   (double) s.preRollSeconds.load());
    tport->setProperty ("post_roll_seconds",  (double) s.postRollSeconds.load());
    tport->setProperty ("pre_roll_enabled",   s.preRollEnabled.load());
    tport->setProperty ("post_roll_enabled",  s.postRollEnabled.load());

    // MIDI controller bindings. Each entry stamps a (channel, dataNumber,
    // trigger) source onto a target enum + per-strip index. Only emit
    // when at least one binding exists so the JSON stays compact for
    // sessions that never wire a controller.
    if (! s.midiBindings.current().empty())
    {
        juce::Array<juce::var> arr;
        for (const auto& b : s.midiBindings.current())
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("channel",     b.channel);
            o->setProperty ("data",        b.dataNumber);
            o->setProperty ("trigger",     (int) b.trigger);
            o->setProperty ("target",      (int) b.target);
            o->setProperty ("target_idx",  b.targetIndex);
            o->setProperty ("param_idx",   b.paramIndex);
            o->setProperty ("button_mode", (int) b.buttonMode);
            arr.add (juce::var (o));
        }
        tport->setProperty ("midi_bindings", arr);
    }
    tport->setProperty ("oversampling_factor", s.oversamplingFactor.load());
    root->setProperty ("transport", juce::var (tport));

    return juce::JSON::toString (juce::var (root), false /*allOnOneLine*/);
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
    // of the state being saved — the only one if the target was lost.
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
    juce::var root = juce::JSON::parse (source);
    if (! root.isObject()) return false;

    s.missingAudioFilesAfterLoad.clear();

    // Format version gate. Missing key (pre-versioning sessions) is
    // treated as v1 — the format was effectively stable at v1 when the
    // version field landed. Future-versioned sessions are rejected up
    // front rather than partial-loaded; downgrading Dusk Studio to read a
    // session saved by a newer build silently dropping new state is
    // the worst-case bug class.
    const int fileVersion = root.hasProperty ("version")
        ? (int) root["version"]
        : 1;
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

    // Peek at transport.tempo_bpm BEFORE the track loop so legacy sessions
    // (no recorded_at_bpm field on MidiRegion) get anchored to their own
    // saved tempo rather than the struct default of 120. The transport
    // block is parsed in full further down; this is a read-only peek.
    double sessionLoadBpm = (double) s.tempoBpm.load (std::memory_order_relaxed);
    if (auto tportPeek = root["transport"]; tportPeek.isObject())
    {
        if (tportPeek.hasProperty ("tempo_bpm"))
            sessionLoadBpm = (double) tportPeek["tempo_bpm"];
    }

    if (auto tracks = root["tracks"]; tracks.isArray())
    {
        const int n = juce::jmin (Session::kNumTracks, tracks.size());
        for (int i = 0; i < n; ++i)
            restoreTrack (s.track (i), tracks[i], sessionLoadBpm,
                          s.getSessionDirectory(), s.missingAudioFilesAfterLoad);
    }
    if (auto busesArr = root["buses"]; busesArr.isArray())
    {
        const int n = juce::jmin (Session::kNumBuses, busesArr.size());
        for (int i = 0; i < n; ++i)
            restoreBus (s.bus (i), busesArr[i], sessionLoadBpm);
    }
    if (auto auxLanesArr = root["aux_lanes"]; auxLanesArr.isArray())
    {
        const int n = juce::jmin (Session::kNumAuxLanes, auxLanesArr.size());
        for (int i = 0; i < n; ++i)
        {
            const auto v = auxLanesArr[i];
            if (! v.isObject()) continue;
            auto& lane = s.auxLane (i);
            if (auto str = v["name"].toString();   str.isNotEmpty()) lane.name   = str;
            if (auto str = v["colour"].toString(); str.isNotEmpty()) lane.colour = hexToColour (str, lane.colour);
            if (v.hasProperty ("return_level_db"))
                lane.params.returnLevelDb.store ((float) (double) v["return_level_db"]);
            if (v.hasProperty ("mute"))
                lane.params.mute.store ((bool) v["mute"]);
            if (v.hasProperty ("output_pair"))
                lane.params.outputPair.store ((int) v["output_pair"]);
            else
                lane.params.outputPair.store (-1);   // model default: Master only
            if (auto slots = v["plugin_slots"]; slots.isArray())
            {
                const int sn = juce::jmin (AuxLaneParams::kMaxLanePlugins, slots.size());
                for (int p = 0; p < sn; ++p)
                {
                    auto sv = slots[p];
                    if (! sv.isObject()) continue;
                    lane.pluginDescriptionXml[(size_t) p] = sv["plugin_desc_xml"].toString();
                    lane.pluginStateBase64[(size_t) p]    = sv["plugin_state"]   .toString();

                    if (auto hwi = sv["hardware_insert"]; hwi.isObject())
                    {
                        auto& hw = lane.hardwareInserts[(size_t) p];
                        if (hwi.hasProperty ("enabled"))
                            hw.enabled.store ((bool) hwi["enabled"]);

                        auto fresh = std::make_unique<HardwareInsertRouting>();
                        *fresh = hw.routing.current();
                        if (hwi.hasProperty ("output_ch_l"))     fresh->outputChL      = (int) hwi["output_ch_l"];
                        if (hwi.hasProperty ("output_ch_r"))     fresh->outputChR      = (int) hwi["output_ch_r"];
                        if (hwi.hasProperty ("input_ch_l"))      fresh->inputChL       = (int) hwi["input_ch_l"];
                        if (hwi.hasProperty ("input_ch_r"))      fresh->inputChR       = (int) hwi["input_ch_r"];
                        if (hwi.hasProperty ("latency_samples")) fresh->latencySamples = (int) hwi["latency_samples"];
                        if (hwi.hasProperty ("format"))          fresh->format         = (int) hwi["format"];
                        hw.routing.publish (std::move (fresh));

                        if (hwi.hasProperty ("output_gain_db"))
                            hw.outputGainDb.store ((float) (double) hwi["output_gain_db"]);
                        if (hwi.hasProperty ("input_gain_db"))
                            hw.inputGainDb .store ((float) (double) hwi["input_gain_db"]);
                        if (hwi.hasProperty ("dry_wet"))
                            hw.dryWet      .store ((float) (double) hwi["dry_wet"]);
                    }
                }
            }

            // Mode publish happens AFTER lane mutations below — same
            // ordering rationale as the track-load block: avoid the
            // audio thread reading half-rebuilt lane vectors.
            for (auto& al : lane.params.automationLanes)
                al.publishPoints ({});
            if (auto autoObj = v["automation"]; autoObj.isObject())
            {
                for (int p = 0; p < kNumAutomationParams; ++p)
                {
                    const char* key = automationParamKey ((AutomationParam) p);
                    if (! autoObj.hasProperty (key)) continue;
                    auto pts = autoObj[key];
                    if (! pts.isArray()) continue;
                    std::vector<AutomationPoint> tmp;
                    tmp.reserve ((size_t) pts.size());
                    for (int k = 0; k < pts.size(); ++k)
                    {
                        auto pv = pts[k];
                        if (! pv.isObject()) continue;
                        AutomationPoint pt;
                        pt.timeSamples   = (juce::int64) pv["t"];
                        pt.value         = juce::jlimit (0.0f, 1.0f, (float) (double) pv["v"]);
                        pt.recordedAtBPM = pv.hasProperty ("bpm")
                            ? (float) (double) pv["bpm"]
                            : (float) sessionLoadBpm;
                        tmp.push_back (pt);
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
            if (v.hasProperty ("automation_mode"))
                lane.params.automationMode.store ((int) v["automation_mode"],
                                                   std::memory_order_release);
        }
    }
    if (auto markersArr = root["markers"]; markersArr.isArray())
    {
        s.getMarkers().clear();
        for (int i = 0; i < markersArr.size(); ++i)
        {
            auto v = markersArr[i];
            if (! v.isObject()) continue;
            // Use the public addMarker so the inserted-sorted invariant
            // holds even if the JSON happened to be out of order.
            const auto idx = s.addMarker ((juce::int64) v["time"], v["name"].toString());
            if (auto col = v["colour"].toString(); col.isNotEmpty())
                s.getMarkers()[(size_t) idx].colour = hexToColour (col, juce::Colour (0xffe0a050));
        }
    }
    if (auto master = root["master"]; master.isObject())
    {
        // Reset to struct defaults when a key is absent — load() reuses the live
        // session (no pre-load reset), so a conditional store would inherit the
        // previously-loaded session's value. (Audible master state: level /
        // routing / mute.)
        s.master().faderDb.store    (master.hasProperty ("fader_db")    ? (float) (double) master["fader_db"] : 0.0f);
        s.master().outputPair.store (master.hasProperty ("output_pair") ? (int) master["output_pair"]         : -1);
        s.master().mute.store       (master.hasProperty ("mute")        ? (bool) master["mute"]               : false);
        // Reset-when-absent too (same rationale as the level/routing/mute lines
        // above): a conditional store would inherit the previously-loaded
        // session's tape / mono-sum state.
        s.master().monoSum.store     (master.hasProperty ("mono_sum")     ? (bool) master["mono_sum"]     : false);
        s.master().tapeEnabled.store (master.hasProperty ("tape_enabled") ? (bool) master["tape_enabled"] : false);
        s.master().tapeHQ.store      (master.hasProperty ("tape_hq")      ? (bool) master["tape_hq"]      : false);
        s.master().tapeStateBase64 = master.hasProperty ("tape_state") ? master["tape_state"].toString()
                                                                       : juce::String();

        // Pultec EQ. Missing keys keep the in-memory default (matches
        // the per-track / per-bus pattern).
        auto loadMasterFloat = [&master] (std::atomic<float>& dst, const char* key)
        {
            if (master.hasProperty (key)) dst.store ((float) (double) master[key]);
        };
        auto loadMasterBool = [&master] (std::atomic<bool>& dst, const char* key)
        {
            if (master.hasProperty (key)) dst.store ((bool) master[key]);
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
        // Mode publish happens AFTER lane mutations below — same
        // ordering rationale as the track-load + aux-load blocks.
        for (auto& al : s.master().automationLanes)
            al.publishPoints ({});
        if (auto autoObj = master["automation"]; autoObj.isObject())
        {
            for (int p = 0; p < kNumAutomationParams; ++p)
            {
                const char* key = automationParamKey ((AutomationParam) p);
                if (! autoObj.hasProperty (key)) continue;
                auto pts = autoObj[key];
                if (! pts.isArray()) continue;
                std::vector<AutomationPoint> tmp;
                tmp.reserve ((size_t) pts.size());
                for (int k = 0; k < pts.size(); ++k)
                {
                    auto pv = pts[k];
                    if (! pv.isObject()) continue;
                    AutomationPoint pt;
                    pt.timeSamples   = (juce::int64) pv["t"];
                    pt.value         = juce::jlimit (0.0f, 1.0f, (float) (double) pv["v"]);
                    pt.recordedAtBPM = pv.hasProperty ("bpm")
                        ? (float) (double) pv["bpm"]
                        : (float) sessionLoadBpm;
                    tmp.push_back (pt);
                }
                s.master().automationLanes[(size_t) p].publishPoints (std::move (tmp));
            }
        }
        if (master.hasProperty ("automation_mode"))
            s.master().automationMode.store ((int) master["automation_mode"],
                                              std::memory_order_release);
    }
    if (auto mast = root["mastering"]; mast.isObject())
    {
        auto& m = s.mastering();
        if (mast.hasProperty ("source_file"))
        {
            const juce::String p = mast["source_file"].toString();
            m.sourceFile = resolvePortablePath (p, s.getSessionDirectory(),
                                                s.missingAudioFilesAfterLoad);
        }
        auto loadF = [&] (const char* k, std::atomic<float>& dst)
            { if (mast.hasProperty (k)) dst.store ((float) (double) mast[k]); };
        auto loadB = [&] (const char* k, std::atomic<bool>& dst)
            { if (mast.hasProperty (k)) dst.store ((bool) mast[k]); };
        auto loadI = [&] (const char* k, std::atomic<int>& dst)
            { if (mast.hasProperty (k)) dst.store ((int) mast[k]); };
        loadB ("eq_enabled",        m.eqEnabled);
        for (int b = 0; b < MasteringParams::kNumEqBands; ++b)
        {
            const auto idx = juce::String (b);
            loadF (("eq_band_" + idx + "_freq").toRawUTF8(),    m.eqBandFreq[b]);
            loadF (("eq_band_" + idx + "_gain_db").toRawUTF8(), m.eqBandGainDb[b]);
            loadF (("eq_band_" + idx + "_q").toRawUTF8(),       m.eqBandQ[b]);
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
        // Limiter fields are newly persisted, so old sessions lack them — reset
        // to the struct defaults when absent instead of inheriting the prior
        // session's limiter state (loadB/loadF/loadI conditional-store, no reset).
        m.limiterEnabled.store    (mast.hasProperty ("limiter_enabled")     ? (bool) mast["limiter_enabled"]              : true);
        m.limiterDriveDb.store    (mast.hasProperty ("limiter_drive_db")    ? (float) (double) mast["limiter_drive_db"]   : 0.0f);
        m.limiterCeilingDb.store  (mast.hasProperty ("limiter_ceiling_db")  ? (float) (double) mast["limiter_ceiling_db"] : -0.3f);
        m.limiterReleaseMs.store  (mast.hasProperty ("limiter_release_ms")  ? (float) (double) mast["limiter_release_ms"] : 100.0f);
        m.limiterMode.store       (mast.hasProperty ("limiter_mode")        ? (int) mast["limiter_mode"]                  : 0);
        m.limiterStereoLink.store (mast.hasProperty ("limiter_stereo_link") ? (bool) mast["limiter_stereo_link"]          : true);
        if (mast.hasProperty ("target_preset"))
            m.targetPresetIndex.store ((int) mast["target_preset"]);
    }
    if (auto tport = root["transport"]; tport.isObject())
    {
        if (tport.hasProperty ("loop_enabled"))  s.savedLoopEnabled  = (bool) tport["loop_enabled"];
        if (tport.hasProperty ("loop_start"))    s.savedLoopStart    = (juce::int64) tport["loop_start"];
        if (tport.hasProperty ("loop_end"))      s.savedLoopEnd      = (juce::int64) tport["loop_end"];
        if (tport.hasProperty ("punch_enabled")) s.savedPunchEnabled = (bool) tport["punch_enabled"];
        if (tport.hasProperty ("punch_in"))      s.savedPunchIn      = (juce::int64) tport["punch_in"];
        if (tport.hasProperty ("punch_out"))     s.savedPunchOut     = (juce::int64) tport["punch_out"];
        if (tport.hasProperty ("snap_to_grid"))  s.snapToGrid        = (bool) tport["snap_to_grid"];
        if (tport.hasProperty ("piano_roll_key_snap"))
            s.pianoRollKeySnap = (bool) tport["piano_roll_key_snap"];
        if (tport.hasProperty ("snap_resolution"))
        {
            const int i = (int) tport["snap_resolution"];
            if (i >= 0 && i <= (int) SnapResolution::CDFrames) s.snapResolution = (SnapResolution) i;
        }
        // edit_mode intentionally not restored - the edit tool is transient UI
        // state, not session content. A persisted Cut/Range would reload as the
        // tapestrip's tool with no on-screen selector to change it back. Always
        // start in the Session.h default (Grab).
        if (tport.hasProperty ("tempo_bpm"))         s.tempoBpm.store         ((float) (double) tport["tempo_bpm"]);
        if (tport.hasProperty ("tempo_points"))
        {
            std::vector<TempoPoint> pts;
            if (auto* arr = tport["tempo_points"].getArray())
                for (const auto& v : *arr)
                    if (auto* o = v.getDynamicObject())
                        if (o->hasProperty ("sample") && o->hasProperty ("bpm"))
                            pts.push_back ({ (juce::int64) o->getProperty ("sample"),
                                             (float) (double) o->getProperty ("bpm") });
            s.tempoMap.setPoints (std::move (pts));
        }
        else
            s.tempoMap.setPoints ({});   // no map in the file → clear any stale map from a prior load
        if (tport.hasProperty ("ui_stage"))          s.uiStage.store          ((int)  tport["ui_stage"]);
        if (tport.hasProperty ("sync_source_input"))
            s.syncSourceInputIdentifier = tport["sync_source_input"].toString();
        if (tport.hasProperty ("sync_follow_tempo"))
            s.externalSyncFollowsTempo.store ((bool) tport["sync_follow_tempo"]);
        if (tport.hasProperty ("sync_chase_transport"))
            s.externalSyncChasesTransport.store ((bool) tport["sync_chase_transport"]);
        if (tport.hasProperty ("sync_output"))
            s.syncOutputIdentifier = tport["sync_output"].toString();
        if (tport.hasProperty ("sync_emit_clock"))
            s.syncOutputEmitClock.store ((bool) tport["sync_emit_clock"]);
        if (tport.hasProperty ("mcu_input_id"))
            s.mcu.inputIdentifier = tport["mcu_input_id"].toString();
        if (tport.hasProperty ("mcu_output_id"))
            s.mcu.outputIdentifier = tport["mcu_output_id"].toString();
        if (tport.hasProperty ("mcu_assign_mode"))
        {
            const int m = juce::jlimit (0, 6, (int) tport["mcu_assign_mode"]);
            s.mcu.assignMode.store (m, std::memory_order_relaxed);
        }
        if (tport.hasProperty ("beats_per_bar"))     s.beatsPerBar.store      ((int)    tport["beats_per_bar"]);
        if (tport.hasProperty ("beat_unit"))         s.beatUnit.store         ((int)    tport["beat_unit"]);
        if (tport.hasProperty ("metronome_enabled")) s.metronomeEnabled.store ((bool)   tport["metronome_enabled"]);
        if (tport.hasProperty ("metronome_vol_db"))  s.metronomeVolDb.store   ((float) (double) tport["metronome_vol_db"]);
        if (tport.hasProperty ("metronome_click_recording"))
            s.metronomeClickWhileRecording.store ((bool) tport["metronome_click_recording"]);
        if (tport.hasProperty ("metronome_click_playing"))
            s.metronomeClickWhilePlaying  .store ((bool) tport["metronome_click_playing"]);
        if (tport.hasProperty ("metronome_only_countin"))
            s.metronomeOnlyDuringCountIn  .store ((bool) tport["metronome_only_countin"]);
        if (tport.hasProperty ("metronome_polyphonic"))
            s.metronomePolyphonic         .store ((bool) tport["metronome_polyphonic"]);
        if (tport.hasProperty ("count_in_enabled"))  s.countInEnabled.store   ((bool)   tport["count_in_enabled"]);
        if (tport.hasProperty ("time_display_mode")) s.timeDisplayMode.store  ((int)    tport["time_display_mode"]);
        // jumpback_seconds was a previous-version Session field powering
        // the standalone "« 5s" jumpback button; the button has been
        // removed in favor of the DP-24SD-style multi-action REW. We
        // silently ignore the legacy field on load so older session.json
        // files still parse cleanly.
        if (tport.hasProperty ("last_record_point")) s.lastRecordPointSamples.store ((juce::int64) tport["last_record_point"]);
        if (tport.hasProperty ("pre_roll_seconds"))  s.preRollSeconds.store   ((float)  (double) tport["pre_roll_seconds"]);
        if (tport.hasProperty ("post_roll_seconds")) s.postRollSeconds.store  ((float)  (double) tport["post_roll_seconds"]);
        // Default true when absent (Session.h) so an older file lacking the key
        // doesn't inherit a disabled flag from a previously-loaded session.
        s.preRollEnabled.store  (tport.hasProperty ("pre_roll_enabled")  ? (bool) tport["pre_roll_enabled"]  : true);
        s.postRollEnabled.store (tport.hasProperty ("post_roll_enabled") ? (bool) tport["post_roll_enabled"] : true);

        // Build the bindings list off-snapshot, then publish atomically so
        // the audio thread either sees the prior set or the new one - never
        // a half-loaded state.
        auto fresh = std::make_unique<std::vector<MidiBinding>>();
        if (auto arr = tport["midi_bindings"]; arr.isArray())
        {
            for (int i = 0; i < arr.size(); ++i)
            {
                auto v = arr[i];
                if (! v.isObject()) continue;
                MidiBinding b;
                b.channel     = juce::jlimit (0, 16,
                    v.hasProperty ("channel") ? (int) v["channel"] : 0);
                b.dataNumber  = juce::jlimit (0, 127,
                    v.hasProperty ("data") ? (int) v["data"] : 0);
                const int rawTrig = v.hasProperty ("trigger") ? (int) v["trigger"]
                                                              : (int) MidiBindingTrigger::CC;
                switch (rawTrig)
                {
                    case (int) MidiBindingTrigger::Note:       b.trigger = MidiBindingTrigger::Note;       break;
                    case (int) MidiBindingTrigger::PitchBend:  b.trigger = MidiBindingTrigger::PitchBend;  break;
                    case (int) MidiBindingTrigger::MmcCommand: b.trigger = MidiBindingTrigger::MmcCommand; break;
                    default:                                   b.trigger = MidiBindingTrigger::CC;         break;
                }
                const int rawTgt = v.hasProperty ("target") ? (int) v["target"]
                                                            : (int) MidiBindingTarget::None;
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
                const int rawIdx = v.hasProperty ("target_idx") ? (int) v["target_idx"] : 0;
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
                if (v.hasProperty ("param_idx"))
                    b.paramIndex = juce::jlimit (0, 65535, (int) v["param_idx"]);
                if (v.hasProperty ("button_mode"))
                {
                    const int bm = juce::jlimit (0, 1, (int) v["button_mode"]);
                    b.buttonMode = (MidiButtonMode) bm;
                }
                if (b.isValid())
                    fresh->push_back (b);
            }
        }
        s.midiBindings.publish (std::move (fresh));
        if (tport.hasProperty ("oversampling_factor"))
        {
            const int f = (int) tport["oversampling_factor"];
            s.oversamplingFactor.store ((f == 2 || f == 4) ? f : 1, std::memory_order_relaxed);
        }
    }
    // Bulk load wrote solo / armed atoms directly - resync the RT counters
    // so the audio thread's any-X-soloed reads are correct on first callback.
    s.recomputeRtCounters();
    return true;
}
} // namespace duskstudio
