#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"

using Catch::Matchers::WithinAbs;

namespace
{
void setFaderGroup (duskstudio::Session& s, int t, int gid, float db)
{
    s.track (t).strip.faderGroupId.store (gid, std::memory_order_relaxed);
    s.track (t).strip.faderDb.store (db, std::memory_order_relaxed);
}

float fader (duskstudio::Session& s, int t)
{
    return s.track (t).strip.faderDb.load (std::memory_order_relaxed);
}
} // namespace

// setTrackFaderGrouped is the audio-thread fader-write path used by the
// control surface (MCU pitch-bend) and MIDI bindings. When the moved fader
// belongs to a group it shifts every peer by the same dB delta, preserving
// the group's relative balance - the UI drag path has its own anchored
// propagation, this covers the non-gesture writes.
TEST_CASE ("setTrackFaderGrouped shifts group peers by the same dB delta", "[session][group]")
{
    using namespace duskstudio;
    Session s;

    SECTION ("grouped move preserves relative offsets")
    {
        setFaderGroup (s, 0, 1,   0.0f);
        setFaderGroup (s, 1, 1,  -6.0f);
        setFaderGroup (s, 2, 1, -12.0f);
        setFaderGroup (s, 5, 0,  -3.0f);   // ungrouped bystander

        s.setTrackFaderGrouped (0, 3.0f);   // +3 dB delta

        REQUIRE_THAT (fader (s, 0), WithinAbs ( 3.0f, 1e-4));
        REQUIRE_THAT (fader (s, 1), WithinAbs (-3.0f, 1e-4));
        REQUIRE_THAT (fader (s, 2), WithinAbs (-9.0f, 1e-4));
        REQUIRE_THAT (fader (s, 5), WithinAbs (-3.0f, 1e-4));   // untouched
    }

    SECTION ("moving a non-master member drives the whole group")
    {
        setFaderGroup (s, 0, 1,   0.0f);
        setFaderGroup (s, 1, 1,  -6.0f);
        setFaderGroup (s, 2, 1, -12.0f);

        s.setTrackFaderGrouped (1, -10.0f);   // delta -4 from -6

        REQUIRE_THAT (fader (s, 0), WithinAbs ( -4.0f, 1e-4));
        REQUIRE_THAT (fader (s, 1), WithinAbs (-10.0f, 1e-4));
        REQUIRE_THAT (fader (s, 2), WithinAbs (-16.0f, 1e-4));
    }

    SECTION ("ungrouped fader does not propagate")
    {
        setFaderGroup (s, 0, 0,  0.0f);
        setFaderGroup (s, 1, 0, -6.0f);

        s.setTrackFaderGrouped (0, -20.0f);

        REQUIRE_THAT (fader (s, 0), WithinAbs (-20.0f, 1e-4));
        REQUIRE_THAT (fader (s, 1), WithinAbs ( -6.0f, 1e-4));   // independent
    }

    SECTION ("peers clamp at the fader ceiling")
    {
        setFaderGroup (s, 0, 1, 10.0f);
        setFaderGroup (s, 1, 1, 11.0f);

        s.setTrackFaderGrouped (0, 12.0f);   // +2 delta; peer 13 -> clamps to ceiling

        REQUIRE_THAT (fader (s, 0), WithinAbs (12.0f, 1e-4));
        REQUIRE_THAT (fader (s, 1), WithinAbs (12.0f, 1e-4));   // kFaderMaxDb
    }

    SECTION ("separate groups are independent")
    {
        setFaderGroup (s, 0, 1, 0.0f);
        setFaderGroup (s, 1, 1, 0.0f);
        setFaderGroup (s, 2, 2, 0.0f);
        setFaderGroup (s, 3, 2, 0.0f);

        s.setTrackFaderGrouped (0, 5.0f);

        REQUIRE_THAT (fader (s, 0), WithinAbs (5.0f, 1e-4));
        REQUIRE_THAT (fader (s, 1), WithinAbs (5.0f, 1e-4));
        REQUIRE_THAT (fader (s, 2), WithinAbs (0.0f, 1e-4));   // group 2 untouched
        REQUIRE_THAT (fader (s, 3), WithinAbs (0.0f, 1e-4));
    }
}
