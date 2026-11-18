#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/builtin/DafPlugin.h"
#include "session/CompMigrationTables.h"
#include "session/CompParams.h"

#include <core/MultiCompModes.hpp>
#include <core/MultiCompParams.hpp>
#include <core/MultiCompVcaLaw.hpp>

#include <cstddef>
#include <string>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// The session stores Multi-Comp 2's parameters in the plug-in's host domain
// under its own symbols, and the format 11 migration lands old values on its
// laws. Both were copied from the donor so the session layer builds without
// it; these hold each copy to the donor it came from.

TEST_CASE ("Comp donor guard: every stored parameter has the plug-in's host range, default and type",
           "[compressor][session][donor]")
{
    const auto plugin = duskstudio::builtin::createMultiComp2 (0);
    REQUIRE (plugin != nullptr);
    const auto& descs = plugin->params();

    for (std::size_t i = 0; i < duskstudio::kCompParamCount; ++i)
    {
        const auto& spec = duskstudio::compParamSpec ((duskstudio::CompParamId) i);
        CAPTURE (spec.symbol);
        const duskstudio::builtin::DafParamDesc* desc = nullptr;
        for (const auto& d : descs)
            if (d.symbol == spec.symbol)
                desc = &d;
        REQUIRE (desc != nullptr);
        CHECK_FALSE (desc->isOutput);
        CHECK_THAT (desc->minValue, WithinAbs (spec.hostMin, 0.0f));
        CHECK_THAT (desc->maxValue, WithinAbs (spec.hostMax, 0.0f));
        // A tapered control's default is the plug-in's plainToHost of its
        // plain default, worked at run time through std::pow; the session
        // holds it as a literal, so the two may sit a float step apart.
        CHECK_THAT (desc->defaultValue, WithinAbs (spec.hostDefault, 2.0e-6f));
        CHECK (desc->isInteger == spec.integer);
    }
}

namespace mirror = duskstudio::legacycomp::mirror;

TEST_CASE ("Comp donor guard: the migration's copied tables equal Multi-Comp 2's", "[compressor][session][donor]")
{
    SECTION ("Opto Gain taper")
    {
        CHECK_THAT (mirror::kOptoGainSilentKnobMax, WithinAbs (duskaudio::kOptoGainSilentKnobMax, 0.0f));
        CHECK_THAT (mirror::kOptoGainMuteDb, WithinAbs (duskaudio::kOptoGainMuteDb, 0.0f));
        REQUIRE (mirror::kOptoGainTaper.size() == duskaudio::kOptoGainTaper.size());
        for (std::size_t i = 0; i < mirror::kOptoGainTaper.size(); ++i)
        {
            CAPTURE (i);
            CHECK_THAT (mirror::kOptoGainTaper[i].x, WithinAbs (duskaudio::kOptoGainTaper[i].knob, 0.0f));
            CHECK_THAT (mirror::kOptoGainTaper[i].y, WithinAbs (duskaudio::kOptoGainTaper[i].gainDb, 0.0f));
        }
    }

    SECTION ("FET BLACK voice")
    {
        const auto& voice = duskaudio::kFetBlackVoice;
        for (std::size_t i = 0; i < mirror::kFetInputRelativeDb.size(); ++i)
        {
            CAPTURE (i);
            CHECK_THAT (mirror::kFetInputRelativeDb[i], WithinAbs (voice.inputRelativeDb[i], 0.0f));
            CHECK_THAT (mirror::kFetOutputRelativeDb[i], WithinAbs (voice.outputRelativeDb[i], 0.0f));
        }
        CHECK_THAT (mirror::kFetInputTrimDb, WithinAbs (voice.inputTrimDb, 0.0f));
        CHECK_THAT (mirror::kFetOutputTrimDb, WithinAbs (voice.outputTrimDb, 0.0f));
        // The migration reads the four ratio positions it maps old ratios onto.
        for (std::size_t r = 0; r < mirror::kFetReductionSlopes.size(); ++r)
        {
            CAPTURE (r);
            CHECK_THAT (mirror::kFetReductionSlopes[r], WithinAbs (voice.reductionSlopes[r], 0.0f));
            CHECK_THAT (mirror::kFetAttackScales[r], WithinAbs (voice.attackScales[r], 0.0f));
        }
    }

    SECTION ("FET BLACK dynamics")
    {
        // The donor stores these as floats; the migration does its arithmetic
        // in double, so each copy is the float's decimal, within a float step.
        const auto& d = duskaudio::kFetDynamicsDefaults;
        CHECK_THAT (mirror::kFbAttackSeconds,   WithinRel ((double) d[38], 1.0e-7));
        CHECK_THAT (mirror::kFbAttackSlope,     WithinRel ((double) d[39], 1.0e-7));
        CHECK_THAT (mirror::kFbAttackCurve,     WithinRel ((double) d[40], 1.0e-7));
        CHECK_THAT (mirror::kFbRatioExponent,   WithinRel ((double) d[41], 1.0e-7));
        CHECK_THAT (mirror::kFbReleasePotFixed, WithinRel ((double) d[60], 1.0e-7));
        CHECK_THAT (mirror::kFbReleasePotStart, WithinRel ((double) d[61], 1.0e-7));
        CHECK_THAT (mirror::kFbReleasePotEnd,   WithinRel ((double) d[62], 1.0e-7));
    }

    SECTION ("VCA COMPRESSION law")
    {
        REQUIRE (mirror::kVcaCompressLaw.size() == duskaudio::vcaLaw::kCompressLaw.size());
        for (std::size_t i = 0; i < mirror::kVcaCompressLaw.size(); ++i)
        {
            CAPTURE (i);
            CHECK_THAT (mirror::kVcaCompressLaw[i].x, WithinAbs (duskaudio::vcaLaw::kCompressLaw[i].position, 0.0f));
            CHECK_THAT (mirror::kVcaCompressLaw[i].y, WithinAbs (duskaudio::vcaLaw::kCompressLaw[i].ratio, 0.0f));
        }
    }

    SECTION ("VCA detector calibration")
    {
        using duskaudio::vcaLaw::kDetectorCalibration;
        using duskaudio::vcaLaw::kHotDetectorCalibration;
        REQUIRE (mirror::kVcaDetectorCalibration.size()
                 == kDetectorCalibration.size() + kHotDetectorCalibration.size() - 1);
        REQUIRE_THAT (kHotDetectorCalibration.front().levelDb, WithinAbs (kDetectorCalibration.back().levelDb, 0.0f));
        for (std::size_t i = 0; i < mirror::kVcaDetectorCalibration.size(); ++i)
        {
            const bool hot = i >= kDetectorCalibration.size();
            const auto& donor = hot ? kHotDetectorCalibration[i - kDetectorCalibration.size() + 1]
                                    : kDetectorCalibration[i];
            CAPTURE (i, hot);
            CHECK_THAT (mirror::kVcaDetectorCalibration[i].x, WithinAbs (donor.levelDb, 0.0f));
            CHECK_THAT (mirror::kVcaDetectorCalibration[i].y, WithinAbs (donor.correctionDb, 0.0f));
        }
    }
}
