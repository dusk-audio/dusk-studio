#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/CompParams.h"

#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

namespace
{
std::vector<std::string> symbolsOf (CompStage stage)
{
    std::vector<std::string> out;
    for (const auto id : compStageParams (stage))
        out.emplace_back (compParamSpec (id).symbol);
    return out;
}

CompParamId idOf (const std::string& symbol)
{
    for (std::size_t i = 0; i < kCompParamCount; ++i)
        if (symbol == compParamSpec ((CompParamId) i).symbol)
            return (CompParamId) i;
    FAIL ("no parameter " << symbol);
    return CompParamId::Mix;
}

float plainToHostSkewed (float plain, float min, float max, float skew)
{
    return std::pow ((plain - min) / (max - min), skew);
}
} // namespace

TEST_CASE ("CompParams starts every parameter at its spec default", "[session][comp]")
{
    const CompParams comp;
    std::set<std::string> seen;
    for (std::size_t i = 0; i < kCompParamCount; ++i)
    {
        const auto id = (CompParamId) i;
        const auto& spec = compParamSpec (id);
        CAPTURE (spec.symbol);
        CHECK (seen.insert (spec.symbol).second);
        CHECK (spec.hostMin < spec.hostMax);
        CHECK (spec.hostDefault >= spec.hostMin);
        CHECK (spec.hostDefault <= spec.hostMax);
        if (spec.integer) CHECK_THAT (spec.hostDefault, WithinAbs (std::round (spec.hostDefault), 1e-9));
        CHECK_THAT (comp.get (id), WithinAbs (spec.hostDefault, 1e-6));
    }
}

// The tapered controls' host defaults, by Multi-Comp 2's plainToHost of the
// plain defaults (daf-plugin/MultiCompParams.hpp).
TEST_CASE ("CompParams tapered defaults are the plug-in's knob positions", "[session][comp]")
{
    const auto def = [] (const char* symbol) { return compParamSpec (idOf (symbol)).hostDefault; };
    CHECK_THAT (def ("sidechain_hp"), WithinAbs (0.0f, 1e-7));
    CHECK_THAT (def ("fet_attack"), WithinAbs (plainToHostSkewed (0.2f, 0.02f, 80.0f, 0.3f), 1e-6));
    CHECK_THAT (def ("bus_fade_rate"), WithinAbs (0.5f + 0.5f * (24.8f - 6.0f) / (60.0f - 6.0f), 1e-6));
    CHECK_THAT (def ("mb_crossover_1"), WithinAbs (plainToHostSkewed (200.0f, 20.0f, 500.0f, 0.4f), 1e-6));
    CHECK_THAT (def ("mb_crossover_2"), WithinAbs (plainToHostSkewed (2000.0f, 200.0f, 5000.0f, 0.4f), 1e-6));
    CHECK_THAT (def ("mb_crossover_3"), WithinAbs (plainToHostSkewed (8000.0f, 2000.0f, 16000.0f, 0.4f), 1e-6));
    for (int band = 0; band < kCompBandCount; ++band)
    {
        CAPTURE (band);
        const auto at = [band] (CompBandField f) { return compParamSpec (compBandParam (band, f)).hostDefault; };
        CHECK_THAT (at (CompBandField::Ratio), WithinAbs (plainToHostSkewed (4.0f, 1.0f, 20.0f, 0.5f), 1e-6));
        CHECK_THAT (at (CompBandField::Attack), WithinAbs (plainToHostSkewed (10.0f, 0.1f, 100.0f, 0.4f), 1e-6));
        CHECK_THAT (at (CompBandField::Release), WithinAbs (plainToHostSkewed (100.0f, 10.0f, 1000.0f, 0.4f), 1e-6));
        CHECK_THAT (at (CompBandField::Enabled), WithinAbs (1.0f, 1e-9));
    }
    CHECK_THAT (def ("opto_gain"), WithinAbs (23.409226f, 1e-5));
    CHECK_THAT (def ("vca_compression"), WithinAbs (50.4944f, 1e-5));
}

TEST_CASE ("CompParams set clamps, rounds integers and defaults non-finite values", "[session][comp]")
{
    CompParams comp;
    SECTION ("continuous")
    {
        comp.set (CompParamId::VcaThreshold, -80.0f);
        CHECK_THAT (comp.get (CompParamId::VcaThreshold), WithinAbs (-55.0f, 1e-6));
        comp.set (CompParamId::VcaThreshold, 7.0f);
        CHECK_THAT (comp.get (CompParamId::VcaThreshold), WithinAbs (0.0f, 1e-6));
        comp.set (CompParamId::VcaThreshold, -12.25f);
        CHECK_THAT (comp.get (CompParamId::VcaThreshold), WithinAbs (-12.25f, 1e-6));
        comp.set (CompParamId::FetAttack, 0.37f);
        CHECK_THAT (comp.get (CompParamId::FetAttack), WithinAbs (0.37f, 1e-6));
        comp.set (CompParamId::FetAttack, 1.5f);
        CHECK_THAT (comp.get (CompParamId::FetAttack), WithinAbs (1.0f, 1e-6));
    }
    SECTION ("integer")
    {
        comp.set (CompParamId::FetRatio, 2.4f);
        CHECK_THAT (comp.get (CompParamId::FetRatio), WithinAbs (2.0f, 1e-6));
        comp.set (CompParamId::FetRatio, 2.6f);
        CHECK_THAT (comp.get (CompParamId::FetRatio), WithinAbs (3.0f, 1e-6));
        comp.set (CompParamId::FetRatio, 9.0f);
        CHECK_THAT (comp.get (CompParamId::FetRatio), WithinAbs (4.0f, 1e-6));
        comp.set (CompParamId::BusAttack, -3.0f);
        CHECK_THAT (comp.get (CompParamId::BusAttack), WithinAbs (0.0f, 1e-6));
        comp.set (compBandParam (2, CompBandField::Solo), 0.9999f);
        CHECK_THAT (comp.get (compBandParam (2, CompBandField::Solo)), WithinAbs (1.0f, 1e-6));
    }
    SECTION ("non-finite")
    {
        comp.set (CompParamId::BusThreshold, -9.0f);
        comp.set (CompParamId::BusThreshold, std::numeric_limits<float>::quiet_NaN());
        CHECK_THAT (comp.get (CompParamId::BusThreshold), WithinAbs (0.0f, 1e-6));
        comp.set (CompParamId::OptoGain, std::numeric_limits<float>::infinity());
        CHECK_THAT (comp.get (CompParamId::OptoGain), WithinAbs (23.409226f, 1e-5));
        comp.set (CompParamId::BusHeadroom, -std::numeric_limits<float>::infinity());
        CHECK_THAT (comp.get (CompParamId::BusHeadroom), WithinAbs (3.0f, 1e-6));
    }
}

TEST_CASE ("CompParams stage lists name what each stage stores", "[session][comp]")
{
    CHECK (symbolsOf (CompStage::Strip) == std::vector<std::string> {
        "mix", "sidechain_hp", "auto_makeup",
        "opto_peak_reduction", "opto_gain", "opto_limit", "opto_meter",
        "fet_input", "fet_output", "fet_attack", "fet_release", "fet_ratio", "fet_threshold", "fet_meter",
        "vca_threshold", "vca_compression", "vca_output" });
    CHECK (symbolsOf (CompStage::Bus) == std::vector<std::string> {
        "sidechain_hp", "auto_makeup", "bus_threshold", "bus_ratio", "bus_attack", "bus_release",
        "bus_makeup", "bus_mix", "bus_headroom", "bus_fade_rate", "bus_fade" });

    std::vector<std::string> multiband {
        "auto_makeup", "mb_crossover_1", "mb_crossover_2", "mb_crossover_3", "mb_mix", "mb_output" };
    for (int band = 0; band < kCompBandCount; ++band)
        for (const char* field : { "mb_threshold", "mb_ratio", "mb_attack", "mb_release",
                                   "mb_makeup", "mb_bypass", "mb_solo", "mb_enabled" })
            multiband.push_back (std::string (field) + "_" + std::to_string (band));
    CHECK (symbolsOf (CompStage::Multiband) == multiband);

    for (const auto stage : { CompStage::Strip, CompStage::Bus, CompStage::Multiband })
        for (const auto& symbol : symbolsOf (stage))
        {
            CAPTURE (symbol);
            CHECK (symbol != "mode");
            CHECK (symbol != "bypass");
        }
    CHECK (std::string (compParamSpec (compBandParam (3, CompBandField::Makeup)).symbol) == "mb_makeup_3");
}

TEST_CASE ("CompParams resets, copies and snapshots its values", "[session][comp]")
{
    CompParams comp;
    comp.set (CompParamId::OptoGain, 60.0f);
    comp.set (CompParamId::BusRatio, 2.0f);
    comp.set (compBandParam (1, CompBandField::Threshold), -33.0f);

    CompParams copy;
    copy.copyFrom (comp);
    CHECK_THAT (copy.get (CompParamId::OptoGain), WithinAbs (60.0f, 1e-6));
    CHECK_THAT (copy.get (CompParamId::BusRatio), WithinAbs (2.0f, 1e-6));
    CHECK_THAT (copy.get (compBandParam (1, CompBandField::Threshold)), WithinAbs (-33.0f, 1e-6));

    CompParams restored;
    restored.setValues (comp.values());
    const auto want = comp.values();
    const auto got = restored.values();
    for (std::size_t i = 0; i < kCompParamCount; ++i)
        CHECK_THAT (got[i], WithinAbs (want[i], 1e-9));

    comp.resetToDefaults (CompStage::Strip);
    CHECK_THAT (comp.get (CompParamId::OptoGain), WithinAbs (23.409226f, 1e-5));
    CHECK_THAT (comp.get (CompParamId::BusRatio), WithinAbs (2.0f, 1e-6));
    CHECK_THAT (comp.get (compBandParam (1, CompBandField::Threshold)), WithinAbs (-33.0f, 1e-6));

    comp.resetToDefaults (CompStage::Bus);
    CHECK_THAT (comp.get (CompParamId::BusRatio), WithinAbs (0.0f, 1e-6));
    CHECK_THAT (comp.get (compBandParam (1, CompBandField::Threshold)), WithinAbs (-33.0f, 1e-6));

    comp.resetToDefaults (CompStage::Multiband);
    CHECK_THAT (comp.get (compBandParam (1, CompBandField::Threshold)), WithinAbs (-20.0f, 1e-6));
}
