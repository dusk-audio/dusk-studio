#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"

using Catch::Matchers::WithinAbs;

// applyTempoChange retimes the timelineStart of tempo-locked MIDI regions
// by the oldBpm/newBpm ratio and rebuilds lengthInSamples from
// lengthInTicks at the new tempo. Float regions (tempoLock = false) keep
// their sample positions and have lengthInTicks rebuilt instead.
//
// The "musical position preserved on locked, sample position preserved
// on float" contract is the whole point of the tempoLock field, so the
// regression test pins it concretely with a 120 → 60 BPM halving.
TEST_CASE ("applyTempoChange halving BPM doubles locked region positions, leaves float regions",
           "[session][tempo]")
{
    using namespace duskstudio;

    constexpr double sr = 48000.0;
    Session s;
    s.tempoBpm.store (120.0f, std::memory_order_relaxed);

    s.track (0).midiRegions.mutate ([] (std::vector<MidiRegion>& v)
    {
        MidiRegion locked;
        locked.timelineStart   = 48000;   // 1 second at 48 kHz = 2 beats @ 120 BPM
        locked.lengthInSamples = 24000;   // 0.5 second = 1 beat @ 120 BPM
        locked.lengthInTicks   = 480;     // 1 quarter note
        locked.tempoLock       = true;
        locked.recordedAtBPM   = 120.0;
        v.push_back (std::move (locked));

        MidiRegion floating;
        floating.timelineStart   = 96000;   // 2 seconds
        floating.lengthInSamples = 12000;
        floating.lengthInTicks   = 240;
        floating.tempoLock       = false;
        floating.recordedAtBPM   = 120.0;
        v.push_back (std::move (floating));
    });

    applyTempoChange (s, 60.0f, sr);

    REQUIRE_THAT ((double) s.tempoBpm.load (std::memory_order_relaxed),
                  WithinAbs (60.0, 1e-4));

    const auto& v = s.track (0).midiRegions.current();
    REQUIRE (v.size() == 2);

    // Locked: same 2 beats now occupies 4 seconds (BPM halved → samples doubled).
    REQUIRE (v[0].timelineStart   == 96000);
    REQUIRE (v[0].lengthInSamples == 48000);  // 1 quarter at 60 BPM = 1 sec = 48 ksamples
    REQUIRE (v[0].lengthInTicks   == 480);
    REQUIRE (v[0].tempoLock       == true);

    // Float: same sample positions, lengthInTicks recomputed at new tempo
    // (12 ksamples at 60 BPM = 0.25 sec = 0.25 beats = 120 ticks).
    REQUIRE (v[1].timelineStart   == 96000);
    REQUIRE (v[1].lengthInSamples == 12000);
    REQUIRE (v[1].lengthInTicks   == 120);
    REQUIRE (v[1].tempoLock       == false);
}

// Round-trip: 120 → 96 → 120 should land back within rounding tolerance.
// Validates the ratio math doesn't drift catastrophically across
// successive changes (some sample-rounding drift is expected; this
// is a sanity guard, not a bit-exact assertion).
TEST_CASE ("applyTempoChange round-trip 120-96-120 returns near-original positions",
           "[session][tempo]")
{
    using namespace duskstudio;

    constexpr double sr = 48000.0;
    Session s;
    s.tempoBpm.store (120.0f, std::memory_order_relaxed);

    s.track (3).midiRegions.mutate ([] (std::vector<MidiRegion>& v)
    {
        MidiRegion r;
        r.timelineStart   = 240000;
        r.lengthInSamples = 96000;
        r.lengthInTicks   = 1920;
        r.tempoLock       = true;
        r.recordedAtBPM   = 120.0;
        v.push_back (std::move (r));
    });

    applyTempoChange (s, 96.0f, sr);
    applyTempoChange (s, 120.0f, sr);

    const auto& v = s.track (3).midiRegions.current();
    REQUIRE (v.size() == 1);

    // Two halvings introduce up to ~2 samples of integer-rounding drift.
    // Anything beyond that means the inverse ratio math is wrong.
    REQUIRE (std::abs ((juce::int64) v[0].timelineStart   - (juce::int64) 240000) <= 2);
    REQUIRE (std::abs ((juce::int64) v[0].lengthInSamples - (juce::int64) 96000)  <= 2);
    REQUIRE (v[0].lengthInTicks == 1920);
}

// Below-clamp and above-clamp BPMs must not push the audio thread into
// dividing by zero or wrapping signed-int ratios. The function clamps
// to the same 30..300 window the TransportBar UI enforces.
TEST_CASE ("applyTempoChange clamps newBpm to 30..300", "[session][tempo]")
{
    using namespace duskstudio;
    Session s;
    s.tempoBpm.store (120.0f, std::memory_order_relaxed);
    applyTempoChange (s, 5.0f, 48000.0);
    REQUIRE_THAT ((double) s.tempoBpm.load (std::memory_order_relaxed),
                  WithinAbs (30.0, 1e-4));
    applyTempoChange (s, 1000.0f, 48000.0);
    REQUIRE_THAT ((double) s.tempoBpm.load (std::memory_order_relaxed),
                  WithinAbs (300.0, 1e-4));
}
