#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/CompModeMap.h"
#include "session/Session.h"

#include <initializer_list>
#include <tuple>
#include <utility>

using Catch::Matchers::WithinAbs;
using namespace duskstudio;

namespace
{
using Id = CompParamId;
using comp::Role;

constexpr int kStripModes[] = { comp::kOptoMode, comp::kFetMode, comp::kVcaMode };
constexpr int kAllModes[]   = { comp::kOptoMode, comp::kFetMode, comp::kVcaMode, comp::kBusMode };
constexpr Role kRoles[]     = { Role::Threshold, Role::Makeup, Role::Ratio, Role::Attack, Role::Release };

float hostMin (Id id) { return compParamSpec (id).hostMin; }
float hostMax (Id id) { return compParamSpec (id).hostMax; }
float hostDefault (Id id) { return compParamSpec (id).hostDefault; }

// Every strip parameter but `moved` still holds its default.
void requireOthersUntouched (const CompParams& comp, Id moved)
{
    for (const auto id : compStageParams (CompStage::Strip))
    {
        if (id == moved) continue;
        CAPTURE (compParamSpec (id).symbol);
        REQUIRE_THAT (comp.get (id), WithinAbs (hostDefault (id), 1e-6f));
    }
}
} // namespace

// The map is the routing every compressor control shares, so its table is the
// plug-in's own controls per mode: Opto peak reduction / gain, FET input /
// output / ratio / attack / release, VCA threshold / compression / output, and
// Bus threshold / makeup / ratio / attack / release.
TEST_CASE ("comp roles map each mode onto Multi-Comp 2's own controls", "[comp]")
{
    const auto check = [] (int mode, Role role, bool exists, Id id, bool reversed)
    {
        CAPTURE (mode, (int) role);
        const auto p = comp::roleParam (mode, role);
        REQUIRE (p.exists == exists);
        REQUIRE (comp::modeHasRole (mode, role) == exists);
        if (! exists) return;
        REQUIRE (p.id == id);
        REQUIRE (p.reversed == reversed);
    };
    check (comp::kOptoMode, Role::Threshold, true,  Id::OptoPeakReduction, true);
    check (comp::kOptoMode, Role::Makeup,    true,  Id::OptoGain, false);
    check (comp::kOptoMode, Role::Ratio,     false, Id::Mix, false);
    check (comp::kOptoMode, Role::Attack,    false, Id::Mix, false);
    check (comp::kOptoMode, Role::Release,   false, Id::Mix, false);

    check (comp::kFetMode, Role::Threshold, true, Id::FetInput, true);
    check (comp::kFetMode, Role::Makeup,    true, Id::FetOutput, false);
    check (comp::kFetMode, Role::Ratio,     true, Id::FetRatio, false);
    check (comp::kFetMode, Role::Attack,    true, Id::FetAttack, false);
    check (comp::kFetMode, Role::Release,   true, Id::FetRelease, false);

    check (comp::kVcaMode, Role::Threshold, true,  Id::VcaThreshold, false);
    check (comp::kVcaMode, Role::Makeup,    true,  Id::VcaOutput, false);
    check (comp::kVcaMode, Role::Ratio,     true,  Id::VcaCompression, false);
    check (comp::kVcaMode, Role::Attack,    false, Id::Mix, false);
    check (comp::kVcaMode, Role::Release,   false, Id::Mix, false);

    check (comp::kBusMode, Role::Threshold, true, Id::BusThreshold, false);
    check (comp::kBusMode, Role::Makeup,    true, Id::BusMakeup, false);
    check (comp::kBusMode, Role::Ratio,     true, Id::BusRatio, false);
    check (comp::kBusMode, Role::Attack,    true, Id::BusAttack, false);
    check (comp::kBusMode, Role::Release,   true, Id::BusRelease, false);
}

TEST_CASE ("comp makeup fans out to the active mode's audible param", "[comp][makeup]")
{
    Session s;
    auto& strip = s.track (0).strip;

    for (const auto& [mode, id] : { std::pair<int, Id> { comp::kOptoMode, Id::OptoGain },
                                    std::pair<int, Id> { comp::kFetMode,  Id::FetOutput },
                                    std::pair<int, Id> { comp::kVcaMode,  Id::VcaOutput } })
    {
        CAPTURE (mode);
        strip.comp.resetToDefaults (CompStage::Strip);
        strip.compMode.store (mode, std::memory_order_relaxed);
        REQUIRE (comp::applyTrackRoleFrac (strip, Role::Makeup, 0.75f));
        REQUIRE_THAT (strip.comp.get (id),
                      WithinAbs (hostMin (id) + 0.75f * (hostMax (id) - hostMin (id)), 1e-4f));
        requireOthersUntouched (strip.comp, id);
    }

    BusParams bus;
    REQUIRE (comp::applyRoleFrac (bus.comp, comp::kBusMode, Role::Makeup, 0.5f));
    REQUIRE_THAT (bus.comp.get (Id::BusMakeup), WithinAbs (10.0f, 1e-4f));
}

// Push-to-reset lands on the plug-in's own default for the active mode, which
// for every makeup is unity: Opto's gain dial at its unity position, the output
// controls at 0 dB.
TEST_CASE ("comp makeup reset returns each mode to the plug-in's unity default", "[comp][makeup]")
{
    Session s;
    auto& strip = s.track (0).strip;
    const CompParams fresh;

    for (const auto& [mode, id] : { std::pair<int, Id> { comp::kOptoMode, Id::OptoGain },
                                    std::pair<int, Id> { comp::kFetMode,  Id::FetOutput },
                                    std::pair<int, Id> { comp::kVcaMode,  Id::VcaOutput } })
    {
        CAPTURE (mode);
        strip.compMode.store (mode, std::memory_order_relaxed);
        strip.comp.set (id, hostMax (id));
        REQUIRE (comp::resetTrackRole (strip, Role::Makeup));
        REQUIRE_THAT (strip.comp.get (id), WithinAbs (fresh.get (id), 1e-6f));
    }
    REQUIRE_THAT (fresh.get (Id::OptoGain), WithinAbs (23.4092255f, 1e-6f));
    REQUIRE_THAT (fresh.get (Id::FetOutput), WithinAbs (0.0f, 1e-6f));
    REQUIRE_THAT (fresh.get (Id::VcaOutput), WithinAbs (0.0f, 1e-6f));
}

// A binding's apply and its soft-takeover read-back are the same two calls;
// they have to stay inverses in every mode and role or pickup latches at the
// wrong position.
TEST_CASE ("comp role read-back is the exact inverse of the apply", "[comp][binding]")
{
    for (const int mode : kAllModes)
        for (const Role role : kRoles)
        {
            if (! comp::modeHasRole (mode, role)) continue;
            const auto p = comp::roleParam (mode, role);
            const auto& spec = compParamSpec (p.id);
            CAPTURE (mode, spec.symbol);
            CompParams c;
            if (spec.integer)
            {
                const int steps = (int) (spec.hostMax - spec.hostMin);
                for (int step = 0; step <= steps; ++step)
                {
                    const float frac = (float) step / (float) steps;
                    REQUIRE (comp::applyRoleFrac (c, mode, role, frac));
                    REQUIRE_THAT (comp::roleFrac (c, mode, role), WithinAbs (frac, 1e-6f));
                }
                continue;
            }
            for (const float frac : { 0.0f, 0.1f, 0.37f, 0.5f, 0.9f, 1.0f })
            {
                REQUIRE (comp::applyRoleFrac (c, mode, role, frac));
                REQUIRE_THAT (comp::roleFrac (c, mode, role), WithinAbs (frac, 1e-5f));
            }
        }
}

// Past either end the apply saturates at the end of the plug-in's host range
// instead of wrapping or writing a value the plug-in would refuse, and the
// read-back reports the end, so a relative control nudged past it steps
// straight back.
TEST_CASE ("comp makeup saturates at each mode's host range", "[comp][makeup]")
{
    Session s;
    auto& strip = s.track (0).strip;

    for (const auto& [mode, id] : { std::pair<int, Id> { comp::kOptoMode, Id::OptoGain },
                                    std::pair<int, Id> { comp::kFetMode,  Id::FetOutput },
                                    std::pair<int, Id> { comp::kVcaMode,  Id::VcaOutput } })
    {
        CAPTURE (mode);
        strip.compMode.store (mode, std::memory_order_relaxed);
        comp::applyTrackRoleFrac (strip, Role::Makeup, 1.5f);
        REQUIRE_THAT (strip.comp.get (id), WithinAbs (hostMax (id), 1e-5f));
        REQUIRE_THAT (comp::trackRoleFrac (strip, Role::Makeup), WithinAbs (1.0f, 1e-6f));
        comp::applyTrackRoleFrac (strip, Role::Makeup, -0.5f);
        REQUIRE_THAT (strip.comp.get (id), WithinAbs (hostMin (id), 1e-5f));
        REQUIRE_THAT (comp::trackRoleFrac (strip, Role::Makeup), WithinAbs (0.0f, 1e-6f));
    }
}

// A value from outside the host range (a hand-edited session, a default that
// moved) must not hand a relative control a base it can never write back.
TEST_CASE ("comp role read-back clamps an out-of-range param", "[comp]")
{
    Session s;
    auto& strip = s.track (0).strip;

    strip.compMode.store (comp::kFetMode, std::memory_order_relaxed);
    strip.comp.atom (Id::FetOutput).store (35.0f, std::memory_order_relaxed);
    REQUIRE_THAT (comp::trackRoleFrac (strip, Role::Makeup), WithinAbs (1.0f, 1e-6f));
    REQUIRE (comp::nudgeTrackRole (strip, Role::Makeup, -1));
    REQUIRE_THAT (strip.comp.get (Id::FetOutput), WithinAbs (20.0f - 0.4f, 1e-4f));

    strip.compMode.store (comp::kOptoMode, std::memory_order_relaxed);
    strip.comp.atom (Id::OptoPeakReduction).store (140.0f, std::memory_order_relaxed);
    REQUIRE_THAT (comp::trackRoleFrac (strip, Role::Threshold), WithinAbs (0.0f, 1e-6f));

    strip.compMode.store (comp::kVcaMode, std::memory_order_relaxed);
    strip.comp.atom (Id::VcaThreshold).store (-80.0f, std::memory_order_relaxed);
    REQUIRE_THAT (comp::trackRoleFrac (strip, Role::Threshold), WithinAbs (0.0f, 1e-6f));
    REQUIRE_THAT (comp::trackThresholdAxisValue (strip), WithinAbs (-55.0f, 1e-4f));
}

// TrackCompMakeup sweeps the CC across the whole host range of the active
// mode's makeup, the plug-in's own knob travel, linearly.
TEST_CASE ("comp makeup binding sweeps each mode's host range", "[comp][makeup][binding]")
{
    Session s;
    auto& strip = s.track (0).strip;

    for (const float frac : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        CAPTURE (frac);
        strip.compMode.store (comp::kOptoMode, std::memory_order_relaxed);
        comp::applyTrackRoleFrac (strip, Role::Makeup, frac);
        REQUIRE_THAT (strip.comp.get (Id::OptoGain), WithinAbs (frac * 100.0f, 1e-4f));

        strip.compMode.store (comp::kFetMode, std::memory_order_relaxed);
        comp::applyTrackRoleFrac (strip, Role::Makeup, frac);
        REQUIRE_THAT (strip.comp.get (Id::FetOutput), WithinAbs (-20.0f + frac * 40.0f, 1e-4f));

        strip.compMode.store (comp::kVcaMode, std::memory_order_relaxed);
        comp::applyTrackRoleFrac (strip, Role::Makeup, frac);
        REQUIRE_THAT (strip.comp.get (Id::VcaOutput), WithinAbs (-20.0f + frac * 40.0f, 1e-4f));
    }
}

// Read-back from a value the binding did not write (a knob, a V-pot, a session
// load) still has to report the position pickup expects.
TEST_CASE ("comp makeup binding read-back reports the param's position", "[comp][makeup][binding]")
{
    Session s;
    auto& strip = s.track (0).strip;

    strip.compMode.store (comp::kOptoMode, std::memory_order_relaxed);
    strip.comp.set (Id::OptoGain, 80.0f);
    REQUIRE_THAT (comp::trackRoleFrac (strip, Role::Makeup), WithinAbs (0.8f, 1e-6f));

    strip.compMode.store (comp::kFetMode, std::memory_order_relaxed);
    strip.comp.set (Id::FetOutput, 10.0f);
    REQUIRE_THAT (comp::trackRoleFrac (strip, Role::Makeup), WithinAbs (0.75f, 1e-6f));

    strip.compMode.store (comp::kVcaMode, std::memory_order_relaxed);
    strip.comp.set (Id::VcaOutput, -20.0f);
    REQUIRE_THAT (comp::trackRoleFrac (strip, Role::Makeup), WithinAbs (0.0f, 1e-6f));
}

// The plug-in gives each mode its own threshold-shaped control. VCA's is a
// threshold; Opto's PEAK REDUCTION and FET's INPUT compress more as they turn
// up, so a threshold move runs them the other way.
TEST_CASE ("comp threshold fans out to the active mode's audible param", "[comp][threshold]")
{
    Session s;
    auto& strip = s.track (0).strip;

    SECTION ("Opto: a lower threshold is more peak reduction")
    {
        strip.compMode.store (comp::kOptoMode, std::memory_order_relaxed);
        comp::applyTrackRoleFrac (strip, Role::Threshold, 0.25f);
        REQUIRE_THAT (strip.comp.get (Id::OptoPeakReduction), WithinAbs (75.0f, 1e-4f));
        requireOthersUntouched (strip.comp, Id::OptoPeakReduction);
    }

    SECTION ("FET: a lower threshold is more input drive, not the hidden threshold")
    {
        strip.compMode.store (comp::kFetMode, std::memory_order_relaxed);
        comp::applyTrackRoleFrac (strip, Role::Threshold, 0.25f);
        REQUIRE_THAT (strip.comp.get (Id::FetInput), WithinAbs (25.0f, 1e-4f));
        requireOthersUntouched (strip.comp, Id::FetInput);
    }

    SECTION ("VCA: the fraction is the threshold's own position")
    {
        strip.compMode.store (comp::kVcaMode, std::memory_order_relaxed);
        comp::applyTrackRoleFrac (strip, Role::Threshold, 0.4f);
        REQUIRE_THAT (strip.comp.get (Id::VcaThreshold), WithinAbs (-55.0f + 0.4f * 55.0f, 1e-4f));
        requireOthersUntouched (strip.comp, Id::VcaThreshold);
    }

    SECTION ("Bus: the bus threshold, -30 to +15 dB")
    {
        BusParams bus;
        comp::applyRoleFrac (bus.comp, comp::kBusMode, Role::Threshold, 0.6f);
        REQUIRE_THAT (bus.comp.get (Id::BusThreshold), WithinAbs (-3.0f, 1e-4f));
    }
}

// The handle's scale: a threshold in dB for VCA and Bus, so a VCA handle on
// the fader scale's -18 tick is a -18 dB threshold, and 0 down to -60 for Opto
// and FET, their whole host travel with none at the top.
TEST_CASE ("comp threshold handle axis is dB where the parameter is a threshold", "[comp][threshold]")
{
    REQUIRE_THAT (comp::thresholdAxis (comp::kVcaMode).lo, WithinAbs (-55.0f, 1e-6f));
    REQUIRE_THAT (comp::thresholdAxis (comp::kVcaMode).hi, WithinAbs (0.0f, 1e-6f));
    REQUIRE_THAT (comp::thresholdAxis (comp::kBusMode).lo, WithinAbs (-30.0f, 1e-6f));
    REQUIRE_THAT (comp::thresholdAxis (comp::kBusMode).hi, WithinAbs (15.0f, 1e-6f));
    for (const int mode : { comp::kOptoMode, comp::kFetMode })
    {
        REQUIRE_THAT (comp::thresholdAxis (mode).lo, WithinAbs (-60.0f, 1e-6f));
        REQUIRE_THAT (comp::thresholdAxis (mode).hi, WithinAbs (0.0f, 1e-6f));
    }

    Session s;
    auto& strip = s.track (0).strip;
    strip.compMode.store (comp::kVcaMode, std::memory_order_relaxed);
    comp::applyTrackThresholdAxisValue (strip, -18.0f);
    REQUIRE_THAT (strip.comp.get (Id::VcaThreshold), WithinAbs (-18.0f, 1e-4f));
    REQUIRE_THAT (comp::trackThresholdAxisValue (strip), WithinAbs (-18.0f, 1e-4f));
    comp::applyTrackThresholdAxisValue (strip, -60.0f);
    REQUIRE_THAT (strip.comp.get (Id::VcaThreshold), WithinAbs (-55.0f, 1e-4f));

    strip.compMode.store (comp::kOptoMode, std::memory_order_relaxed);
    comp::applyTrackThresholdAxisValue (strip, -30.0f);
    REQUIRE_THAT (strip.comp.get (Id::OptoPeakReduction), WithinAbs (50.0f, 1e-4f));
    comp::applyTrackThresholdAxisValue (strip, 12.0f);
    REQUIRE_THAT (strip.comp.get (Id::OptoPeakReduction), WithinAbs (0.0f, 1e-4f));

    strip.compMode.store (comp::kFetMode, std::memory_order_relaxed);
    comp::applyTrackThresholdAxisValue (strip, 0.0f);
    REQUIRE_THAT (strip.comp.get (Id::FetInput), WithinAbs (-20.0f, 1e-4f));
    comp::applyTrackThresholdAxisValue (strip, -60.0f);
    REQUIRE_THAT (strip.comp.get (Id::FetInput), WithinAbs (40.0f, 1e-4f));
    comp::applyTrackThresholdAxisValue (strip, -20.0f);
    REQUIRE_THAT (strip.comp.get (Id::FetInput), WithinAbs (0.0f, 1e-4f));
    REQUIRE_THAT (comp::trackThresholdAxisValue (strip), WithinAbs (-20.0f, 1e-4f));
}

// Past an end the threshold saturates and the read-back reports the end.
TEST_CASE ("comp threshold saturates at each mode's host range", "[comp][threshold]")
{
    Session s;
    auto& strip = s.track (0).strip;

    strip.compMode.store (comp::kOptoMode, std::memory_order_relaxed);
    comp::applyTrackRoleFrac (strip, Role::Threshold, -1.0f);
    REQUIRE_THAT (strip.comp.get (Id::OptoPeakReduction), WithinAbs (100.0f, 1e-4f));
    comp::applyTrackRoleFrac (strip, Role::Threshold, 2.0f);
    REQUIRE_THAT (strip.comp.get (Id::OptoPeakReduction), WithinAbs (0.0f, 1e-4f));

    strip.compMode.store (comp::kFetMode, std::memory_order_relaxed);
    comp::applyTrackRoleFrac (strip, Role::Threshold, -1.0f);
    REQUIRE_THAT (strip.comp.get (Id::FetInput), WithinAbs (40.0f, 1e-4f));
    comp::applyTrackRoleFrac (strip, Role::Threshold, 2.0f);
    REQUIRE_THAT (strip.comp.get (Id::FetInput), WithinAbs (-20.0f, 1e-4f));

    strip.compMode.store (comp::kVcaMode, std::memory_order_relaxed);
    comp::applyTrackThresholdAxisValue (strip, -90.0f);
    REQUIRE_THAT (strip.comp.get (Id::VcaThreshold), WithinAbs (-55.0f, 1e-4f));
    comp::applyTrackThresholdAxisValue (strip, 30.0f);
    REQUIRE_THAT (strip.comp.get (Id::VcaThreshold), WithinAbs (0.0f, 1e-4f));
}

// Double-click on the handle and push on the V-pot return the threshold to
// the plug-in's own default: no reduction on Opto, unity input on FET, VCA's
// -27 dB and the bus's 0 dB.
TEST_CASE ("comp threshold reset returns to the plug-in's default in every mode", "[comp][threshold]")
{
    Session s;
    auto& strip = s.track (0).strip;
    for (const auto& [mode, id, expected] : { std::tuple<int, Id, float> { comp::kOptoMode, Id::OptoPeakReduction, 0.0f },
                                              std::tuple<int, Id, float> { comp::kFetMode,  Id::FetInput, 0.0f },
                                              std::tuple<int, Id, float> { comp::kVcaMode,  Id::VcaThreshold, -27.0f } })
    {
        CAPTURE (mode);
        strip.compMode.store (mode, std::memory_order_relaxed);
        comp::applyTrackRoleFrac (strip, Role::Threshold, 0.1f);
        REQUIRE (comp::resetTrackRole (strip, Role::Threshold));
        REQUIRE_THAT (strip.comp.get (id), WithinAbs (expected, 1e-6f));
    }

    BusParams bus;
    comp::applyRoleFrac (bus.comp, comp::kBusMode, Role::Threshold, 0.1f);
    REQUIRE (comp::resetRole (bus.comp, comp::kBusMode, Role::Threshold));
    REQUIRE_THAT (bus.comp.get (Id::BusThreshold), WithinAbs (0.0f, 1e-6f));
}

// TrackCompThresh: the fraction runs a threshold's way in every mode, 0 the
// most compression and 1 the least, linearly across the host range.
TEST_CASE ("comp threshold binding runs toward less compression in every mode", "[comp][threshold][binding]")
{
    Session s;
    auto& strip = s.track (0).strip;

    for (const float frac : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        CAPTURE (frac);
        strip.compMode.store (comp::kOptoMode, std::memory_order_relaxed);
        comp::applyTrackRoleFrac (strip, Role::Threshold, frac);
        REQUIRE_THAT (strip.comp.get (Id::OptoPeakReduction), WithinAbs ((1.0f - frac) * 100.0f, 1e-4f));
        REQUIRE_THAT (comp::trackRoleFrac (strip, Role::Threshold), WithinAbs (frac, 1e-6f));

        strip.compMode.store (comp::kFetMode, std::memory_order_relaxed);
        comp::applyTrackRoleFrac (strip, Role::Threshold, frac);
        REQUIRE_THAT (strip.comp.get (Id::FetInput), WithinAbs (40.0f - frac * 60.0f, 1e-4f));
        REQUIRE_THAT (comp::trackRoleFrac (strip, Role::Threshold), WithinAbs (frac, 1e-6f));

        strip.compMode.store (comp::kVcaMode, std::memory_order_relaxed);
        comp::applyTrackRoleFrac (strip, Role::Threshold, frac);
        REQUIRE_THAT (strip.comp.get (Id::VcaThreshold), WithinAbs (-55.0f + frac * 55.0f, 1e-4f));
        REQUIRE_THAT (comp::trackRoleFrac (strip, Role::Threshold), WithinAbs (frac, 1e-6f));
    }
}

// Opto has no ratio, attack or release and VCA no attack or release: the
// plug-in exposes none, so a control fanning out through the map leaves the
// strip alone and reports that it moved nothing.
TEST_CASE ("comp roles a mode lacks move nothing", "[comp][timing]")
{
    Session s;
    auto& strip = s.track (0).strip;

    strip.compMode.store (comp::kOptoMode, std::memory_order_relaxed);
    for (const Role role : { Role::Ratio, Role::Attack, Role::Release })
    {
        REQUIRE_FALSE (comp::nudgeTrackRole (strip, role, 20));
        REQUIRE_FALSE (comp::applyTrackRoleFrac (strip, role, 0.9f));
        REQUIRE_FALSE (comp::resetTrackRole (strip, role));
        REQUIRE (comp::trackRoleFrac (strip, role) < 0.0f);
    }

    strip.compMode.store (comp::kVcaMode, std::memory_order_relaxed);
    for (const Role role : { Role::Attack, Role::Release })
    {
        REQUIRE_FALSE (comp::nudgeTrackRole (strip, role, 20));
        REQUIRE_FALSE (comp::applyTrackRoleFrac (strip, role, 0.9f));
        REQUIRE (comp::trackRoleFrac (strip, role) < 0.0f);
    }

    const CompParams fresh;
    for (const auto id : compStageParams (CompStage::Strip))
    {
        CAPTURE (compParamSpec (id).symbol);
        REQUIRE_THAT (strip.comp.get (id), WithinAbs (fresh.get (id), 1e-6f));
    }
}

// FET's attack and release are the plug-in's 1-7 dials over their legacy host
// ranges (0..1 for attack, 50..1100 for release); the bus's are steps.
TEST_CASE ("comp attack / release travel the plug-in's host ranges", "[comp][timing]")
{
    Session s;
    auto& strip = s.track (0).strip;
    strip.compMode.store (comp::kFetMode, std::memory_order_relaxed);

    comp::applyTrackRoleFrac (strip, Role::Attack, 0.5f);
    REQUIRE_THAT (strip.comp.get (Id::FetAttack), WithinAbs (0.5f, 1e-6f));
    comp::applyTrackRoleFrac (strip, Role::Release, 0.5f);
    REQUIRE_THAT (strip.comp.get (Id::FetRelease), WithinAbs (575.0f, 1e-3f));
    comp::applyTrackRoleFrac (strip, Role::Release, 2.0f);
    REQUIRE_THAT (strip.comp.get (Id::FetRelease), WithinAbs (1100.0f, 1e-3f));
    REQUIRE_THAT (strip.comp.get (Id::VcaCompression), WithinAbs (hostDefault (Id::VcaCompression), 1e-6f));

    BusParams bus;
    comp::applyRoleFrac (bus.comp, comp::kBusMode, Role::Attack, 0.45f);
    REQUIRE_THAT (bus.comp.get (Id::BusAttack), WithinAbs (2.0f, 1e-6f));
    comp::applyRoleFrac (bus.comp, comp::kBusMode, Role::Release, 1.0f);
    REQUIRE_THAT (bus.comp.get (Id::BusRelease), WithinAbs (4.0f, 1e-6f));
}

// A stepped control moves one step per event whatever the detent count (an
// accelerated encoder sends up to 63 in one), and a continuous one consumes
// every detent at a hundredth of its host range each.
TEST_CASE ("comp ratio nudge steps the FET switch one rung and sweeps VCA", "[comp][ratio]")
{
    Session s;
    auto& strip = s.track (0).strip;

    strip.compMode.store (comp::kFetMode, std::memory_order_relaxed);
    comp::nudgeTrackRole (strip, Role::Ratio, 1);
    REQUIRE_THAT (strip.comp.get (Id::FetRatio), WithinAbs (1.0f, 1e-6f));
    comp::nudgeTrackRole (strip, Role::Ratio, 63);
    REQUIRE_THAT (strip.comp.get (Id::FetRatio), WithinAbs (2.0f, 1e-6f));
    comp::nudgeTrackRole (strip, Role::Ratio, -63);
    REQUIRE_THAT (strip.comp.get (Id::FetRatio), WithinAbs (1.0f, 1e-6f));
    comp::nudgeTrackRole (strip, Role::Ratio, 0);
    REQUIRE_THAT (strip.comp.get (Id::FetRatio), WithinAbs (1.0f, 1e-6f));
    for (int i = 0; i < 8; ++i) comp::nudgeTrackRole (strip, Role::Ratio, 4);
    REQUIRE_THAT (strip.comp.get (Id::FetRatio), WithinAbs (hostMax (Id::FetRatio), 1e-6f));
    for (int i = 0; i < 8; ++i) comp::nudgeTrackRole (strip, Role::Ratio, -4);
    REQUIRE_THAT (strip.comp.get (Id::FetRatio), WithinAbs (0.0f, 1e-6f));

    strip.compMode.store (comp::kVcaMode, std::memory_order_relaxed);
    const float start = strip.comp.get (Id::VcaCompression);
    comp::nudgeTrackRole (strip, Role::Ratio, 5);
    REQUIRE_THAT (strip.comp.get (Id::VcaCompression), WithinAbs (start + 5.0f, 1e-4f));
    comp::nudgeTrackRole (strip, Role::Ratio, -1000);
    REQUIRE_THAT (strip.comp.get (Id::VcaCompression), WithinAbs (0.0f, 1e-6f));
    comp::nudgeTrackRole (strip, Role::Ratio, 1000);
    REQUIRE_THAT (strip.comp.get (Id::VcaCompression), WithinAbs (100.0f, 1e-6f));

    BusParams bus;
    comp::nudgeRole (bus.comp, comp::kBusMode, Role::Ratio, 40);
    REQUIRE_THAT (bus.comp.get (Id::BusRatio), WithinAbs (1.0f, 1e-6f));
}

// A step has to keep moving at the ends of the range: one detent up from the
// floor moves off it, one more down holds there.
TEST_CASE ("comp nudge still moves at the floor and holds past it", "[comp][ratio]")
{
    Session s;
    auto& strip = s.track (0).strip;
    strip.compMode.store (comp::kVcaMode, std::memory_order_relaxed);
    strip.comp.set (Id::VcaCompression, 0.0f);

    comp::nudgeTrackRole (strip, Role::Ratio, 1);
    REQUIRE_THAT (strip.comp.get (Id::VcaCompression), WithinAbs (1.0f, 1e-6f));
    comp::nudgeTrackRole (strip, Role::Ratio, -3);
    REQUIRE_THAT (strip.comp.get (Id::VcaCompression), WithinAbs (0.0f, 1e-6f));

    // A threshold role turned the way it runs: up the role, down the input.
    strip.compMode.store (comp::kFetMode, std::memory_order_relaxed);
    strip.comp.set (Id::FetInput, -20.0f);
    comp::nudgeTrackRole (strip, Role::Threshold, 1);
    REQUIRE_THAT (strip.comp.get (Id::FetInput), WithinAbs (-20.0f, 1e-6f));
    comp::nudgeTrackRole (strip, Role::Threshold, -1);
    REQUIRE_THAT (strip.comp.get (Id::FetInput), WithinAbs (-19.4f, 1e-4f));
}

// VCA's ratio role is the compression knob's whole travel, up to the plug-in's
// INF end: a control stopping short would read a strip a surface parked
// higher as less and write that back on the next touch.
TEST_CASE ("comp ratio role reaches the VCA compression ceiling", "[comp][ratio]")
{
    Session s;
    auto& strip = s.track (0).strip;
    strip.compMode.store (comp::kVcaMode, std::memory_order_relaxed);

    comp::applyTrackRoleFrac (strip, Role::Ratio, 1.0f);
    REQUIRE_THAT (strip.comp.get (Id::VcaCompression), WithinAbs (100.0f, 1e-6f));
    REQUIRE_THAT (comp::trackRoleFrac (strip, Role::Ratio), WithinAbs (1.0f, 1e-6f));
    REQUIRE_THAT (comp::hostDomain (Id::VcaCompression).hi, WithinAbs (100.0f, 1e-6f));
    REQUIRE_THAT (comp::hostDomain (Id::FetRatio).hi, WithinAbs (4.0f, 1e-6f));
}

// Push-to-reset lands on the active mode's own default, so a reset in FET does
// not leave the strip holding another mode's value, and every default is the
// one a fresh strip holds.
TEST_CASE ("comp reset defaults match a fresh strip's values", "[comp][timing]")
{
    Session s;
    const CompParams fresh;
    auto& strip = s.track (0).strip;

    for (const int mode : kStripModes)
        for (const Role role : kRoles)
        {
            if (! comp::modeHasRole (mode, role)) continue;
            const auto id = comp::roleParam (mode, role).id;
            CAPTURE (mode, compParamSpec (id).symbol);
            strip.compMode.store (mode, std::memory_order_relaxed);
            comp::applyTrackRoleFrac (strip, role, 0.83f);
            REQUIRE (comp::resetTrackRole (strip, role));
            REQUIRE_THAT (strip.comp.get (id), WithinAbs (fresh.get (id), 1e-6f));
            REQUIRE_THAT (fresh.get (id), WithinAbs (hostDefault (id), 1e-6f));
        }
    for (const auto id : compStageParams (CompStage::Strip))
        REQUIRE_THAT (strip.comp.get (id), WithinAbs (fresh.get (id), 1e-6f));
}
