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

void Session::setTrackArmed (int trackIndex, bool armed) noexcept
{
    if (trackIndex < 0 || trackIndex >= kNumTracks) return;
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

namespace
{
// Per-param normalize / denormalize. value lives in 0..1 in the lane so the
// JSON schema and thinning constants stay range-independent.
float denormalizeAutomation (AutomationParam p, float v) noexcept
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
} // namespace

float evaluateLane (const AutomationLane& lane, juce::int64 t,
                    AutomationParam param) noexcept
{
    const auto& pts = lane.points;
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
} // namespace duskstudio
