#include "RegionEditActions.h"
#include "../engine/AudioEngine.h"
#include "../engine/PlaybackEngine.h"
#include "../engine/Transport.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>

namespace duskstudio
{
namespace
{
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

bool MidiRegionEditAction::perform()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    if (frozenLocked (session, trackIdx)) return false;
    auto& v = session.track (trackIdx).midiRegions.currentMutable();
    if (regionIdx < 0 || regionIdx >= (int) v.size()) return false;
    v[(size_t) regionIdx] = afterState;
    rebuildPlaybackIfStopped (engine);
    return true;
}

bool MidiRegionEditAction::undo()
{
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    if (frozenLocked (session, trackIdx)) return false;
    auto& v = session.track (trackIdx).midiRegions.currentMutable();
    if (regionIdx < 0 || regionIdx >= (int) v.size()) return false;
    v[(size_t) regionIdx] = beforeState;
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
    auto& v = session.track (trackIdx).midiRegions.currentMutable();
    if (regionIdx < 0 || regionIdx >= (int) v.size()) return false;
    removed = v[(size_t) regionIdx];
    haveRemoved = true;
    v.erase (v.begin() + regionIdx);
    rebuildPlaybackIfStopped (engine);
    return true;
}

bool DeleteMidiRegionAction::undo()
{
    if (! haveRemoved) return false;
    if (trackIdx < 0 || trackIdx >= Session::kNumTracks) return false;
    if (frozenLocked (session, trackIdx)) return false;
    auto& v = session.track (trackIdx).midiRegions.currentMutable();
    const int insertAt = std::min (regionIdx, (int) v.size());
    v.insert (v.begin() + insertAt, removed);
    rebuildPlaybackIfStopped (engine);
    return true;
}

// CloneTrackAction

// POD snapshot of every per-track field the clone should duplicate.
// Atomics are loaded into plain values; vectors are copied by value.
// Plugin state is captured as the (descriptionXml, stateBase64) pair the
// session-restore path already understands.
struct CloneTrackAction::Impl
{
    juce::String name;
    juce::Colour colour;
    int mode = 0;

    // ChannelStripParams.
    float faderDb = 0.0f, pan = 0.0f;
    bool  mute = false, solo = false, phaseInvert = false;
    std::array<bool, ChannelStripParams::kNumBuses> busAssign {};
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
    float compThresholdDb = 0.0f;
    float compOptoPeakRed = 30.0f, compOptoGain = 50.0f;
    bool  compOptoLimit = false;
    float compFetInput = 0.0f, compFetOutput = 0.0f, compFetAttack = 0.2f, compFetRelease = 400.0f;
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

    // Plugin slot - description + state captured live from the engine.
    juce::String pluginDescXml;
    juce::String pluginStateB64;

    // Region / MIDI region content.
    std::vector<AudioRegion> regions;
    std::vector<MidiRegion>  midiRegions;
};

namespace
{
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
    s.compThresholdDb = t.strip.compThresholdDb.load (std::memory_order_relaxed);
    s.compOptoPeakRed = t.strip.compOptoPeakRed.load (std::memory_order_relaxed);
    s.compOptoGain    = t.strip.compOptoGain.load    (std::memory_order_relaxed);
    s.compOptoLimit   = t.strip.compOptoLimit.load   (std::memory_order_relaxed);
    s.compFetInput    = t.strip.compFetInput.load    (std::memory_order_relaxed);
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
    s.pluginDescXml  = slot.getDescriptionXmlForSave();
    s.pluginStateB64 = slot.getStateBase64ForSave();

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
    t.strip.compThresholdDb.store (s.compThresholdDb, std::memory_order_relaxed);
    t.strip.compOptoPeakRed.store (s.compOptoPeakRed, std::memory_order_relaxed);
    t.strip.compOptoGain.store    (s.compOptoGain,    std::memory_order_relaxed);
    t.strip.compOptoLimit.store   (s.compOptoLimit,   std::memory_order_relaxed);
    t.strip.compFetInput.store    (s.compFetInput,    std::memory_order_relaxed);
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

    // Plugin: replay through the live slot. Empty descriptionXml
    // legitimately means "no plugin" - restoreFromSavedState handles
    // that as the unloaded steady state.
    juce::String err;
    engine.getStrip (idx).getPluginSlot().restoreFromSavedState (
        s.pluginDescXml, s.pluginStateB64, err);
    if (err.isNotEmpty())
        DBG ("CloneTrackAction: plugin restore failed on strip " << idx
              << " (" << s.pluginDescXml << "): " << err);

    t.regions = s.regions;
    t.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (s.midiRegions));

    // Persist the post-restore plugin state on Session so a save right
    // after a clone (with no manual edits in between) round-trips
    // correctly. publishPluginStateForSave does this for ALL slots; for
    // a single track we mirror by hand.
    t.pluginDescriptionXml = s.pluginDescXml;
    t.pluginStateBase64    = s.pluginStateB64;
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
    const auto lastEnd = beforeRegions.back().timelineStart
                         + beforeRegions.back().lengthInSamples;
    const auto totalLen = lastEnd - firstStart;

    // Sort descending so the larger indices erase first and the smaller
    // index that holds the merged region stays valid.
    std::vector<int> sortedDesc = indices;
    std::sort (sortedDesc.begin(), sortedDesc.end(), std::greater<int>());

    if (sameFile && abuts)
    {
        // Cheap merge: keep the leading region, extend its length, drop
        // the rest. Outer fadeIn / fadeOutShape from first / last are
        // preserved; inner fades vanish along with the joints.
        AudioRegion merged = beforeRegions.front();
        merged.timelineStart   = firstStart;
        merged.sourceOffset    = firstSrcOffset;
        merged.lengthInSamples = totalLen;
        merged.fadeOutSamples  = beforeRegions.back().fadeOutSamples;
        merged.fadeOutShape    = beforeRegions.back().fadeOutShape;
        merged.previousTakes   = beforeRegions.front().previousTakes;

        const int leadIdx = indices.front();
        for (int idx : sortedDesc)
            if (idx != leadIdx)
                regs.erase (regs.begin() + idx);
        resultInsertedAt = leadIdx;
        if (resultInsertedAt >= 0 && resultInsertedAt < (int) regs.size())
            regs[(size_t) resultInsertedAt] = merged;
        rebuildPlaybackIfStopped (engine);
        return true;
    }

    // Slow path: render to a new WAV in <session>/takes/. Mix every
    // selected region into one buffer at its proper timeline offset
    // (gaps become silence; overlaps sum). Uses the source files'
    // sample rate / channel count from the leading region.
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    auto firstReader = std::unique_ptr<juce::AudioFormatReader> (
        fm.createReaderFor (beforeRegions.front().file));
    if (firstReader == nullptr) return false;
    const double sr   = firstReader->sampleRate;
    const int    bits = std::max (16, (int) firstReader->bitsPerSample);
    const int    chs  = std::clamp ((int) beforeRegions.front().numChannels, 1, 2);

    const auto totalSamples = (int) std::clamp<std::int64_t> (
        totalLen, 1, std::numeric_limits<int>::max());
    juce::AudioBuffer<float> mixBuf (chs, totalSamples);
    mixBuf.clear();

    for (const auto& reg : beforeRegions)
    {
        std::unique_ptr<juce::AudioFormatReader> rdr (fm.createReaderFor (reg.file));
        if (rdr == nullptr) continue;
        const int regSamples = (int) std::clamp<std::int64_t> (
            reg.lengthInSamples, 0, std::numeric_limits<int>::max());
        if (regSamples == 0) continue;
        juce::AudioBuffer<float> tmp (chs, regSamples);
        tmp.clear();
        if (! rdr->read (&tmp, 0, regSamples, reg.sourceOffset, true, chs > 1))
            return false;   // unreadable region body - abort the join, leave regions unchanged
                            // (no output file created yet, nothing to clean up)
        const int destOffset = (int) (reg.timelineStart - firstStart);
        for (int c = 0; c < chs; ++c)
            mixBuf.addFrom (c, destOffset, tmp, c, 0, regSamples);
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
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> out (outFile.createOutputStream());
    if (out == nullptr) return false;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (out.get(), sr, (std::uint32_t) chs, bits, {}, 0));
    if (writer == nullptr) { out.reset(); outFile.deleteFile(); return false; }
    out.release();   // ownership transferred to writer
    if (! writer->writeFromAudioSampleBuffer (mixBuf, 0, totalSamples))
    {
        writer.reset();
        outFile.deleteFile();
        return false;
    }
    writer.reset();
    renderedFile = outFile;

    AudioRegion merged = beforeRegions.front();
    merged.file            = outFile;
    merged.timelineStart   = firstStart;
    merged.sourceOffset    = 0;
    merged.lengthInSamples = totalLen;
    merged.numChannels     = chs;
    merged.fadeInSamples   = beforeRegions.front().fadeInSamples;
    merged.fadeInShape     = beforeRegions.front().fadeInShape;
    merged.fadeOutSamples  = beforeRegions.back().fadeOutSamples;
    merged.fadeOutShape    = beforeRegions.back().fadeOutShape;
    merged.previousTakes.clear();

    const int leadIdx = indices.front();
    for (int idx : sortedDesc)
        if (idx != leadIdx)
            regs.erase (regs.begin() + idx);
    resultInsertedAt = leadIdx;
    if (resultInsertedAt >= 0 && resultInsertedAt < (int) regs.size())
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

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rdr (fm.createReaderFor (beforeState.file));
        if (rdr == nullptr) return false;

        // Bound the channel count: corrupt session data could carry a wild
        // numChannels and blow up the buffer allocation. Cap to 1-2 (the app is
        // stereo-max) and to what the reader actually has.
        const int chs = std::clamp ((int) beforeState.numChannels,
                                     1, std::min (2, (int) rdr->numChannels));
        const std::int64_t len = std::clamp<std::int64_t> (
            beforeState.lengthInSamples, 1, std::numeric_limits<int>::max());
        const double sr   = rdr->sampleRate;
        const int    bits = std::max (16, (int) rdr->bitsPerSample);

        auto takesDir = session.getSessionDirectory().getChildFile ("takes");
        if (! takesDir.exists() && takesDir.createDirectory().failed()) return false;
        auto outFile = takesDir.getNonexistentChildFile (
            beforeState.file.getFileNameWithoutExtension() + "-reversed", ".wav", false);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> out (outFile.createOutputStream());
        if (out == nullptr) return false;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (out.get(), sr, (std::uint32_t) chs, bits, {}, 0));
        if (writer == nullptr) { out.reset(); outFile.deleteFile(); return false; }
        out.release();   // ownership -> writer

        // Stream the source tail-first in bounded chunks: read the chunk ending
        // at `remaining`, reverse each channel, append it. This reverses the
        // whole region without ever holding it all in RAM, and a failed read or
        // write aborts + discards the partial file instead of committing garbage.
        constexpr int kChunk = 1 << 16;   // 64k frames per chunk
        juce::AudioBuffer<float> chunk (chs, kChunk);
        std::int64_t remaining = len;
        bool ioOk = true;
        while (remaining > 0)
        {
            const int n = (int) std::min ((std::int64_t) kChunk, remaining);
            chunk.clear();
            if (! rdr->read (&chunk, 0, n,
                             beforeState.sourceOffset + (remaining - n), true, chs > 1))
            { ioOk = false; break; }
            for (int c = 0; c < chs; ++c)
                std::reverse (chunk.getWritePointer (c), chunk.getWritePointer (c) + n);
            if (! writer->writeFromAudioSampleBuffer (chunk, 0, n))
            { ioOk = false; break; }
            remaining -= n;
        }
        writer.reset();   // close the file before any delete
        if (! ioOk) { outFile.deleteFile(); return false; }
        renderedFile = outFile;

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
