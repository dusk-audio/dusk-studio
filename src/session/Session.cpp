#include "Session.h"

namespace duskstudio
{
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
    const float clampedNew = juce::jlimit (ChannelStripParams::kFaderMinDb,
                                           ChannelStripParams::kFaderMaxDb, newDb);

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
        peer.faderDb.store (juce::jlimit (ChannelStripParams::kFaderMinDb,
                                          ChannelStripParams::kFaderMaxDb, pv + delta),
                            std::memory_order_relaxed);
    }
}

void Session::setTrackArmed (int trackIndex, bool armed) noexcept
{
    if (trackIndex < 0 || trackIndex >= kNumTracks) return;
    // A frozen track can't record (playback is the baked WAV). Refuse arming it
    // from ANY path — the strip button, the selected-track shortcut, MCU, MIDI
    // bindings all route through here. (The strip button still shows its own
    // alert + toggle rollback for UI feedback; this is the shared backstop.)
    if (armed && tracks[(size_t) trackIndex].frozen.load (std::memory_order_relaxed))
        return;
    auto& a = tracks[(size_t) trackIndex].recordArmed;
    const bool prev = a.exchange (armed, std::memory_order_relaxed);
    if (prev != armed)
        armedTrackCount.fetch_add (armed ? 1 : -1, std::memory_order_relaxed);
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

void Session::setSessionDirectory (const juce::File& dir)
{
    sessionDir = dir;
    if (! sessionDir.exists())
        sessionDir.createDirectory();
    auto audioDir = getAudioDirectory();
    if (! audioDir.exists())
        audioDir.createDirectory();
}

int Session::addMarker (juce::int64 timelineSamples, const juce::String& name)
{
    Marker m;
    m.timelineSamples = juce::jmax ((juce::int64) 0, timelineSamples);
    m.name = name.isNotEmpty()
                ? name
                : juce::String ("Marker ") + juce::String ((int) markers.size() + 1);
    // Soft amber - reads cleanly against the dark ruler band and doesn't
    // collide with the green/blue/orange palette already used for tracks
    // and loop/punch brackets.
    m.colour = juce::Colour (0xffe0a050);

    auto it = std::lower_bound (markers.begin(), markers.end(), m.timelineSamples,
        [] (const Marker& lhs, juce::int64 t) { return lhs.timelineSamples < t; });
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

int Session::findMarkerNear (juce::int64 timelineSamples,
                              juce::int64 toleranceSamples) const noexcept
{
    int closest = -1;
    juce::int64 closestDist = toleranceSamples;
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
    v = juce::jlimit (0.0f, 1.0f, v);
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
            return juce::jlimit (0.0f, 1.0f, (denormValue - lo) / (hi - lo));
        }
        case AutomationParam::Pan:
            return juce::jlimit (0.0f, 1.0f, (denormValue + 1.0f) * 0.5f);

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
            return juce::jlimit (0.0f, 1.0f, (denormValue - lo) / (hi - lo));
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

float evaluateLane (const std::vector<AutomationPoint>& pts, juce::int64 t,
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
        [] (const AutomationPoint& pt, juce::int64 q) { return pt.timeSamples < q; });
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
    // Discrete params (mute / solo) are bit-exact — RDP'd values would
    // round wrong and silently lose state transitions. Skip thinning;
    // the existing same-sample coalesce in captureWritePoint is enough.
    if (! isContinuousParam (param)) return;
    if (points.size() <= 2) return;

    // Negative epsilon would make `worst > epsilon` always true (since
    // perpendicularDistance returns |…| ≥ 0), so every interior point
    // would be marked keep — defeating the thin entirely. Clamp for
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

    // Thin via mutatePoints (copy → thin → atomic publish) so a stray reader
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
    // negative tempos into the audio thread's tick→sample math. Same
    // limits the TransportBar's spinner and tap-tempo enforce. Note:
    // juce::jlimit returns NaN when its input is NaN (the comparisons
    // it relies on are both false for NaN), so an explicit isfinite
    // check has to run first.
    if (! std::isfinite (newBpm)) newBpm = 120.0f;
    newBpm = juce::jlimit (30.0f, 300.0f, newBpm);

    if (sampleRate > 0.0 && oldBpm > 0.0f && newBpm > 0.0f
        && std::abs (oldBpm - newBpm) > 1e-4f)
    {
        const double oldB = (double) oldBpm;
        const double newB = (double) newBpm;
        const double factor = oldB / newB;   // positions in beats stay fixed → scale samples

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
                        r.timelineStart = (juce::int64) std::llround (
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
