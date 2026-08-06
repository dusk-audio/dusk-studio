#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/CompMakeupMap.h"
#include "session/Session.h"

using Catch::Matchers::WithinAbs;
using namespace duskstudio;

TEST_CASE ("comp makeup fans out to the active mode's audible param",
           "[comp][makeup]")
{
    Session s;
    auto& strip = s.track (0).strip;

    SECTION ("Opto: dB rides the donor's 0..100 dial at 0.8 dB per unit")
    {
        strip.compMode.store (0, std::memory_order_relaxed);
        comp::applyTrackCompMakeupDb (strip, 6.0f);
        REQUIRE_THAT (strip.compOptoGain.load (std::memory_order_relaxed),
                      WithinAbs (57.5f, 1e-4f));
    }

    SECTION ("FET: dB lands on the OUTPUT param")
    {
        strip.compMode.store (1, std::memory_order_relaxed);
        comp::applyTrackCompMakeupDb (strip, -3.5f);
        REQUIRE_THAT (strip.compFetOutput.load (std::memory_order_relaxed),
                      WithinAbs (-3.5f, 1e-4f));
    }

    SECTION ("VCA: dB lands on the OUTPUT param")
    {
        strip.compMode.store (2, std::memory_order_relaxed);
        comp::applyTrackCompMakeupDb (strip, 9.25f);
        REQUIRE_THAT (strip.compVcaOutput.load (std::memory_order_relaxed),
                      WithinAbs (9.25f, 1e-4f));
    }

    SECTION ("only the active mode's param moves")
    {
        strip.compMode.store (1, std::memory_order_relaxed);
        comp::applyTrackCompMakeupDb (strip, 8.0f);
        REQUIRE_THAT (strip.compVcaOutput.load (std::memory_order_relaxed),
                      WithinAbs (0.0f, 1e-4f));
        REQUIRE_THAT (strip.compOptoGain.load (std::memory_order_relaxed),
                      WithinAbs (50.0f, 1e-4f));
    }
}

// Unity has to be unity in every mode: Opto's dial centre is 50, the two
// output params sit at 0 dB.
TEST_CASE ("comp makeup 0 dB is unity in every mode", "[comp][makeup]")
{
    Session s;
    auto& strip = s.track (0).strip;

    strip.compMode.store (0, std::memory_order_relaxed);
    strip.compOptoGain.store (80.0f, std::memory_order_relaxed);
    comp::applyTrackCompMakeupDb (strip, 0.0f);
    REQUIRE_THAT (strip.compOptoGain.load (std::memory_order_relaxed),
                  WithinAbs (50.0f, 1e-4f));

    strip.compMode.store (1, std::memory_order_relaxed);
    strip.compFetOutput.store (12.0f, std::memory_order_relaxed);
    comp::applyTrackCompMakeupDb (strip, 0.0f);
    REQUIRE_THAT (strip.compFetOutput.load (std::memory_order_relaxed),
                  WithinAbs (0.0f, 1e-4f));
}

TEST_CASE ("comp makeup read-back is the exact inverse of the fan-out",
           "[comp][makeup]")
{
    Session s;
    auto& strip = s.track (0).strip;

    for (int mode = 0; mode <= 2; ++mode)
    {
        strip.compMode.store (mode, std::memory_order_relaxed);
        const auto domain = comp::makeupDomainFor (strip);
        for (float db : { domain.minDb, -0.5f, 0.0f, 3.0f, domain.maxDb })
        {
            comp::applyTrackCompMakeupDb (strip, db);
            REQUIRE_THAT (comp::trackCompMakeupDb (strip), WithinAbs (db, 1e-3f));
        }
    }
}

// Each mode's dial has its own reach: Opto spans +/-40 dB, the FET and VCA
// output params +/-20. Past the edge the fan-out saturates instead of
// wrapping or writing an out-of-range value the donor would clamp anyway,
// and the read-back reports the saturated value so a relative control
// nudged past the edge steps straight back down.
TEST_CASE ("comp makeup saturates at each mode's domain edge", "[comp][makeup]")
{
    Session s;
    auto& strip = s.track (0).strip;

    strip.compMode.store (0, std::memory_order_relaxed);
    REQUIRE_THAT (comp::makeupDomainFor (strip).maxDb, WithinAbs (40.0f, 1e-6f));
    comp::applyTrackCompMakeupDb (strip, 48.0f);
    REQUIRE_THAT (strip.compOptoGain.load (std::memory_order_relaxed),
                  WithinAbs (100.0f, 1e-4f));
    REQUIRE_THAT (comp::trackCompMakeupDb (strip), WithinAbs (40.0f, 1e-4f));
    comp::applyTrackCompMakeupDb (strip, -48.0f);
    REQUIRE_THAT (strip.compOptoGain.load (std::memory_order_relaxed),
                  WithinAbs (0.0f, 1e-4f));
    REQUIRE_THAT (comp::trackCompMakeupDb (strip), WithinAbs (-40.0f, 1e-4f));

    strip.compMode.store (1, std::memory_order_relaxed);
    REQUIRE_THAT (comp::makeupDomainFor (strip).maxDb, WithinAbs (20.0f, 1e-6f));
    comp::applyTrackCompMakeupDb (strip, 24.0f);
    REQUIRE_THAT (strip.compFetOutput.load (std::memory_order_relaxed),
                  WithinAbs (20.0f, 1e-4f));

    strip.compMode.store (2, std::memory_order_relaxed);
    comp::applyTrackCompMakeupDb (strip, -40.0f);
    REQUIRE_THAT (strip.compVcaOutput.load (std::memory_order_relaxed),
                  WithinAbs (-20.0f, 1e-4f));
}

// A param carrying a value from outside its documented range (a hand-edited
// session, a donor default that moved) must not hand a relative control a
// base it can never write back.
TEST_CASE ("comp makeup read-back clamps an out-of-range param",
           "[comp][makeup]")
{
    Session s;
    auto& strip = s.track (0).strip;

    strip.compMode.store (1, std::memory_order_relaxed);
    strip.compFetOutput.store (35.0f, std::memory_order_relaxed);
    REQUIRE_THAT (comp::trackCompMakeupDb (strip), WithinAbs (20.0f, 1e-4f));

    strip.compMode.store (0, std::memory_order_relaxed);
    strip.compOptoGain.store (140.0f, std::memory_order_relaxed);
    REQUIRE_THAT (comp::trackCompMakeupDb (strip), WithinAbs (40.0f, 1e-4f));
}

// The binding pair is what MidiBindingTarget::TrackCompMakeup drives: apply
// maps the CC fraction through the active mode's domain, the soft-takeover
// read-back inverts it. Both directions are pinned against the per-mode
// ranges the binding has always used - Opto is a straight 0..100 % sweep of
// the fraction, FET / VCA a -20..+20 dB sweep - because a bound CC that
// moved would be a regression on the path this control already had right.
TEST_CASE ("comp makeup binding pair matches the per-mode CC ranges",
           "[comp][makeup][binding]")
{
    Session s;
    auto& strip = s.track (0).strip;

    for (float frac : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        strip.compMode.store (0, std::memory_order_relaxed);
        comp::applyTrackCompMakeupDb (strip, comp::makeupBindingFracToDb (strip, frac));
        REQUIRE_THAT (strip.compOptoGain.load (std::memory_order_relaxed),
                      WithinAbs (frac * 100.0f, 1e-4f));
        REQUIRE_THAT (comp::makeupBindingFrac (strip), WithinAbs (frac, 1e-4f));

        strip.compMode.store (1, std::memory_order_relaxed);
        comp::applyTrackCompMakeupDb (strip, comp::makeupBindingFracToDb (strip, frac));
        REQUIRE_THAT (strip.compFetOutput.load (std::memory_order_relaxed),
                      WithinAbs (-20.0f + frac * 40.0f, 1e-4f));
        REQUIRE_THAT (comp::makeupBindingFrac (strip), WithinAbs (frac, 1e-4f));

        strip.compMode.store (2, std::memory_order_relaxed);
        comp::applyTrackCompMakeupDb (strip, comp::makeupBindingFracToDb (strip, frac));
        REQUIRE_THAT (strip.compVcaOutput.load (std::memory_order_relaxed),
                      WithinAbs (-20.0f + frac * 40.0f, 1e-4f));
        REQUIRE_THAT (comp::makeupBindingFrac (strip), WithinAbs (frac, 1e-4f));
    }
}

// Read-back from a param the binding did not write (UI knob, MCU, session
// load) still has to report the position pickup expects.
TEST_CASE ("comp makeup binding read-back reports the param's position",
           "[comp][makeup][binding]")
{
    Session s;
    auto& strip = s.track (0).strip;

    strip.compMode.store (0, std::memory_order_relaxed);
    strip.compOptoGain.store (80.0f, std::memory_order_relaxed);
    REQUIRE_THAT (comp::makeupBindingFrac (strip), WithinAbs (0.8f, 1e-4f));

    strip.compMode.store (1, std::memory_order_relaxed);
    strip.compFetOutput.store (10.0f, std::memory_order_relaxed);
    REQUIRE_THAT (comp::makeupBindingFrac (strip), WithinAbs (0.75f, 1e-4f));

    strip.compMode.store (2, std::memory_order_relaxed);
    strip.compVcaOutput.store (-20.0f, std::memory_order_relaxed);
    REQUIRE_THAT (comp::makeupBindingFrac (strip), WithinAbs (0.0f, 1e-4f));
}
