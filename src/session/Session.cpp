#include "Session.h"
#include "TrackMove.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace duskstudio
{
namespace
{
constexpr auto kRelaxed = std::memory_order_relaxed;

// Every field a track carries, in the order a landing has to store them in: the
// frequencies and voicing before the dial words that pair with them, a frozen
// region before the flag that gates it, the lanes before the mode that plays
// them. Staging, refreshing and landing a moved track all walk this one list.
template <typename Src, typename Fields>
void eachTrackField (Track& dst, Src& src, Fields& f)
{
    f.value (dst.name, src.name);
    f.value (dst.colour, src.colour);

    auto& d = dst.strip;
    auto& s = src.strip;
    f.atomic (d.faderDb, s.faderDb);
    f.atomic (d.pan, s.pan);
    f.atomic (d.mute, s.mute);
    f.atomic (d.solo, s.solo);
    f.atomic (d.phaseInvert, s.phaseInvert);
    f.atomic (d.insertBypassed, s.insertBypassed);
    f.atomic (d.faderGroupId, s.faderGroupId);
    for (size_t i = 0; i < d.busAssign.size(); ++i)
        f.atomic (d.busAssign[i], s.busAssign[i]);
    f.atomic (d.auxSendsBypassed, s.auxSendsBypassed);
    for (size_t i = 0; i < d.auxSendDb.size(); ++i)
    {
        f.atomic (d.auxSendDb[i], s.auxSendDb[i]);
        f.atomic (d.auxSendPreFader[i], s.auxSendPreFader[i]);
    }

    f.atomic (d.hpfEnabled, s.hpfEnabled);
    f.atomic (d.hpfFreq, s.hpfFreq);
    f.atomic (d.lpfEnabled, s.lpfEnabled);
    f.atomic (d.lpfFreq, s.lpfFreq);
    f.atomic (d.lfGainDb, s.lfGainDb);
    f.atomic (d.lfFreq, s.lfFreq);
    f.atomic (d.lmGainDb, s.lmGainDb);
    f.atomic (d.lmFreq, s.lmFreq);
    f.atomic (d.lmQ, s.lmQ);
    f.atomic (d.hmGainDb, s.hmGainDb);
    f.atomic (d.hmFreq, s.hmFreq);
    f.atomic (d.hmQ, s.hmQ);
    f.atomic (d.hfGainDb, s.hfGainDb);
    f.atomic (d.hfFreq, s.hfFreq);
    f.atomic (d.eqBlackMode, s.eqBlackMode);
    for (size_t i = 0; i < d.eqFreqDial.size(); ++i)
        f.dial (d.eqFreqDial[i], s.eqFreqDial[i]);
    f.atomic (d.eqEnabled, s.eqEnabled, std::memory_order_release);

    f.atomic (d.compEnabled, s.compEnabled);
    f.atomic (d.compMode, s.compMode);
    f.atomic (d.compModePicked, s.compModePicked);
    f.atomic (d.compOptoPeakRed, s.compOptoPeakRed);
    f.atomic (d.compOptoGain, s.compOptoGain);
    f.atomic (d.compOptoLimit, s.compOptoLimit);
    f.atomic (d.compFetInput, s.compFetInput);
    f.atomic (d.compFetOutput, s.compFetOutput);
    f.atomic (d.compFetAttack, s.compFetAttack);
    f.atomic (d.compFetRelease, s.compFetRelease);
    f.atomic (d.compFetRatio, s.compFetRatio);
    f.atomic (d.compFetThresholdDb, s.compFetThresholdDb);
    f.atomic (d.compVcaThreshDb, s.compVcaThreshDb);
    f.atomic (d.compVcaRatio, s.compVcaRatio);
    f.atomic (d.compVcaAttack, s.compVcaAttack);
    f.atomic (d.compVcaRelease, s.compVcaRelease);
    f.atomic (d.compVcaOutput, s.compVcaOutput);
    f.atomic (d.compVcaOverEasy, s.compVcaOverEasy);
    f.atomic (d.compVcaDetectorClassic, s.compVcaDetectorClassic);

    f.atomic (d.liveFaderDb, s.liveFaderDb);
    f.atomic (d.livePan, s.livePan);
    for (size_t i = 0; i < d.liveAuxSendDb.size(); ++i)
        f.atomic (d.liveAuxSendDb[i], s.liveAuxSendDb[i]);
    f.atomic (d.liveMute, s.liveMute);
    f.atomic (d.liveSolo, s.liveSolo);
    f.gesture (d.faderTouched);
    f.gesture (d.panTouched);
    for (auto& touched : d.auxSendTouched)
        f.gesture (touched);

    auto& dh = dst.hardwareInsert;
    auto& sh = src.hardwareInsert;
    f.atomic (dh.enabled, sh.enabled);
    f.routing (dh.routing, sh.routing);
    f.atomic (dh.outputGainDb, sh.outputGainDb);
    f.atomic (dh.inputGainDb, sh.inputGainDb);
    f.atomic (dh.dryWet, sh.dryWet);
    f.atomic (dh.pingResult, sh.pingResult);
    f.atomic (dh.pingPending, sh.pingPending);

    f.atomic (dst.mode, src.mode);
    f.atomic (dst.recordArmed, src.recordArmed);
    f.atomic (dst.inputMonitor, src.inputMonitor);
    f.atomic (dst.printEffects, src.printEffects);
    f.atomic (dst.inputSource, src.inputSource);
    f.atomic (dst.inputSourceR, src.inputSourceR);
    f.atomic (dst.midiInputIndex, src.midiInputIndex);
    f.value (dst.midiInputIdentifier, src.midiInputIdentifier);
    f.atomic (dst.midiOutputIndex, src.midiOutputIndex);
    f.value (dst.midiOutputIdentifier, src.midiOutputIdentifier);
    f.atomic (dst.midiChannel, src.midiChannel);
    f.gesture (dst.midiActivity);

    f.value (dst.regions, src.regions);
    f.value (dst.takes, src.takes);
    f.midiRegions (dst.midiRegions, src.midiRegions);

    f.value (dst.pluginDescriptor, src.pluginDescriptor);
    f.value (dst.pluginLegacyDescriptionXml, src.pluginLegacyDescriptionXml);
    f.value (dst.pluginStateBase64, src.pluginStateBase64);
    f.value (dst.nativeClapPath, src.nativeClapPath);
    f.value (dst.nativeClapPluginId, src.nativeClapPluginId);
    f.value (dst.nativeClapStateBase64, src.nativeClapStateBase64);
    f.value (dst.nativeLv2Path, src.nativeLv2Path);
    f.value (dst.nativeLv2PluginId, src.nativeLv2PluginId);
    f.value (dst.nativeLv2StateBase64, src.nativeLv2StateBase64);
    f.value (dst.lv2StateTag, src.lv2StateTag);
    f.value (dst.nativeVst3Path, src.nativeVst3Path);
    f.value (dst.nativeVst3PluginId, src.nativeVst3PluginId);
    f.value (dst.nativeVst3StateBase64, src.nativeVst3StateBase64);
    f.value (dst.nativeAuIdentifier, src.nativeAuIdentifier);
    f.value (dst.nativeAuStateBase64, src.nativeAuStateBase64);
    f.value (dst.nativeMultisamplePath, src.nativeMultisamplePath);
    f.value (dst.nativeMultisampleStateBase64, src.nativeMultisampleStateBase64);
    f.value (dst.builtinUnitId, src.builtinUnitId);
    f.value (dst.builtinStateBase64, src.builtinStateBase64);

    f.value (dst.frozenAudioPath, src.frozenAudioPath);
    f.value (dst.frozenRegion, src.frozenRegion);
    f.atomic (dst.frozenPluginBypass, src.frozenPluginBypass);
    f.atomic (dst.frozen, src.frozen, std::memory_order_release);

    f.atomic (dst.meterGrDb, src.meterGrDb);
    f.atomic (dst.meterInputDb, src.meterInputDb);
    f.atomic (dst.meterInputRDb, src.meterInputRDb);
    f.atomic (dst.meterOutLDb, src.meterOutLDb);
    f.atomic (dst.meterOutRDb, src.meterOutRDb);

    for (size_t p = 0; p < dst.automationLanes.size(); ++p)
    {
        f.lane (p, dst.automationLanes[p], src.automationLanes[p]);
        f.atomic (dst.automationLanes[p].passOpen, src.automationLanes[p].passOpen,
                  std::memory_order_release);
    }
    f.atomic (dst.automationMode, src.automationMode, std::memory_order_release);
}

// Live track -> stage: everything copied, each snapshot's value into a fresh
// one ready to publish.
struct CopyOut
{
    StagedTrack& stage;

    template <typename T> void value (T& to, const T& from) { to = from; }
    template <typename T>
    void atomic (std::atomic<T>& to, const std::atomic<T>& from,
                 std::memory_order = kRelaxed) noexcept { to.store (from.load (kRelaxed), kRelaxed); }
    void dial (LegacyEqDial& to, const LegacyEqDial& from) noexcept { to.setRaw (from.raw()); }
    template <typename T> void gesture (std::atomic<T>&) noexcept {}
    void routing (AtomicSnapshot<HardwareInsertRouting>&, const AtomicSnapshot<HardwareInsertRouting>& from)
    {
        stage.routing = std::make_unique<HardwareInsertRouting> (from.current());
    }
    void midiRegions (AtomicSnapshot<std::vector<MidiRegion>>&,
                      const AtomicSnapshot<std::vector<MidiRegion>>& from)
    {
        stage.midiRegions = std::make_unique<std::vector<MidiRegion>> (from.current());
    }
    void lane (size_t p, AutomationLane&, const AutomationLane& from)
    {
        stage.lanes[p] = std::make_unique<std::vector<AutomationPoint>> (from.pointsConst());
    }
};

// Live track -> stage, the atomics alone: whatever the audio thread or a
// control surface changed after the stage was built.
struct RefreshAtomics
{
    template <typename T> void value (T&, const T&) noexcept {}
    template <typename T>
    void atomic (std::atomic<T>& to, const std::atomic<T>& from,
                 std::memory_order = kRelaxed) noexcept { to.store (from.load (kRelaxed), kRelaxed); }
    void dial (LegacyEqDial& to, const LegacyEqDial& from) noexcept { to.setRaw (from.raw()); }
    template <typename T> void gesture (std::atomic<T>&) noexcept {}
    template <typename Snapshot> void routing (Snapshot&, const Snapshot&) noexcept {}
    template <typename Snapshot> void midiRegions (Snapshot&, const Snapshot&) noexcept {}
    void lane (size_t, AutomationLane&, const AutomationLane&) noexcept {}
};

// Stage -> live track: values swapped in, so what they replace is freed with the
// stage rather than here; atomics stored; the staged snapshots published.
struct Land
{
    StagedTrack& stage;

    template <typename T> void value (T& to, T& from) { using std::swap; swap (to, from); }
    template <typename T>
    void atomic (std::atomic<T>& to, const std::atomic<T>& from,
                 std::memory_order order = kRelaxed) noexcept { to.store (from.load (kRelaxed), order); }
    void dial (LegacyEqDial& to, const LegacyEqDial& from) noexcept { to.setRaw (from.raw()); }
    template <typename T> void gesture (std::atomic<T>& to) noexcept { to.store (T {}, kRelaxed); }
    void routing (AtomicSnapshot<HardwareInsertRouting>& to, AtomicSnapshot<HardwareInsertRouting>&) noexcept
    {
        to.publish (std::move (stage.routing));
    }
    void midiRegions (AtomicSnapshot<std::vector<MidiRegion>>& to,
                      AtomicSnapshot<std::vector<MidiRegion>>&) noexcept
    {
        to.publish (std::move (stage.midiRegions));
    }
    void lane (size_t p, AutomationLane& to, AutomationLane&) noexcept
    {
        to.snapshot.publish (std::move (stage.lanes[p]));
    }
};
} // namespace

Session::Session()
{
    for (int i = 0; i < kNumTracks; ++i)
    {
        tracks[(size_t) i].name = juce::String (i + 1);
        tracks[(size_t) i].colour = juce::Colour::fromHSV (i / (float) kNumTracks, 0.45f, 0.75f, 1.0f);
    }

    static const char* defaultBusNames[] = { "BUS 1", "BUS 2", "BUS 3", "BUS 4" };
    for (int i = 0; i < kNumBuses; ++i)
    {
        buses[(size_t) i].name = defaultBusNames[i];
        buses[(size_t) i].colour = juce::Colour::fromHSV (0.55f + i * 0.07f, 0.35f, 0.72f, 1.0f);
    }

    static const char* defaultLaneNames[] = { "AUX 1", "AUX 2", "AUX 3", "AUX 4" };
    for (int i = 0; i < kNumAuxLanes; ++i)
    {
        auxLanes[(size_t) i].name = defaultLaneNames[i];
        // Different hue band so the AUX UI reads differently from the bus
        // strips and the track palette.
        auxLanes[(size_t) i].colour = juce::Colour::fromHSV (0.78f + i * 0.05f, 0.40f, 0.78f, 1.0f);
    }
}

bool Session::anyTrackSoloed() const noexcept
{
    // Scan liveSolo so automated solos count toward the global "any
    // soloed?" check. liveSolo is written by AudioEngine's per-track
    // routing block at the top of every callback - a Read-mode lane
    // overriding manual solo to true is reflected in this scan even
    // though `setTrackSoloed`'s counter wouldn't capture it. 16
    // relaxed atomic loads is in the noise compared to the rest of
    // the per-block work.
    for (auto& t : tracks)
        if (t.strip.liveSolo.load (std::memory_order_relaxed))
            return true;
    return false;
}

bool Session::anyBusSoloed() const noexcept
{
    return soloBusCount.load (std::memory_order_relaxed) > 0;
}

bool Session::anyTrackArmed() const noexcept
{
    return armedTrackCount.load (std::memory_order_relaxed) > 0;
}

void Session::setTrackSoloed (int trackIndex, bool soloed) noexcept
{
    if (trackIndex < 0 || trackIndex >= kNumTracks) return;
    auto& a = tracks[(size_t) trackIndex].strip.solo;
    const bool prev = a.exchange (soloed, std::memory_order_relaxed);
    if (prev != soloed)
        soloTrackCount.fetch_add (soloed ? 1 : -1, std::memory_order_relaxed);
}

void Session::setBusSoloed (int busIndex, bool soloed) noexcept
{
    if (busIndex < 0 || busIndex >= kNumBuses) return;
    auto& a = buses[(size_t) busIndex].strip.solo;
    const bool prev = a.exchange (soloed, std::memory_order_relaxed);
    if (prev != soloed)
        soloBusCount.fetch_add (soloed ? 1 : -1, std::memory_order_relaxed);
}

// Called from the control-surface paths (MCU MIDI thread, MIDI-binding apply),
// never the UI drag (that path has its own gesture-anchored propagation). Every
// access is a relaxed atomic, so concurrent callers are data-race-free; the only
// hazard is a transient mis-tracked delta if two surfaces ride the same group at
// the exact same instant, which self-corrects on the next move. No mutex on
// purpose: the binding-apply caller can run on the audio thread, where locking
// is forbidden.
void Session::setTrackFaderGrouped (int ti, float newDb) noexcept
{
    if (ti < 0 || ti >= kNumTracks) return;
    auto& strip = tracks[(size_t) ti].strip;
    const float clampedNew = std::clamp (newDb, ChannelStripParams::kFaderMinDb,
                                          ChannelStripParams::kFaderMaxDb);

    const int gid = strip.faderGroupId.load (std::memory_order_relaxed);
    if (gid == 0)
    {
        strip.faderDb.store (clampedNew, std::memory_order_relaxed);
        return;
    }

    // Delta is computed against this fader's own previous value, so the move
    // is incremental - unlike the UI drag path there's no gesture anchor to
    // diff against. Peers shift by the same dB; a peer pinned at a rail just
    // clamps (matches hardware group behaviour, may break the offset there).
    const float delta = clampedNew - strip.faderDb.load (std::memory_order_relaxed);
    strip.faderDb.store (clampedNew, std::memory_order_relaxed);
    if (delta == 0.0f) return;

    for (int t = 0; t < kNumTracks; ++t)
    {
        if (t == ti) continue;
        auto& peer = tracks[(size_t) t].strip;
        if (peer.faderGroupId.load (std::memory_order_relaxed) != gid) continue;
        const float pv = peer.faderDb.load (std::memory_order_relaxed);
        peer.faderDb.store (std::clamp (pv + delta, ChannelStripParams::kFaderMinDb,
                                        ChannelStripParams::kFaderMaxDb),
                            std::memory_order_relaxed);
    }
}

void Session::setTrackArmed (int trackIndex, bool armed) noexcept
{
    if (trackIndex < 0 || trackIndex >= kNumTracks) return;
    // A frozen track can't record (playback is the baked WAV). Refuse arming it
    // from ANY path - the strip button, the selected-track shortcut, MCU, MIDI
    // bindings all route through here. (The strip button still shows its own
    // alert + toggle rollback for UI feedback; this is the shared backstop.)
    if (armed && tracks[(size_t) trackIndex].frozen.load (std::memory_order_relaxed))
        return;
    // Nothing to capture from: arming would light ARM over a recording that
    // writes no file and reports nothing.
    if (armed && missingInputForTrack (trackIndex) != kInputAvailable)
        return;
    auto& a = tracks[(size_t) trackIndex].recordArmed;
    const bool prev = a.exchange (armed, std::memory_order_relaxed);
    if (prev != armed)
        armedTrackCount.fetch_add (armed ? 1 : -1, std::memory_order_relaxed);
}

int Session::disarmAudioTracksWithoutInput() noexcept
{
    int disarmed = 0;
    for (int i = 0; i < kNumTracks; ++i)
    {
        auto& t = tracks[(size_t) i];
        if (! t.recordArmed.load (std::memory_order_relaxed)) continue;
        if (missingInputForTrack (i) == kInputAvailable) continue;
        setTrackArmed (i, false);
        ++disarmed;
    }
    return disarmed;
}

int Session::missingInputForTrack (int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kNumTracks) return kInputAvailable;
    if (tracks[(size_t) trackIndex].mode.load (std::memory_order_relaxed)
            == (int) Track::Mode::Midi)
        return kInputAvailable;
    const int width = deviceCaptureChannels.load (std::memory_order_relaxed);
    if (width == kCaptureWidthUnknown) return kInputAvailable;

    const int left = resolveInputForTrack (trackIndex);
    if (left < 0) return kNoInputSelected;
    if (left >= width) return left;

    if (tracks[(size_t) trackIndex].mode.load (std::memory_order_relaxed)
            == (int) Track::Mode::Stereo)
    {
        const int right = resolveInputRForTrack (trackIndex);
        if (right < 0) return kNoInputSelected;
        if (right >= width) return right;
    }
    return kInputAvailable;
}

void Session::recomputeRtCounters() noexcept
{
    int s = 0, a = 0, ar = 0;
    for (auto& t : tracks)
    {
        if (t.strip.solo.load (std::memory_order_relaxed)) ++s;
        if (t.recordArmed.load (std::memory_order_relaxed)) ++ar;
    }
    for (auto& b : buses)
        if (b.strip.solo.load (std::memory_order_relaxed)) ++a;

    soloTrackCount .store (s,  std::memory_order_relaxed);
    soloBusCount   .store (a,  std::memory_order_relaxed);
    armedTrackCount.store (ar, std::memory_order_relaxed);
}

int Session::resolveInputForTrack (int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kNumTracks) return -1;
    const auto& t = tracks[(size_t) trackIndex];
    // MIDI-mode tracks have no audio input - their source is the MIDI
    // device routed via midiInputIndex into the strip's instrument
    // plugin. Returning -1 here keeps the audio-thread path that pulls
    // device-input audio (and the input meter that reports it) silent
    // for instrument tracks instead of pointlessly metering whatever
    // audio channel happens to share the track index.
    if (t.mode.load (std::memory_order_relaxed) == (int) Track::Mode::Midi)
        return -1;
    const int src = t.inputSource.load (std::memory_order_relaxed);
    if (src == -2) return trackIndex;  // follow track index
    return src;                         // -1 = none, 0..N = explicit input
}

int Session::resolveInputRForTrack (int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kNumTracks) return -1;
    const auto& t = tracks[(size_t) trackIndex];
    // R channel only valid in stereo mode.
    if (t.mode.load (std::memory_order_relaxed) != (int) Track::Mode::Stereo)
        return -1;
    const int rSrc = t.inputSourceR.load (std::memory_order_relaxed);
    if (rSrc == -2)
    {
        // "Follow" semantics for R - paired adjacent to the L source.
        const int lSrc = t.inputSource.load (std::memory_order_relaxed);
        const int lResolved = (lSrc == -2) ? trackIndex : lSrc;
        return (lResolved >= 0) ? lResolved + 1 : -1;
    }
    return rSrc;                        // -1 = none, 0..N = explicit input
}

std::optional<Session::StagedTrackMove> Session::stageTrackMove (const TrackMovePlan& requested,
                                                                const TrackSlotMask& refollow) const
{
    const auto plan = trackMoveFromNewToOld (requested.newToOld);
    if (! plan) return std::nullopt;

    StagedTrackMove staged { *plan, refollow, {} };
    for (int to = 0; to < kNumTracks; ++to)
    {
        const int from = plan->newToOld[(size_t) to];
        if (from == to) continue;
        auto stage = std::make_unique<StagedTrack>();
        CopyOut copy { *stage };
        eachTrackField (stage->fields, tracks[(size_t) from], copy);
        auto& tag = stage->fields.lv2StateTag;
        if (tag.empty())
            tag = defaultLv2StateTag (from);
        if (tag == defaultLv2StateTag (to))
            tag.clear();
        staged.into[(size_t) to] = std::move (stage);
    }
    return staged;
}

void Session::landTrackMove (StagedTrackMove& staged)
{
    // Every stage takes its source's atomics before any track is written, as
    // the sources are the tracks about to be overwritten.
    RefreshAtomics refresh;
    for (int to = 0; to < kNumTracks; ++to)
    {
        auto* stage = staged.into[(size_t) to].get();
        if (stage == nullptr) continue;
        const int from = staged.plan.newToOld[(size_t) to];
        const Track& source = tracks[(size_t) from];
        eachTrackField (stage->fields, source, refresh);

        auto& input = stage->fields.inputSource;
        int landed = followInputAfterMove (input.load (kRelaxed), from);
        if (staged.refollow[(size_t) to] && landed == to)
            landed = kInputFollowsTrack;
        input.store (landed, kRelaxed);
    }

    for (int to = 0; to < kNumTracks; ++to)
        if (auto* stage = staged.into[(size_t) to].get())
        {
            Land land { *stage };
            eachTrackField (tracks[(size_t) to], stage->fields, land);
        }

    if (auto& audition = takeAudition; audition.trackIdx >= 0 && audition.trackIdx < kNumTracks)
        audition.trackIdx = staged.plan.oldToNew[(size_t) audition.trackIdx];
}

bool Session::permuteTracks (const TrackMovePlan& plan, const TrackSlotMask& refollow)
{
    auto staged = stageTrackMove (plan, refollow);
    if (! staged) return false;
    landTrackMove (*staged);
    return true;
}

std::string Session::defaultLv2StateTag (int trackIndex)
{
    const int number = trackIndex + 1;
    return std::string (number < 10 ? "track0" : "track") + std::to_string (number);
}

std::string Session::lv2StateTagFor (int trackIndex) const
{
    const auto& kept = track (trackIndex).lv2StateTag;
    return kept.empty() ? defaultLv2StateTag (trackIndex) : kept;
}

bool Session::isLv2StateTag (const std::string& tag) noexcept
{
    const auto digit = [] (char c) { return c >= '0' && c <= '9'; };
    return tag.size() == 7 && tag.compare (0, 5, "track") == 0 && digit (tag[5]) && digit (tag[6]);
}

void Session::repairLv2StateTags()
{
    for (int i = 0; i < kNumTracks; ++i)
    {
        auto& tag = tracks[(size_t) i].lv2StateTag;
        if (! tag.empty() && (! isLv2StateTag (tag) || tag == defaultLv2StateTag (i)))
            tag.clear();
    }

    // A kept tag that clashes falls back to its slot's own, which can clash
    // with another kept tag in turn; each pass drops at least one, so this
    // settles within kNumTracks passes.
    for (bool dropped = true; dropped;)
    {
        dropped = false;
        std::map<std::string, int> uses;
        for (int i = 0; i < kNumTracks; ++i)
            ++uses[lv2StateTagFor (i)];
        for (auto& t : tracks)
            if (! t.lv2StateTag.empty() && uses[t.lv2StateTag] > 1)
            {
                t.lv2StateTag.clear();
                dropped = true;
            }
    }
}

void Session::setSessionDirectory (const juce::File& dir)
{
    sessionDir = dir;
    if (! sessionDir.exists())
        sessionDir.createDirectory();
    auto audioDir = getAudioDirectory();
    if (! audioDir.exists())
        audioDir.createDirectory();
}

int Session::addMarker (std::int64_t timelineSamples, const juce::String& name)
{
    Marker m;
    m.timelineSamples = std::max ((std::int64_t) 0, timelineSamples);
    m.name = name.isNotEmpty()
                ? name
                : juce::String ("Marker ") + juce::String ((int) markers.size() + 1);
    // Soft amber - reads cleanly against the dark ruler band and doesn't
    // collide with the green/blue/orange palette already used for tracks
    // and loop/punch brackets.
    m.colour = juce::Colour (0xffe0a050);

    auto it = std::lower_bound (markers.begin(), markers.end(), m.timelineSamples,
        [] (const Marker& lhs, std::int64_t t) { return lhs.timelineSamples < t; });
    const int insertedIdx = (int) (it - markers.begin());
    markers.insert (it, std::move (m));
    return insertedIdx;
}

void Session::removeMarker (int index)
{
    if (index < 0 || index >= (int) markers.size()) return;
    markers.erase (markers.begin() + index);
}

void Session::renameMarker (int index, const juce::String& name)
{
    if (index < 0 || index >= (int) markers.size()) return;
    markers[(size_t) index].name = name;
}

int Session::findMarkerNear (std::int64_t timelineSamples,
                              std::int64_t toleranceSamples) const noexcept
{
    int closest = -1;
    std::int64_t closestDist = toleranceSamples;
    for (int i = 0; i < (int) markers.size(); ++i)
    {
        const auto dist = std::abs (markers[(size_t) i].timelineSamples - timelineSamples);
        if (dist <= closestDist)
        {
            closestDist = dist;
            closest = i;
        }
    }
    return closest;
}

// Per-param normalize / denormalize. value lives in 0..1 in the lane so the
// JSON schema and thinning constants stay range-independent.
// Exposed via Session.h so the editor UI can convert between displayed
// (dB / pan / 0|1) and stored (0..1) values when drawing automation.
float denormalizeAutomationValue (AutomationParam p, float v) noexcept
{
    v = std::clamp (v, 0.0f, 1.0f);
    switch (p)
    {
        case AutomationParam::FaderDb:
            return ChannelStripParams::kFaderMinDb
                 + v * (ChannelStripParams::kFaderMaxDb - ChannelStripParams::kFaderMinDb);

        case AutomationParam::Pan:
            return v * 2.0f - 1.0f;

        case AutomationParam::Mute:
        case AutomationParam::Solo:
            return v >= 0.5f ? 1.0f : 0.0f;

        case AutomationParam::AuxSend1:
        case AutomationParam::AuxSend2:
        case AutomationParam::AuxSend3:
        case AutomationParam::AuxSend4:
            // Below the bottom of the visible range we snap to the off
            // sentinel so the audio thread can short-circuit silent sends.
            if (v <= 0.0f)
                return ChannelStripParams::kAuxSendOffDb;
            return ChannelStripParams::kAuxSendMinDb
                 + v * (ChannelStripParams::kAuxSendMaxDb - ChannelStripParams::kAuxSendMinDb);

        case AutomationParam::kCount:
            break;
    }
    return 0.0f;
}

float normalizeAutomationValue (AutomationParam p, float denormValue) noexcept
{
    switch (p)
    {
        case AutomationParam::FaderDb:
        {
            const float lo = ChannelStripParams::kFaderMinDb;
            const float hi = ChannelStripParams::kFaderMaxDb;
            return std::clamp ((denormValue - lo) / (hi - lo), 0.0f, 1.0f);
        }
        case AutomationParam::Pan:
            return std::clamp ((denormValue + 1.0f) * 0.5f, 0.0f, 1.0f);

        case AutomationParam::Mute:
        case AutomationParam::Solo:
            return denormValue >= 0.5f ? 1.0f : 0.0f;

        case AutomationParam::AuxSend1:
        case AutomationParam::AuxSend2:
        case AutomationParam::AuxSend3:
        case AutomationParam::AuxSend4:
        {
            if (denormValue <= ChannelStripParams::kAuxSendOffDb + 0.1f) return 0.0f;
            const float lo = ChannelStripParams::kAuxSendMinDb;
            const float hi = ChannelStripParams::kAuxSendMaxDb;
            return std::clamp ((denormValue - lo) / (hi - lo), 0.0f, 1.0f);
        }

        case AutomationParam::kCount:
            break;
    }
    return 0.0f;
}

namespace { float denormalizeAutomation (AutomationParam p, float v) noexcept
{
    return denormalizeAutomationValue (p, v);
} } // namespace

float evaluateLane (const std::vector<AutomationPoint>& pts, std::int64_t t,
                    AutomationParam param) noexcept
{
    if (pts.empty()) return 0.0f;

    // Hold-first below the lane, hold-last above it.
    if (t <= pts.front().timeSamples)
        return denormalizeAutomation (param, pts.front().value);
    if (t >= pts.back().timeSamples)
        return denormalizeAutomation (param, pts.back().value);

    // Binary search for the bracket [lo, hi] s.t. lo.t <= t < hi.t. Lane is
    // sorted ascending by timeSamples (invariant maintained by the writer
    // and by SessionSerializer::load); std::lower_bound gives the first
    // point with timeSamples >= t, then back up by one for the lower side.
    auto it = std::lower_bound (pts.begin(), pts.end(), t,
        [] (const AutomationPoint& pt, std::int64_t q) { return pt.timeSamples < q; });
    if (it == pts.begin())
        return denormalizeAutomation (param, pts.front().value);
    const auto& hi = *it;
    const auto& lo = *(it - 1);

    if (! isContinuousParam (param))
        return denormalizeAutomation (param, lo.value);   // hold-previous for discrete

    const auto span = hi.timeSamples - lo.timeSamples;
    if (span <= 0)
        return denormalizeAutomation (param, hi.value);
    const float frac = (float) ((double) (t - lo.timeSamples) / (double) span);
    const float v = lo.value + frac * (hi.value - lo.value);
    return denormalizeAutomation (param, v);
}

// Perpendicular distance from `p` to the chord `[a, b]` in (time, value)
// space, with time normalized to value's range so dense-in-time bursts
// don't dominate the metric. Used by thinAutomationLane.
namespace
{
double perpendicularDistance (const AutomationPoint& p,
                                const AutomationPoint& a,
                                const AutomationPoint& b) noexcept
{
    const double dt = (double) (b.timeSamples - a.timeSamples);
    if (dt <= 0.0) return std::abs ((double) p.value - (double) a.value);
    // Linear interpolant on the chord at p.timeSamples.
    const double frac = (double) (p.timeSamples - a.timeSamples) / dt;
    const double interp = (double) a.value + frac * ((double) b.value - (double) a.value);
    return std::abs ((double) p.value - interp);
}

// Ramer-Douglas-Peucker, iterative on a stack so we don't recurse into
// stack overflow on a million-point lane.
void rdp (const std::vector<AutomationPoint>& in,
          std::size_t lo, std::size_t hi,
          double epsilon,
          std::vector<char>& keep)
{
    std::vector<std::pair<std::size_t, std::size_t>> stack;
    stack.reserve (32);
    stack.emplace_back (lo, hi);
    while (! stack.empty())
    {
        const auto [a, b] = stack.back();
        stack.pop_back();
        if (b <= a + 1) continue;
        double worst = 0.0;
        std::size_t worstIdx = a;
        for (std::size_t i = a + 1; i < b; ++i)
        {
            const double d = perpendicularDistance (in[i], in[a], in[b]);
            if (d > worst) { worst = d; worstIdx = i; }
        }
        if (worst > epsilon)
        {
            keep[worstIdx] = 1;
            stack.emplace_back (a, worstIdx);
            stack.emplace_back (worstIdx, b);
        }
    }
}
} // namespace

void thinAutomationLane (std::vector<AutomationPoint>& points,
                            AutomationParam param,
                            double epsilon) noexcept
{
    // Discrete params (mute / solo) are bit-exact - RDP'd values would
    // round wrong and silently lose state transitions. Skip thinning;
    // a recorded pass already keeps only the changes.
    if (! isContinuousParam (param)) return;
    if (points.size() <= 2) return;

    // Negative epsilon would make `worst > epsilon` always true (since
    // perpendicularDistance returns |...| ≥ 0), so every interior point
    // would be marked keep - defeating the thin entirely. Clamp for
    // safety; the caller should already pass a non-negative value.
    epsilon = std::max (epsilon, 0.0);

    std::vector<char> keep (points.size(), 0);
    keep.front() = 1;
    keep.back()  = 1;
    rdp (points, 0, points.size() - 1, epsilon, keep);

    std::vector<AutomationPoint> thinned;
    thinned.reserve (points.size());
    for (std::size_t i = 0; i < points.size(); ++i)
        if (keep[i]) thinned.push_back (points[i]);

    points = std::move (thinned);
}

void handleWritePassComplete (Session& s) noexcept
{
    // Normalized-space epsilon: 0.002 = 0.2% of the 0..1 lane storage
    // range. Audibly inaudible (~0.024 dB on a 12-to-(-100) dB fader)
    // and aggressive enough to drop ~90% of timer-tick duplicates on a
    // typical "ride" gesture.
    constexpr double kEpsilon = 0.002;

    // Thin via mutatePoints (copy -> thin -> atomic publish) so a stray reader
    // never sees the reshape mid-flight. Skip lanes that can't change (discrete,
    // or ≤ 2 points) to avoid a pointless republish of an identical vector.
    const auto thinLane = [&] (AutomationLane& lane, AutomationParam p)
    {
        if (! isContinuousParam (p) || lane.pointsConst().size() <= 2) return;
        lane.mutatePoints ([p, kEpsilon] (std::vector<AutomationPoint>& v)
                            { thinAutomationLane (v, p, kEpsilon); });
    };

    for (int t = 0; t < Session::kNumTracks; ++t)
        for (int p = 0; p < kNumAutomationParams; ++p)
            thinLane (s.track (t).automationLanes[(size_t) p], (AutomationParam) p);

    for (int a = 0; a < Session::kNumAuxLanes; ++a)
        for (int p = 0; p < kNumAutomationParams; ++p)
            thinLane (s.auxLane (a).params.automationLanes[(size_t) p], (AutomationParam) p);

    for (int p = 0; p < kNumAutomationParams; ++p)
        thinLane (s.master().automationLanes[(size_t) p], (AutomationParam) p);
}

void applyTempoChange (Session& s, float newBpm, double sampleRate) noexcept
{
    const float oldBpm = s.tempoBpm.load (std::memory_order_relaxed);

    // Clamp before storing so an external caller can't push 0 / NaN /
    // negative tempos into the audio thread's tick->sample math. Same
    // limits the TransportBar's spinner and tap-tempo enforce. Note:
    // std::clamp returns NaN when its input is NaN (the comparisons
    // it relies on are both false for NaN), so an explicit isfinite
    // check has to run first.
    if (! std::isfinite (newBpm)) newBpm = 120.0f;
    newBpm = std::clamp (newBpm, 30.0f, 300.0f);

    if (sampleRate > 0.0 && oldBpm > 0.0f && newBpm > 0.0f
        && std::abs (oldBpm - newBpm) > 1e-4f)
    {
        const double oldB = (double) oldBpm;
        const double newB = (double) newBpm;
        const double factor = oldB / newB;   // positions in beats stay fixed -> scale samples

        for (int ti = 0; ti < Session::kNumTracks; ++ti)
        {
            auto& holder = s.track (ti).midiRegions;
            if (holder.current().empty()) continue;

            // mutate() copies the current snapshot, runs the lambda, and
            // republishes with release ordering. The audio thread's next
            // read() picks up the new positions atomically.
            holder.mutate ([factor, sampleRate, newBpm] (std::vector<MidiRegion>& v)
            {
                for (auto& r : v)
                {
                    if (r.tempoLock)
                    {
                        r.timelineStart = (std::int64_t) std::llround (
                            (double) r.timelineStart * factor);
                        r.lengthInSamples = ticksToSamples (r.lengthInTicks,
                                                              sampleRate, newBpm);
                    }
                    else
                    {
                        // Float regions keep sample positions; rebuild
                        // musical length so the piano-roll grid + the
                        // scheduling math stay consistent at the new
                        // tempo.
                        r.lengthInTicks = samplesToTicks (r.lengthInSamples,
                                                           sampleRate, newBpm);
                    }
                }
            });
        }
    }

    s.tempoBpm.store (newBpm, std::memory_order_release);
}
} // namespace duskstudio
