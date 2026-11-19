#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/MultibandCompPresets.h"

#if DUSKSTUDIO_HAS_DUSK_DSP
  #include "TestTempDirectory.h"
  #include "dsp/MultiCompStage.h"
  #include "session/Session.h"
  #include "session/SessionSerializer.h"

  #include <cmath>
  #include <cstdlib>
  #include <memory>
  #include <string>
#endif

using namespace duskstudio::mbpresets;

// Guards the transcribed DP-24 preset table against data-entry errors: every
// value must land inside Multi-Comp 2's multiband ranges, or it would clamp
// silently on apply.
TEST_CASE ("multiband comp presets stay within donor parameter ranges")
{
    REQUIRE (kNumPresets == 9);

    const auto checkBand = [] (const Band& b)
    {
        REQUIRE (b.thresholdDb >= -60.0f); REQUIRE (b.thresholdDb <= 0.0f);
        REQUIRE (b.ratio       >=   1.0f); REQUIRE (b.ratio       <= 20.0f);
        REQUIRE (b.makeupDb    >= -12.0f); REQUIRE (b.makeupDb    <= 12.0f);
        REQUIRE (b.attackMs    >=  0.1f);  REQUIRE (b.attackMs    <= 100.0f);
        REQUIRE (b.releaseMs   >=  10.0f); REQUIRE (b.releaseMs   <= 1000.0f);
    };

    for (int i = 0; i < kNumPresets; ++i)
    {
        const auto& p = kPresets[(size_t) i];
        REQUIRE (p.name != nullptr);
        checkBand (p.low);
        checkBand (p.mid);
        checkBand (p.high);

        // crossover_1 range [20, 500]; hi-xover feeds crossover_3 [2000, 16000].
        REQUIRE (p.lowXoverHz >= 20.0f);   REQUIRE (p.lowXoverHz <= 500.0f);
        REQUIRE (p.hiXoverHz  >= 2000.0f); REQUIRE (p.hiXoverHz  <= 16000.0f);
        REQUIRE (p.hiXoverHz  >  p.lowXoverHz);
    }
}

#if DUSKSTUDIO_HAS_DUSK_DSP
namespace
{
using duskstudio::CompBandField;
using duskstudio::CompParamId;
using duskstudio::CompParams;
using duskstudio::compBandParam;
using Catch::Matchers::WithinAbs;

// The plain number Multi-Comp 2 prints for a host value: "3.17 kHz" reads 3170.
double printed (const duskstudio::MultiCompStage& stage, CompParamId id, float host)
{
    const auto text = stage.valueText (id, host);
    CAPTURE (text);
    char* end = nullptr;
    const double number = std::strtod (text.c_str(), &end);
    REQUIRE (end != text.c_str());
    return text.find ("kHz") != std::string::npos ? number * 1000.0 : number;
}
} // namespace

TEST_CASE ("every multiband preset loads to the chart's numbers as Multi-Comp 2 reads them back",
           "[mastering][compressor]")
{
    duskstudio::MultiCompStage stage (duskstudio::CompStage::Multiband);
    for (int i = 0; i < kNumPresets; ++i)
    {
        const auto& preset = kPresets[(size_t) i];
        CAPTURE (preset.name);
        CompParams comp;
        // Off the defaults first, so every value the preset owns must move.
        comp.set (CompParamId::AutoMakeup, 1.0f);
        comp.set (compBandParam (2, CompBandField::Enabled), 1.0f);
        comp.set (compBandParam (0, CompBandField::Solo), 1.0f);
        comp.set (compBandParam (3, CompBandField::Bypass), 1.0f);
        comp.set (CompParamId::MbOutput, 3.0f);
        REQUIRE (apply (i, stage, comp));

        // Each control within its own step: tenths of a dB, a ratio or a ms
        // of attack, a whole ms of release, a whole Hz of crossover.
        const auto reads = [&stage, &comp] (CompParamId id, double plain, double step)
        {
            CAPTURE (duskstudio::compParamSpec (id).symbol);
            CHECK_THAT (printed (stage, id, comp.get (id)), WithinAbs (plain, step));
        };
        const auto band = [&] (int b, const Band& values)
        {
            CAPTURE (b);
            reads (compBandParam (b, CompBandField::Threshold), values.thresholdDb, 0.1);
            reads (compBandParam (b, CompBandField::Ratio),     values.ratio,       0.1);
            reads (compBandParam (b, CompBandField::Attack),    values.attackMs,    0.1);
            reads (compBandParam (b, CompBandField::Release),   values.releaseMs,   1.0);
            reads (compBandParam (b, CompBandField::Makeup),    values.makeupDb,    0.1);
            CHECK_THAT (comp.get (compBandParam (b, CompBandField::Enabled)), WithinAbs (1.0f, 0.0f));
            CHECK_THAT (comp.get (compBandParam (b, CompBandField::Bypass)), WithinAbs (0.0f, 0.0f));
            CHECK_THAT (comp.get (compBandParam (b, CompBandField::Solo)), WithinAbs (0.0f, 0.0f));
        };
        band (0, preset.low);
        band (1, preset.mid);
        band (3, preset.high);
        // High-mid is out, so Multi-Comp 2 splits low-mid from high at
        // crossover 3, and crossover 2 sits where its 1.5x spacing leaves
        // both chart splits as they are.
        CHECK_THAT (comp.get (compBandParam (2, CompBandField::Enabled)), WithinAbs (0.0f, 0.0f));
        CHECK_THAT (comp.get (compBandParam (2, CompBandField::Solo)), WithinAbs (0.0f, 0.0f));
        reads (CompParamId::MbCrossover1, preset.lowXoverHz, 1.0);
        reads (CompParamId::MbCrossover3, preset.hiXoverHz, 1.0);
        const double split2 = printed (stage, CompParamId::MbCrossover2, comp.get (CompParamId::MbCrossover2));
        CHECK (split2 >= 1.5 * preset.lowXoverHz);
        CHECK (split2 * 1.5 <= preset.hiXoverHz);
        CHECK_THAT (comp.get (CompParamId::AutoMakeup), WithinAbs (0.0f, 0.0f));
        // Output is the user's, not the preset's.
        CHECK_THAT (comp.get (CompParamId::MbOutput), WithinAbs (3.0f, 0.0f));
    }
    CompParams untouched;
    CHECK_FALSE (apply (-1, stage, untouched));
    CHECK_FALSE (apply (kNumPresets, stage, untouched));
    for (const auto id : duskstudio::compStageParams (duskstudio::CompStage::Multiband))
        CHECK_THAT (untouched.get (id), WithinAbs (duskstudio::compParamSpec (id).hostDefault, 0.0f));
}

TEST_CASE ("a mastering session saves and restores the multiband bands a preset wrote",
           "[mastering][compressor][session]")
{
    duskstudio::MultiCompStage stage (duskstudio::CompStage::Multiband);
    auto saved = std::make_unique<duskstudio::Session>();
    auto& mastering = saved->mastering();
    REQUIRE (apply (6, stage, mastering.comp));
    mastering.comp.set (compBandParam (0, CompBandField::Threshold), -27.5f);
    mastering.comp.set (CompParamId::MbOutput, -1.5f);
    mastering.compEnabled.store (true);

    duskstudio::test::TempDirectory dir ("dusk-mb-preset-");
    const auto file = dir.path() / "session.json";
    REQUIRE (duskstudio::SessionSerializer::save (*saved, file));

    auto loaded = std::make_unique<duskstudio::Session>();
    REQUIRE (duskstudio::SessionSerializer::load (*loaded, file));
    CHECK (loaded->mastering().compEnabled.load());
    for (const auto id : duskstudio::compStageParams (duskstudio::CompStage::Multiband))
    {
        CAPTURE (duskstudio::compParamSpec (id).symbol);
        CHECK_THAT (loaded->mastering().comp.get (id), WithinAbs (mastering.comp.get (id), 0.0f));
    }
}
#endif
