#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/CompModeMap.h"
#include "session/Session.h"

#include <algorithm>

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
        for (float db : { domain.lo, -0.5f, 0.0f, 3.0f, domain.hi })
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
    REQUIRE_THAT (comp::makeupDomainFor (strip).hi, WithinAbs (40.0f, 1e-6f));
    comp::applyTrackCompMakeupDb (strip, 48.0f);
    REQUIRE_THAT (strip.compOptoGain.load (std::memory_order_relaxed),
                  WithinAbs (100.0f, 1e-4f));
    REQUIRE_THAT (comp::trackCompMakeupDb (strip), WithinAbs (40.0f, 1e-4f));
    comp::applyTrackCompMakeupDb (strip, -48.0f);
    REQUIRE_THAT (strip.compOptoGain.load (std::memory_order_relaxed),
                  WithinAbs (0.0f, 1e-4f));
    REQUIRE_THAT (comp::trackCompMakeupDb (strip), WithinAbs (-40.0f, 1e-4f));

    strip.compMode.store (1, std::memory_order_relaxed);
    REQUIRE_THAT (comp::makeupDomainFor (strip).hi, WithinAbs (20.0f, 1e-6f));
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

// The donor gives each mode its own threshold-shaped param, and only Opto's
// runs backwards: PEAK REDUCTION is a 0..100 % dial where more is more
// compression, so the dB figure has to be inverted rather than copied.
TEST_CASE ("comp threshold fans out to the active mode's audible param",
           "[comp][threshold]")
{
    Session s;
    auto& strip = s.track (0).strip;

    SECTION ("Opto: dB inverts onto the peak-reduction dial")
    {
        strip.compMode.store (0, std::memory_order_relaxed);
        comp::applyTrackCompThresholdDb (strip, -30.0f);
        REQUIRE_THAT (strip.compOptoPeakRed.load (std::memory_order_relaxed),
                      WithinAbs (50.0f, 1e-4f));
        comp::applyTrackCompThresholdDb (strip, 0.0f);
        REQUIRE_THAT (strip.compOptoPeakRed.load (std::memory_order_relaxed),
                      WithinAbs (0.0f, 1e-4f));
    }

    SECTION ("FET: dB lands on the donor's threshold, not its input drive")
    {
        strip.compMode.store (1, std::memory_order_relaxed);
        comp::applyTrackCompThresholdDb (strip, -18.5f);
        REQUIRE_THAT (strip.compFetThresholdDb.load (std::memory_order_relaxed),
                      WithinAbs (-18.5f, 1e-4f));
        REQUIRE_THAT (strip.compFetInput.load (std::memory_order_relaxed),
                      WithinAbs (0.0f, 1e-4f));
    }

    SECTION ("VCA: dB lands on the threshold param")
    {
        strip.compMode.store (2, std::memory_order_relaxed);
        comp::applyTrackCompThresholdDb (strip, -24.0f);
        REQUIRE_THAT (strip.compVcaThreshDb.load (std::memory_order_relaxed),
                      WithinAbs (-24.0f, 1e-4f));
    }

    SECTION ("only the active mode's param moves")
    {
        strip.compMode.store (0, std::memory_order_relaxed);
        comp::applyTrackCompThresholdDb (strip, -12.0f);
        REQUIRE_THAT (strip.compFetThresholdDb.load (std::memory_order_relaxed),
                      WithinAbs (-10.0f, 1e-4f));
        REQUIRE_THAT (strip.compVcaThreshDb.load (std::memory_order_relaxed),
                      WithinAbs (12.0f, 1e-4f));
    }
}

// Domains are the donor's setter contracts - -60..0 dBFS on FET, -38..+12 dB
// on VCA, and Opto's dial-derived -60..0. Past an edge the fan-out saturates
// and the read-back reports the saturated value, so a relative control
// nudged past the edge steps straight back.
TEST_CASE ("comp threshold saturates at each mode's domain edge",
           "[comp][threshold]")
{
    Session s;
    auto& strip = s.track (0).strip;

    strip.compMode.store (0, std::memory_order_relaxed);
    REQUIRE_THAT (comp::thresholdDomainFor (strip).lo, WithinAbs (-60.0f, 1e-6f));
    REQUIRE_THAT (comp::thresholdDomainFor (strip).hi, WithinAbs (0.0f, 1e-6f));
    comp::applyTrackCompThresholdDb (strip, -90.0f);
    REQUIRE_THAT (strip.compOptoPeakRed.load (std::memory_order_relaxed),
                  WithinAbs (100.0f, 1e-4f));
    REQUIRE_THAT (comp::trackCompThresholdDb (strip), WithinAbs (-60.0f, 1e-4f));
    comp::applyTrackCompThresholdDb (strip, 12.0f);
    REQUIRE_THAT (strip.compOptoPeakRed.load (std::memory_order_relaxed),
                  WithinAbs (0.0f, 1e-4f));

    strip.compMode.store (1, std::memory_order_relaxed);
    REQUIRE_THAT (comp::thresholdDomainFor (strip).lo, WithinAbs (-60.0f, 1e-6f));
    REQUIRE_THAT (comp::thresholdDomainFor (strip).hi, WithinAbs (0.0f, 1e-6f));
    comp::applyTrackCompThresholdDb (strip, 6.0f);
    REQUIRE_THAT (strip.compFetThresholdDb.load (std::memory_order_relaxed),
                  WithinAbs (0.0f, 1e-4f));

    strip.compMode.store (2, std::memory_order_relaxed);
    REQUIRE_THAT (comp::thresholdDomainFor (strip).lo, WithinAbs (-38.0f, 1e-6f));
    REQUIRE_THAT (comp::thresholdDomainFor (strip).hi, WithinAbs (12.0f, 1e-6f));
    comp::applyTrackCompThresholdDb (strip, -60.0f);
    REQUIRE_THAT (strip.compVcaThreshDb.load (std::memory_order_relaxed),
                  WithinAbs (-38.0f, 1e-4f));
    comp::applyTrackCompThresholdDb (strip, 30.0f);
    REQUIRE_THAT (strip.compVcaThreshDb.load (std::memory_order_relaxed),
                  WithinAbs (12.0f, 1e-4f));
}

TEST_CASE ("comp threshold read-back is the exact inverse of the fan-out",
           "[comp][threshold]")
{
    Session s;
    auto& strip = s.track (0).strip;

    for (int mode = 0; mode <= 2; ++mode)
    {
        strip.compMode.store (mode, std::memory_order_relaxed);
        const auto domain = comp::thresholdDomainFor (strip);
        for (float db : { domain.lo, -24.0f, -6.0f, domain.hi })
        {
            comp::applyTrackCompThresholdDb (strip, db);
            REQUIRE_THAT (comp::trackCompThresholdDb (strip), WithinAbs (db, 1e-3f));
        }
    }
}

// Neutral is the top of the domain, not zero: VCA at 0 dB still compresses
// everything above 0 dBFS, so its no-compression parking spot is +12.
TEST_CASE ("comp threshold reset parks at no compression in every mode",
           "[comp][threshold]")
{
    Session s;
    auto& strip = s.track (0).strip;

    strip.compMode.store (0, std::memory_order_relaxed);
    strip.compOptoPeakRed.store (70.0f, std::memory_order_relaxed);
    comp::resetTrackCompThreshold (strip);
    REQUIRE_THAT (strip.compOptoPeakRed.load (std::memory_order_relaxed),
                  WithinAbs (0.0f, 1e-4f));

    strip.compMode.store (1, std::memory_order_relaxed);
    strip.compFetThresholdDb.store (-40.0f, std::memory_order_relaxed);
    comp::resetTrackCompThreshold (strip);
    REQUIRE_THAT (strip.compFetThresholdDb.load (std::memory_order_relaxed),
                  WithinAbs (0.0f, 1e-4f));

    strip.compMode.store (2, std::memory_order_relaxed);
    strip.compVcaThreshDb.store (-20.0f, std::memory_order_relaxed);
    comp::resetTrackCompThreshold (strip);
    REQUIRE_THAT (strip.compVcaThreshDb.load (std::memory_order_relaxed),
                  WithinAbs (12.0f, 1e-4f));
}

TEST_CASE ("comp threshold read-back clamps an out-of-range param",
           "[comp][threshold]")
{
    Session s;
    auto& strip = s.track (0).strip;

    strip.compMode.store (2, std::memory_order_relaxed);
    strip.compVcaThreshDb.store (-55.0f, std::memory_order_relaxed);
    REQUIRE_THAT (comp::trackCompThresholdDb (strip), WithinAbs (-38.0f, 1e-4f));

    strip.compMode.store (0, std::memory_order_relaxed);
    strip.compOptoPeakRed.store (130.0f, std::memory_order_relaxed);
    REQUIRE_THAT (comp::trackCompThresholdDb (strip), WithinAbs (-60.0f, 1e-4f));
}

// The binding pair is what MidiBindingTarget::TrackCompThresh drives. Unlike
// makeup, this one changes what a bound CC does: the fraction sweeps the
// mode's threshold in dB, so 0 is always maximum compression and 1 always
// none, where the open-coded fan-out ran Opto the other way round from the
// other two and pointed FET at its input drive.
TEST_CASE ("comp threshold binding pair sweeps the mode's dB domain",
           "[comp][threshold][binding]")
{
    Session s;
    auto& strip = s.track (0).strip;

    for (float frac : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        strip.compMode.store (0, std::memory_order_relaxed);
        comp::applyTrackCompThresholdDb (strip, comp::thresholdBindingFracToDb (strip, frac));
        REQUIRE_THAT (strip.compOptoPeakRed.load (std::memory_order_relaxed),
                      WithinAbs ((1.0f - frac) * 100.0f, 1e-3f));
        REQUIRE_THAT (comp::thresholdBindingFrac (strip), WithinAbs (frac, 1e-4f));

        strip.compMode.store (1, std::memory_order_relaxed);
        comp::applyTrackCompThresholdDb (strip, comp::thresholdBindingFracToDb (strip, frac));
        REQUIRE_THAT (strip.compFetThresholdDb.load (std::memory_order_relaxed),
                      WithinAbs (-60.0f + frac * 60.0f, 1e-4f));
        REQUIRE_THAT (comp::thresholdBindingFrac (strip), WithinAbs (frac, 1e-4f));

        strip.compMode.store (2, std::memory_order_relaxed);
        comp::applyTrackCompThresholdDb (strip, comp::thresholdBindingFracToDb (strip, frac));
        REQUIRE_THAT (strip.compVcaThreshDb.load (std::memory_order_relaxed),
                      WithinAbs (-38.0f + frac * 50.0f, 1e-4f));
        REQUIRE_THAT (comp::thresholdBindingFrac (strip), WithinAbs (frac, 1e-4f));
    }
}

// Opto's cell has no ratio and no timing params - the donor exposes none, so
// a control fanning out through the map has to leave the strip alone instead
// of writing VCA's atoms, which Opto never reads.
TEST_CASE ("comp ratio / attack / release no-op in Opto", "[comp][timing]")
{
    Session s;
    auto& strip = s.track (0).strip;
    strip.compMode.store (0, std::memory_order_relaxed);

    REQUIRE_FALSE (comp::compModeHasTimingControls (strip));

    comp::nudgeTrackCompRatio (strip, 20, 0.2f);
    comp::applyTrackCompAttackMs (strip, 25.0f);
    comp::applyTrackCompReleaseMs (strip, 900.0f);
    comp::resetTrackCompRatio (strip);
    comp::resetTrackCompAttack (strip);
    comp::resetTrackCompRelease (strip);

    REQUIRE_THAT (strip.compVcaRatio.load (std::memory_order_relaxed), WithinAbs (4.0f, 1e-4f));
    REQUIRE_THAT (strip.compVcaAttack.load (std::memory_order_relaxed), WithinAbs (1.0f, 1e-4f));
    REQUIRE_THAT (strip.compVcaRelease.load (std::memory_order_relaxed), WithinAbs (100.0f, 1e-4f));
    REQUIRE (strip.compFetRatio.load (std::memory_order_relaxed) == 0);
    REQUIRE_THAT (strip.compFetAttack.load (std::memory_order_relaxed), WithinAbs (0.2f, 1e-4f));
    REQUIRE_THAT (strip.compFetRelease.load (std::memory_order_relaxed), WithinAbs (400.0f, 1e-4f));
}

TEST_CASE ("comp attack / release fan out over the donor's per-mode ranges",
           "[comp][timing]")
{
    Session s;
    auto& strip = s.track (0).strip;

    SECTION ("FET: 0.02..80 ms attack, 50..1100 ms release")
    {
        strip.compMode.store (1, std::memory_order_relaxed);
        REQUIRE_THAT (comp::attackDomainFor (strip).lo,  WithinAbs (0.02f, 1e-6f));
        REQUIRE_THAT (comp::attackDomainFor (strip).hi,  WithinAbs (80.0f, 1e-6f));
        REQUIRE_THAT (comp::releaseDomainFor (strip).lo, WithinAbs (50.0f, 1e-6f));
        REQUIRE_THAT (comp::releaseDomainFor (strip).hi, WithinAbs (1100.0f, 1e-6f));

        comp::applyTrackCompAttackMs (strip, 40.0f);
        REQUIRE_THAT (strip.compFetAttack.load (std::memory_order_relaxed), WithinAbs (40.0f, 1e-4f));
        REQUIRE_THAT (comp::trackCompAttackMs (strip), WithinAbs (40.0f, 1e-4f));
        comp::applyTrackCompAttackMs (strip, 200.0f);
        REQUIRE_THAT (strip.compFetAttack.load (std::memory_order_relaxed), WithinAbs (80.0f, 1e-4f));

        comp::applyTrackCompReleaseMs (strip, 5.0f);
        REQUIRE_THAT (strip.compFetRelease.load (std::memory_order_relaxed), WithinAbs (50.0f, 1e-4f));
        comp::applyTrackCompReleaseMs (strip, 3000.0f);
        REQUIRE_THAT (strip.compFetRelease.load (std::memory_order_relaxed), WithinAbs (1100.0f, 1e-4f));
    }

    SECTION ("VCA: 0.1..50 ms attack, 10..5000 ms release")
    {
        strip.compMode.store (2, std::memory_order_relaxed);
        REQUIRE_THAT (comp::attackDomainFor (strip).hi,  WithinAbs (50.0f, 1e-6f));
        REQUIRE_THAT (comp::releaseDomainFor (strip).hi, WithinAbs (5000.0f, 1e-6f));

        comp::applyTrackCompAttackMs (strip, 200.0f);
        REQUIRE_THAT (strip.compVcaAttack.load (std::memory_order_relaxed), WithinAbs (50.0f, 1e-4f));
        REQUIRE_THAT (strip.compFetAttack.load (std::memory_order_relaxed), WithinAbs (0.2f, 1e-4f));

        comp::applyTrackCompReleaseMs (strip, 4000.0f);
        REQUIRE_THAT (strip.compVcaRelease.load (std::memory_order_relaxed), WithinAbs (4000.0f, 1e-4f));
        REQUIRE_THAT (comp::trackCompReleaseMs (strip), WithinAbs (4000.0f, 1e-4f));
    }
}

// FET's ratio is a five-position switch and VCA's a continuous 1..120:1
// sweep, so the relative control steps one and scales the other. An
// accelerated encoder packs up to 63 detents into a single event, which
// would otherwise slam the switch end to end - FET moves one rung per event
// whatever the count, while VCA consumes them all.
TEST_CASE ("comp ratio nudge steps the FET switch one rung and scales VCA",
           "[comp][ratio]")
{
    Session s;
    auto& strip = s.track (0).strip;
    constexpr float f = 1.06f;

    strip.compMode.store (1, std::memory_order_relaxed);
    comp::nudgeTrackCompRatio (strip, 1, f);
    REQUIRE (strip.compFetRatio.load (std::memory_order_relaxed) == 1);
    comp::nudgeTrackCompRatio (strip, 63, f);
    REQUIRE (strip.compFetRatio.load (std::memory_order_relaxed) == 2);
    comp::nudgeTrackCompRatio (strip, -63, f);
    REQUIRE (strip.compFetRatio.load (std::memory_order_relaxed) == 1);
    comp::nudgeTrackCompRatio (strip, 0, f);
    REQUIRE (strip.compFetRatio.load (std::memory_order_relaxed) == 1);
    for (int i = 0; i < 8; ++i) comp::nudgeTrackCompRatio (strip, 4, f);
    REQUIRE (strip.compFetRatio.load (std::memory_order_relaxed) == comp::kFetRatioMaxIndex);
    for (int i = 0; i < 8; ++i) comp::nudgeTrackCompRatio (strip, -4, f);
    REQUIRE (strip.compFetRatio.load (std::memory_order_relaxed) == 0);

    // VCA scales 6 % per detent from the 4:1 default, same factor as the
    // timing encoders, so the whole 1..120 range is 82 detents.
    strip.compMode.store (2, std::memory_order_relaxed);
    comp::nudgeTrackCompRatio (strip, 5, f);
    REQUIRE_THAT (strip.compVcaRatio.load (std::memory_order_relaxed),
                  WithinAbs (5.3529023f, 1e-4f));
    comp::nudgeTrackCompRatio (strip, -1000, f);
    REQUIRE_THAT (strip.compVcaRatio.load (std::memory_order_relaxed), WithinAbs (1.0f, 1e-4f));
    comp::nudgeTrackCompRatio (strip, 1000, f);
    REQUIRE_THAT (strip.compVcaRatio.load (std::memory_order_relaxed), WithinAbs (120.0f, 1e-4f));
}

// A scaling step has to keep working at 1:1, where the range bottoms out:
// one detent up must move off the floor, and one down must hold there.
TEST_CASE ("comp ratio scaling still moves at the VCA floor", "[comp][ratio]")
{
    Session s;
    auto& strip = s.track (0).strip;
    strip.compMode.store (2, std::memory_order_relaxed);
    strip.compVcaRatio.store (comp::kVcaRatioMin, std::memory_order_relaxed);

    comp::nudgeTrackCompRatio (strip, 1, 1.06f);
    REQUIRE_THAT (strip.compVcaRatio.load (std::memory_order_relaxed),
                  WithinAbs (1.06f, 1e-6f));

    strip.compVcaRatio.store (comp::kVcaRatioMin, std::memory_order_relaxed);
    comp::nudgeTrackCompRatio (strip, -1, 1.06f);
    REQUIRE_THAT (strip.compVcaRatio.load (std::memory_order_relaxed),
                  WithinAbs (1.0f, 1e-6f));
}

// The comp editor's RATIO knob takes its sweep from this domain. VCA has to
// reach the donor's 120:1 ceiling: a knob stopping at 20 would read a strip
// the surface or a binding parked higher as 20:1 and write that back on the
// next touch. FET's is the 1..20 sweep its rung ladder maps from.
TEST_CASE ("comp ratio knob domain reaches the VCA ceiling", "[comp][ratio]")
{
    Session s;
    auto& strip = s.track (0).strip;

    strip.compMode.store (2, std::memory_order_relaxed);
    REQUIRE_THAT (comp::ratioKnobDomainFor (strip).lo, WithinAbs (1.0f, 1e-6f));
    REQUIRE_THAT (comp::ratioKnobDomainFor (strip).hi, WithinAbs (120.0f, 1e-6f));

    // A strip parked at the ceiling sits inside the knob's sweep, so the
    // editor displays it and hands it straight back.
    strip.compVcaRatio.store (comp::kVcaRatioMax, std::memory_order_relaxed);
    const auto domain = comp::ratioKnobDomainFor (strip);
    const float shown = std::clamp (strip.compVcaRatio.load (std::memory_order_relaxed),
                                    domain.lo, domain.hi);
    REQUIRE_THAT (shown, WithinAbs (120.0f, 1e-4f));

    strip.compMode.store (1, std::memory_order_relaxed);
    REQUIRE_THAT (comp::ratioKnobDomainFor (strip).hi, WithinAbs (20.0f, 1e-6f));

    strip.compMode.store (0, std::memory_order_relaxed);
    REQUIRE_THAT (comp::ratioKnobDomainFor (strip).hi, WithinAbs (0.0f, 1e-6f));
}

// Push-to-reset lands on the active mode's own default. Those constants sit
// in the map, so pin them against a fresh strip - drift there would reset a
// FET strip to VCA's timings.
TEST_CASE ("comp reset defaults match a fresh strip's per-mode values",
           "[comp][timing]")
{
    Session s;
    const auto& fresh = s.track (1).strip;
    REQUIRE (fresh.compFetRatio.load (std::memory_order_relaxed) == comp::kFetRatioDefaultIndex);
    REQUIRE_THAT (fresh.compFetAttack.load (std::memory_order_relaxed),
                  WithinAbs (comp::kFetAttackDefaultMs, 1e-6f));
    REQUIRE_THAT (fresh.compFetRelease.load (std::memory_order_relaxed),
                  WithinAbs (comp::kFetReleaseDefaultMs, 1e-6f));
    REQUIRE_THAT (fresh.compVcaRatio.load (std::memory_order_relaxed),
                  WithinAbs (comp::kVcaRatioDefault, 1e-6f));
    REQUIRE_THAT (fresh.compVcaAttack.load (std::memory_order_relaxed),
                  WithinAbs (comp::kVcaAttackDefaultMs, 1e-6f));
    REQUIRE_THAT (fresh.compVcaRelease.load (std::memory_order_relaxed),
                  WithinAbs (comp::kVcaReleaseDefaultMs, 1e-6f));

    auto& strip = s.track (0).strip;
    strip.compMode.store (1, std::memory_order_relaxed);
    strip.compFetRatio.store (3, std::memory_order_relaxed);
    strip.compFetAttack.store (50.0f, std::memory_order_relaxed);
    strip.compFetRelease.store (900.0f, std::memory_order_relaxed);
    comp::resetTrackCompRatio (strip);
    comp::resetTrackCompAttack (strip);
    comp::resetTrackCompRelease (strip);
    REQUIRE (strip.compFetRatio.load (std::memory_order_relaxed) == 0);
    REQUIRE_THAT (strip.compFetAttack.load (std::memory_order_relaxed), WithinAbs (0.2f, 1e-4f));
    REQUIRE_THAT (strip.compFetRelease.load (std::memory_order_relaxed), WithinAbs (400.0f, 1e-4f));
    REQUIRE_THAT (strip.compVcaAttack.load (std::memory_order_relaxed), WithinAbs (1.0f, 1e-4f));
}
