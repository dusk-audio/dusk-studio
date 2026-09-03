#include "RegionEditActions.h"
#include "../engine/AudioEngine.h"
#include "../engine/LegacyStateBase64.h"
#include "../engine/PlaybackEngine.h"
#include "../engine/Transport.h"
#include "../engine/hosting/NativeStateIdentity.h"
#include "../engine/audiofile/FileReader.h"
#include "../engine/audiofile/FileWriter.h"
#include "../foundation/Base64.h"
#include "../foundation/Decibels.h"
#include "../foundation/PlanarBuffer.h"

#include <algorithm>

namespace duskstudio
{
namespace
{
template <typename FileType>
std::filesystem::path audioPath (const FileType& file)
{
    return std::filesystem::u8path (file.getFullPathName().toStdString());
}

void rebuildPlaybackIfStopped (AudioEngine& engine)
{
    if (engine.getTransport().getState() == Transport::State::Stopped)
    {
        engine.getPlaybackEngine().preparePlayback();
    }
    else
    {
        // Transport rolling: full preparePlayback would tear down
        // readers mid-stream. Push the latest gain / mute through
        // the lightweight live-refresh path instead so Normalize +
        // gain-handle drags become audible immediately. Structural
        // changes (move / trim / split / join) won't take effect
        // until the next Stop+Play, by design.
        engine.getPlaybackEngine().refreshLiveRegionParams();
    }
}

bool indexValid (Session& s, int trackIdx, int regionIdx)
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    auto& regs = s.track (trackIdx).regions;
    return regionIdx >= 0 && regionIdx < (int) regs.size();
}

// Frozen tracks are edit-locked: their audio/MIDI is baked into a rendered WAV,
// so any region edit would silently desync it. Region actions bail here (the
// gesture reverts, model unchanged) - unfreeze to edit.
bool frozenLocked (Session& s, int trackIdx)
{
    return trackIdx >= 0 && trackIdx < Session::kNumTracks
        && s.track (trackIdx).frozen.load (std::memory_order_relaxed);
}

// Join helpers. The selection is timeline-sorted (lead = earliest start), so
// the lead is not necessarily the lowest numeric index, and every erase below
// it shifts it down one slot. The bounds check is separate so the slow path
// can validate BEFORE rendering its WAV - failing afterwards would orphan it.
bool joinSelectionInBounds (const std::vector<AudioRegion>& regs,
                            const std::vector<int>& sortedDesc)
{
    return ! sortedDesc.empty()
        && sortedDesc.front() < (int) regs.size()
        && sortedDesc.back() >= 0;
}

// Erases every selected region except the lead and returns the lead's
// post-erase slot; -1 (with regs untouched) when the selection is invalid.
int eraseJoinedAndGetMergedSlot (std::vector<AudioRegion>& regs,
                                 const std::vector<int>& indices,
                                 const std::vector<int>& sortedDesc)
{
    if (indices.empty() || ! joinSelectionInBounds (regs, sortedDesc))
        return -1;
    const int leadIdx = indices.front();
    int mergedIdx = leadIdx;
    for (int idx : sortedDesc)
        if (idx != leadIdx)
        {
            regs.erase (regs.begin() + idx);
            if (idx < leadIdx)
                --mergedIdx;
        }
    return mergedIdx;
}
} // namespace

// RegionEditAction

RegionEditAction::RegionEditAction (Session& s, AudioEngine& e,
                                      int t, int idx,
                                      const AudioRegion& b, const AudioRegion& a)
    : session (s), engine (e), trackIdx (t), regionIdx (idx),
      beforeState (b), afterState (a)
{}

bool RegionEditAction::perform()
{
    if (! indexValid (session, trackIdx, regionIdx)) return false;
    if (frozenLocked (session, trackIdx)) return false;
    session.track (trackIdx).regions[(size_t) regionIdx] = afterState;
    rebuildPlaybackIfStopped (engine);
    return true;
}

bool RegionEditAction::undo()
{
    if (! indexValid (session, trackIdx, regionIdx)) return false;
    if (frozenLocked (session, trackIdx)) return false;
    session.track (trackIdx).regions[(size_t) regionIdx] = beforeState;
    rebuildPlaybackIfStopped (engine);
    return true;
}

// MidiRegionEditAction

MidiRegionEditAction::MidiRegionEditAction (Session& s, AudioEngine& e,
                                                int t, int idx,
                                                const MidiRegion& b, const MidiRegion& a)
    : session (s), engine (e), trackIdx (t), regionIdx (idx),
      beforeState (b), afterState (a)
{}

// Assigning a whole MidiRegion frees the old notes/ccs storage, so this
// must swap-publish like Create/RecordCommit - currentMutable() is only
// safe for value edits inside existing entries.
bool MidiRegionEditAction::perform()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    if (frozenLocked (session, trackIdx)) return false;
    // Bail before mutate(): a no-op publish would burn the snapshot's single
    // retire slot on an identical vector.
    if (regionIdx < 0
        || regionIdx >= (int) session.track (trackIdx).midiRegions.current().size())
        return false;
    session.track (trackIdx).midiRegions.mutate (
        [this] (std::vector<MidiRegion>& mregs)
        {
            mregs[(size_t) regionIdx] = afterState;
        });
    rebuildPlaybackIfStopped (engine);
    return true;
}

bool MidiRegionEditAction::undo()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    if (frozenLocked (session, trackIdx)) return false;
    if (regionIdx < 0
        || regionIdx >= (int) session.track (trackIdx).midiRegions.current().size())
        return false;
    session.track (trackIdx).midiRegions.mutate (
        [this] (std::vector<MidiRegion>& mregs)
        {
            mregs[(size_t) regionIdx] = beforeState;
        });
    rebuildPlaybackIfStopped (engine);
    return true;
}

// SplitRegionAction

SplitRegionAction::SplitRegionAction (Session& s, AudioEngine& e,
                                        int t, int idx, std::int64_t atSample)
    : session (s), engine (e), trackIdx (t), regionIdx (idx), splitAt (atSample)
{}

bool SplitRegionAction::perform()
{
    if (! indexValid (session, trackIdx, regionIdx)) return false;
    if (frozenLocked (session, trackIdx)) return false;
    auto& regs = session.track (trackIdx).regions;
    auto& orig = regs[(size_t) regionIdx];

    if (splitAt <= orig.timelineStart
        || splitAt >= orig.timelineStart + orig.lengthInSamples)
        return false;

    originalState = orig;

    AudioRegion right = orig;
    const auto leftLen = splitAt - orig.timelineStart;
    right.timelineStart   = splitAt;
    right.sourceOffset    = orig.sourceOffset + leftLen;
    right.lengthInSamples = orig.lengthInSamples - leftLen;

    orig.lengthInSamples  = leftLen;

    // Clamp fades against the new half-lengths so the original's existing
    // fadeIn / fadeOut don't exceed their half's length. Split itself does
    // NOT introduce auto-fades - the boundary is a hard cut by default.
    // Crossfades only happen when regions are dragged to actually overlap;
    // PlaybackEngine's implicit-overlap detection handles that case.
    orig.fadeInSamples   = std::clamp<std::int64_t> (orig.fadeInSamples, 0, orig.lengthInSamples);
    orig.fadeOutSamples  = std::clamp<std::int64_t> (orig.fadeOutSamples, 0, orig.lengthInSamples - orig.fadeInSamples);
    right.fadeInSamples  = std::clamp<std::int64_t> (right.fadeInSamples, 0, right.lengthInSamples);
    right.fadeOutSamples = std::clamp<std::int64_t> (right.fadeOutSamples, 0, right.lengthInSamples - right.fadeInSamples);

    regs.insert (regs.begin() + regionIdx + 1, right);

    rebuildPlaybackIfStopped (engine);
    return true;
}

bool SplitRegionAction::undo()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    if (frozenLocked (session, trackIdx)) return false;
    auto& regs = session.track (trackIdx).regions;

    // Remove the right half (idx+1) and restore the left to its full extent.
    // Guard both ends: a negative regionIdx would erase/index out of range,
    // and the upper bound ensures the right half exists. After this,
    // regionIdx is in [0, size-2], so the post-erase access stays in range.
    if (regionIdx < 0 || regionIdx + 1 >= (int) regs.size()) return false;
    regs.erase (regs.begin() + regionIdx + 1);
    regs[(size_t) regionIdx] = originalState;

    rebuildPlaybackIfStopped (engine);
    return true;
}

// PasteRegionAction

PasteRegionAction::PasteRegionAction (Session& s, AudioEngine& e,
                                        int t, const AudioRegion& r)
    : session (s), engine (e), trackIdx (t), regionToInsert (r)
{}

bool PasteRegionAction::perform()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    if (frozenLocked (session, trackIdx)) return false;
    auto& regs = session.track (trackIdx).regions;
    insertedAt = (int) regs.size();
    regs.push_back (regionToInsert);
    rebuildPlaybackIfStopped (engine);
    return true;
}

bool PasteRegionAction::undo()
{
    if (insertedAt < 0) return false;
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    if (frozenLocked (session, trackIdx)) return false;
    auto& regs = session.track (trackIdx).regions;
    if (insertedAt >= (int) regs.size()) return false;
    regs.erase (regs.begin() + insertedAt);
    rebuildPlaybackIfStopped (engine);
    return true;
}

// CreateMidiRegionAction

CreateMidiRegionAction::CreateMidiRegionAction (Session& s,
                                                  int t,
                                                  std::int64_t startSamples,
                                                  std::int64_t lenSamples,
                                                  std::int64_t lenTicks)
    : session (s), trackIdx (t),
      timelineStart (startSamples),
      lengthInSamples (lenSamples),
      lengthInTicks (lenTicks)
{}

bool CreateMidiRegionAction::perform()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    if (frozenLocked (session, trackIdx)) return false;

    MidiRegion region;
    region.timelineStart   = timelineStart;
    region.lengthInSamples = lengthInSamples;
    region.lengthInTicks   = lengthInTicks;
    region.recordedAtBPM   = (double) session.tempoBpm.load (std::memory_order_relaxed);

    int idx = -1;
    session.track (trackIdx).midiRegions.mutate (
        [&region, &idx] (std::vector<MidiRegion>& mregs)
        {
            mregs.push_back (std::move (region));
            idx = (int) mregs.size() - 1;
        });
    insertedAt = idx;
    return idx >= 0;
}

bool CreateMidiRegionAction::undo()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    if (frozenLocked (session, trackIdx)) return false;
    if (insertedAt < 0) return false;

    bool removed = false;
    const int target = insertedAt;
    session.track (trackIdx).midiRegions.mutate (
        [target, &removed] (std::vector<MidiRegion>& mregs)
        {
            if (target >= 0 && target < (int) mregs.size())
            {
                mregs.erase (mregs.begin() + target);
                removed = true;
            }
        });
    return removed;
}

// DeleteRegionAction

DeleteRegionAction::DeleteRegionAction (Session& s, AudioEngine& e,
                                          int t, int idx)
    : session (s), engine (e), trackIdx (t), regionIdx (idx)
{}

bool DeleteRegionAction::perform()
{
    if (! indexValid (session, trackIdx, regionIdx)) return false;
    if (frozenLocked (session, trackIdx)) return false;
    auto& regs = session.track (trackIdx).regions;
    removed = regs[(size_t) regionIdx];
    haveRemoved = true;
    regs.erase (regs.begin() + regionIdx);
    rebuildPlaybackIfStopped (engine);
    return true;
}

bool DeleteRegionAction::undo()
{
    if (! haveRemoved) return false;
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    if (frozenLocked (session, trackIdx)) return false;
    auto& regs = session.track (trackIdx).regions;

    const int insertAt = std::min (regionIdx, (int) regs.size());
    regs.insert (regs.begin() + insertAt, removed);
    rebuildPlaybackIfStopped (engine);
    return true;
}

// DeleteMidiRegionAction

DeleteMidiRegionAction::DeleteMidiRegionAction (Session& s, AudioEngine& e,
                                                    int t, int idx)
    : session (s), engine (e), trackIdx (t), regionIdx (idx)
{}

bool DeleteMidiRegionAction::perform()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    if (frozenLocked (session, trackIdx)) return false;
    const auto& mregsNow = session.track (trackIdx).midiRegions.current();
    if (regionIdx < 0 || regionIdx >= (int) mregsNow.size()) return false;
    removed = mregsNow[(size_t) regionIdx];
    haveRemoved = true;
    session.track (trackIdx).midiRegions.mutate (
        [this] (std::vector<MidiRegion>& mregs)
        {
            mregs.erase (mregs.begin() + regionIdx);
        });
    rebuildPlaybackIfStopped (engine);
    return true;
}

bool DeleteMidiRegionAction::undo()
{
    if (! haveRemoved) return false;
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    if (frozenLocked (session, trackIdx)) return false;
    session.track (trackIdx).midiRegions.mutate (
        [this] (std::vector<MidiRegion>& mregs)
        {
            const int insertAt = std::min (regionIdx, (int) mregs.size());
            mregs.insert (mregs.begin() + insertAt, removed);
        });
    rebuildPlaybackIfStopped (engine);
    return true;
}

// CloneTrackAction

namespace
{
#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_LV2 \
    || DUSKSTUDIO_HAS_NATIVE_VST3
// A bundle-path native insert (CLAP / LV2 / VST3): identity plus the opaque
// state blob. Empty path means "no plugin on this rung". Like the Audio Unit
// pair below, the persisted reference is kept when the plugin is offline so
// clone/undo doesn't discard something a later launch can reconnect.
struct CloneNativeSnapshot
{
    juce::String         path;
    juce::String         pluginId;
    std::vector<uint8_t> state;
};

enum class CloneNativeKind { None, Clap, Lv2, Vst3 };
#endif
} // namespace

// POD snapshot of every per-track field the clone should duplicate.
// Atomics are loaded into plain values; vectors are copied by value.
// Plugin state is captured as one descriptor/legacy-fallback/state unit.
struct CloneTrackAction::Impl
{
    juce::String name;
    juce::Colour colour;
    int mode = 0;

    // ChannelStripParams.
    float faderDb = 0.0f, pan = 0.0f;
    bool  mute = false, solo = false, phaseInvert = false;
    std::array<bool, ChannelStripParams::kNumBuses> busAssign {};
    bool auxSendsBypassed = false;
    std::array<float, ChannelStripParams::kNumAuxSends> auxSendDb {};
    std::array<bool,  ChannelStripParams::kNumAuxSends> auxSendPreFader {};

    bool  hpfEnabled = false; float hpfFreq = 20.0f;
    float lfGainDb = 0.0f, lfFreq = 100.0f;
    float lmGainDb = 0.0f, lmFreq = 600.0f, lmQ = 0.7f;
    float hmGainDb = 0.0f, hmFreq = 2000.0f, hmQ = 0.7f;
    float hfGainDb = 0.0f, hfFreq = 8000.0f;
    bool  eqBlackMode = false;

    bool  compEnabled = false;
    int   compMode = 2;
    float compOptoPeakRed = 30.0f, compOptoGain = 50.0f;
    bool  compOptoLimit = false;
    float compFetInput = 0.0f, compFetOutput = 0.0f, compFetAttack = 0.2f, compFetRelease = 400.0f;
    float compFetThresholdDb = -10.0f;
    int   compFetRatio = 0;
    float compVcaThreshDb = 0.0f, compVcaRatio = 4.0f, compVcaAttack = 1.0f, compVcaRelease = 100.0f, compVcaOutput = 0.0f;
    bool  compVcaOverEasy = false;
    bool  compVcaDetectorClassic = false;

    // Recording surface.
    bool  recordArmed = false;
    bool  inputMonitor = false;
    bool  printEffects = false;
    int   inputSource = -2, inputSourceR = -2;
    int   midiInputIndex = -1, midiChannel = 0;

    // Plugin slot - descriptor, raw legacy fallback, and state captured live.
    ClonePluginSnapshot plugin;

#if DUSKSTUDIO_HAS_MULTISAMPLE
    // Native multisample instrument - not a JUCE-hosted plugin, so it needs
    // its own pair alongside the description above.
    juce::String         multisamplePath;
    std::vector<uint8_t> multisampleState;
#endif

#if DUSKSTUDIO_HAS_NATIVE_CLAP
    CloneNativeSnapshot clap;
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
    CloneNativeSnapshot lv2;
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    CloneNativeSnapshot vst3;
#endif
#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_LV2 \
    || DUSKSTUDIO_HAS_NATIVE_VST3
    CloneNativeKind nativeKind = CloneNativeKind::None;
#endif

#if DUSKSTUDIO_HAS_NATIVE_AU
    // Native Audio Unit identity and property-list state. Keep the persisted
    // pair when the component is offline so clone/undo does not discard a
    // missing unit that can be restored on a later machine.
    juce::String         auIdentifier;
    std::vector<uint8_t> auState;
#endif

    // Region / MIDI region content.
    std::vector<AudioRegion> regions;
    std::vector<MidiRegion>  midiRegions;
};

namespace
{
#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_LV2 \
    || DUSKSTUDIO_HAS_NATIVE_VST3 || DUSKSTUDIO_HAS_NATIVE_AU
std::vector<uint8_t> decodeCarriedState (const juce::String& base64)
{
    auto blob = dusk::base64::decode (base64.toRawUTF8(), base64.getNumBytesAsUTF8());

    // Same fallback as AudioEngine's decodeBase64Blob: a 0.13.1-migrated slot
    // carries the JUCE MemoryBlock form on the native key, which RFC 4648
    // rejects outright. Dropping it here would clone the slot empty and let the
    // next save write defaults over the only surviving copy.
    if (blob.empty())
        blob = duskstudio::decodeLegacyStateBase64 (base64.toStdString());
    return blob;
}
#endif

#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_LV2 \
    || DUSKSTUDIO_HAS_NATIVE_VST3
// Live slot first (session.json's fields are only kept fresh during save); fall
// back to what the track carries so an offline plugin still survives the clone.
template <typename Slot>
void captureNativeSlot (const char* format, const Slot& slot,
                        const juce::String& carriedPath,
                        const juce::String& carriedPluginId,
                        const juce::String& carriedState, CloneNativeSnapshot& out)
{
    if (slot.isLoaded())
    {
        out.path     = juce::String::fromUTF8 (slot.getPath().c_str());
        out.pluginId = juce::String::fromUTF8 (slot.getPluginId().c_str());
        // A plugin that reports success but hands back nothing must not lose the
        // identity-matched copy either. Mirrors captureNativeState in the engine.
        if (! slot.saveState (out.state) || out.state.empty())
        {
            out.state = decodeCarriedState (carriedState);
            hosting::retainStateForLiveIdentity (
                { format, carriedPath.toStdString(), carriedPluginId.toStdString() },
                { format, slot.getPath(), slot.getPluginId() },
                out.state);
        }
        return;
    }
    if (carriedPath.isEmpty()) return;
    out.path     = carriedPath;
    out.pluginId = carriedPluginId;
    out.state    = decodeCarriedState (carriedState);
}

// Replay the single native rung selected by the source snapshot. The caller
// enforces the session-restore precedence before entering here.
template <typename LoadFn, typename RestoreStateFn, typename MarkFailedFn>
void applyNativeSlot (AudioEngine& engine, const CloneNativeSnapshot& s,
                      const char* label, int idx, LoadFn&& load,
                      RestoreStateFn&& restoreState, MarkFailedFn&& markFailed)
{
    if (s.path.isEmpty()) return;

    engine.suspendProcessing();
    std::string err;
    bool loaded = load (err);
    if (loaded && ! s.state.empty() && ! restoreState())
    {
        loaded = false;
        err = "state restore failed";
    }
    engine.resumeProcessing();

    if (! loaded)
    {
        // Keep the reference so a save right after the clone still round-trips
        // it and the load can be retried - same as a failed session restore.
        markFailed();
        juce::ignoreUnused (label, idx);
        DBG ("CloneTrackAction: " << label << " restore failed on strip " << idx
              << " (" << s.path << "): " << err.c_str());
    }
}
#endif

CloneTrackAction::Impl captureTrack (Track& t, AudioEngine& engine, int idx)
{
    CloneTrackAction::Impl s;
    s.name        = t.name;
    s.colour      = t.colour;
    s.mode        = t.mode.load (std::memory_order_relaxed);

    s.faderDb     = t.strip.faderDb.load (std::memory_order_relaxed);
    s.pan         = t.strip.pan.load     (std::memory_order_relaxed);
    s.mute        = t.strip.mute.load    (std::memory_order_relaxed);
    s.solo        = t.strip.solo.load    (std::memory_order_relaxed);
    s.phaseInvert = t.strip.phaseInvert.load (std::memory_order_relaxed);
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
        s.busAssign[(size_t) i] = t.strip.busAssign[(size_t) i].load (std::memory_order_relaxed);
    s.auxSendsBypassed = t.strip.auxSendsBypassed.load (std::memory_order_relaxed);
    for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
    {
        s.auxSendDb      [(size_t) i] = t.strip.auxSendDb[(size_t) i].load (std::memory_order_relaxed);
        s.auxSendPreFader[(size_t) i] = t.strip.auxSendPreFader[(size_t) i].load (std::memory_order_relaxed);
    }

    s.hpfEnabled = t.strip.hpfEnabled.load (std::memory_order_relaxed);
    s.hpfFreq    = t.strip.hpfFreq.load    (std::memory_order_relaxed);
    s.lfGainDb   = t.strip.lfGainDb.load   (std::memory_order_relaxed);
    s.lfFreq     = t.strip.lfFreq.load     (std::memory_order_relaxed);
    s.lmGainDb   = t.strip.lmGainDb.load   (std::memory_order_relaxed);
    s.lmFreq     = t.strip.lmFreq.load     (std::memory_order_relaxed);
    s.lmQ        = t.strip.lmQ.load        (std::memory_order_relaxed);
    s.hmGainDb   = t.strip.hmGainDb.load   (std::memory_order_relaxed);
    s.hmFreq     = t.strip.hmFreq.load     (std::memory_order_relaxed);
    s.hmQ        = t.strip.hmQ.load        (std::memory_order_relaxed);
    s.hfGainDb   = t.strip.hfGainDb.load   (std::memory_order_relaxed);
    s.hfFreq     = t.strip.hfFreq.load     (std::memory_order_relaxed);
    s.eqBlackMode = t.strip.eqBlackMode.load (std::memory_order_relaxed);

    s.compEnabled    = t.strip.compEnabled.load    (std::memory_order_relaxed);
    s.compMode       = t.strip.compMode.load       (std::memory_order_relaxed);
    s.compOptoPeakRed = t.strip.compOptoPeakRed.load (std::memory_order_relaxed);
    s.compOptoGain    = t.strip.compOptoGain.load    (std::memory_order_relaxed);
    s.compOptoLimit   = t.strip.compOptoLimit.load   (std::memory_order_relaxed);
    s.compFetInput    = t.strip.compFetInput.load    (std::memory_order_relaxed);
    s.compFetThresholdDb = t.strip.compFetThresholdDb.load (std::memory_order_relaxed);
    s.compFetOutput   = t.strip.compFetOutput.load   (std::memory_order_relaxed);
    s.compFetAttack   = t.strip.compFetAttack.load   (std::memory_order_relaxed);
    s.compFetRelease  = t.strip.compFetRelease.load  (std::memory_order_relaxed);
    s.compFetRatio    = t.strip.compFetRatio.load    (std::memory_order_relaxed);
    s.compVcaThreshDb = t.strip.compVcaThreshDb.load (std::memory_order_relaxed);
    s.compVcaRatio    = t.strip.compVcaRatio.load    (std::memory_order_relaxed);
    s.compVcaAttack   = t.strip.compVcaAttack.load   (std::memory_order_relaxed);
    s.compVcaRelease  = t.strip.compVcaRelease.load  (std::memory_order_relaxed);
    s.compVcaOutput   = t.strip.compVcaOutput.load   (std::memory_order_relaxed);
    s.compVcaOverEasy = t.strip.compVcaOverEasy.load (std::memory_order_relaxed);
    s.compVcaDetectorClassic = t.strip.compVcaDetectorClassic.load (std::memory_order_relaxed);

    s.recordArmed    = t.recordArmed.load    (std::memory_order_relaxed);
    s.inputMonitor   = t.inputMonitor.load   (std::memory_order_relaxed);
    s.printEffects   = t.printEffects.load   (std::memory_order_relaxed);
    s.inputSource    = t.inputSource.load    (std::memory_order_relaxed);
    s.inputSourceR   = t.inputSourceR.load   (std::memory_order_relaxed);
    s.midiInputIndex = t.midiInputIndex.load (std::memory_order_relaxed);
    s.midiChannel    = t.midiChannel.load    (std::memory_order_relaxed);

    // Plugin: pull from the live slot, not the (potentially stale)
    // session.json fields. Those are only kept fresh during save.
    auto& slot = engine.getStrip (idx).getPluginSlot();
    s.plugin.descriptor = slot.getDescriptorForSave();
    s.plugin.legacyDescriptionXml = slot.getLegacyDescriptionXmlForSave();
    s.plugin.stateBase64 = slot.getStateBase64ForSave();

#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_LV2 \
    || DUSKSTUDIO_HAS_NATIVE_VST3 || DUSKSTUDIO_HAS_NATIVE_AU
    auto& strip = engine.getStrip (idx);
    bool liveNativeCaptured = false;
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    if (strip.isNativeClapLoaded())
    {
        captureNativeSlot ("CLAP", strip.getNativeClapSlot(), t.nativeClapPath,
                           t.nativeClapPluginId, t.nativeClapStateBase64, s.clap);
        s.nativeKind = CloneNativeKind::Clap;
        liveNativeCaptured = true;
    }
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
    if (! liveNativeCaptured && strip.isNativeLv2Loaded())
    {
        // A clone snapshot must not rotate the source slot's file-backed state
        // generations or make the destination point into the source slot's
        // directory. Capture the portable/control blob, then restore the live
        // slot's normal per-session directory for the next real save.
        auto& lv2Slot = strip.getNativeLv2Slot();
        const auto sessionDir = engine.getSession().getSessionDirectory();
        const auto stateDir = sessionDir == juce::File()
            ? std::filesystem::path {}
            : std::filesystem::u8path (
                sessionDir.getChildFile ("state").getChildFile ("lv2")
                    .getChildFile ("track" + juce::String (idx + 1).paddedLeft ('0', 2))
                    .getFullPathName().toStdString());
        lv2Slot.setStateDirectory ({});
        captureNativeSlot ("LV2", lv2Slot, t.nativeLv2Path, t.nativeLv2PluginId,
                           t.nativeLv2StateBase64, s.lv2);
        lv2Slot.setStateDirectory (stateDir);
        s.nativeKind = CloneNativeKind::Lv2;
        liveNativeCaptured = true;
    }
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    if (! liveNativeCaptured && strip.isNativeVst3Loaded())
    {
        captureNativeSlot ("VST3", strip.getNativeVst3Slot(), t.nativeVst3Path,
                           t.nativeVst3PluginId, t.nativeVst3StateBase64, s.vst3);
        s.nativeKind = CloneNativeKind::Vst3;
        liveNativeCaptured = true;
    }
#endif
#if DUSKSTUDIO_HAS_NATIVE_AU
    auto& auSlot = strip.getNativeAuSlot();
    if (! liveNativeCaptured && auSlot.isLoaded())
    {
        s.auIdentifier = juce::String::fromUTF8 (auSlot.getPluginId().c_str());
        if (! auSlot.saveState (s.auState) || s.auState.empty())
        {
            s.auState = decodeCarriedState (t.nativeAuStateBase64);
            hosting::retainStateForLiveIdentity (
                { "AU", t.nativeAuIdentifier.toStdString(), {} },
                { "AU", auSlot.getPluginId(), {} },
                s.auState);
        }
        liveNativeCaptured = true;
    }
#endif
#if DUSKSTUDIO_HAS_MULTISAMPLE
    if (strip.isNativeMultisampleLoaded())
        liveNativeCaptured = true;
#endif

    // With no live owner, preserve one offline reference using the same
    // precedence as session restore. This is what makes AU clone/undo safe
    // when the other side carries a missing native plugin.
    if (! liveNativeCaptured)
    {
        bool carried = false;
#if DUSKSTUDIO_HAS_NATIVE_CLAP
        if (t.nativeClapPath.isNotEmpty())
        {
            captureNativeSlot ("CLAP", strip.getNativeClapSlot(), t.nativeClapPath,
                               t.nativeClapPluginId, t.nativeClapStateBase64, s.clap);
            s.nativeKind = CloneNativeKind::Clap;
            carried = true;
        }
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
        if (! carried && t.nativeLv2Path.isNotEmpty())
        {
            captureNativeSlot ("LV2", strip.getNativeLv2Slot(), t.nativeLv2Path,
                               t.nativeLv2PluginId, t.nativeLv2StateBase64, s.lv2);
            s.nativeKind = CloneNativeKind::Lv2;
            carried = true;
        }
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
        if (! carried && t.nativeVst3Path.isNotEmpty())
        {
            captureNativeSlot ("VST3", strip.getNativeVst3Slot(), t.nativeVst3Path,
                               t.nativeVst3PluginId, t.nativeVst3StateBase64, s.vst3);
            s.nativeKind = CloneNativeKind::Vst3;
            carried = true;
        }
#endif
#if DUSKSTUDIO_HAS_NATIVE_AU
        if (! carried && t.nativeAuIdentifier.isNotEmpty())
        {
            s.auIdentifier = t.nativeAuIdentifier;
            s.auState = decodeCarriedState (t.nativeAuStateBase64);
        }
#endif
    }
#endif

#if DUSKSTUDIO_HAS_MULTISAMPLE
    auto& msSlot = engine.getStrip (idx).getNativeMultisampleSlot();
    // The editor can clear the soundfont in place, leaving a loaded slot with no
    // file - the live path decides, as in publishPluginStateForSave. applyTrack
    // reads an empty path as "no multisample", so a blob captured beside one
    // would be stranded.
    const auto liveSoundfont = juce::String::fromUTF8 (
        msSlot.isLoaded() ? msSlot.getLoadedSoundfontPath().c_str() : "");
    if (liveSoundfont.isNotEmpty())
    {
        s.multisamplePath = liveSoundfont;
        msSlot.saveState (s.multisampleState);
    }
#endif

    s.regions     = t.regions;
    s.midiRegions = t.midiRegions.current();   // snapshot of the live vector
    return s;
}

void applyTrack (Track& t, AudioEngine& engine, int idx,
                  const CloneTrackAction::Impl& s)
{
    t.name   = s.name;
    t.colour = s.colour;
    t.mode.store (s.mode, std::memory_order_relaxed);

    t.strip.faderDb.store     (s.faderDb,     std::memory_order_relaxed);
    t.strip.pan.store         (s.pan,         std::memory_order_relaxed);
    t.strip.mute.store        (s.mute,        std::memory_order_relaxed);
    t.strip.solo.store        (s.solo,        std::memory_order_relaxed);
    t.strip.phaseInvert.store (s.phaseInvert, std::memory_order_relaxed);
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
        t.strip.busAssign[(size_t) i].store (s.busAssign[(size_t) i], std::memory_order_relaxed);
    t.strip.auxSendsBypassed.store (s.auxSendsBypassed, std::memory_order_relaxed);
    for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
    {
        t.strip.auxSendDb      [(size_t) i].store (s.auxSendDb[(size_t) i],      std::memory_order_relaxed);
        t.strip.auxSendPreFader[(size_t) i].store (s.auxSendPreFader[(size_t) i], std::memory_order_relaxed);
    }

    t.strip.hpfEnabled.store (s.hpfEnabled, std::memory_order_relaxed);
    t.strip.hpfFreq.store    (s.hpfFreq,    std::memory_order_relaxed);
    t.strip.lfGainDb.store   (s.lfGainDb,   std::memory_order_relaxed);
    t.strip.lfFreq.store     (s.lfFreq,     std::memory_order_relaxed);
    t.strip.lmGainDb.store   (s.lmGainDb,   std::memory_order_relaxed);
    t.strip.lmFreq.store     (s.lmFreq,     std::memory_order_relaxed);
    t.strip.lmQ.store        (s.lmQ,        std::memory_order_relaxed);
    t.strip.hmGainDb.store   (s.hmGainDb,   std::memory_order_relaxed);
    t.strip.hmFreq.store     (s.hmFreq,     std::memory_order_relaxed);
    t.strip.hmQ.store        (s.hmQ,        std::memory_order_relaxed);
    t.strip.hfGainDb.store   (s.hfGainDb,   std::memory_order_relaxed);
    t.strip.hfFreq.store     (s.hfFreq,     std::memory_order_relaxed);
    t.strip.eqBlackMode.store (s.eqBlackMode, std::memory_order_relaxed);

    t.strip.compEnabled.store    (s.compEnabled,    std::memory_order_relaxed);
    t.strip.compMode.store       (s.compMode,       std::memory_order_relaxed);
    t.strip.compOptoPeakRed.store (s.compOptoPeakRed, std::memory_order_relaxed);
    t.strip.compOptoGain.store    (s.compOptoGain,    std::memory_order_relaxed);
    t.strip.compOptoLimit.store   (s.compOptoLimit,   std::memory_order_relaxed);
    t.strip.compFetInput.store    (s.compFetInput,    std::memory_order_relaxed);
    t.strip.compFetThresholdDb.store (s.compFetThresholdDb, std::memory_order_relaxed);
    t.strip.compFetOutput.store   (s.compFetOutput,   std::memory_order_relaxed);
    t.strip.compFetAttack.store   (s.compFetAttack,   std::memory_order_relaxed);
    t.strip.compFetRelease.store  (s.compFetRelease,  std::memory_order_relaxed);
    t.strip.compFetRatio.store    (s.compFetRatio,    std::memory_order_relaxed);
    t.strip.compVcaThreshDb.store (s.compVcaThreshDb, std::memory_order_relaxed);
    t.strip.compVcaRatio.store    (s.compVcaRatio,    std::memory_order_relaxed);
    t.strip.compVcaAttack.store   (s.compVcaAttack,   std::memory_order_relaxed);
    t.strip.compVcaRelease.store  (s.compVcaRelease,  std::memory_order_relaxed);
    t.strip.compVcaOutput.store   (s.compVcaOutput,   std::memory_order_relaxed);
    t.strip.compVcaOverEasy.store (s.compVcaOverEasy, std::memory_order_relaxed);
    t.strip.compVcaDetectorClassic.store (s.compVcaDetectorClassic, std::memory_order_relaxed);

    t.recordArmed.store    (s.recordArmed,    std::memory_order_relaxed);
    t.inputMonitor.store   (s.inputMonitor,   std::memory_order_relaxed);
    t.printEffects.store   (s.printEffects,   std::memory_order_relaxed);
    t.inputSource.store    (s.inputSource,    std::memory_order_relaxed);
    t.inputSourceR.store   (s.inputSourceR,   std::memory_order_relaxed);
    t.midiInputIndex.store (s.midiInputIndex, std::memory_order_relaxed);
    t.midiChannel.store    (s.midiChannel,    std::memory_order_relaxed);

    // recordArmed was written directly above (bypassing setTrackArmed),
    // which means the armedTrackCount counter Session uses for the
    // anyTrackArmed() fast-path is out of sync. Resync now so a
    // subsequent engine.record() doesn't see a stale "no tracks armed"
    // and silently bail.
    engine.getSession().recomputeRtCounters();

    // Plugin: replay through the live slot.
    juce::String err;
    if (! engine.getStrip (idx).getPluginSlot().restoreFromSavedState (
            s.plugin.descriptor, s.plugin.legacyDescriptionXml,
            s.plugin.stateBase64, err))
    {
        DBG ("CloneTrackAction: plugin restore failed on strip " << idx
              << ": " << err);
    }

    t.regions = s.regions;
    t.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (s.midiRegions));

    // Persist the post-restore plugin state on Session so a save right
    // after a clone (with no manual edits in between) round-trips
    // correctly. publishPluginStateForSave does this for ALL slots; for
    // a single track we mirror by hand.
    s.plugin.publishTo (t);

#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_LV2 \
    || DUSKSTUDIO_HAS_NATIVE_VST3 || DUSKSTUDIO_HAS_NATIVE_AU
    // Replay exactly one native owner after the standard slot, matching session
    // restore precedence so clone/undo can safely cross an existing native
    // destination on every platform.
    {
        auto& strip = engine.getStrip (idx);
        bool nativeSelected = false;

#if DUSKSTUDIO_HAS_NATIVE_CLAP
        t.nativeClapPath.clear();
        t.nativeClapPluginId.clear();
        t.nativeClapStateBase64.clear();
        if (s.nativeKind == CloneNativeKind::Clap)
        {
            applyNativeSlot (engine, s.clap, "CLAP", idx,
                [&] (std::string& loadErr)
                { return strip.loadNativeClap (
                    juce::File (s.clap.path), loadErr, s.clap.pluginId); },
                [&] { return strip.getNativeClapSlot().loadState (s.clap.state); },
                [&] { strip.markNativeClapRestoreFailed(); });
            t.nativeClapPath = s.clap.path;
            t.nativeClapPluginId = s.clap.pluginId;
            t.nativeClapStateBase64 = s.clap.state.empty() ? juce::String()
                : juce::Base64::toBase64 (s.clap.state.data(), s.clap.state.size());
            nativeSelected = true;
        }
#endif

#if DUSKSTUDIO_HAS_NATIVE_LV2
        t.nativeLv2Path.clear();
        t.nativeLv2PluginId.clear();
        t.nativeLv2StateBase64.clear();
        if (! nativeSelected && s.nativeKind == CloneNativeKind::Lv2)
        {
            applyNativeSlot (engine, s.lv2, "LV2", idx,
                [&] (std::string& loadErr)
                { return strip.loadNativeLv2 (
                    juce::File (s.lv2.path), loadErr, s.lv2.pluginId); },
                [&] { return strip.getNativeLv2Slot().loadState (s.lv2.state); },
                [&] { strip.markNativeLv2RestoreFailed(); });
            t.nativeLv2Path = s.lv2.path;
            t.nativeLv2PluginId = s.lv2.pluginId;
            t.nativeLv2StateBase64 = s.lv2.state.empty() ? juce::String()
                : juce::Base64::toBase64 (s.lv2.state.data(), s.lv2.state.size());
            nativeSelected = true;
        }
#endif

#if DUSKSTUDIO_HAS_NATIVE_VST3
        t.nativeVst3Path.clear();
        t.nativeVst3PluginId.clear();
        t.nativeVst3StateBase64.clear();
        if (! nativeSelected && s.nativeKind == CloneNativeKind::Vst3)
        {
            applyNativeSlot (engine, s.vst3, "VST3", idx,
                [&] (std::string& loadErr)
                { return strip.loadNativeVst3 (
                    juce::File (s.vst3.path), loadErr, s.vst3.pluginId); },
                [&] { return strip.getNativeVst3Slot().loadState (s.vst3.state); },
                [&] { strip.markNativeVst3RestoreFailed(); });
            t.nativeVst3Path = s.vst3.path;
            t.nativeVst3PluginId = s.vst3.pluginId;
            t.nativeVst3StateBase64 = s.vst3.state.empty() ? juce::String()
                : juce::Base64::toBase64 (s.vst3.state.data(), s.vst3.state.size());
            nativeSelected = true;
        }
#endif

#if DUSKSTUDIO_HAS_NATIVE_AU
        t.nativeAuIdentifier.clear();
        t.nativeAuStateBase64.clear();
        if (! nativeSelected && s.auIdentifier.isNotEmpty())
        {
            engine.suspendProcessing();
            std::string auErr;
            bool loaded = strip.loadNativeAu (s.auIdentifier, auErr);
            if (loaded && ! s.auState.empty())
                loaded = strip.getNativeAuSlot().loadState (s.auState);
            engine.resumeProcessing();

            if (! loaded)
            {
                strip.markNativeAuRestoreFailed();
                DBG ("CloneTrackAction: Audio Unit restore failed on strip " << idx
                      << " (" << s.auIdentifier << "): " << auErr.c_str());
            }
            t.nativeAuIdentifier = s.auIdentifier;
            t.nativeAuStateBase64 = s.auState.empty() ? juce::String()
                : juce::Base64::toBase64 (s.auState.data(), s.auState.size());
            nativeSelected = true;
        }
#endif

        if (nativeSelected)
        {
            // The native load evicted the standard slot; keep the Session model
            // one-host as well so an immediate save/redo cannot resurrect it.
            t.pluginDescriptor.reset();
            t.pluginLegacyDescriptionXml.clear();
            t.pluginStateBase64.clear();
        }
        else
        {
            bool anyLoaded = false;
#if DUSKSTUDIO_HAS_NATIVE_CLAP
            anyLoaded = anyLoaded || strip.isNativeClapLoaded();
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
            anyLoaded = anyLoaded || strip.isNativeLv2Loaded();
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
            anyLoaded = anyLoaded || strip.isNativeVst3Loaded();
#endif
#if DUSKSTUDIO_HAS_NATIVE_AU
            anyLoaded = anyLoaded || strip.isNativeAuLoaded();
#endif
            if (anyLoaded) engine.suspendProcessing();
#if DUSKSTUDIO_HAS_NATIVE_CLAP
            strip.unloadNativeClap();
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
            strip.unloadNativeLv2();
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
            strip.unloadNativeVst3();
#endif
#if DUSKSTUDIO_HAS_NATIVE_AU
            strip.unloadNativeAu();
#endif
            if (anyLoaded) engine.resumeProcessing();
        }
    }
#endif

#if DUSKSTUDIO_HAS_MULTISAMPLE
    // After the JUCE replay: a multisample load evicts the JUCE slot, so doing
    // it second keeps "one host per insert" whichever way the clone goes.
    {
        auto& strip = engine.getStrip (idx);
        if (s.multisamplePath.isNotEmpty() || strip.isNativeMultisampleLoaded())
        {
            // Two-phase: parse the soundfont off the engine gate, fence the swap
            // only (see NativeMultisampleSlot::prime).
            NativeMultisampleSlot::PrimedLoad primed;
            std::string msErr;
            if (s.multisamplePath.isNotEmpty())
                primed = strip.primeNativeMultisample (juce::File (s.multisamplePath),
                                                        msErr, &s.multisampleState);
            const bool msStateRestored = ! primed.stateRestoreFailed;
            strip.getNativeMultisampleSlot().drainPendingLoads();

            engine.suspendProcessing();
            bool msLoaded = false;
            if (primed && msStateRestored)
                msLoaded = strip.commitNativeMultisample (std::move (primed));
            else
                strip.unloadNativeMultisample();
            engine.resumeProcessing();

            if ((! msLoaded || ! msStateRestored) && s.multisamplePath.isNotEmpty())
            {
                // Keep the reference so a save right after the clone still
                // round-trips it and the load can be retried, exactly like a
                // failed restore in consumePluginStateAfterLoad.
                strip.markNativeMultisampleRestoreFailed();
                DBG ("CloneTrackAction: multisample restore failed on strip " << idx
                      << " (" << s.multisamplePath << "): " << msErr.c_str());
            }
        }

        // Unconditional, like the plugin writes above: a clone from a source
        // with no multisample has to clear whatever the destination held.
        t.nativeMultisamplePath = s.multisamplePath;
        t.nativeMultisampleStateBase64 = s.multisampleState.empty()
            ? juce::String()
            : juce::Base64::toBase64 (s.multisampleState.data(), s.multisampleState.size());
    }
#endif
}
} // namespace

CloneTrackAction::CloneTrackAction (Session& s, AudioEngine& e,
                                      int srcTrack, int dstTrack)
    : session (s), engine (e), srcIdx (srcTrack), dstIdx (dstTrack)
{}

CloneTrackAction::~CloneTrackAction() = default;

bool CloneTrackAction::perform()
{
    if (srcIdx < 0 || srcIdx >= Session::kNumTracks) return false;
    if (dstIdx < 0 || dstIdx >= Session::kNumTracks) return false;
    if (srcIdx == dstIdx) return false;
    if (engine.getTransport().isRecording()) return false;
    // The clone snapshot doesn't carry the frozen flag / frozenRegion, so
    // cloning to or from a frozen track would desync. Refuse - unfreeze first.
    if (session.track (srcIdx).frozen.load (std::memory_order_relaxed)
        || session.track (dstIdx).frozen.load (std::memory_order_relaxed)) return false;

    // First perform: capture both before-state of the destination
    // (for undo) and after-state from the source (for redo). Subsequent
    // perform()s (i.e. redo) just re-apply the captured after-state.
    if (beforeState == nullptr)
    {
        beforeState = std::make_unique<Impl> (
            captureTrack (session.track (dstIdx), engine, dstIdx));
        afterState = std::make_unique<Impl> (
            captureTrack (session.track (srcIdx), engine, srcIdx));
        // Tag the cloned name so the user can tell duplicates apart.
        afterState->name = afterState->name + " (copy)";
    }

    applyTrack (session.track (dstIdx), engine, dstIdx, *afterState);
    rebuildPlaybackIfStopped (engine);
    return true;
}

bool CloneTrackAction::undo()
{
    if (beforeState == nullptr) return false;
    if (dstIdx < 0 || dstIdx >= Session::kNumTracks) return false;
    if (engine.getTransport().isRecording()) return false;
    // The Impl snapshot doesn't carry frozen state, so restoring beforeState onto a
    // now-frozen destination would desync its baked WAV. Refuse - mirrors perform()'s
    // frozen guard. Unfreeze first to undo.
    if (session.track (dstIdx).frozen.load (std::memory_order_relaxed)) return false;
    applyTrack (session.track (dstIdx), engine, dstIdx, *beforeState);
    rebuildPlaybackIfStopped (engine);
    return true;
}

// JoinRegionsAction

JoinRegionsAction::JoinRegionsAction (Session& s, AudioEngine& e,
                                       int t, const std::vector<int>& idxs)
    : session (s), engine (e), trackIdx (t), indices (idxs)
{
    // Deduplicate + sort by timelineStart so the action records a stable
    // order independent of the user's click sequence.
    std::sort (indices.begin(), indices.end());
    indices.erase (std::unique (indices.begin(), indices.end()), indices.end());
    if (trackIdx >= 0 && trackIdx < Session::kNumTracks)
    {
        auto& regs = session.track (trackIdx).regions;
        std::sort (indices.begin(), indices.end(),
                    [&] (int a, int b)
                    {
                        if (a < 0 || a >= (int) regs.size()) return false;
                        if (b < 0 || b >= (int) regs.size()) return true;
                        return regs[(size_t) a].timelineStart
                             < regs[(size_t) b].timelineStart;
                    });
    }
}

bool JoinRegionsAction::perform()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    if (frozenLocked (session, trackIdx)) return false;
    auto& regs = session.track (trackIdx).regions;
    if (indices.size() < 2) return false;

    if (! firstPerformDone)
    {
        beforeRegions.clear();
        beforeRegions.reserve (indices.size());
        for (int idx : indices)
        {
            if (idx < 0 || idx >= (int) regs.size()) return false;
            beforeRegions.push_back (regs[(size_t) idx]);
        }
        firstPerformDone = true;
    }
    else if (beforeRegions.size() != indices.size())
    {
        return false;   // shouldn't happen, defensive
    }

    // Fast-path eligibility: every selected region references the same
    // file, the sourceOffsets form a contiguous run, and timelinePositions
    // abut. "Abut" tolerates a 1-sample rounding gap so a series of splits
    // that snapped to slightly different sub-sample boundaries still
    // collapse cleanly.
    constexpr std::int64_t kAbutTolerance = 1;
    auto abs64 = [] (std::int64_t v) noexcept -> std::int64_t
    { return v < 0 ? -v : v; };
    bool sameFile = true, abuts = true;
    for (size_t i = 1; i < beforeRegions.size(); ++i)
    {
        const auto& prev = beforeRegions[i - 1];
        const auto& cur  = beforeRegions[i];
        if (cur.file != prev.file) { sameFile = false; break; }
        const auto prevEnd = prev.timelineStart + prev.lengthInSamples;
        const auto gap = cur.timelineStart - prevEnd;
        const auto srcDelta = cur.sourceOffset
                                - (prev.sourceOffset + prev.lengthInSamples);
        if (abs64 (gap) > kAbutTolerance) { abuts = false; break; }
        if (abs64 (srcDelta) > kAbutTolerance) { abuts = false; break; }
    }

    const auto firstStart = beforeRegions.front().timelineStart;
    const auto firstSrcOffset = beforeRegions.front().sourceOffset;
    // Latest end over ALL regions, not back()'s end: regions are sorted by
    // start, and an earlier-starting region can outlast the last-starting
    // one (overlap/containment). back()'s end would undersize the slow
    // path's mix buffer and the merged length. The latest-ending region is
    // also the one whose audio closes the merged result, so its fade-out is
    // the one both paths carry over.
    std::int64_t lastEnd = firstStart;
    const AudioRegion* latestEnding = &beforeRegions.front();
    for (const auto& r : beforeRegions)
    {
        const auto end = r.timelineStart + r.lengthInSamples;
        if (end > lastEnd) { lastEnd = end; latestEnding = &r; }
    }
    const auto totalLen = lastEnd - firstStart;

    // Sort descending so the larger indices erase first and the smaller
    // index that holds the merged region stays valid.
    std::vector<int> sortedDesc = indices;
    std::sort (sortedDesc.begin(), sortedDesc.end(), std::greater<int>());

    // The cheap merge keeps the lead region's gainDb / muted for the whole
    // result, which is only right when every joined region carries the same
    // values - a muted or re-gained later region must go through the render
    // path, which bakes those per region. Exact float compare is fine: equal
    // means untouched-default or the same drag, anything else renders.
    bool uniformGainMute = true;
    for (std::size_t i = 1; i < beforeRegions.size(); ++i)
        if (beforeRegions[i].muted != beforeRegions.front().muted
            || beforeRegions[i].gainDb != beforeRegions.front().gainDb)
        {
            uniformGainMute = false;
            break;
        }

    if (sameFile && abuts && uniformGainMute)
    {
        // Cheap merge: keep the leading region, extend its length, drop
        // the rest. Outer fadeIn from the first and fadeOut from the
        // latest-ending region are preserved; inner fades vanish along
        // with the joints.
        AudioRegion merged = beforeRegions.front();
        merged.timelineStart   = firstStart;
        merged.sourceOffset    = firstSrcOffset;
        merged.lengthInSamples = totalLen;
        merged.fadeOutSamples  = latestEnding->fadeOutSamples;
        merged.fadeOutShape    = latestEnding->fadeOutShape;
        merged.fadeOutAuto     = latestEnding->fadeOutAuto;
        merged.previousTakes   = beforeRegions.front().previousTakes;

        const int mergedIdx = eraseJoinedAndGetMergedSlot (regs, indices, sortedDesc);
        if (mergedIdx < 0)
            return false;
        resultInsertedAt = mergedIdx;
        regs[(size_t) resultInsertedAt] = merged;
        rebuildPlaybackIfStopped (engine);
        return true;
    }

    // Slow path: render to a new WAV in <session>/takes/. Mix every
    // selected region into one buffer at its proper timeline offset
    // (gaps become silence; overlaps sum). Uses the source files'
    // sample rate / channel count from the leading region.
    if (! joinSelectionInBounds (regs, sortedDesc))
        return false;
    auto firstReader = dusk::audio::FileReader::open (
        audioPath (beforeRegions.front().file));
    if (firstReader == nullptr) return false;
    const double sr   = firstReader->info().sampleRate;
    const int    bits = std::max (16, firstReader->info().bitsPerSample);
    const int    chs  = std::clamp ((int) beforeRegions.front().numChannels, 1, 2);

    const auto totalSamples = (int) std::clamp<std::int64_t> (
        totalLen, 1, std::numeric_limits<int>::max());
    dusk::audio::PlanarBuffer mixBuf;
    mixBuf.setSize (chs, totalSamples);

    for (const auto& reg : beforeRegions)
    {
        if (reg.muted) continue;
        auto rdr = dusk::audio::FileReader::open (audioPath (reg.file));
        if (rdr == nullptr) continue;
        const int regSamples = (int) std::clamp<std::int64_t> (
            reg.lengthInSamples, 0, std::numeric_limits<int>::max());
        if (regSamples == 0) continue;
        dusk::audio::PlanarBuffer tmp;
        tmp.setSize (chs, regSamples);
        if (rdr->read (tmp.data(), chs, reg.sourceOffset, regSamples) != regSamples)
            return false;   // unreadable region body - abort the join, leave regions unchanged
                            // (no output file created yet, nothing to clean up)
        const float gain = dusk::audio::decibelsToGain (
            std::clamp (reg.gainDb, -60.0f, 24.0f), -60.0f);
        const int destOffset = (int) (reg.timelineStart - firstStart);
        for (int c = 0; c < chs; ++c)
        {
            float* src = tmp.channel (c);
            for (int i = 0; i < regSamples; ++i) src[i] *= gain;
            dusk::audio::vecAdd (mixBuf.channel (c) + destOffset, src, regSamples);
        }
    }

    auto takesDir = session.getSessionDirectory().getChildFile ("takes");
    if (! takesDir.exists())
    {
        const auto res = takesDir.createDirectory();
        if (res.failed()) return false;
    }
    auto outFile = takesDir.getNonexistentChildFile (
        beforeRegions.front().file.getFileNameWithoutExtension() + "-joined",
        ".wav", false);
    dusk::audio::WriteSpec writeSpec;
    writeSpec.sampleRate    = sr;
    writeSpec.numChannels   = chs;
    writeSpec.bitsPerSample = bits;
    writeSpec.format        = dusk::audio::WriteSpec::Format::Wav;
    auto writer = dusk::audio::FileWriter::create (audioPath (outFile), writeSpec);
    if (writer == nullptr) { outFile.deleteFile(); return false; }
    if (! writer->write (mixBuf.data(), chs, totalSamples))
    {
        writer.reset();
        outFile.deleteFile();
        return false;
    }
    writer.reset();

    AudioRegion merged = beforeRegions.front();
    merged.file            = outFile;
    merged.timelineStart   = firstStart;
    merged.sourceOffset    = 0;
    merged.lengthInSamples = totalLen;
    merged.numChannels     = chs;
    merged.fadeInSamples   = beforeRegions.front().fadeInSamples;
    merged.fadeInShape     = beforeRegions.front().fadeInShape;
    merged.fadeOutSamples  = latestEnding->fadeOutSamples;
    merged.fadeOutShape    = latestEnding->fadeOutShape;
    merged.fadeOutAuto     = latestEnding->fadeOutAuto;
    merged.previousTakes.clear();
    // Gain and mute are baked into the rendered file.
    merged.gainDb          = 0.0f;
    merged.muted           = false;

    const int mergedIdx = eraseJoinedAndGetMergedSlot (regs, indices, sortedDesc);
    if (mergedIdx < 0)
    {
        outFile.deleteFile();
        return false;
    }
    resultInsertedAt = mergedIdx;
    regs[(size_t) resultInsertedAt] = merged;
    rebuildPlaybackIfStopped (engine);
    return true;
}

bool JoinRegionsAction::undo()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    if (frozenLocked (session, trackIdx)) return false;
    if (resultInsertedAt < 0) return false;
    auto& regs = session.track (trackIdx).regions;
    if (resultInsertedAt >= (int) regs.size()) return false;

    // Pair every original index with its captured region snapshot, sort by
    // ASCENDING numeric index, and re-insert in that order. `indices` is
    // sorted by timelineStart (set in the ctor); using timeline order here
    // would shift later inserts off-by-N every time an earlier insert
    // landed past a low-numbered slot.
    std::vector<std::pair<int, AudioRegion>> pairs;
    pairs.reserve (indices.size());
    for (size_t i = 0; i < indices.size() && i < beforeRegions.size(); ++i)
        pairs.emplace_back (indices[i], beforeRegions[i]);
    std::sort (pairs.begin(), pairs.end(),
                [] (const auto& a, const auto& b) { return a.first < b.first; });

    regs.erase (regs.begin() + resultInsertedAt);
    for (const auto& [idx, reg] : pairs)
    {
        if (idx < 0 || idx > (int) regs.size()) return false;
        regs.insert (regs.begin() + idx, reg);
    }
    rebuildPlaybackIfStopped (engine);
    return true;
}

// RecordCommitAction

RecordCommitAction::RecordCommitAction (Session& s, AudioEngine& e,
                                          std::vector<TrackDiff> d)
    : session (s), engine (e), diffs (std::move (d)) {}

bool RecordCommitAction::perform()
{
    // The first perform() is the UndoManager's bookkeeping replay of
    // the commit RecordManager already applied to session state - no
    // need to write the after-state again. Subsequent calls (redo
    // after an undo) actually mutate state.
    if (! firstPerformDone)
    {
        firstPerformDone = true;
        return true;
    }
    for (const auto& d : diffs)
    {
        if (d.trackIndex < 0 || d.trackIndex >= Session::kNumTracks) continue;
        if (frozenLocked (session, d.trackIndex)) continue;   // frozen track is edit-locked
        session.track (d.trackIndex).regions = d.audioAfter;
        session.track (d.trackIndex).midiRegions.mutate (
            [&d] (std::vector<MidiRegion>& mregs) { mregs = d.midiAfter; });
    }
    rebuildPlaybackIfStopped (engine);
    return true;
}

bool RecordCommitAction::undo()
{
    for (const auto& d : diffs)
    {
        if (d.trackIndex < 0 || d.trackIndex >= Session::kNumTracks) continue;
        if (frozenLocked (session, d.trackIndex)) continue;   // frozen track is edit-locked
        session.track (d.trackIndex).regions = d.audioBefore;
        session.track (d.trackIndex).midiRegions.mutate (
            [&d] (std::vector<MidiRegion>& mregs) { mregs = d.midiBefore; });
    }
    rebuildPlaybackIfStopped (engine);
    return true;
}

// ReverseRegionAction

ReverseRegionAction::ReverseRegionAction (Session& s, AudioEngine& e,
                                            int t, int idx)
    : session (s), engine (e), trackIdx (t), regionIdx (idx)
{}

bool ReverseRegionAction::perform()
{
    if (! indexValid (session, trackIdx, regionIdx)) return false;
    if (frozenLocked (session, trackIdx)) return false;
    if (session.track (trackIdx).regions[(size_t) regionIdx].locked) return false;   // locked region

    // First perform renders the reversed WAV + captures before/after; redo just
    // re-applies the captured after-state.
    if (! firstPerformDone)
    {
        beforeState = session.track (trackIdx).regions[(size_t) regionIdx];

        auto rdr = dusk::audio::FileReader::open (audioPath (beforeState.file));
        if (rdr == nullptr) return false;

        // Bound the channel count: corrupt session data could carry a wild
        // numChannels and blow up the buffer allocation. Cap to 1-2 (the app is
        // stereo-max) and to what the reader actually has.
        const int readerChannels = rdr->info().numChannels;
        const int chs = std::clamp ((int) beforeState.numChannels,
                                     1, std::max (1, std::min (2, readerChannels)));
        const std::int64_t len = std::clamp<std::int64_t> (
            beforeState.lengthInSamples, 1, std::numeric_limits<int>::max());
        const double sr   = rdr->info().sampleRate;
        const int    bits = std::max (16, rdr->info().bitsPerSample);

        auto takesDir = session.getSessionDirectory().getChildFile ("takes");
        if (! takesDir.exists() && takesDir.createDirectory().failed()) return false;
        auto outFile = takesDir.getNonexistentChildFile (
            beforeState.file.getFileNameWithoutExtension() + "-reversed", ".wav", false);
        dusk::audio::WriteSpec writeSpec;
        writeSpec.sampleRate    = sr;
        writeSpec.numChannels   = chs;
        writeSpec.bitsPerSample = bits;
        writeSpec.format        = dusk::audio::WriteSpec::Format::Wav;
        auto writer = dusk::audio::FileWriter::create (audioPath (outFile), writeSpec);
        if (writer == nullptr) { outFile.deleteFile(); return false; }

        // Stream the source tail-first in bounded chunks: read the chunk ending
        // at `remaining`, reverse each channel, append it. This reverses the
        // whole region without ever holding it all in RAM, and a failed read or
        // write aborts + discards the partial file instead of committing garbage.
        constexpr int kChunk = 1 << 16;   // 64k frames per chunk
        dusk::audio::PlanarBuffer chunk;
        chunk.setSize (chs, kChunk);
        std::int64_t remaining = len;
        bool ioOk = true;
        while (remaining > 0)
        {
            const int n = (int) std::min ((std::int64_t) kChunk, remaining);
            chunk.clear();
            if (rdr->read (chunk.data(), chs,
                           beforeState.sourceOffset + (remaining - n), n) != n)
            { ioOk = false; break; }
            for (int c = 0; c < chs; ++c)
                std::reverse (chunk.channel (c), chunk.channel (c) + n);
            if (! writer->write (chunk.data(), chs, n))
            { ioOk = false; break; }
            remaining -= n;
        }
        writer.reset();   // close the file before any delete
        if (! ioOk) { outFile.deleteFile(); return false; }

        afterState = beforeState;
        afterState.file            = outFile;
        afterState.sourceOffset    = 0;
        afterState.lengthInSamples = len;
        afterState.numChannels     = chs;
        // Reversed audio's head is the original tail, so swap the fades to keep
        // each ramp on the same material.
        std::swap (afterState.fadeInSamples, afterState.fadeOutSamples);
        std::swap (afterState.fadeInShape,   afterState.fadeOutShape);
        std::swap (afterState.fadeInAuto,    afterState.fadeOutAuto);
        firstPerformDone = true;
    }

    if (! indexValid (session, trackIdx, regionIdx)) return false;
    session.track (trackIdx).regions[(size_t) regionIdx] = afterState;
    rebuildPlaybackIfStopped (engine);
    return true;
}

bool ReverseRegionAction::undo()
{
    if (! indexValid (session, trackIdx, regionIdx)) return false;
    if (frozenLocked (session, trackIdx)) return false;
    session.track (trackIdx).regions[(size_t) regionIdx] = beforeState;
    rebuildPlaybackIfStopped (engine);
    return true;
}

SetTempoMapAction::SetTempoMapAction (AudioEngine& e,
                                        std::vector<TempoPoint> b,
                                        std::vector<TempoPoint> a)
    : engine (e), before (std::move (b)), after (std::move (a)) {}

bool SetTempoMapAction::perform() { engine.setTempoPoints (after);  return true; }
bool SetTempoMapAction::undo()    { engine.setTempoPoints (before); return true; }

AutomationLaneEditAction::AutomationLaneEditAction (Session& s, int t, int p,
                                                      std::vector<AutomationPoint> b,
                                                      std::vector<AutomationPoint> a)
    : session (s), trackIdx (t), paramIdx (p),
      before (std::move (b)), after (std::move (a)) {}

bool AutomationLaneEditAction::apply (const std::vector<AutomationPoint>& pts)
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks
        || paramIdx < 0 || paramIdx >= kNumAutomationParams)
        return false;
    auto& trk = session.track (trackIdx);
    // Atomic publish: the audio thread reads this lane lock-free in Read/Touch
    // mode, so undo/redo (which can fire mid-playback) must swap the whole
    // vector, not mutate it in place. The publish IS the release the audio
    // thread's acquire-load pairs with; no separate mode re-store needed.
    trk.automationLanes[(size_t) paramIdx].publishPoints (pts);
    return true;
}

bool AutomationLaneEditAction::perform() { return apply (after); }
bool AutomationLaneEditAction::undo()    { return apply (before); }
} // namespace duskstudio
